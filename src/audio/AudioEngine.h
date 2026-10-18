#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "PadSampler.h"
#include "dsp/Classic12Bit.h"
#include "dsp/TimeStretch.h"
#include "RecentAudio.h"
#include "PatternSequencer.h"
#include <array>
#include <atomic>
#include <vector>
#include <unordered_set>
namespace studiosp::audio {
class AudioEngine {
public:
    static constexpr int NUM_PADS = 16;
    PatternSequencer sequencer;
    juce::CriticalSection mutex;
    // Non-audio model; publish mutations through editPad/replacePad.
    std::array<model::PadData, NUM_PADS> pads;
    std::atomic<float> gain{0.8f}, cutoff{20000}, drive{0}, delayMix{0}, peak{0};
    std::atomic<bool> recording{false};
    std::atomic<int> recordedSamples{0};
    std::atomic<unsigned int> activePads{0};
    std::atomic<float> inputPeak{0};
    std::atomic<bool> previewPlaying{false};
    explicit AudioEngine(size_t historyBudgetBytes = 256u * 1024u * 1024u, size_t historyStepLimit = 128)
        : historyBudget(historyBudgetBytes), historySteps(historyStepLimit) {
        for (int i = 0; i < NUM_PADS; ++i) { pads[i].padIndex = i; published[i] = nullptr; voicePins[i] = nullptr; tailPins[i] = nullptr; }
        for (int i = 0; i < NUM_PADS; ++i) publishPad(i);
        reclaimer.startThread();
    }
    ~AudioEngine() { reclaimer.stopThread(-1); }
    void prepareToPlay(double rate, int) {
        const juce::ScopedLock guard(mutex);
        if (!std::isfinite(rate) || rate < 8000 || rate > 192000) rate = 48000;
        sampleRate = rate; stopAll(); recording = false; target = -1; recordedSamples = 0;
        const bool followHost = sequencer.followsHost() && sequencer.isArmed();
        sequencer.stop(); sequencer.prepare(); if (followHost) sequencer.start();
        peak = 0; inputPeak = 0;
        commands.finishedRead(commands.getNumReady()); // Drain as the consumer; never reset the producer index.
        panicRequested = false; mixTransitionLeft = 0; mixTransitionPending = false; lastMix.fill(0);
        delay.setSize(2, std::max(1, static_cast<int>(rate * 0.375)));
        delay.clear(); delayPosition = 0; filterState.fill(0);
        capture.setSize(2, static_cast<int>(rate * 60)); captureState = CaptureState::idle;
        recent.prepare(rate); recentResetTransition = false;
    }
    // Single producer (message thread), single consumer (audio callback).
    void enqueue(int pad, bool down, float velocity = 1, uint64_t auditionGeneration = 0) {
        if (down && commands.getFreeSpace() <= NUM_PADS) return; // Reserve room for gate releases.
        int a, n, b, m; commands.prepareToWrite(1, a, n, b, m);
        if (n) { events[static_cast<size_t>(a)] = {pad, down, velocity, auditionGeneration}; commands.finishedWrite(1); }
    }
    void panic() { sequencer.stop(); panicRequested = true; }
    void newProject() {
        const juce::ScopedLock guard(mutex);
        cancelResampling(); stopPreview();
        for (int i = 0; i < NUM_PADS; ++i) replacePad(i, model::PadData{}, false);
        resetHistory();
        sequencer.reset();
        gain = 0.8f; cutoff = 20000; drive = 0; delayMix = 0;
        clearRecentAudio();
        panic();
    }
    void clearRecentAudio() { recent.requestReset(); }
    model::PadData rescueRecentAudio() const {
        const juce::ScopedLock guard(mutex); return recent.snapshot();
    }
    RecentAudio::Mark markRecentAudio() const { return recent.mark(); }
    model::PadData rescueRecentAudio(RecentAudio::Mark position) const {
        const juce::ScopedLock guard(mutex); return recent.snapshot(position);
    }
    bool commitDraft(int destination, model::PadData data, uint64_t expectedRevision, bool allowOverwrite) {
        if (destination < 0 || destination >= NUM_PADS || !data.hasSample()) return false;
        const juce::ScopedLock guard(mutex);
        if (revisions[destination] != expectedRevision || (!allowOverwrite && pads[destination].hasSample())) return false;
        replacePad(destination, std::move(data)); return true;
    }
    void stopPreview() { ++previewGeneration; previewPublished = nullptr; previewStopRequested = true; }
    void setPreview(model::PadData data) {
        data = toPlaybackPad(std::move(data));
        data.mode = model::PlaybackMode::OneShot; data.volume = 0.65f;
        const juce::ScopedLock guard(mutex);
        snapshots.push_back(std::make_unique<const model::PadData>(std::move(data)));
        previewPublished = snapshots.back().get();
        enqueue(NUM_PADS, true, 1, ++previewGeneration);
    }
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi, int inputChannels = 0,
                      PatternSequencer::HostPosition host = {}) {
        // No editor lock, buffer copies, allocation or shared ownership changes here.
        // Cancellation is acknowledged by this thread before another capture may start.
        if (recent.resetIfRequested()) recentResetTransition = true;
        auto cancelled = CaptureState::cancelRequested;
        captureState.compare_exchange_strong(cancelled, CaptureState::idle);
        auto armed = CaptureState::armed;
        captureState.compare_exchange_strong(armed, CaptureState::capturing);
        acknowledgeCaptureStop();
        inputChannels = std::min(inputChannels, buffer.getNumChannels());
        float inputMaximum = 0;
        for (int ch = 0; ch < inputChannels; ++ch)
            inputMaximum = std::max(inputMaximum, buffer.getMagnitude(ch, 0, buffer.getNumSamples()));
        inputPeak = inputMaximum;
        if (captureState.load() == CaptureState::capturing && captureInput.load()) {
            const int position = recordedSamples.load();
            const int count = std::min(buffer.getNumSamples(), capture.getNumSamples() - position);
            for (int ch = 0; ch < 2; ++ch) {
                const int source = captureChannel.load() < 0 ? std::min(ch, inputChannels - 1) : captureChannel.load();
                if (source >= 0 && source < inputChannels) capture.copyFrom(ch, position, buffer, source, 0, count);
                else capture.clear(ch, position, count);
            }
            recordedSamples = position + count;
            if (position + count == capture.getNumSamples()) markCaptureFull();
        }
        buffer.clear(); // Input is recorded dry; it is never sent to the speakers.
        if (sequencer.beginBlock(host, buffer.getNumSamples(), sampleRate)) {
            stopAll(true); delay.clear(); filterState.fill(0);
        }
        if (previewStopRequested.exchange(false)) { previewVoice.stop(); previewPin = nullptr; previewPlaying = false; }
        if (panicRequested.exchange(false)) {
            stopAll(true); delay.clear(); filterState.fill(0);
            mixTransitionLeft = std::max(1, static_cast<int>(std::ceil(sampleRate * PadSamplerVoice::transitionMs / 1000.0)));
            mixTransitionLength = mixTransitionLeft; mixTransitionPending = true;
            commands.finishedRead(commands.getNumReady());
        }
        int a, n, b, m; commands.prepareToRead(commands.getNumReady(), a, n, b, m);
        for (int i = 0; i < n; ++i) dispatch(events[static_cast<size_t>(a + i)]);
        for (int i = 0; i < m; ++i) dispatch(events[static_cast<size_t>(b + i)]);
        commands.finishedRead(n + m);
        int cursor = 0;
        for (const auto metadata : midi) {
            // Only channel note/CC messages are supported; skip long SysEx without constructing an allocating MidiMessage.
            if (metadata.numBytes > 3) continue;
            const int offset = juce::jlimit(cursor, buffer.getNumSamples(), metadata.samplePosition);
            render(buffer, cursor, offset - cursor); cursor = offset;
            const auto message = metadata.getMessage();
            if (message.isNoteOnOrOff()) {
                const int pad = message.getNoteNumber() - 36;
                if (pad >= 0 && pad < NUM_PADS) dispatch({pad, message.isNoteOn(), message.getFloatVelocity()});
            } else if (message.isAllNotesOff() || message.isAllSoundOff()) stopAll(true);
        }
        render(buffer, cursor, buffer.getNumSamples() - cursor); midi.clear();
        const float frequency = cutoff.load(), distortion = drive.load(), wet = delayMix.load(), master = gain.load();
        const float coefficient = 1 - std::exp(-2 * juce::MathConstants<float>::pi * std::min(frequency, static_cast<float>(sampleRate * 0.45)) / static_cast<float>(sampleRate));
        for (int i = 0; i < buffer.getNumSamples(); ++i) {
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch) {
                float value = buffer.getSample(ch, i);
                filterState[static_cast<size_t>(ch)] += coefficient * (value - filterState[static_cast<size_t>(ch)]);
                value = frequency >= 19999 ? value : filterState[static_cast<size_t>(ch)];
                if (distortion > 0.001f) value = std::tanh(value * (1 + distortion * 8)) / std::tanh(1 + distortion * 8);
                const float delayed = delay.getSample(ch, delayPosition);
                delay.setSample(ch, delayPosition, value + delayed * 0.35f);
                buffer.setSample(ch, i, (value + delayed * wet) * master);
            }
            if (mixTransitionPending) {
                for (int ch = 0; ch < std::min(2, buffer.getNumChannels()); ++ch) mixCorrection[ch] = lastMix[ch] - buffer.getSample(ch, i);
                mixTransitionPending = false;
            }
            for (int ch = 0; ch < std::min(2, buffer.getNumChannels()); ++ch) {
                lastMix[ch] = buffer.getSample(ch, i) + (mixTransitionLeft > 0 ? mixCorrection[ch] * static_cast<float>(mixTransitionLeft) / mixTransitionLength : 0);
                buffer.setSample(ch, i, lastMix[ch]);
            }
            if (mixTransitionLeft > 0) --mixTransitionLeft;
            delayPosition = (delayPosition + 1) % delay.getNumSamples();
        }
        // Capture float mix before output protection, without reallocating inside the callback.
        if (captureState.load() == CaptureState::capturing && !captureInput.load()) {
            const int position = recordedSamples.load();
            const int count = std::min(buffer.getNumSamples(), capture.getNumSamples() - position);
            for (int ch = 0; ch < 2; ++ch) capture.copyFrom(ch, position, buffer, std::min(ch, buffer.getNumChannels() - 1), 0, count);
            recordedSamples = position + count;
            if (position + count == capture.getNumSamples()) markCaptureFull();
        }
        acknowledgeCaptureStop();
        if (recentResetTransition) {
            // Do not retain the release tail belonging to the previous project.
            if (mixTransitionLeft == 0) recentResetTransition = false;
        } else recent.write(buffer, false);
        // Browser audition bypasses pad FX and capture; it never occupies a pad.
        previewVoice.renderNextBlock(buffer, 0, buffer.getNumSamples());
        previewPin = previewVoice.snapshot(); previewPlaying = previewVoice.active();
        float maximum = 0;
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch) for (int i = 0; i < buffer.getNumSamples(); ++i) {
            const float value = juce::jlimit(-0.988553f, 0.988553f, buffer.getSample(ch, i));
            buffer.setSample(ch, i, value); maximum = std::max(maximum, std::abs(value));
        }
        peak = maximum;
        unsigned int mask = 0;
        for (int i = 0; i < NUM_PADS; ++i) if (voices[i].active()) mask |= 1u << i;
        activePads = mask;
    }
    void replacePad(int index, model::PadData data, bool remember = true) {
        if (index < 0 || index >= NUM_PADS) return;
        data.audioData.buildPeaks();
        const juce::ScopedLock guard(mutex);
        if (remember) { undo.push_back({{{index, pads[index]}}}); retireHistory(redo); }
        data.padIndex = index; pads[index] = std::move(data); publishPad(index); trimHistory();
    }
    model::PadData getPad(int index) const { const juce::ScopedLock guard(mutex); return pads[static_cast<size_t>(index)]; }
    void editPad(int index, const std::function<void(model::PadData&)>& edit) {
        const juce::ScopedLock guard(mutex); edit(pads[index]); publishPad(index); trimHistory();
    }
    void rememberPadEdit(int index) {
        const juce::ScopedLock guard(mutex); undo.push_back({{{index, pads[index]}}}); retireHistory(redo);
        trimHistory();
    }
    void collectRetired() {
        reclaimer.notify();
    }
