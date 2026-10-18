#pragma once
#include "audio/AudioEngine.h"
class StudioSPProcessor : public juce::AudioProcessor {
public:
    StudioSPProcessor() : AudioProcessor(BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true).withOutput("Output", juce::AudioChannelSet::stereo(), true)) {}
    studiosp::audio::AudioEngine engine;
    const juce::String getName() const override { return "StudioSP"; }
    void prepareToPlay(double rate, int block) override {
        if (!clockInitialised.exchange(true) && wrapperType != wrapperType_Undefined && wrapperType != wrapperType_Standalone)
            engine.sequencer.setHostSync(true);
        engine.prepareToPlay(rate, block);
    }
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layout) const override {
        const auto input = layout.getMainInputChannelSet();
        return layout.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
            && (input.isDisabled() || input == juce::AudioChannelSet::mono() || input == juce::AudioChannelSet::stereo());
    }
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override {
        juce::ScopedNoDenormals noDenormals;
        studiosp::audio::PatternSequencer::HostPosition transport;
        if (auto* hostPlayHead = getPlayHead()) if (const auto position = hostPlayHead->getPosition()) {
            const auto bpm = position->getBpm(), ppq = position->getPpqPosition();
            if (bpm && ppq) transport = {true, position->getIsPlaying(), *bpm, *ppq};
        }
        engine.processBlock(buffer, midi, getTotalNumInputChannels(), transport);
    }
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 5; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Default"; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock& destination) override {
        std::array<studiosp::model::PadData, 16> saved;
        { const juce::ScopedLock guard(engine.mutex); saved = engine.pads; }
        juce::ValueTree root("StudioSP"); root.setProperty("version", 1, nullptr);
        root.setProperty("gain", engine.gain.load(), nullptr); root.setProperty("cutoff", engine.cutoff.load(), nullptr);
        root.setProperty("drive", engine.drive.load(), nullptr); root.setProperty("delay", engine.delayMix.load(), nullptr);
        const auto sequence = engine.sequencer.snapshot();
        juce::MemoryOutputStream patternData;
        patternData.writeInt(1); patternData.writeDouble(sequence.bpm); patternData.writeDouble(sequence.swing);
        patternData.writeInt(sequence.pattern);
        for (const auto& pattern : sequence.rows) for (auto row : pattern) patternData.writeShort(static_cast<short>(row));
        root.setProperty("sequencer", patternData.getMemoryBlock(), nullptr);
        root.setProperty("sequencerHostSync", engine.sequencer.followsHost(), nullptr);
        for (const auto& pad : saved) {
            juce::ValueTree child("Pad");
            child.setProperty("name", juce::String(pad.sampleName), nullptr); child.setProperty("rate", pad.sampleRate, nullptr);
            child.setProperty("volume", pad.volume, nullptr); child.setProperty("pitch", pad.pitchSemitones, nullptr);
            child.setProperty("pan", pad.pan, nullptr); child.setProperty("mode", static_cast<int>(pad.mode), nullptr);
            child.setProperty("choke", pad.chokeGroup, nullptr);
            child.setProperty("classic12Bit", pad.classic12Bit, nullptr);
            child.setProperty("stretchRatio", pad.stretchRatio, nullptr);
            child.setProperty("fadeIn", pad.fadeInMs, nullptr); child.setProperty("fadeOut", pad.fadeOutMs, nullptr);
            child.setProperty("start", static_cast<juce::int64>(pad.startSample), nullptr);
            child.setProperty("end", static_cast<juce::int64>(pad.endSample), nullptr);
            child.setProperty("loopStart", static_cast<juce::int64>(pad.loopStartSample), nullptr);
            child.setProperty("loopEnd", static_cast<juce::int64>(pad.loopEndSample), nullptr);
            juce::MemoryOutputStream audio;
            audio.writeInt(pad.audioData.getNumSamples()); audio.writeInt(pad.audioData.getNumChannels());
            for (int ch = 0; ch < pad.audioData.getNumChannels(); ++ch) for (int i = 0; i < pad.audioData.getNumSamples(); ++i) audio.writeFloat(pad.audioData.getSample(ch, i));
            child.setProperty("audio", audio.getMemoryBlock(), nullptr); root.addChild(child, -1, nullptr);
        }
        juce::MemoryOutputStream stream(destination, false); root.writeToStream(stream);
    }
    void setStateInformation(const void* bytes, int size) override {
        restoreState(bytes, size);
    }
    bool restoreState(const void* bytes, int size) {
        if (size <= 0 || bytes == nullptr) return false;
        auto root = juce::ValueTree::readFromData(bytes, static_cast<size_t>(size));
        if (!root.hasType("StudioSP") || static_cast<int>(root["version"]) != 1 || root.getNumChildren() != 16) return false;
        studiosp::audio::PatternSequencer::State sequence;
        if (root.hasProperty("sequencer")) {
            const auto property = root["sequencer"]; const auto* block = property.getBinaryData();
            if (!block || block->getSize() != 152) return false;
            juce::MemoryInputStream stream(*block, false);
            if (stream.readInt() != 1) return false;
            sequence.bpm = stream.readDouble(); sequence.swing = stream.readDouble(); sequence.pattern = stream.readInt();
            if (!std::isfinite(sequence.bpm) || sequence.bpm < 40 || sequence.bpm > 240
                || !std::isfinite(sequence.swing) || sequence.swing < 0 || sequence.swing > 75
                || sequence.pattern < 0 || sequence.pattern > 3) return false;
            for (auto& pattern : sequence.rows) for (auto& row : pattern) row = static_cast<uint16_t>(stream.readShort());
        }
        std::array<studiosp::model::PadData, 16> loaded;
        for (int index = 0; index < 16; ++index) {
            const auto child = root.getChild(index); const auto audioProperty = child["audio"];
            const auto* block = audioProperty.getBinaryData();
            if (!child.hasType("Pad") || !block || block->getSize() < 8) return false;
            juce::MemoryInputStream stream(*block, false);
            const int count = stream.readInt(), channels = stream.readInt(); const double rate = child["rate"];
            if (!std::isfinite(rate) || rate < 8000 || rate > 192000 || count < 0 || count > rate * 300 || channels < 0 || channels > 2
                || (count > 0 && channels == 0) || block->getSize() != 8 + static_cast<size_t>(count) * static_cast<size_t>(channels) * 4) return false;
            auto& pad = loaded[index]; pad.padIndex = index; pad.sampleName = child["name"].toString().toStdString(); pad.sampleRate = rate;
            pad.audioData.setSize(channels, count);
            for (int ch = 0; ch < channels; ++ch) for (int i = 0; i < count; ++i) {
                const float value = stream.readFloat(); if (!std::isfinite(value)) return false; pad.audioData.setSample(ch, i, value);
            }
            const float volume = child["volume"], pitch = child["pitch"], pan = child["pan"];
            if (!std::isfinite(volume) || !std::isfinite(pitch) || !std::isfinite(pan)) return false;
            pad.volume = juce::jlimit(0.0f, 2.0f, volume); pad.pitchSemitones = juce::jlimit(-24.0f, 24.0f, pitch); pad.pan = juce::jlimit(-1.0f, 1.0f, pan);
            pad.mode = static_cast<studiosp::model::PlaybackMode>(juce::jlimit(0, 2, static_cast<int>(child["mode"])));
            pad.chokeGroup = juce::jlimit(0, 8, static_cast<int>(child["choke"]));
            const float fadeIn = child.getProperty("fadeIn", 0.0f), fadeOut = child.getProperty("fadeOut", 0.0f);
            if (!std::isfinite(fadeIn) || !std::isfinite(fadeOut)) return false;
            pad.fadeInMs = juce::jlimit(0.0f, 100.0f, fadeIn); pad.fadeOutMs = juce::jlimit(0.0f, 100.0f, fadeOut);
            pad.startSample = juce::jlimit<int64_t>(0, count, static_cast<juce::int64>(child["start"]));
            pad.endSample = juce::jlimit<int64_t>(pad.startSample, count, static_cast<juce::int64>(child["end"]));
            pad.loopStartSample = juce::jlimit<int64_t>(pad.startSample, pad.endSample, static_cast<juce::int64>(child["loopStart"]));
            pad.loopEndSample = juce::jlimit<int64_t>(pad.loopStartSample, pad.endSample, static_cast<juce::int64>(child["loopEnd"]));
            pad.stretchRatio = static_cast<double>(child.getProperty("stretchRatio", 1.0));
            if (!std::isfinite(pad.stretchRatio) || pad.stretchRatio < 0.5 || pad.stretchRatio > 2
                || count * pad.stretchRatio > rate * 300) return false;
            pad.classic12Bit = child.getProperty("classic12Bit", false);
            try {
                if (pad.hasSample() && pad.stretchRatio != 1.0)
                    pad.stretchedAudioData = studiosp::audio::dsp::makeStretchedSample(pad.audioData, pad.sampleRate, pad.stretchRatio);
                if (pad.classic12Bit && pad.hasSample()) pad.classicAudioData = studiosp::audio::dsp::makeClassicSample(
                    studiosp::audio::AudioEngine::playbackAudio(pad), pad.sampleRate);
            } catch (const std::exception&) { return false; }
        }
        const float gain = root["gain"], cutoff = root["cutoff"], drive = root["drive"], delay = root["delay"];
        if (!std::isfinite(gain) || !std::isfinite(cutoff) || !std::isfinite(drive) || !std::isfinite(delay)) return false;
        const juce::ScopedLock guard(engine.mutex);
        engine.cancelResampling();
        for (int i = 0; i < 16; ++i) engine.replacePad(i, std::move(loaded[i]), false);
        engine.resetHistory(); engine.clearRecentAudio(); engine.panic();
        engine.sequencer.apply(sequence);
        engine.sequencer.setHostSync(static_cast<bool>(root.getProperty("sequencerHostSync", false)));
        clockInitialised = true;
        engine.gain = juce::jlimit(0.0f, 1.5f, gain); engine.cutoff = juce::jlimit(40.0f, 20000.0f, cutoff);
        engine.drive = juce::jlimit(0.0f, 1.0f, drive); engine.delayMix = juce::jlimit(0.0f, 1.0f, delay);
        return true;
    }
private:
    std::atomic<bool> clockInitialised{false};
};
