#include "audio/PadSampler.h"
#include "PluginProcessor.h"
#include <iostream>
#include <cstdlib>
#include <array>
#include <thread>
#include <semaphore>
#include <chrono>
#include <new>
#include <limits>
#include "ui/WaveformViewport.h"
#include "model/SampleLibrary.h"
#include "model/SampleFolderSettings.h"
#include "audio/PadVariations.h"
#include "audio/RecentAudio.h"
#include "audio/PatternSequencer.h"
#include "audio/PatternMidi.h"
#include "ui/MainComponent.h"
#include "TestSupport.h"
thread_local bool auditingAudio = false;
thread_local int audioAllocations = 0, audioDeletes = 0;
void runMidiTests();
void* operator new(std::size_t bytes) {
    if (auditingAudio) ++audioAllocations;
    if (auto* pointer = std::malloc(bytes ? bytes : 1)) return pointer;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* pointer) noexcept { if (auditingAudio && pointer) ++audioDeletes; std::free(pointer); }
void operator delete[](void* pointer) noexcept { ::operator delete(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { ::operator delete(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { ::operator delete(pointer); }
int runTests() {
    using namespace studiosp;
    {
        juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_VST3);
        StudioSPProcessor plugin;
        juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Undefined);
        plugin.prepareToPlay(48000, 127);
        require(plugin.engine.sequencer.followsHost() && plugin.engine.sequencer.isArmed(), "New VST3 instances must default to DAW sync");
        plugin.prepareToPlay(44100, 256);
        require(plugin.engine.sequencer.isArmed(), "Preparing the device again must retain host-follow arming");
    }
    {
        struct Host : juce::AudioPlayHead {
            juce::Optional<PositionInfo> position;
            juce::Optional<PositionInfo> getPosition() const override { return position; }
        } host;
        StudioSPProcessor synced; synced.setPlayHead(&host); synced.prepareToPlay(48000, 127);
        auto& sequence = synced.engine.sequencer; sequence.setHostSync(true);
        model::PadData impulse; impulse.audioData.setSize(1, 32); impulse.audioData.writable().clear();
        impulse.audioData.setSample(0, 0, 0.5f); impulse.endSample = 32; impulse.fadeInMs = impulse.fadeOutMs = 0;
        synced.engine.replacePad(0, impulse, false); synced.engine.gain = 1;
        for (int step = 0; step < 16; ++step) sequence.setStep(0, 0, step, true);
        sequence.setSwing(50);
        juce::AudioBuffer<float> frames(2, 127); juce::MidiBuffer notes;
        auto process = [&](double ppq, double bpm, bool playing) {
            juce::AudioPlayHead::PositionInfo info; info.setBpm(bpm); info.setPpqPosition(ppq); info.setIsPlaying(playing);
            host.position = info;
            notes.addEvent(juce::MidiMessage::noteOn(1, 51, 1.0f), 23);
            audioAllocations = audioDeletes = 0; auditingAudio = true;
            synced.processBlock(frames, notes); auditingAudio = false;
            require(audioAllocations == 0 && audioDeletes == 0, "Host sync must not allocate in the audio callback");
        };
        process(0, 120, false); require(frames.getMagnitude(0, 127) == 0 && !sequence.isPlaying(), "DAW STOP must keep the sequencer silent");
        process(0, 120, true); require(std::abs(frames.getSample(0, 0) - 0.5f) < 1e-4f, "DAW PLAY must trigger step one");
        process(0.37, 120, true);
        require(std::abs(frames.getSample(0, 120) - 0.5f) < 1e-4f && frames.getSample(0, 119) == 0,
            "Seek must align to the next swung step using host PPQ");
        process(0.49, 60, true);
        require(frames.getMagnitude(0, 127) < 1e-4f, "Seek between steps must not invent a trigger beyond the smooth release tail");
        process(0.499, 60, true);
        require(std::abs(frames.getSample(0, 48) - 0.5f) < 1e-4f && sequence.effectiveBpm() == 60,
            "Tempo changes must use DAW BPM at exact sample offsets");
        sequence.requestPattern(1); sequence.setStep(1, 0, 0, true);
        process(3.999, 120, true);
        require(sequence.currentPattern() == 1 && std::abs(frames.getSample(0, 24) - 0.5f) < 1e-4f,
            "Pending patterns must switch at the host-aligned bar boundary");
        process(0, 120, true);
        require(sequence.currentStep() == 0 && std::abs(frames.getSample(0, 0) - 0.5f) < 1e-4f, "DAW loop rewind must realign step one");
        process(0, 120, false); require(!sequence.isPlaying(), "Host transport stop must be reflected in the sequencer");
        host.position.reset(); synced.processBlock(frames, notes);
        require(!sequence.isPlaying() && !sequence.hostAvailable(), "Missing playhead must wait silently without free-running");
        sequence.setBpm(95); sequence.setHostSync(false); sequence.start(); synced.processBlock(frames, notes);
        require(sequence.isPlaying() && sequence.effectiveBpm() == 95, "Internal mode must remain available without a host");
        sequence.setHostSync(true);
        juce::MemoryBlock state; synced.getStateInformation(state); StudioSPProcessor restored;
        require(restored.restoreState(state.getData(), static_cast<int>(state.getSize()))
            && restored.engine.sequencer.followsHost() && !restored.engine.sequencer.isPlaying(),
            "DAW clock selection must persist and wait for a host transport");
        auto legacy = juce::ValueTree::readFromData(state.getData(), state.getSize()); legacy.removeProperty("sequencerHostSync", nullptr);
        juce::MemoryOutputStream bytes; legacy.writeToStream(bytes);
        require(restored.restoreState(bytes.getData(), static_cast<int>(bytes.getDataSize()))
            && !restored.engine.sequencer.followsHost(), "Existing projects must retain internal tempo");
    }
    {
        audio::PatternSequencer sequence; sequence.setHostSync(true); sequence.setSwing(50);
        int event = 0;
        for (int start = 0; start < 100000; start += 127) {
            sequence.beginBlock({true, true, 120, start / 24000.0}, 127, 48000);
            auto verify = [&](int origin, int count) {
                sequence.process(count, 48000, [](int, int) {}, [&](int offset, unsigned, unsigned) {
                    const int expected = (event / 2) * 12000 + (event % 2) * 9000;
                    require(start + origin + offset == expected, "Continuous host sync must emit each swung step exactly once across split blocks");
                    ++event;
                });
            };
            verify(0, 23); verify(23, 104);
        }
        require(event == 17, "Host clock must preserve the four-beat pattern length");
        sequence.beginBlock({true, true, std::numeric_limits<double>::quiet_NaN(), 0}, 127, 48000);
        require(!sequence.hostAvailable() && !sequence.isPlaying(), "Invalid host tempo must wait silently");
        sequence.beginBlock({true, true, 120, std::numeric_limits<double>::infinity()}, 127, 48000);
        require(!sequence.hostAvailable(), "Invalid host position must not enter the clock");
    }
    {
        audio::PatternSequencer sequence;
        sequence.setStep(0, 0, 0, true); sequence.setStep(0, 1, 1, true);
        sequence.setStep(1, 2, 0, true);
        sequence.setSwing(50); sequence.start();
        std::vector<std::pair<int, unsigned>> hits;
        int position = 0;
        auto run = [&](int count) {
            sequence.process(count, 48000, [&](int, int) {}, [&](int offset, unsigned mask, unsigned) {
                hits.emplace_back(position + offset, mask);
            });
            position += count;
        };
        run(777); sequence.requestPattern(1);
        run(95224);
        require(hits.size() >= 17 && hits[0] == std::pair<int, unsigned>{0, 1}
            && hits[1] == std::pair<int, unsigned>{9000, 2}, "Swing must delay the second sixteenth at 120 BPM by 3000 samples");
        require(hits[2].first == 12000 && hits[16] == std::pair<int, unsigned>{96000, 4},
            "Swing must preserve pair duration and switch patterns only at the bar boundary across blocks");
        sequence.stop(); auto previous = hits.size(); run(1000);
        require(hits.size() == previous + 1 && hits.back().second == 0 && sequence.currentStep() == -1,
            "Stopped sequencer must only release the preceding gates");
        sequence.start(); run(1);
        require(hits.back() == std::pair<int, unsigned>{97001, 4}, "Restart must begin at step one of the requested pattern");
        sequence.reset(); const auto state = sequence.snapshot();
        require(state.bpm == 120 && state.swing == 0 && state.rows[0][0] == 0 && !sequence.isPlaying(),
            "Reset must clear patterns and restore tempo and swing");
    }
    {
        StudioSPProcessor source; source.prepareToPlay(48000, 127); source.engine.gain = 1;
        auto& seq = source.engine.sequencer;
        model::PadData impulse; impulse.audioData.setSize(1, 32); impulse.audioData.writable().clear();
        impulse.audioData.setSample(0, 0, 0.5f); impulse.endSample = 32;
        impulse.fadeInMs = impulse.fadeOutMs = 0;
        source.engine.replacePad(0, impulse, false);
        seq.setBpm(120); seq.setSwing(50); seq.setStep(0, 0, 0, true); seq.setStep(0, 0, 1, true);
        seq.start();
        juce::AudioBuffer<float> block(2, 127); juce::MidiBuffer notes;
        for (int start = 0; start < 9300; start += 127) {
            // Include a MIDI event to exercise scheduler spans split inside a block.
            notes.addEvent(juce::MidiMessage::noteOn(1, 51, 1.0f), 23);
            audioAllocations = audioDeletes = 0; auditingAudio = true;
            source.processBlock(block, notes); auditingAudio = false;
            require(audioAllocations == 0 && audioDeletes == 0, "Sequencer audio callback must not allocate or delete");
            for (int i = 0; i < block.getNumSamples(); ++i) {
                const int frame = start + i;
                require(std::abs(block.getSample(0, i) - (frame == 0 || frame == 9000 ? 0.5f : 0.0f)) < 1e-4f,
                    "Sequencer triggers must reach audio at exact swung sample offsets across MIDI and block boundaries");
            }
        }
        seq.setStep(3, 15, 15, true); seq.requestPattern(3); seq.setBpm(143); seq.setSwing(37);
        juce::MemoryBlock bytes; source.getStateInformation(bytes); StudioSPProcessor restored;
        require(restored.restoreState(bytes.getData(), static_cast<int>(bytes.getSize())), "Sequencer state must load");
        const auto loaded = restored.engine.sequencer.snapshot();
        require(loaded.bpm == 143 && loaded.swing == 37 && loaded.pattern == 3 && loaded.rows[3][15] == 32768
            && loaded.rows[0][0] == 3 && !restored.engine.sequencer.isPlaying(), "All patterns, BPM and swing must round-trip in STOP");
        auto tree = juce::ValueTree::readFromData(bytes.getData(), bytes.getSize()); tree.removeProperty("sequencer", nullptr);
        juce::MemoryOutputStream legacy; tree.writeToStream(legacy);
        require(restored.restoreState(legacy.getData(), static_cast<int>(legacy.getDataSize()))
            && restored.engine.sequencer.getBpm() == 120 && restored.engine.sequencer.row(3, 15) == 0,
            "Legacy projects must restore empty patterns and default tempo");
        tree.setProperty("sequencer", juce::MemoryBlock(1), nullptr); juce::MemoryOutputStream invalid; tree.writeToStream(invalid);
        require(!restored.restoreState(invalid.getData(), static_cast<int>(invalid.getDataSize())), "Malformed sequencer data must be rejected");
        source.engine.newProject();
        require(!seq.isPlaying() && seq.getBpm() == 120 && seq.row(0, 0) == 0 && seq.row(3, 15) == 0,
            "New Project must reset the sequencer");
    }
    {
        audio::PatternSequencer sequence;
        sequence.setBpm(std::numeric_limits<double>::quiet_NaN());
        sequence.setSwing(std::numeric_limits<double>::infinity());
        require(sequence.getBpm() == 120 && sequence.getSwing() == 0, "Non-finite clock settings must be ignored");
        sequence.setBpm(999); sequence.setSwing(-5);
        require(sequence.getBpm() == 240 && sequence.getSwing() == 0, "Clock settings must respect supported limits");
        sequence.setBpm(143); sequence.start();
        int absolute = 0, event = 0;
        for (int block = 0; block < 1500; ++block) {
            sequence.process(127, 44100, [](int, int) {}, [&](int offset, unsigned, unsigned) {
                const auto expected = static_cast<int>(std::ceil(event * (44100.0 * 15.0 / 143.0) - 1e-7));
                require(absolute + offset == expected, "Fractional step timing must not accumulate rounding drift");
                ++event;
            });
            absolute += 127;
        }
        require(event > 32, "Clock drift test must cross multiple pattern wraps");
    }
    {
        audio::RecentAudio recent; recent.prepare(10);
        require(!recent.snapshot().hasSample(), "Recent audio must start empty");
        juce::AudioBuffer<float> frames(2, 50);
        for (int block = 0; block < 8; ++block) {
            for (int i = 0; i < 50; ++i) {
                frames.setSample(0, i, static_cast<float>(block * 50 + i) / 1000);
                frames.setSample(1, i, -static_cast<float>(block * 50 + i) / 1000);
            }
            recent.write(frames);
        }
        const auto saved = recent.snapshot();
        require(saved.audioData.getNumSamples() == 300 && saved.sampleRate == 10,
                "Rescue must return exactly the last thirty seconds after ring wrap");
        require(std::abs(saved.audioData.getSample(0, 0) - 0.1f) < 1e-6f
                && std::abs(saved.audioData.getSample(1, 299) + 0.399f) < 1e-6f,
                "Rescue must keep chronological order and both stereo channels");
        const auto marked = recent.mark(); juce::AudioBuffer<float> shortlyAfter(2, 10); shortlyAfter.clear(); recent.write(shortlyAfter);
        require(std::abs(recent.snapshot(marked).audioData.getSample(0, 0) - 0.1f) < 1e-6f,
                "Rescue must end at the button press rather than include audio played later");
        recent.write(frames);
        require(!recent.snapshot(marked).hasSample(), "An overwritten rescue mark must fail instead of returning newer audio");
        recent.requestReset(); require(!recent.snapshot().hasSample(), "Reset must hide the previous project's audio immediately");
        recent.write(frames);
        require(recent.snapshot().audioData.getNumSamples() == 50, "Reset must discard previous ring contents");
        recent.prepare(20); recent.write(frames);
        require(recent.snapshot().sampleRate == 20 && recent.snapshot().audioData.getNumSamples() == 50,
                "Device reconfiguration must reset capture length and sample rate");
    }
    {
        audio::RecentAudio recent; recent.prepare(1000);
        std::atomic<bool> running{true};
        std::thread writer([&] {
            juce::AudioBuffer<float> block(2, 64);
            for (int batch = 0; batch < 520; ++batch) {
                for (int i = 0; i < 64; ++i) {
                    const float value = static_cast<float>(batch * 64 + i) / 100000;
                    block.setSample(0, i, value); block.setSample(1, i, -value);
                }
                recent.write(block); juce::Thread::sleep(1);
            }
            running = false;
        });
        int successful = 0;
        while (running.load()) {
            const auto snapshot = recent.snapshot();
            if (!snapshot.hasSample()) { juce::Thread::sleep(1); continue; }
            ++successful;
            for (int i = 0; i < snapshot.audioData.getNumSamples(); ++i) {
                const float left = snapshot.audioData.getSample(0, i), right = snapshot.audioData.getSample(1, i);
                require(left == -right, "Concurrent rescue must never mix different stereo frames");
                if (i > 0) require(std::abs(left - snapshot.audioData.getSample(0, i - 1) - 0.00001f) < 1e-6f,
                                   "Concurrent rescue must return a contiguous performance");
            }
            juce::Thread::sleep(1);
        }
        writer.join(); require(successful > 0, "Rescue must succeed while audio continues writing");
    }
    {
        audio::AudioEngine live; live.prepareToPlay(8000, 40);
        model::PadData source; source.sampleRate = 8000; source.audioData.setSize(1, 400);
        for (int i = 0; i < 400; ++i) source.audioData.setSample(0, i, 0.1f);
        source.endSample = source.loopEndSample = 400; source.fadeInMs = source.fadeOutMs = 0;
        live.replacePad(0, source, false); live.gain = 0.9f; live.drive = 0.4f;
        live.setPreview(source); live.enqueue(0, true);
        juce::AudioBuffer<float> output(2, 40); juce::MidiBuffer midi;
        auditingAudio = true; audioAllocations = audioDeletes = 0; live.processBlock(output, midi); auditingAudio = false;
        require(audioAllocations == 0 && audioDeletes == 0, "Retrospective capture must not allocate or delete on the audio thread");
        auto rescued = live.rescueRecentAudio();
        const float processed = std::tanh(0.1f * 4.2f) / std::tanh(4.2f) * 0.9f;
        require(rescued.audioData.getNumSamples() == 40 && std::abs(rescued.audioData.getSample(0, 4) - processed) < 1e-5f
                && output.getSample(0, 4) > processed + 0.05f, "Rescue must include master effects and exclude audition audio");
        rescued.startSample = 5; rescued.endSample = 25;
        auto trimmed = audio::trimRescuedAudio(rescued);
        require(trimmed.audioData.getNumSamples() == 20 && trimmed.startSample == 0 && trimmed.endSample == 20,
                "Rescue Save must store only the selected audio fragment");
        const auto version = live.getRevisions()[3];
        require(live.commitDraft(3, trimmed, version, false) && live.history(false) && !live.getPad(3).hasSample()
                && live.history(true), "Saving a draft to an empty pad must support Undo and Redo");
        require(!live.commitDraft(3, trimmed, live.getRevisions()[3], false), "Occupied pads must require explicit overwrite approval");
        require(!live.commitDraft(3, trimmed, version, true), "A stale overwrite confirmation must not replace changed pads");
        live.newProject(); require(!live.rescueRecentAudio().hasSample(), "New Project must immediately hide old retrospective audio");
        for (int i = 0; i < 8; ++i) live.processBlock(output, midi);
        const auto fresh = live.rescueRecentAudio();
        for (int i = 0; i < fresh.audioData.getNumSamples(); ++i)
            require(fresh.audioData.getSample(0, i) == 0, "New Project must not retain the old project's release tail");
    }
    {
        model::PadData source; source.audioData.setSize(1, 480);
        source.audioData.writable().clear(); source.endSample = source.loopEndSample = 480;
        source.audioData.buildPeaks(); source.sampleName = "Original";
        audio::VariationOptions options;
        const auto variations = audio::makePadVariations(source, options, 42);
        for (const auto& candidate : variations) {
            require(candidate.pitchSemitones != source.pitchSemitones, "Variations must create distinct pitch alternatives");
            require(candidate.audioData.identity() == source.audioData.identity() && candidate.startSample >= 0
                    && candidate.endSample <= 480 && candidate.startSample < candidate.endSample,
                    "Variations must share immutable originals and keep valid trim boundaries");
        }
        require(source.pitchSemitones == 0 && source.endSample == 480 && source.sampleName == "Original",
                "Generating alternatives must preserve the source pad");
        options.pitch = options.trim = options.fades = options.classic = false;
        for (const auto& candidate : audio::makePadVariations(source, options, 43))
            require(candidate.pitchSemitones == source.pitchSemitones && candidate.startSample == source.startSample
                    && candidate.endSample == source.endSample && candidate.fadeOutMs == source.fadeOutMs,
                    "Unchecked variation options must remain unchanged");
        options.classic = true;
        const auto classics = audio::makePadVariations(source, options, 44);
        require(classics[1].classic12Bit && classics[3].classic12Bit
                && classics[1].classicAudioData.identity() == classics[3].classicAudioData.identity(),
                "Classic alternatives must share one offline conversion");
        bool cancelled = false;
        try { audio::makePadVariations(source, options, 45, [] { return true; }); }
        catch (const std::exception&) { cancelled = true; }
        require(cancelled, "Variation generation must honour cancellation");
        options.classic = false; options.pitch = options.trim = options.fades = true; options.amount = 0;
        const auto unchanged = audio::makePadVariations(source, options, 46);
        require(unchanged[0].pitchSemitones == source.pitchSemitones && unchanged[0].endSample == source.endSample,
                "Zero intensity must preserve the original settings");
        std::array<model::PadData, 16> full; full.fill(source);
        ui::CreativePanel panel; panel.setSize(1048, 700); panel.showVariations(source, 0, full);
        juce::ComboBox* target = nullptr;
        juce::TextButton* save = nullptr;
        for (auto* child : panel.getChildren()) {
            if (auto* combo = dynamic_cast<juce::ComboBox*>(child)) if (combo->getName() == "Draft destination") target = combo;
            if (auto* button = dynamic_cast<juce::TextButton*>(child)) if (button->getButtonText() == "GUARDAR EN PAD") save = button;
        }
        require(target && save && target->getSelectedId() == 0 && !save->isEnabled() && target->indexOfItemId(1) < 0,
                "A full bank must require choosing a destination and must exclude the variation's source pad");
    }
    {
        audio::AudioEngine fresh;
        fresh.prepareToPlay(48000, 40);
        model::PadData loaded; loaded.audioData.setSize(1, 256);
        loaded.audioData.writable().clear(); loaded.audioData.setSample(0, 4, 0.5f);
        loaded.endSample = loaded.loopEndSample = 256;
        fresh.replacePad(0, loaded); fresh.replacePad(15, loaded);
        fresh.gain = 1.4f; fresh.cutoff = 800; fresh.drive = 0.7f; fresh.delayMix = 0.8f;
        fresh.setPreview(loaded); fresh.enqueue(0, true);
        require(fresh.startResampling(2), "New project fixture must arm a recording");
        fresh.newProject();
        for (int i = 0; i < 16; ++i) {
            const auto data = fresh.getPad(i);
            require(!data.hasSample() && data.padIndex == i && data.volume == 1 && data.pitchSemitones == 0
                    && data.mode == model::PlaybackMode::OneShot && !data.classic12Bit && data.stretchRatio == 1,
                    "New project must restore every pad to its initial state");
        }
        require(fresh.gain == 0.8f && fresh.cutoff == 20000 && fresh.drive == 0 && fresh.delayMix == 0,
                "New project must restore global effect defaults");
        require(!fresh.history(false) && !fresh.history(true) && !fresh.hasCapture(),
                "New project must clear Undo/Redo and cancel pending recording");
        juce::AudioBuffer<float> silence(2, 40); juce::MidiBuffer notes;
        for (int i = 0; i < 8; ++i) fresh.processBlock(silence, notes);
        require(silence.getMagnitude(0, 40) == 0 && !fresh.previewPlaying && !fresh.finishResampling(),
                "New project must discard queued notes, preview and cancelled capture");
        require(fresh.startResampling(0), "Recording must work again after the new project reset");
    }
    {
        audio::AudioEngine bounded;
        for (int i = 0; i < 140; ++i) {
            bounded.rememberPadEdit(0);
            bounded.editPad(0, [i](auto& data) { data.pitchSemitones = static_cast<float>(i); });
        }
        int undone = 0;
        while (bounded.history(false)) ++undone;
        require(undone == 128 && bounded.getPad(0).pitchSemitones == 11,
                "History must retain only the newest 128 operations");
        int redone = 0;
        while (bounded.history(true)) ++redone;
        require(redone == 128 && bounded.getPad(0).pitchSemitones == 139,
                "All retained operations must remain redoable after trimming");
    }
    {
        const auto makeSample = [](float level) {
            model::PadData data;
            data.audioData.setSize(1, 256); data.audioData.writable().clear();
            for (int i = 0; i < 256; ++i) data.audioData.setSample(0, i, level);
            data.endSample = data.loopEndSample = 256;
            data.fadeInMs = data.fadeOutMs = 0;
            data.audioData.buildPeaks();
            return data;
        };
        const auto a = makeSample(0.1f), b = makeSample(0.2f), c = makeSample(0.3f), d = makeSample(0.4f);
        audio::AudioEngine bounded(a.audioData.memoryBytes() * 2);
        for (const auto* sample : {&a, &b, &c, &d}) bounded.replacePad(0, *sample);
        require(bounded.history(false) && bounded.getPad(0).audioData.identity() == c.audioData.identity(),
                "Memory trimming must preserve the newest undo");
        require(bounded.history(false) && bounded.getPad(0).audioData.identity() == b.audioData.identity()
                && !bounded.history(false), "Memory budget must evict old audio across the entire history");
        require(bounded.history(true) && bounded.history(true)
                && bounded.getPad(0).audioData.identity() == d.audioData.identity(), "Retained audio must survive redo");
        bounded.history(false); bounded.replacePad(0, a);
        require(!bounded.history(true), "A new edit must invalidate redo after memory trimming");

        audio::AudioEngine shared(a.audioData.memoryBytes());
        for (int i = 0; i < 4; ++i) shared.replacePad(i, a, false);
        shared.replacePad(15, b, false);
        require(shared.chopPad(15, 4) && shared.history(false), "Chop history must count shared audio only once");
        for (int i = 0; i < 4; ++i)
            require(shared.getPad(i).audioData.identity() == a.audioData.identity(), "Chop undo must restore all destinations as a group");
        require(shared.history(true) && shared.getPad(3).audioData.identity() == b.audioData.identity(),
                "Grouped chop must remain redoable within the memory budget");

        auto derived = a; derived.stretchedAudioData = b.audioData; derived.classicAudioData = c.audioData;
        audio::AudioEngine conversions(a.audioData.memoryBytes());
        conversions.replacePad(0, derived, false); conversions.replacePad(0, a);
        require(!conversions.history(false), "Memory budget must include both stretched and classic buffers");

        audio::AudioEngine noExtraAudio(0);
        {
            auto looping = makeSample(0.1f); looping.mode = model::PlaybackMode::Loop;
            noExtraAudio.replacePad(0, looping, false);
        }
        noExtraAudio.prepareToPlay(48000, 40);
        noExtraAudio.rememberPadEdit(0); noExtraAudio.editPad(0, [](auto& data) { data.volume = 0.5f; });
        require(noExtraAudio.history(false), "Parameter edits sharing current audio must fit a zero extra-audio budget");
        juce::AudioBuffer<float> playing(2, 40); juce::MidiBuffer notes;
        noExtraAudio.enqueue(0, true); noExtraAudio.processBlock(playing, notes);
        noExtraAudio.replacePad(0, d);
        require(!noExtraAudio.history(false), "A zero budget must discard replaced audio from undo");
        juce::Thread::sleep(80); // Allow the reclamation thread to destroy evicted history.
        auditingAudio = true; audioAllocations = audioDeletes = 0;
        noExtraAudio.processBlock(playing, notes);
        auditingAudio = false;
        require(audioAllocations == 0 && audioDeletes == 0 && std::abs(playing.getSample(0, 4) - 0.08f) < 1e-5f,
                "History eviction must preserve playing snapshots without allocating or deleting on the audio thread");
    }
    const auto libraryRoot = juce::File::getCurrentWorkingDirectory().getChildFile("sample-search-test-" + juce::Uuid().toString());
    const auto nested = libraryRoot.getChildFile("Drums");
    require(nested.createDirectory().wasOk(), "Search fixture directory must be created");
    require(nested.getChildFile("Kick.WAV").replaceWithText("fixture") && libraryRoot.getChildFile("Bass.flac").replaceWithText("fixture") && libraryRoot.getChildFile("ignore.txt").replaceWithText("fixture"), "Search fixture files must be created");
    const auto library = model::scanSampleFolders({libraryRoot, nested, libraryRoot.getChildFile("missing")});
    require(library.size() == 2, "Local scan must recurse, ignore other formats and deduplicate overlapping roots");
    require(model::searchSamples(library, "  kIcK  ").size() == 1 && model::searchSamples(library, "Drums").size() == 1, "Search must match names and folder paths without case sensitivity");
    require(model::searchSamples(library, "").size() == 2 && model::searchSamples(library, "not-found").empty(), "Empty search must show all samples; unmatched search must be empty");
    int cancellationChecks = 0;
    require(model::scanSampleFolders({libraryRoot}, [&] { return ++cancellationChecks >= 3; }).empty()
        && cancellationChecks == 3, "Cancellation during traversal must stop before completing the scan");
    require(nested.getChildFile("Empty").createDirectory().wasOk(), "Empty-folder fixture must be created");
    cancellationChecks = 0;
    require(model::scanSampleFolders({nested.getChildFile("Empty")}, [&] { return ++cancellationChecks >= 2; }).empty()
        && cancellationChecks == 2, "Scans without audio files must also honour cancellation");
    const auto settingsFile = libraryRoot.getChildFile("settings/folders.json");
    const std::vector<juce::File> savedFolders{nested, libraryRoot.getChildFile("Disconnected disk")};
    require(model::writeSampleFolders(settingsFile, savedFolders) && model::readSampleFolders(settingsFile) == savedFolders,
        "Folder settings must round-trip absolute paths and retain disconnected locations");
    require(settingsFile.replaceWithText("broken JSON") && model::readSampleFolders(settingsFile).empty(),
        "Invalid folder settings must safely fall back to an empty library");
    require(libraryRoot.deleteRecursively(), "Search fixtures must be cleaned up");
    std::array<float, 256> constant; constant.fill(0.5f);
    require(std::abs(audio::dsp::SincInterpolator::interpolate(constant.data(), 256, 91.37) - 0.5f) < 1e-5f, "Sinc must preserve DC gain");
    require(audio::dsp::SincInterpolator::interpolate(nullptr, 0, 0) == 0, "Empty input must be silent");
    std::array<float, 256> alternating;
    for (int i = 0; i < 256; ++i) alternating[i] = i % 2 ? -1.0f : 1.0f;
    require(std::abs(audio::dsp::SincInterpolator::interpolate(alternating.data(), 256, 100, 2)) < 0.01f, "Downsampling must suppress source Nyquist");
    model::PadData pad; pad.sampleRate = 24000; pad.fadeInMs = pad.fadeOutMs = 0; pad.audioData.setSize(1, 16);
    for (int i = 0; i < 16; ++i) pad.audioData.setSample(0, i, 0.5f);
    pad.endSample = 16;
    audio::PadSamplerVoice voice;
    juce::AudioBuffer<float> output(2, 40); output.clear();
    voice.trigger(pad, 1, 48000); voice.renderNextBlock(output, 0, 40);
    require(!voice.active(), "One shot must stop at sample end");
    require(std::abs(output.getSample(0, 30) - 0.5f) < 1e-4f, "Source rate must be converted to host rate");
    require(output.getSample(1, 8) == output.getSample(0, 8), "Mono samples must feed both stereo outputs");
    require(output.getSample(0, 33) == 0, "One shot must not render beyond end");
    pad.mode = model::PlaybackMode::Gate; voice.trigger(pad, 1, 48000); voice.release(); require(!voice.active(), "Gate must stop on release");
    pad.mode = model::PlaybackMode::Loop; pad.loopStartSample = 4; pad.loopEndSample = 8;
    voice.trigger(pad, 1, 48000); voice.renderNextBlock(output, 0, 40); require(voice.active(), "Loop must wrap");
    voice.trigger(pad, 1, 48000); require(voice.active(), "Second loop trigger must start a bounded release tail");
    for (int i = 0; i < 8; ++i) voice.renderNextBlock(output, 0, 40);
    require(!voice.active(), "Loop toggle release must terminate");
    pad.mode = model::PlaybackMode::OneShot; pad.pan = 1;
    output.clear(); voice.trigger(pad, 0.5f, 48000); voice.renderNextBlock(output, 0, 4);
    require(output.getSample(0, 1) == 0 && std::abs(output.getSample(1, 1) - 0.25f) < 1e-4f, "Pan and velocity must be applied");
    StudioSPProcessor processor;
    auto& engine = processor.engine;
    processor.prepareToPlay(48000, 40); engine.gain = 1;
    pad.mode = model::PlaybackMode::OneShot; pad.pan = 0; pad.volume = 1; pad.pitchSemitones = 0;
    engine.replacePad(0, pad);
    output.clear(); juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 36, 1.0f), 8);
    processor.processBlock(output, midi);
    require(output.getSample(0, 7) == 0 && std::abs(output.getSample(0, 8) - 0.5f) < 1e-4f, "MIDI must respect sample offset");
    require(midi.isEmpty(), "Consumed MIDI must not pass through a sampler with no MIDI output");
    pad.chokeGroup = 1; engine.replacePad(0, pad); engine.replacePad(3, pad);
    midi.addEvent(juce::MidiMessage::noteOn(1, 36, 1.0f), 0);
    midi.addEvent(juce::MidiMessage::noteOn(1, 39, 1.0f), 8);
    processor.processBlock(output, midi);
    require(std::abs(output.getSample(0, 10) - 0.5f) < 1e-4f, "Choke must crossfade equal DC signals without a level jump");
    engine.panic(); processor.processBlock(output, midi);
    for (int i = 0; i < 8; ++i) processor.processBlock(output, midi); // Let STOP finish before testing an isolated bounce.
    engine.startResampling(1); engine.enqueue(0, true);
    processor.processBlock(output, midi); engine.requestStopResampling(); processor.processBlock(output, midi);
    require(engine.finishResampling(), "Capture must produce a sample after audio acknowledges stop");
    require(engine.pads[1].audioData.getNumSamples() == 40 && engine.pads[1].sampleRate == 48000, "Capture must use host rate and exact frame count");
    require(std::abs(engine.pads[1].audioData.getSample(0, 4) - 0.5f) < 1e-4f, "Internal bounce must capture the mix");
    require(engine.history(false) && !engine.pads[1].hasSample(), "Undo must restore empty destination");
    require(engine.history(true) && engine.pads[1].hasSample(), "Redo must restore capture");
    // Float headroom is preserved internally, before the sample-peak clipper.
    pad.volume = 2; engine.replacePad(0, pad); engine.gain = 1.5f;
    engine.startResampling(2); engine.enqueue(0, true); processor.processBlock(output, midi);
    const float protectedOutput = output.getSample(0, 4);
    engine.requestStopResampling(); processor.processBlock(output, midi); engine.finishResampling();
    require(engine.pads[2].audioData.getSample(0, 4) > 1.4f && protectedOutput < 0.99f, "Capture must precede output protection");
    engine.enqueue(0, true); engine.panic(); processor.processBlock(output, midi);
    for (int i = 0; i < 8; ++i) processor.processBlock(output, midi);
    require(output.getMagnitude(0, 40) == 0, "Panic must discard queued triggers and finish its release");
    juce::MemoryBlock state; processor.getStateInformation(state);
    StudioSPProcessor restored; restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    require(restored.engine.pads[2].audioData.getNumSamples() == 40 && restored.engine.pads[0].volume == 2, "State must round-trip samples and settings");
    require(restored.engine.pads[2].audioData.getSample(0, 4) == engine.pads[2].audioData.getSample(0, 4), "Float samples must round-trip exactly");
    const char invalid[] = "not a project"; restored.setStateInformation(invalid, sizeof(invalid));
    require(restored.engine.pads[2].hasSample(), "Invalid state must leave loaded project intact");
    // Editing and reclaiming must never mutate or free the playing sample.
    model::PadData sustained; sustained.sampleRate = 48000; sustained.fadeInMs = sustained.fadeOutMs = 0;
    sustained.mode = model::PlaybackMode::Loop; sustained.audioData.setSize(1, 1024);
    for (int i = 0; i < 1024; ++i) sustained.audioData.setSample(0, i, 0.25f);
    sustained.endSample = sustained.loopEndSample = 1024;
    // STOP must ramp over multiple callbacks even when per-pad fades are zero.
    {
        audio::AudioEngine stopping; stopping.prepareToPlay(48000, 40); stopping.gain = 1;
        stopping.replacePad(0, sustained); stopping.enqueue(0, true); stopping.processBlock(output, midi);
        const float beforeStop = output.getSample(0, 39);
        stopping.panic(); stopping.processBlock(output, midi);
        require(std::abs(output.getSample(0, 0) - beforeStop) < 0.01f && output.getSample(0, 39) > 0,
            "STOP must preserve continuity and ramp instead of hard-cutting zero-fade audio");
        for (int i = 0; i < 8; ++i) stopping.processBlock(output, midi);
        require(output.getMagnitude(0, 40) == 0, "STOP transition must terminate in bounded time");
    }
    // Variable host blocks, retrigger/choke, pinned old versions and RT new/delete audit.
    {
        audio::AudioEngine transitions; transitions.prepareToPlay(48000, 512); transitions.gain = 1;
        auto positive = sustained; positive.mode = model::PlaybackMode::OneShot; positive.chokeGroup = 1;
        auto negative = positive;
        for (int i = 0; i < 1024; ++i) negative.audioData.setSample(0, i, -0.25f);
        transitions.replacePad(0, positive); transitions.replacePad(1, negative);
        transitions.enqueue(0, true); transitions.processBlock(output, midi);
        float last = output.getSample(0, 39);
        transitions.enqueue(1, true);
        for (int blockSize : {1, 7, 40, 128, 256}) {
            juce::AudioBuffer<float> block(2, blockSize);
            audioAllocations = audioDeletes = 0; auditingAudio = true;
            transitions.processBlock(block, midi); auditingAudio = false;
            require(audioAllocations == 0 && audioDeletes == 0, "Transitions must not allocate or delete C++ objects in the callback");
            require(std::abs(block.getSample(0, 0) - last) < 0.01f, "Choke must remain continuous across variable host blocks");
            for (int i = 1; i < blockSize; ++i) require(std::abs(block.getSample(0, i) - block.getSample(0, i - 1)) < 0.01f, "Choke transition must not introduce a DC step");
            last = block.getSample(0, blockSize - 1);
        }
        require(std::abs(last + 0.25f) < 1e-4f, "Choke must finish on the new voice");
        transitions.replacePad(1, positive); transitions.enqueue(1, true);
        transitions.processBlock(output, midi);
        require(std::abs(output.getSample(0, 0) + 0.25f) < 0.01f, "Retrigger must begin from the previous snapshot output");
        // Repeated retriggers must steal a tail without discontinuity or dangling pointers.
        for (int i = 0; i < 30; ++i) {
            last = output.getSample(0, 39);
            transitions.replacePad(1, i % 2 ? positive : negative, false);
            transitions.enqueue(1, true); transitions.collectRetired();
            audioAllocations = audioDeletes = 0; auditingAudio = true;
            transitions.processBlock(output, midi); auditingAudio = false;
            require(audioAllocations == 0 && audioDeletes == 0, "Tail stealing must remain allocation-free");
            require(std::abs(output.getSample(0, 0) - last) < 0.01f, "Fast retrigger must bridge from the last mixed sample");
        }
        transitions.panic(); transitions.processBlock(output, midi);
        for (int i = 0; i < 8; ++i) transitions.processBlock(output, midi);
        require(output.getMagnitude(0, 40) == 0, "Fast retrigger tails must end after STOP");
        juce::AudioBuffer<float> zero(2, 0); transitions.processBlock(zero, midi);
        transitions.prepareToPlay(44100, 17); transitions.processBlock(output, midi);
        require(output.getMagnitude(0, 40) == 0, "Reprepare must reset queues and transition state");
    }
    // Offline stretch: duration changes while a 440 Hz tone and stereo relation survive.
    {
        model::PadData tone; tone.sampleRate = 48000; tone.audioData.setSize(2, 48000);
        auto& raw = tone.audioData.writable();
        for (int i = 0; i < 48000; ++i) {
            const float value = static_cast<float>(0.3 * std::sin(2 * juce::MathConstants<double>::pi * 440 * i / 48000));
            raw.setSample(0, i, value); raw.setSample(1, i, value * 0.5f);
        }
        tone.endSample = tone.loopEndSample = 48000; tone.audioData.buildPeaks();
        for (double ratio : {0.5, 1.5, 2.0}) {
            const auto stretched = audio::dsp::makeStretchedSample(tone.audioData, 48000, ratio);
            require(stretched.getNumSamples() == static_cast<int>(48000 * ratio), "Stretch must have the requested duration");
            int crossings = 0; const int first = 4000, lastFrame = stretched.getNumSamples() - 4000;
            for (int i = first + 1; i < lastFrame; ++i) {
                const float left = stretched.getSample(0, i), right = stretched.getSample(1, i);
                require(std::isfinite(left) && std::abs(right - left * 0.5f) < 0.02f, "Stretch must preserve finite stereo coherence");
                if (stretched.getSample(0, i - 1) <= 0 && left > 0) ++crossings;
            }
            const double frequency = crossings * 48000.0 / (lastFrame - first);
            require(std::abs(frequency - 440) < 5, "Time-stretch must preserve tone pitch");
        }
        bool cancelled = false;
        try { audio::dsp::makeStretchedSample(tone.audioData, 48000, 1.5, [] { return true; }); }
        catch (const std::runtime_error&) { cancelled = true; }
        require(cancelled, "Stretch must honour worker cancellation");
        bool invalidRatio = false;
        try { audio::dsp::makeStretchedSample(tone.audioData, 48000, std::numeric_limits<double>::quiet_NaN()); }
        catch (const std::invalid_argument&) { invalidRatio = true; }
        require(invalidRatio, "Stretch must reject nonfinite ratios before allocation");
        model::SampleBuffer tiny; tiny.setSize(1, 16); tiny.writable().clear();
        const auto tinyStretch = audio::dsp::makeStretchedSample(tiny, 48000, 2);
        require(tinyStretch.getNumSamples() == 32 && tinyStretch.getSample(0, 20) == 0, "Stretch must support short silent samples");
        for (int i = 0; i < 16; ++i) tiny.setSample(0, i, 0.25f);
        for (double rate : {8000.0, 48000.0, 192000.0}) for (double ratio : {0.5, 2.0}) {
            const auto shortAudio = audio::dsp::makeStretchedSample(tiny, rate, ratio);
            float energy = 0;
            for (int i = 0; i < shortAudio.getNumSamples(); ++i) {
                require(std::isfinite(shortAudio.getSample(0, i)), "Short stretched audio must remain finite");
                energy += std::abs(shortAudio.getSample(0, i));
            }
            require(shortAudio.getNumSamples() == static_cast<int>(16 * ratio) && energy > 0.001f,
                "Short nonzero samples must not become silent at supported sample rates");
        }
        audio::AudioEngine stretchedEngine; stretchedEngine.replacePad(0, tone, false);
        const auto original = stretchedEngine.getPad(0).audioData.identity();
        const auto versions = stretchedEngine.getRevisions();
        stretchedEngine.editPad(0, [](auto& data) { data.pan = 0.1f; });
        require(!stretchedEngine.setStretchRatio(0, 1.5, {}, versions[0]), "Queued stretch must reject a stale source version");
        bool edited = false;
        require(!stretchedEngine.setStretchRatio(0, 1.5, [&] {
            if (!edited) { edited = true; stretchedEngine.editPad(0, [](auto& data) { data.pan = 0.2f; }); }
            return false;
        }), "Stretch must reject edits made during processing");
        require(stretchedEngine.setStretchRatio(0, 1.5), "Offline stretch must publish");
        require(stretchedEngine.getPad(0).audioData.identity() == original && stretchedEngine.getPad(0).stretchedAudioData.getNumSamples() == 72000, "Stretch must preserve the original buffer");
        require(stretchedEngine.setClassicMode(0, true) && stretchedEngine.getPad(0).classicAudioData.getNumSamples() == 39060, "Classic conversion must follow stretched duration");
        require(stretchedEngine.setStretchRatio(0, 1), "Ratio one must restore original duration");
        require(stretchedEngine.history(false) && stretchedEngine.getPad(0).stretchRatio == 1.5, "Undo must restore stretch and classic buffers");
        require(stretchedEngine.history(true) && stretchedEngine.getPad(0).stretchRatio == 1, "Redo must restore original duration");
        StudioSPProcessor stretchedState; stretchedState.engine.replacePad(0, tone, false);
        require(stretchedState.engine.setStretchRatio(0, 1.5), "Stretch persistence fixture must publish");
        juce::MemoryBlock saved; stretchedState.getStateInformation(saved); StudioSPProcessor loaded;
        require(loaded.restoreState(saved.getData(), static_cast<int>(saved.getSize())) && loaded.engine.getPad(0).stretchRatio == 1.5
            && loaded.engine.getPad(0).stretchedAudioData.getNumSamples() == 72000, "State must rebuild stretch while preserving original audio");
        auto legacyStretch = juce::ValueTree::readFromData(saved.getData(), saved.getSize());
        legacyStretch.getChild(0).removeProperty("stretchRatio", nullptr); legacyStretch.getChild(0).removeProperty("classic12Bit", nullptr);
        juce::MemoryOutputStream legacySaved; legacyStretch.writeToStream(legacySaved);
        require(loaded.restoreState(legacySaved.getData(), static_cast<int>(legacySaved.getDataSize())) && loaded.engine.getPad(0).stretchRatio == 1
            && !loaded.engine.getPad(0).classic12Bit, "Old projects must default to original duration and classic mode off");
        // Chop shares buffers, partitions IN/OUT exactly, and undoes every destination together.
        auto chopped = tone; chopped.startSample = chopped.loopStartSample = 101; chopped.endSample = chopped.loopEndSample = 1002;
        stretchedEngine.replacePad(15, chopped, false); const auto previousPads = stretchedEngine.getPad(0);
        const auto staleChop = stretchedEngine.getRevisions(); stretchedEngine.editPad(1, [](auto& data) { data.pan = 0.4f; });
        require(!stretchedEngine.chopPad(15, 4, &staleChop), "Chop must reject changed destinations");
        require(stretchedEngine.chopPad(15, 4), "Chop must partition a valid selected region");
        int64_t boundary = 101;
        for (int i = 0; i < 4; ++i) {
            const auto part = stretchedEngine.getPad(i);
            require(part.audioData.identity() == chopped.audioData.identity() && part.startSample == boundary && part.endSample > part.startSample, "Chop must share audio and produce consecutive nonempty slices");
            boundary = part.endSample;
        }
        require(boundary == 1002 && stretchedEngine.getPad(15).startSample == 101, "Chop must cover the exact trim without altering an outside source pad");
        require(stretchedEngine.history(false) && stretchedEngine.getPad(0).stretchRatio == previousPads.stretchRatio
            && !stretchedEngine.getPad(2).hasSample(), "One Undo must restore all chop destinations");
        require(stretchedEngine.history(true) && stretchedEngine.getPad(3).endSample == 1002, "One Redo must restore all slices");
        require(stretchedEngine.chopPad(0, 16) && stretchedEngine.getPad(15).endSample == 326, "Chop must preserve a source that is also overwritten as a destination");
        require(stretchedEngine.history(false) && stretchedEngine.getPad(15).endSample == 1002, "Undo must restore an overwritten source pad");
    }
    engine.replacePad(0, sustained); engine.gain = 1; engine.enqueue(0, true); processor.processBlock(output, midi);
    std::binary_semaphore ownsLock{0}, releaseLock{0};
    std::thread locker([&] { const juce::ScopedLock lock(engine.mutex); ownsLock.release(); (void) releaseLock.try_acquire_for(std::chrono::milliseconds(100)); });
    ownsLock.acquire(); const auto callbackStart = std::chrono::steady_clock::now();
    processor.processBlock(output, midi); const auto callbackTime = std::chrono::steady_clock::now() - callbackStart;
    releaseLock.release(); locker.join();
    require(callbackTime < std::chrono::milliseconds(90), "Audio callback must not wait on editor lock");
    require(std::abs(output.getSample(0, 4) - 0.25f) < 1e-4f, "Holding the editor lock must not silence audio");
    auto replacement = sustained;
    replacement.audioData.setSample(0, 4, -0.5f);
    require(sustained.audioData.getSample(0, 4) == 0.25f, "Sample mutations must detach shared storage");
    engine.replacePad(0, {}); engine.collectRetired(); processor.processBlock(output, midi);
    require(std::abs(output.getSample(0, 4) - 0.25f) < 1e-4f, "Clearing a pad must preserve the already playing voice");
    engine.panic(); processor.processBlock(output, midi); engine.collectRetired();
    engine.replacePad(0, sustained); engine.enqueue(0, true); processor.processBlock(output, midi);
    for (int i = 0; i < 8; ++i) processor.processBlock(output, midi); // Measure steady playback after the retrigger crossfade.
    std::atomic<bool> failed{false};
    std::thread audioThread([&] {
        juce::AudioBuffer<float> block(2, 40); juce::MidiBuffer notes;
        for (int i = 0; i < 1000; ++i) {
            if (i % 20 == 0) notes.addEvent(juce::MidiMessage::noteOn(1, 37, 1.0f), 0);
            processor.processBlock(block, notes);
            if (!std::isfinite(block.getSample(0, 4)) || block.getSample(0, 4) < 0.24f) failed = true;
        }
    });
    for (int i = 0; i < 500; ++i) {
        engine.replacePad(1, i % 2 ? sustained : model::PadData{}, false);
        engine.editPad(0, [i](auto& data) { data.pitchSemitones = static_cast<float>(i % 12); });
        engine.collectRetired();
        if (i % 50 == 0) { juce::MemoryBlock snapshot; processor.getStateInformation(snapshot); }
    }
    audioThread.join(); require(!failed.load(), "Concurrent publication, reclaim and save must keep audio running");
    engine.panic(); processor.processBlock(output, midi); engine.collectRetired();
    // Fades ramp the beginning/end of a trimmed region and gate note-off.
    auto faded = sustained; faded.mode = model::PlaybackMode::OneShot; faded.fadeInMs = faded.fadeOutMs = 0.2f; faded.endSample = 40;
    voice.stop(); voice.trigger(faded, 1, 48000); output.clear(); voice.renderNextBlock(output, 0, 40);
    require(output.getSample(0, 0) == 0 && output.getSample(0, 39) == 0, "Fade boundaries must reach silence");
    require(output.getSample(0, 4) > 0 && output.getSample(0, 4) < output.getSample(0, 15), "Attack must ramp into the sample");
    faded.mode = model::PlaybackMode::Gate; faded.endSample = 1024; voice.stop(); voice.trigger(faded, 1, 48000);
    voice.renderNextBlock(output, 0, 20); voice.release(); require(voice.active(), "Gate fade must not hard-stop on release");
    output.clear(); voice.renderNextBlock(output, 0, 20); require(!voice.active(), "Gate fade must finish after its configured duration");
    ui::WaveformViewport viewport; viewport.setSize(1000); viewport.zoom(2, 0.5);
    require(viewport.sampleAt(0) == 250 && viewport.sampleAt(1) == 750, "Zoom must preserve anchor");
    viewport.pan(10); require(viewport.sampleAt(1) == 1000, "Pan must stay within sample");
    viewport.fitSelection(100, 200); require(viewport.sampleAt(0) == 100 && viewport.sampleAt(1) == 200, "Selection fit must display trimmed region");
    int64_t start = 100, end = 200;
    ui::WaveformViewport::moveMarker(true, 999, 1000, start, end); require(start == 199, "Start marker must not cross end");
    ui::WaveformViewport::moveMarker(false, -100, 1000, start, end); require(end == 200, "End marker must not cross start");
    faded.audioData.buildPeaks(); const auto peak = faded.audioData.range(0, 1024);
    require(peak.high == 0.25f, "Cached waveform must preserve exact peaks");
    model::SampleBuffer spikes; spikes.setSize(2, 1025);
    spikes.writable().clear(); spikes.setSample(0, 255, 0.8f); spikes.setSample(1, 256, -0.9f); spikes.buildPeaks();
    const auto stereoPeak = spikes.range(254, 257);
    require(stereoPeak.high == 0.8f && stereoPeak.low == -0.9f, "Waveform must preserve isolated peaks and both stereo channels");
    engine.replacePad(0, faded); processor.getStateInformation(state); require(restored.restoreState(state.getData(), static_cast<int>(state.getSize())), "New state must load");
    require(restored.engine.getPad(0).fadeInMs == faded.fadeInMs && restored.engine.getPad(0).fadeOutMs == faded.fadeOutMs, "Fades must persist");
    auto legacy = juce::ValueTree::readFromData(state.getData(), state.getSize());
    for (int i = 0; i < 16; ++i) { legacy.getChild(i).removeProperty("fadeIn", nullptr); legacy.getChild(i).removeProperty("fadeOut", nullptr); }
    juce::MemoryOutputStream legacyBytes; legacy.writeToStream(legacyBytes);
    require(restored.restoreState(legacyBytes.getData(), static_cast<int>(legacyBytes.getDataSize())) && restored.engine.getPad(0).fadeInMs == 0 && restored.engine.getPad(0).fadeOutMs == 0, "Existing projects must retain zero fades when fields are missing");
    engine.rememberPadEdit(0); engine.editPad(0, [](auto& data) { data.startSample = 100; });
    require(engine.history(false) && engine.getPad(0).startSample == 0, "Undo must restore marker edits");
    require(engine.history(true) && engine.getPad(0).startSample == 100, "Redo must restore marker edits");
    // Interface input must be captured before clearing the host buffer, without monitoring or FX.
    StudioSPProcessor recorder; recorder.prepareToPlay(48000, 40);
    juce::AudioBuffer<float> incoming(2, 40); juce::MidiBuffer emptyMidi;
    const auto fillInput = [&] { for (int i = 0; i < 40; ++i) { incoming.setSample(0, i, 0.2f); incoming.setSample(1, i, -0.4f); } };
    recorder.engine.gain = 0; recorder.engine.drive = 1;
    require(recorder.engine.startResampling(3, true, -1), "Stereo input recording must start");
    fillInput(); recorder.processBlock(incoming, emptyMidi);
    require(incoming.getMagnitude(0, 40) == 0, "Input must not feed speakers");
    require(std::abs(recorder.engine.inputPeak.load() - 0.4f) < 1e-6f, "Input meter must read the dry signal");
    recorder.engine.requestStopResampling(); fillInput(); recorder.processBlock(incoming, emptyMidi);
    require(recorder.engine.finishResampling(), "Stopped input recording must save");
    const auto stereoRecording = recorder.engine.getPad(3);
    require(stereoRecording.audioData.getNumSamples() == 40 && stereoRecording.audioData.getSample(0, 4) == 0.2f && stereoRecording.audioData.getSample(1, 4) == -0.4f, "Stereo capture must preserve dry channels and stop boundary");
    require(recorder.engine.history(false) && !recorder.engine.getPad(3).hasSample(), "Recording replacement must support undo");
    require(recorder.engine.startResampling(4, true, 1), "Second mono input must be selectable");
    fillInput(); recorder.processBlock(incoming, emptyMidi);
    recorder.engine.requestStopResampling(); recorder.processBlock(incoming, emptyMidi);
    require(recorder.engine.finishResampling(), "Mono input recording must save");
    const auto monoRecording = recorder.engine.getPad(4);
    require(monoRecording.audioData.getSample(0, 4) == -0.4f && monoRecording.audioData.getSample(1, 4) == -0.4f, "Selected mono input must duplicate to stereo");
    require(recorder.engine.startResampling(5, true, 1), "Missing-input test must arm");
    fillInput(); recorder.engine.processBlock(incoming, emptyMidi, 0);
    recorder.engine.requestStopResampling(); recorder.engine.processBlock(incoming, emptyMidi, 0);
    require(recorder.engine.finishResampling() && recorder.engine.getPad(5).audioData.getSample(0, 4) == 0, "Disconnected inputs must record silence instead of output or stale data");
    // Cancel at each stage; a stale finalizer must never stop a later recording.
    for (int stage = 0; stage < 3; ++stage) {
        require(recorder.engine.startResampling(6, true, 0), "Cancellation regression must arm");
        if (stage > 0) { fillInput(); recorder.processBlock(incoming, emptyMidi); }
        if (stage > 1) { recorder.engine.requestStopResampling(); recorder.processBlock(incoming, emptyMidi); }
        recorder.engine.cancelResampling();
        require(!recorder.engine.finishResampling(), "Cancelled capture must not save");
        require(!recorder.engine.startResampling(7, true, 0), "Restart must wait for audio cancellation acknowledgement");
        recorder.processBlock(incoming, emptyMidi);
        require(recorder.engine.startResampling(7, true, 0), "Capture must restart after cancellation acknowledgement");
        require(!recorder.engine.finishResampling(), "Stale finalizer must not finish an armed recording");
        fillInput(); recorder.processBlock(incoming, emptyMidi);
        require(recorder.engine.recording.load(), "Stale finalizer must not stop the restarted recording");
        recorder.engine.requestStopResampling(); recorder.processBlock(incoming, emptyMidi);
        require(recorder.engine.finishResampling() && recorder.engine.getPad(7).audioData.getSample(0, 4) == 0.2f,
            "Restarted capture must save the new input without getting stuck");
    }
    // Strong impulses just outside the trim must never leak through interpolation taps.
    model::PadData isolated; isolated.sampleRate = 48000; isolated.fadeInMs = isolated.fadeOutMs = 0;
    isolated.audioData.setSize(1, 64); isolated.audioData.writable().clear();
    isolated.audioData.setSample(0, 15, 1); isolated.audioData.setSample(0, 48, -1);
    isolated.startSample = isolated.loopStartSample = 16; isolated.endSample = isolated.loopEndSample = 48;
    juce::AudioBuffer<float> isolatedOutput(2, 256);
    for (const auto playback : {model::PlaybackMode::OneShot, model::PlaybackMode::Gate, model::PlaybackMode::Loop})
        for (const float semitones : {-12.0f, 0.0f, 12.0f})
            for (const double sourceRate : {24000.0, 48000.0}) {
                isolated.mode = playback; isolated.pitchSemitones = semitones; isolated.sampleRate = sourceRate;
                isolatedOutput.clear(); voice.stop(); voice.trigger(isolated, 1, 48000); voice.renderNextBlock(isolatedOutput, 0, 256);
                require(isolatedOutput.getMagnitude(0, 256) == 0, "Trim must exclude outside audio for pitch, rate conversion and loop playback");
            }
    isolated.mode = model::PlaybackMode::Loop; isolated.pitchSemitones = 12; isolated.sampleRate = 48000;
    isolated.loopStartSample = 24; isolated.loopEndSample = 40;
    isolated.audioData.setSample(0, 23, 1); isolated.audioData.setSample(0, 40, -1);
    isolatedOutput.clear(); voice.stop(); voice.trigger(isolated, 1, 48000); voice.renderNextBlock(isolatedOutput, 0, 256);
    require(isolatedOutput.getMagnitude(0, 20, 236) == 0, "After loop wrap interpolation must exclude the intro and samples past loopEnd");
    {
        audio::AudioEngine audition; audition.prepareToPlay(48000, 40); audition.gain = 0; audition.drive = 1; audition.delayMix = 1;
        model::PadData sample; sample.audioData.setSize(1, 1024); sample.audioData.writable().clear();
        for (int i = 0; i < 1024; ++i) sample.audioData.setSample(0, i, 0.2f);
        sample.endSample = sample.loopEndSample = 1024; sample.fadeInMs = sample.fadeOutMs = 0;
        const auto originalIdentity = audition.getPad(0).audioData.identity();
        audition.setPreview(sample); audition.stopPreview(); audition.processBlock(output, midi);
        require(output.getMagnitude(0, 40) == 0 && !audition.previewPlaying.load(), "STOP PREVIEW must cancel a queued audition before its first block");
        audition.setPreview(sample); audition.processBlock(output, midi);
        require(std::abs(output.getSample(0, 4) - 0.13f) < 1e-5f && audition.previewPlaying.load(), "Preview must bypass pad FX and remain audible with master gain at zero");
        require(audition.getPad(0).audioData.identity() == originalIdentity && !audition.history(false), "Audition must not replace pads or create undo entries");
        require(audition.startResampling(1), "Preview isolation test must arm capture");
        audition.processBlock(output, midi); audition.requestStopResampling(); audition.processBlock(output, midi);
        require(audition.finishResampling() && audition.getPad(1).audioData.getSample(0, 4) == 0, "Preview must be excluded from internal resampling");
        audition.stopPreview(); audition.processBlock(output, midi);
        require(output.getMagnitude(0, 40) == 0 && !audition.previewPlaying.load(), "Stopping audition must silence its voice");
    }
    {
        model::SampleBuffer constant12; constant12.setSize(1, 4096);
        for (int i = 0; i < 4096; ++i) constant12.setSample(0, i, 0.12345f);
        const auto classic = audio::dsp::makeClassicSample(constant12, 48000);
        require(classic.getNumSamples() == static_cast<int>(std::ceil(4096 * 26040.0 / 48000)), "Classic conversion must use precisely 26.04 kHz and preserve duration to one target frame");
        for (int i = 0; i < classic.getNumSamples(); ++i)
            require(classic.getSample(0, i) == audio::dsp::quantize12Bit(0.12345f), "Linear 12-bit conversion must produce the expected quantization level");
        require(audio::dsp::quantize12Bit(-2) == -1 && audio::dsp::quantize12Bit(2) == 2047.0f / 2048,
            "Classic quantization must clip to signed 12-bit limits");
        model::SampleBuffer ultrasonic; ultrasonic.setSize(1, 4096);
        for (int i = 0; i < 4096; ++i) ultrasonic.setSample(0, i, static_cast<float>(std::sin(2 * juce::MathConstants<double>::pi * 18000 * i / 48000)));
        const auto filtered = audio::dsp::makeClassicSample(ultrasonic, 48000);
        float aliasPeak = 0;
        for (int i = 100; i < filtered.getNumSamples() - 100; ++i) aliasPeak = std::max(aliasPeak, std::abs(filtered.getSample(0, i)));
        require(aliasPeak < 0.01f, "Classic downsampling must suppress frequencies above the target Nyquist limit");
        StudioSPProcessor classicProcessor; classicProcessor.prepareToPlay(48000, 40); classicProcessor.engine.gain = 1;
        model::PadData sample; sample.audioData = constant12; sample.endSample = sample.loopEndSample = 4096; sample.fadeInMs = sample.fadeOutMs = 0;
        classicProcessor.engine.replacePad(0, sample, false); const auto sourceIdentity = classicProcessor.engine.getPad(0).audioData.identity();
        require(classicProcessor.engine.setClassicMode(0, true), "Classic mode must enable for a loaded pad");
        require(classicProcessor.engine.getPad(0).audioData.identity() == sourceIdentity && classicProcessor.engine.getPad(0).classic12Bit, "Classic mode must preserve the original sample");
        classicProcessor.engine.enqueue(0, true); classicProcessor.processBlock(output, midi);
        require(std::abs(output.getSample(0, 4) - audio::dsp::quantize12Bit(0.12345f)) < 1e-5f, "Pad playback must use the classic converted buffer");
        juce::MemoryBlock classicState; classicProcessor.getStateInformation(classicState); StudioSPProcessor classicRestored;
        require(classicRestored.restoreState(classicState.getData(), static_cast<int>(classicState.getSize()))
            && classicRestored.engine.getPad(0).classic12Bit && classicRestored.engine.getPad(0).classicAudioData.getNumSamples() == classic.getNumSamples(), "Project restore must rebuild classic audio while retaining the original");
        require(classicProcessor.engine.history(false) && !classicProcessor.engine.getPad(0).classic12Bit, "Undo must restore original sound");
        require(classicProcessor.engine.history(true) && classicProcessor.engine.getPad(0).classic12Bit, "Redo must restore classic mode");
        require(classicProcessor.engine.setClassicMode(0, false) && classicProcessor.engine.getPad(0).audioData.getSample(0, 4) == 0.12345f, "Disabling classic mode must recover original float samples exactly");
    }
    model::PadData previewPad; previewPad.sampleName = "Waveform preview"; previewPad.sampleRate = 48000;
    previewPad.audioData.setSize(1, 24000);
    for (int i = 0; i < 24000; ++i) {
        const double t = static_cast<double>(i) / 48000;
        previewPad.audioData.setSample(0, i, static_cast<float>(std::sin(2 * juce::MathConstants<double>::pi * (70 * t + 30 * t * t)) * std::exp(-t * 6)));
    }
    previewPad.startSample = previewPad.loopStartSample = 2000; previewPad.endSample = previewPad.loopEndSample = 21000;
    previewPad.fadeInMs = 10; previewPad.fadeOutMs = 30; engine.replacePad(0, std::move(previewPad), false);
    processor.prepareToPlay(48000, 40); // Render a stopped editor rather than residual test voices/meters.
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    require(editor->isResizable(), "Editor must allow resizing on small displays");
    require(editor->getWidth() <= 1022 && editor->getHeight() <= 651,
            "Plugin must open at a compact size leaving room for the DAW interface");
    juce::ComboBox* interfaceSize = nullptr;
    for (auto* child : editor->getChildren())
        if (auto* combo = dynamic_cast<juce::ComboBox*>(child))
            if (combo->getName() == "Interface scale") interfaceSize = combo;
    require(interfaceSize != nullptr, "Editor must expose an interface scale selector inside the plugin");
    interfaceSize->setSelectedId(50, juce::dontSendNotification); interfaceSize->onChange();
    require(editor->getWidth() == 730 && editor->getHeight() == 465,
            "Selecting UI 50% must shrink the complete editor");
    interfaceSize->setSelectedId(70, juce::dontSendNotification); interfaceSize->onChange();
    require(editor->getWidth() == 1022 && editor->getHeight() == 651,
            "Selecting UI 70% must resize the complete editor proportionally");
    for (const auto size : {juce::Point<int>{730, 465}, {1460, 930}, {2190, 1395}, {1068, 680}}) {
        editor->setSize(size.x, size.y);
        for (auto* child : editor->getChildren()) {
            if (!child->isVisible()) continue;
            const auto bounds = editor->getLocalArea(child, child->getLocalBounds());
            require(editor->getLocalBounds().contains(bounds), "All editor controls must fit after shrinking and enlarging the window");
            const auto centre = editor->getLocalPoint(child, child->getLocalBounds().toFloat().getCentre());
            require(editor->getComponentAt(centre.roundToInt()) != editor.get(), "Scaled controls must remain hit-testable");
        }
    }
    editor->setVisible(true);
    juce::Image preview(juce::Image::RGB, editor->getWidth(), editor->getHeight(), true, juce::SoftwareImageType{});
    { juce::Graphics graphics(preview); editor->paintEntireComponent(graphics, true); }
    require(preview.getPixelAt(10, 10) == juce::Colour(0xff111416), "Editor preview must include the visible interface");
    auto imageFile = juce::File::getCurrentWorkingDirectory().getChildFile("StudioSP-editor.png");
    { juce::MemoryOutputStream imageStream; juce::PNGImageFormat format; require(format.writeImageToStream(preview, imageStream), "Editor preview must render"); require(imageFile.replaceWithData(imageStream.getData(), imageStream.getDataSize()), "Editor preview must save"); }
   #if !JUCE_ANDROID && !JUCE_IOS
    juce::TextButton* newProject = nullptr;
    for (auto* child : editor->getChildren())
        if (auto* button = dynamic_cast<juce::TextButton*>(child))
            if (button->getButtonText() == "NEW PROJECT") newProject = button;
    require(newProject != nullptr, "Editor must expose a New Project button");
    auto& defaultLook = juce::LookAndFeel::getDefaultLookAndFeel();
    const bool nativeAlerts = defaultLook.isUsingNativeAlertWindows();
    defaultLook.setUsingNativeAlertWindows(false);
    const auto originalAudio = engine.getPad(0).audioData.identity();
    std::function<juce::TextButton*(juce::Component&, const juce::String&)> findButton;
    findButton = [&](juce::Component& parent, const juce::String& name) -> juce::TextButton* {
        for (auto* child : parent.getChildren()) {
            if (auto* button = dynamic_cast<juce::TextButton*>(child)) if (button->getButtonText() == name) return button;
            if (auto* found = findButton(*child, name)) return found;
        }
        return nullptr;
    };
    ui::CreativePanel* creative = nullptr;
    ui::SequencerPanel* sequencerPanel = nullptr;
    for (auto* child : editor->getChildren()) if (auto* panel = dynamic_cast<ui::SequencerPanel*>(child)) sequencerPanel = panel;
    auto* showSequencer = findButton(*editor, "SEQUENCER");
    require(showSequencer && sequencerPanel, "Editor must expose the sequencer panel");
    showSequencer->onClick();
    require(sequencerPanel->isVisible(), "Sequencer button must open the grid");
    auto* patternD = findButton(*sequencerPanel, "D");
    require(patternD != nullptr, "All four patterns must be selectable"); patternD->onClick();
    juce::TextButton* lastStep = nullptr;
    for (auto* child : sequencerPanel->getChildren()) {
        if (child->getName() == "Pad 16, paso 16") lastStep = dynamic_cast<juce::TextButton*>(child);
        if (auto* slider = dynamic_cast<juce::Slider*>(child)) {
            if (slider->getName() == "BPM") slider->setValue(131, juce::sendNotificationSync);
            if (slider->getName() == "Swing") slider->setValue(42, juce::sendNotificationSync);
        }
    }
    require(lastStep != nullptr, "Grid must expose all 256 steps");
    lastStep->setToggleState(true, juce::dontSendNotification); lastStep->onClick();
    require(engine.sequencer.requestedPattern() == 3 && engine.sequencer.row(3, 15) == 32768
        && engine.sequencer.getBpm() == 131 && engine.sequencer.getSwing() == 42,
        "Pattern, step, BPM and swing controls must update the audio model");
    findButton(*sequencerPanel, "PLAY")->onClick(); require(engine.sequencer.isPlaying(), "PLAY must start the sequencer");
    findButton(*sequencerPanel, "STOP PATTERN")->onClick(); require(!engine.sequencer.isPlaying(), "STOP PATTERN must stop playback");
    juce::ComboBox* sequencerClock = nullptr;
    for (auto* child : sequencerPanel->getChildren()) if (child->getName() == "Sequencer clock") sequencerClock = dynamic_cast<juce::ComboBox*>(child);
    require(sequencerClock != nullptr, "Sequencer must expose DAW and internal clock choices");
    sequencerClock->setSelectedId(2, juce::sendNotificationSync);
    engine.processBlock(output, midi, 0, {true, true, 128, 0}); patternD->onClick();
    require(engine.sequencer.followsHost() && engine.sequencer.isPlaying(), "DAW selector must follow the host transport");
    for (auto* child : sequencerPanel->getChildren()) if (auto* slider = dynamic_cast<juce::Slider*>(child))
        if (slider->getName() == "BPM") require(!slider->isEnabled() && slider->getValue() == 128, "DAW BPM must be shown and manual editing disabled");
    auto* recordMidi = findButton(*sequencerPanel, "REC MIDI"); auto* exportMidi = findButton(*sequencerPanel, "EXPORT MIDI");
    require(recordMidi && exportMidi, "Sequencer must expose live MIDI recording and export");
    recordMidi->onClick(); require(engine.sequencer.isRecording(), "REC MIDI must arm overdub");
    juce::ToggleButton* quantizeMidi = nullptr;
    for (auto* child : sequencerPanel->getChildren()) if (auto* toggle = dynamic_cast<juce::ToggleButton*>(child))
        if (toggle->getButtonText() == "Quantize 1/16") quantizeMidi = toggle;
    require(quantizeMidi != nullptr, "Live recording must expose optional quantization");
    quantizeMidi->setToggleState(false, juce::dontSendNotification); quantizeMidi->onClick();
    require(!engine.sequencer.isQuantized(), "Quantization toggle must reach the audio model");
    bool exportRequested = false; auto originalExport = sequencerPanel->onExport;
    sequencerPanel->onExport = [&](audio::PatternSequencer::State state, int pattern, double tempo) {
        exportRequested = pattern == 3 && state.rows[3][15] == 32768 && tempo == 128;
    };
    exportMidi->onClick(); sequencerPanel->onExport = std::move(originalExport);
    require(exportRequested, "EXPORT MIDI must capture the selected pattern and current DAW tempo");
    for (const auto size : {juce::Point<int>{730, 465}, {1460, 930}}) {
        editor->setSize(size.x, size.y);
        for (auto* child : sequencerPanel->getChildren()) {
            require(sequencerPanel->getLocalBounds().contains(child->getBounds()), "All sequencer controls must fit the panel");
            require(editor->getLocalBounds().contains(editor->getLocalArea(child, child->getLocalBounds())),
                "Sequencer controls must fit the scaled editor");
        }
    }
    juce::Image sequencerImage(juce::Image::RGB, editor->getWidth(), editor->getHeight(), true);
    { juce::Graphics graphics(sequencerImage); editor->paintEntireComponent(graphics, true); }
    { juce::MemoryOutputStream stream; juce::PNGImageFormat format;
      require(format.writeImageToStream(sequencerImage, stream)
        && juce::File::getCurrentWorkingDirectory().getChildFile("StudioSP-sequencer.png").replaceWithData(stream.getData(), stream.getDataSize()),
        "Sequencer screenshot must render and save"); }
    sequencerClock->setSelectedId(1, juce::sendNotificationSync);
    findButton(*sequencerPanel, "CERRAR")->onClick(); engine.sequencer.reset();
    editor->setSize(1068, 680);
    for (auto* child : editor->getChildren()) if (auto* panel = dynamic_cast<ui::CreativePanel*>(child)) creative = panel;
    require(creative != nullptr, "Editor must include a creative review panel");
    auto* variantsButton = findButton(*editor, "VARIACIONES");
    auto* rescueButton = findButton(*editor, "RESCATAR / 30 s");
    require(variantsButton && rescueButton, "Both creative tools must be reachable in the main editor");
    int draftDestination = -1;
    for (int i = 1; i < 16; ++i) if (!engine.getPad(i).hasSample()) { draftDestination = i; break; }
    require(draftDestination >= 0, "Creative UI fixture must have an empty destination");
    variantsButton->onClick();
    require(creative->isVisible(), "Variations must open a review without modifying the original");
    auto* generate = findButton(*creative, "GENERAR 4");
    auto* firstVariant = findButton(*creative, "V1");
    auto* saveDraft = findButton(*creative, "GUARDAR EN PAD");
    require(generate && firstVariant && saveDraft, "Review must expose Generate, alternatives and Save");
    const auto saveScreenshot = [&](const juce::String& name) {
        juce::Image image(juce::Image::RGB, editor->getWidth(), editor->getHeight(), true, juce::SoftwareImageType{});
        { juce::Graphics graphics(image); editor->paintEntireComponent(graphics, true); }
        juce::MemoryOutputStream bytes; juce::PNGImageFormat format;
        require(format.writeImageToStream(image, bytes)
                && juce::File::getCurrentWorkingDirectory().getChildFile(name).replaceWithData(bytes.getData(), bytes.getDataSize()),
                "Creative panels must render into inspectable screenshots");
    };
    generate->onClick();
    struct UiCheck : juce::Timer {
        std::function<void()> check;
        void timerCallback() override { check(); }
    } uiCheck;
    int stage = 10, ticks = 0;
    uiCheck.check = [&] {
        require(++ticks < 250, "Creative review and New Project must complete without hanging");
        auto* confirmation = dynamic_cast<juce::AlertWindow*>(juce::Component::getCurrentlyModalComponent());
        if (stage == 10 && firstVariant->isEnabled()) {
            firstVariant->onClick(); saveScreenshot("StudioSP-variations.png"); saveDraft->onClick(); stage = 11;
        } else if (stage == 11 && !creative->isVisible() && engine.getPad(draftDestination).hasSample()) {
            require(engine.getPad(0).audioData.identity() == originalAudio
                    && engine.getPad(draftDestination).pitchSemitones != engine.getPad(0).pitchSemitones,
                    "Accepting a variation must copy it to an empty pad and preserve the original");
            require(engine.history(false) && !engine.getPad(draftDestination).hasSample(), "Accepted variations must support Undo");
            engine.enqueue(0, true); for (int i = 0; i < 600; ++i) processor.processBlock(output, midi);
            rescueButton->onClick(); stage = 12;
        } else if (stage == 12 && creative->isVisible()) {
            saveScreenshot("StudioSP-rescue.png");
            juce::ComboBox* destination = nullptr;
            for (auto* child : creative->getChildren()) if (auto* combo = dynamic_cast<juce::ComboBox*>(child))
                if (combo->getName() == "Draft destination") destination = combo;
            require(destination && destination->getSelectedId() == draftDestination + 1,
                    "Rescue must choose an empty pad by default");
            destination->setSelectedId(1, juce::dontSendNotification); saveDraft->onClick(); stage = 13;
        } else if (stage == 13 && confirmation) {
            require(engine.getPad(0).audioData.identity() == originalAudio, "Overwrite must wait for confirmation");
            confirmation->exitModalState(0); stage = 14;
        } else if (stage == 14 && !confirmation && saveDraft->isEnabled()) {
            require(engine.getPad(0).audioData.identity() == originalAudio, "Cancelling overwrite must preserve the destination");
            findButton(*creative, "CERRAR")->onClick(); newProject->onClick(); stage = 0;
        } else if (stage == 0 && confirmation) {
            require(engine.getPad(0).audioData.identity() == originalAudio,
                    "New Project must request confirmation before discarding samples");
            confirmation->exitModalState(0); stage = 1;
        } else if (stage == 1 && !confirmation && newProject->isEnabled()) {
            require(engine.getPad(0).audioData.identity() == originalAudio,
                    "Cancelling New Project must preserve the current project");
            newProject->onClick(); stage = 2;
        } else if (stage == 2 && confirmation) {
            confirmation->exitModalState(1); stage = 3;
        } else if (stage == 3 && editor->isEnabled() && !engine.getPad(0).hasSample()) {
            require(!engine.history(false), "Confirmed New Project must remove old undo history");
            uiCheck.stopTimer(); juce::MessageManager::getInstance()->stopDispatchLoop();
        }
    };
    uiCheck.startTimer(20);
    juce::MessageManager::getInstance()->runDispatchLoop();
    defaultLook.setUsingNativeAlertWindows(nativeAlerts);
   #endif
    std::cout << "StudioSP DSP, MIDI, resample, history, state, concurrent playback, fades and waveform tests passed\n";
    return 0;
}
int main() {
    try { juce::ScopedJuceInitialiser_GUI gui; runMidiTests(); return runTests(); }
    catch (const std::exception& error) { std::cerr << "Test exception: " << error.what() << '\n'; return 1; }
}