private:
    void reclaimNow() {
        const juce::ScopedLock guard(mutex);
        snapshots.erase(std::remove_if(snapshots.begin(), snapshots.end(), [this](const auto& owned) {
            const auto* pointer = owned.get();
            if (acquiring.load() == pointer || previewPublished.load() == pointer || previewPin.load() == pointer) return false;
            for (int i = 0; i < NUM_PADS; ++i)
                if (published[i].load() == pointer || voicePins[i].load() == pointer || tailPins[i].load() == pointer) return false;
            return true;
        }), snapshots.end());
        retiredHistory.clear();
    }
public:
    static bool readSampleFile(const juce::File& file, model::PadData& data, juce::String& error) {
        juce::AudioFormatManager formats; formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
        if (!reader || reader->lengthInSamples <= 0 || !std::isfinite(reader->sampleRate) || reader->sampleRate < 8000
            || reader->sampleRate > 192000 || reader->numChannels == 0 || reader->lengthInSamples > reader->sampleRate * 300) {
            error = "Archivo no compatible: WAV, FLAC o AIFF hasta 192 kHz y 5 minutos."; return false;
        }
        data = {}; data.sampleName = file.getFileNameWithoutExtension().toStdString(); data.sampleRate = reader->sampleRate;
        data.audioData.setSize(2, static_cast<int>(reader->lengthInSamples));
        if (!reader->read(&data.audioData.writable(), 0, data.audioData.getNumSamples(), 0, true, true)) { error = "No se pudo leer el audio."; return false; }
        for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < data.audioData.getNumSamples(); ++i)
            if (!std::isfinite(data.audioData.getSample(ch, i))) { error = "El archivo contiene valores de audio no validos."; return false; }
        data.endSample = data.loopEndSample = data.audioData.getNumSamples(); return true;
    }
    bool loadFile(int index, const juce::File& file, juce::String& error) {
        model::PadData data;
        if (!readSampleFile(file, data, error)) return false;
        replacePad(index, std::move(data)); return true;
    }
    bool setClassicMode(int index, bool enabled, const std::function<bool()>& cancelled = {}) {
        if (index < 0 || index >= NUM_PADS) return false;
        model::PadData original; uint64_t version;
        { const juce::ScopedLock guard(mutex); original = pads[index]; version = revisions[index]; }
        if (!original.hasSample()) return false;
        auto converted = original.classicAudioData;
        if (enabled && converted.getNumSamples() == 0) converted = dsp::makeClassicSample(playbackAudio(original), original.sampleRate, cancelled);
        const juce::ScopedLock guard(mutex);
        if (revisions[index] != version || (cancelled && cancelled())) return false;
        if (pads[index].classic12Bit == enabled) return true;
        rememberPadEdit(index);
        pads[index].classicAudioData = std::move(converted); pads[index].classic12Bit = enabled; publishPad(index);
        trimHistory();
        return true;
    }
    static const model::SampleBuffer& playbackAudio(const model::PadData& data) {
        return data.stretchedAudioData.getNumSamples() > 0 ? data.stretchedAudioData : data.audioData;
    }
    std::array<uint64_t, NUM_PADS> getRevisions() const { const juce::ScopedLock guard(mutex); return revisions; }
    bool setStretchRatio(int index, double ratio, const std::function<bool()>& cancelled = {}, uint64_t expectedRevision = 0) {
        if (index < 0 || index >= NUM_PADS) return false;
        model::PadData original; uint64_t version;
        { const juce::ScopedLock guard(mutex); original = pads[index]; version = revisions[index]; }
        if (expectedRevision != 0 && version != expectedRevision) return false;
        if (!original.hasSample()) return false;
        auto stretched = dsp::makeStretchedSample(original.audioData, original.sampleRate, ratio, cancelled);
        auto classic = original.classic12Bit ? dsp::makeClassicSample(stretched, original.sampleRate, cancelled) : model::SampleBuffer{};
        const juce::ScopedLock guard(mutex);
        if (revisions[index] != version || (cancelled && cancelled())) return false;
        if (pads[index].stretchRatio == ratio) return true;
        rememberPadEdit(index); pads[index].stretchRatio = ratio;
        pads[index].stretchedAudioData = ratio == 1 ? model::SampleBuffer{} : std::move(stretched);
        pads[index].classicAudioData = std::move(classic); publishPad(index); trimHistory(); return true;
    }
    // Equal slices share original, stretched and classic buffers. One history step.
    // Called on a worker, outside processBlock; all destinations are explicit 1..N.
    bool chopPad(int source, int slices, const std::array<uint64_t, NUM_PADS>* expected = nullptr) {
        if (source < 0 || source >= NUM_PADS || (slices != 4 && slices != 8 && slices != 16)) return false;
        const juce::ScopedLock guard(mutex);
        if (expected) {
            if ((*expected)[source] != revisions[source]) return false;
            for (int i = 0; i < slices; ++i) if ((*expected)[i] != revisions[i]) return false;
        }
        const auto original = pads[source];
        const auto end = std::min<int64_t>(original.endSample, original.audioData.getNumSamples());
        const auto length = end - original.startSample;
        if (!original.hasSample() || original.startSample < 0 || length < slices) return false;
        Change change;
        for (int i = 0; i < slices; ++i) change.entries.push_back({i, pads[i]});
        undo.push_back(std::move(change)); retireHistory(redo);
        for (int i = 0; i < slices; ++i) {
            auto part = original; part.padIndex = i;
            part.sampleName += " / " + std::to_string(i + 1);
            part.startSample = part.loopStartSample = original.startSample + length * i / slices;
            part.endSample = part.loopEndSample = original.startSample + length * (i + 1) / slices;
            part.mode = model::PlaybackMode::OneShot;
            pads[i] = std::move(part); publishPad(i);
        }
        trimHistory();
        return true;
    }
    bool startResampling(int destination, bool fromInput = false, int channel = -1) {
        const juce::ScopedLock guard(mutex);
        if (destination < 0 || destination >= NUM_PADS || captureState.load() != CaptureState::idle || capture.getNumSamples() == 0) return false;
        captureInput = fromInput; captureChannel = channel;
        target = destination; recordedSamples = 0; recording = true; captureState = CaptureState::armed; return true;
    }
    void requestStopResampling() {
        auto current = captureState.load();
        while (current == CaptureState::capturing || current == CaptureState::armed)
            if (captureState.compare_exchange_weak(current, CaptureState::stopRequested)) return;
    }
    void cancelResampling() {
        const juce::ScopedLock guard(mutex); target = -1;
        recording = false;
        if (captureState.load() != CaptureState::idle) captureState = CaptureState::cancelRequested;
    }
    bool isCaptureReady() const { return captureState.load() == CaptureState::ready; }
    bool isCaptureStopping() const { return captureState.load() == CaptureState::stopRequested; }
    bool finishResampling() {
        const juce::ScopedLock guard(mutex);
        if (!isCaptureReady()) return false;
        if (target < 0 || recordedSamples.load() == 0) { target = -1; captureState = CaptureState::idle; return false; }
        model::PadData data; data.sampleName = (captureInput.load() ? "Input " : "Resample ") + std::to_string(target.load() + 1); data.sampleRate = sampleRate;
        data.audioData.setSize(2, recordedSamples.load());
        for (int ch = 0; ch < 2; ++ch) data.audioData.copyFrom(ch, 0, capture, ch, 0, recordedSamples.load());
        data.endSample = data.loopEndSample = recordedSamples.load(); replacePad(target.load(), std::move(data));
        target = -1; captureState = CaptureState::idle; return true;
    }
    bool hasCapture() const { return target >= 0; }
    bool isInputCapture() const { return captureInput.load(); }
    double getSampleRate() const { return sampleRate; }
    bool history(bool forward) {
        const juce::ScopedLock guard(mutex);
        auto& from = forward ? redo : undo; auto& to = forward ? undo : redo;
        if (from.empty()) return false;
        auto change = std::move(from.back()); from.pop_back(); Change inverse;
        for (const auto& entry : change.entries) inverse.entries.push_back({entry.index, pads[entry.index]});
        to.push_back(std::move(inverse));
        for (auto& entry : change.entries) { pads[entry.index] = std::move(entry.data); publishPad(entry.index); }
        trimHistory();
        return true;
    }
    void resetHistory() { const juce::ScopedLock guard(mutex); retireHistory(undo); retireHistory(redo); }
private:
    struct Event { int pad = 0; bool down = false; float velocity = 1; uint64_t previewGeneration = 0; };
    struct Entry { int index; model::PadData data; };
    struct Change { std::vector<Entry> entries; };
    const size_t historyBudget, historySteps;
    std::vector<std::vector<Change>> retiredHistory;
    // Called only under mutex, outside processBlock. Current pads do not count as
    // extra history memory; shared originals, chops and derived buffers count once.
    size_t historyMemoryBytes() const {
        std::unordered_set<const void*> seen;
        const auto visit = [&seen](const model::PadData& pad) {
            size_t bytes = 0;
            for (const auto* buffer : {&pad.audioData, &pad.stretchedAudioData, &pad.classicAudioData})
                if (buffer->identity() && seen.insert(buffer->identity()).second) bytes += buffer->memoryBytes();
            return bytes;
        };
        for (const auto& pad : pads) visit(pad);
        size_t bytes = 0;
        for (const auto* stack : {&undo, &redo})
            for (const auto& change : *stack)
                for (const auto& entry : change.entries) bytes += visit(entry.data);
        return bytes;
    }
    void trimHistory() {
        std::vector<Change> discarded;
        while (undo.size() + redo.size() > historySteps || historyMemoryBytes() > historyBudget) {
            auto& stack = undo.empty() ? redo : undo;
            if (stack.empty()) break;
            discarded.push_back(std::move(stack.front()));
            stack.erase(stack.begin());
        }
        // Keep destruction on the existing reclamation thread; playing voices
        // retain their independently pinned snapshots even after an eviction.
        retireHistory(discarded);
    }
    void retireHistory(std::vector<Change>& historyToRetire) {
        if (!historyToRetire.empty()) retiredHistory.push_back(std::move(historyToRetire));
        historyToRetire.clear(); reclaimer.notify();
    }
    std::array<uint64_t, NUM_PADS> revisions{};
    enum class CaptureState { idle, armed, capturing, stopRequested, ready, cancelRequested };
    std::atomic<CaptureState> captureState{CaptureState::idle};
    std::atomic<bool> captureInput{false};
    std::atomic<int> captureChannel{-1};
    std::array<std::atomic<const model::PadData*>, NUM_PADS> published, voicePins, tailPins;
    std::atomic<const model::PadData*> acquiring{nullptr};
    std::atomic<const model::PadData*> previewPublished{nullptr}, previewPin{nullptr};
    std::atomic<bool> previewStopRequested{false};
    std::atomic<uint64_t> previewGeneration{0};
    PadSamplerVoice previewVoice;
    std::vector<std::unique_ptr<const model::PadData>> snapshots;
    std::array<PadSamplerVoice, NUM_PADS> voices;
    std::array<Event, 128> events;
    juce::AbstractFifo commands{128};
    std::atomic<bool> panicRequested{false};
    std::vector<Change> undo, redo;
    juce::AudioBuffer<float> capture, delay;
    RecentAudio recent;
    bool recentResetTransition = false;
    std::array<float, 2> filterState{};
    std::atomic<double> sampleRate{48000};
    std::atomic<int> target{-1};
    int delayPosition = 0;
    std::array<float, 2> lastMix{}, mixCorrection{};
    int mixTransitionLeft = 0, mixTransitionLength = 1;
    bool mixTransitionPending = false;
    class Reclaimer : public juce::Thread {
    public:
        explicit Reclaimer(AudioEngine& engine) : Thread("StudioSP snapshot reclamation"), owner(engine) {}
        void run() override { while (!threadShouldExit()) { owner.reclaimNow(); wait(30); } }
    private:
        AudioEngine& owner;
    } reclaimer{*this};
    void acknowledgeCaptureStop() {
        if (captureState.load() != CaptureState::stopRequested) return;
        recording = false;
        auto stopped = CaptureState::stopRequested;
        captureState.compare_exchange_strong(stopped, CaptureState::ready);
    }
    void markCaptureFull() {
        recording = false;
        auto capturing = CaptureState::capturing;
        captureState.compare_exchange_strong(capturing, CaptureState::ready);
    }
    static model::PadData toPlaybackPad(model::PadData playback) {
        if (playback.stretchedAudioData.getNumSamples() > 0) {
            const int64_t count = playback.stretchedAudioData.getNumSamples();
            const double scale = playback.stretchRatio;
            playback.startSample = juce::jlimit<int64_t>(0, count - 1, static_cast<int64_t>(std::floor(playback.startSample * scale)));
            playback.endSample = juce::jlimit<int64_t>(playback.startSample + 1, count, static_cast<int64_t>(std::ceil(playback.endSample * scale)));
            playback.loopStartSample = juce::jlimit<int64_t>(playback.startSample, playback.endSample - 1, static_cast<int64_t>(std::floor(playback.loopStartSample * scale)));
            playback.loopEndSample = juce::jlimit<int64_t>(playback.loopStartSample + 1, playback.endSample, static_cast<int64_t>(std::ceil(playback.loopEndSample * scale)));
            playback.audioData = playback.stretchedAudioData;
        }
        if (playback.classic12Bit && playback.classicAudioData.getNumSamples() > 0) {
            const double scale = dsp::classicSampleRate / playback.sampleRate;
            const int64_t count = playback.classicAudioData.getNumSamples();
            playback.startSample = juce::jlimit<int64_t>(0, count - 1, static_cast<int64_t>(std::floor(playback.startSample * scale)));
            playback.endSample = juce::jlimit<int64_t>(playback.startSample + 1, count, static_cast<int64_t>(std::ceil(playback.endSample * scale)));
            playback.loopStartSample = juce::jlimit<int64_t>(playback.startSample, playback.endSample - 1, static_cast<int64_t>(std::floor(playback.loopStartSample * scale)));
            playback.loopEndSample = juce::jlimit<int64_t>(playback.loopStartSample + 1, playback.endSample, static_cast<int64_t>(std::ceil(playback.loopEndSample * scale)));
            playback.audioData = playback.classicAudioData; playback.sampleRate = dsp::classicSampleRate;
        }
        return playback;
    }
    void publishPad(int index) {
        ++revisions[index];
        snapshots.push_back(std::make_unique<const model::PadData>(toPlaybackPad(pads[index]))); published[index].store(snapshots.back().get());
        collectRetired();
    }
    const model::PadData* acquirePad(int index) {
        // Keep seq_cst ordering: reclamation must see either this pin or the voice pin.
        for (int attempt = 0; attempt < 4; ++attempt) {
            const auto* pointer = published[index].load(); acquiring.store(pointer);
            if (published[index].load() == pointer) return pointer;
        }
        acquiring.store(nullptr); return nullptr;
    }
    void updatePins(int index) {
        // Publish the old current pointer as a tail before releasing its pin.
        tailPins[index] = voices[index].tailSnapshot(); voicePins[index] = voices[index].snapshot();
    }
    void stopAll(bool smooth = false) {
        for (int i = 0; i < NUM_PADS; ++i) { if (smooth) voices[i].stopSmooth(); else voices[i].stop(); updatePins(i); } activePads = 0;
        previewVoice.stop(); previewPin = nullptr; previewPlaying = false;
    }
    void dispatch(Event event) {
        if (event.pad == NUM_PADS) {
            if (event.previewGeneration != previewGeneration.load()) return;
            for (int attempt = 0; attempt < 4; ++attempt) {
                const auto* pad = previewPublished.load(); acquiring = pad;
                if (pad == previewPublished.load()) {
                    previewVoice.stop(); if (pad) previewVoice.trigger(*pad, 1, sampleRate);
                    previewPin = previewVoice.snapshot(); acquiring = nullptr; return;
                }
            }
            acquiring = nullptr; return;
        }
        if (event.pad < 0 || event.pad >= NUM_PADS) return;
        if (!event.down) { voices[event.pad].release(); updatePins(event.pad); return; }
        const auto* pad = acquirePad(event.pad); if (!pad) return;
        bool choked = false;
        if (pad->chokeGroup > 0) for (int i = 0; i < NUM_PADS; ++i)
            if (i != event.pad && voices[i].snapshot() && voices[i].snapshot()->chokeGroup == pad->chokeGroup) { voices[i].stopSmooth(); updatePins(i); choked = true; }
        voices[event.pad].trigger(*pad, event.velocity, sampleRate, choked);
        updatePins(event.pad); acquiring.store(nullptr);
    }
    void render(juce::AudioBuffer<float>& buffer, int offset, int count) {
        sequencer.process(count, sampleRate, [&](int start, int length) {
            for (int i = 0; i < NUM_PADS; ++i) { voices[i].renderNextBlock(buffer, offset + start, length); updatePins(i); }
        }, [&](int, unsigned down, unsigned up) {
            for (int i = 0; i < NUM_PADS; ++i) if (up & (1u << i)) dispatch({i, false, 0});
            for (int i = 0; i < NUM_PADS; ++i) if (down & (1u << i)) dispatch({i, true, 1});
        });
    }
};
}
