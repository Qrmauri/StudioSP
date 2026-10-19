#include "TestSupport.h"
#include "PluginProcessor.h"
#include "audio/PatternMidi.h"
#include <limits>

void runMidiTests() {
    using namespace studiosp;
    {
        StudioSPProcessor recorder; recorder.prepareToPlay(48000, 1000);
        auto& seq = recorder.engine.sequencer; seq.setHostSync(true); seq.setRecording(true); seq.setQuantize(false);
        model::PadData impulse; impulse.audioData.setSize(1, 32); impulse.audioData.writable().clear();
        impulse.audioData.setSample(0, 0, 0.5f); impulse.endSample = 32; impulse.fadeInMs = impulse.fadeOutMs = 0;
        recorder.engine.replacePad(0, impulse, false); recorder.engine.gain = 1;
        juce::AudioBuffer<float> frames(2, 1000); juce::MidiBuffer notes;
        notes.addEvent(juce::MidiMessage::noteOn(1, 36, static_cast<juce::uint8>(64)), 100);
        notes.addEvent(juce::MidiMessage::noteOff(1, 36), 300);
        audioAllocations = audioDeletes = 0; auditingAudio = true;
        recorder.engine.processBlock(frames, notes, 0, {true, true, 120, 0}); auditingAudio = false;
        require(audioAllocations == 0 && audioDeletes == 0, "Live MIDI recording must not allocate in the callback");
        require(std::abs(frames.getSample(0, 100) - 0.5f * 64 / 127) < 1e-4f,
            "Recording must monitor incoming MIDI immediately at its real velocity");
        const auto captured = seq.snapshot(); const auto hit = captured.hits[0][0][0];
        require(captured.rows[0][0] == 1 && audio::PatternSequencer::velocity(hit) == 64
            && audio::PatternSequencer::onsetTicks(captured, 0, 0, 0) == 40 && hit.duration == 80,
            "Unquantized recording must preserve note velocity, exact timing and duration");
        seq.setRecording(false); recorder.engine.processBlock(frames, notes, 0, {true, true, 120, 4});
        for (int sample = 0; sample < 1000; ++sample)
            require(std::abs(frames.getSample(0, sample) - (sample == 100 ? 0.5f * 64 / 127 : 0)) < 1e-4f,
                "The recorded unquantized hit must replay through the audio engine at its real timing and velocity");
        seq.setRecording(true);
        juce::MemoryBlock saved; recorder.getStateInformation(saved); StudioSPProcessor restored;
        require(restored.restoreState(saved.getData(), static_cast<int>(saved.getSize())), "Recorded MIDI data must load from a project");
        const auto restoredState = restored.engine.sequencer.snapshot();
        require(restoredState.hits[0][0][0].value == hit.value && restoredState.hits[0][0][0].duration == 80
            && !restoredState.quantize && !restored.engine.sequencer.isRecording(), "Timing, velocity, length and quantize must persist without arming REC");
        auto legacy = juce::ValueTree::readFromData(saved.getData(), saved.getSize());
        legacy.removeProperty("sequencerMidi", nullptr); legacy.removeProperty("sequencerQuantize", nullptr);
        juce::MemoryOutputStream legacyBytes; legacy.writeToStream(legacyBytes);
        require(restored.restoreState(legacyBytes.getData(), static_cast<int>(legacyBytes.getDataSize()))
            && restored.engine.sequencer.row(0, 0) == 1 && restored.engine.sequencer.snapshot().hits[0][0][0].value == 0
            && restored.engine.sequencer.isQuantized(), "Older projects must retain steps with full velocity and default length");
        legacy.setProperty("sequencerMidi", juce::MemoryBlock(3), nullptr); juce::MemoryOutputStream broken;
        legacy.writeToStream(broken);
        require(!restored.restoreState(broken.getData(), static_cast<int>(broken.getDataSize())), "Truncated recorded MIDI state must be rejected");
        auto midi = audio::makePatternMidi(captured, 0, 120);
        juce::MemoryOutputStream output; require(midi.writeTo(output, 0), "Recorded pattern must export to Standard MIDI");
        juce::MemoryInputStream input(output.getData(), output.getDataSize(), false); juce::MidiFile loaded;
        require(loaded.readFrom(input) && loaded.getTimeFormat() == 9600 && loaded.getNumTracks() == 1, "Export must be a readable one-track 9600-PPQ MIDI file");
        bool foundOn = false, foundOff = false, foundTempo = false;
        for (int i = 0; i < loaded.getTrack(0)->getNumEvents(); ++i) {
            const auto& message = loaded.getTrack(0)->getEventPointer(i)->message;
            if (message.isNoteOn()) foundOn = message.getChannel() == 10 && message.getNoteNumber() == 36
                && message.getVelocity() == 64 && message.getTimeStamp() == 40;
            if (message.isNoteOff()) foundOff = message.getTimeStamp() == 120;
            if (message.isTempoMetaEvent()) foundTempo = std::abs(message.getTempoSecondsPerQuarterNote() - 0.5) < 1e-6;
        }
        require(foundOn && foundOff && foundTempo && loaded.getLastTimestamp() == 38400, "Export must retain notes, duration, tempo and one-bar length");
        auto midiFile = juce::File::getCurrentWorkingDirectory().getChildFile("StudioSP-pattern-test.mid");
        require(midiFile.replaceWithData(output.getData(), output.getDataSize()), "Exported MIDI bytes must be writable as a real .mid file");
        seq.stop(); seq.setRecording(true);
        notes.addEvent(juce::MidiMessage::noteOn(1, 37, 1.0f), 100);
        recorder.engine.processBlock(frames, notes, 0, {true, false, 120, 1});
        require(seq.row(0, 1) == 0, "Stopped transport must never record notes");
    }
    {
        audio::PatternSequencer seq; seq.setSwing(50); seq.start(); seq.setRecording(true);
        seq.process(8200, 48000, [](int, int) {}, [](int, unsigned, unsigned) {});
        seq.recordMidi(2, true, 100.0f / 127);
        seq.process(300, 48000, [](int, int) {}, [](int, unsigned, unsigned) {}); seq.recordMidi(2, false);
        bool duplicate = false;
        seq.process(800, 48000, [](int, int) {}, [&](int, unsigned down, unsigned) { if (down & 4) duplicate = true; });
        const auto state = seq.snapshot();
        require(state.rows[0][2] == 2 && audio::PatternSequencer::onsetTicks(state, 0, 2, 1) == 3600
            && audio::PatternSequencer::velocity(state.hits[0][2][1]) == 100 && !duplicate,
            "Quantize must choose the nearest swung step without a second monitored hit in the current cycle");
        int replays = 0;
        seq.process(105001 - 9300, 48000, [](int, int) {}, [&](int offset, unsigned down, unsigned) {
            if (down & 4) { require(offset + 9300 == 105000 && std::abs(seq.eventVelocity(2) - 100.0f / 127) < 1e-6f,
                "Next loop must replay the recorded hit at its quantized time and velocity"); ++replays; }
        });
        require(replays == 1, "The overdub must become part of the next loop");
        seq.requestPattern(1); seq.recordMidi(3, true, 0.5f);
        require(seq.row(0, 3) == 2 && seq.row(1, 3) == 0, "Recording must target the sounding pattern while a change is pending");
        seq.stop(); require(!seq.isRecording(), "Local STOP must disarm live recording");
        seq.reset(); require(seq.isQuantized() && seq.row(0, 2) == 0 && seq.snapshot().hits[0][2][1].value == 0, "New Project must clear recorded data");
    }
    {
        StudioSPProcessor taps; taps.prepareToPlay(48000, 64);
        auto& seq = taps.engine.sequencer; seq.start(); seq.setRecording(true); seq.setQuantize(false);
        taps.engine.enqueue(4, true, 0.75f);
        juce::AudioBuffer<float> frames(2, 64); juce::MidiBuffer notes;
        taps.processBlock(frames, notes); taps.engine.enqueue(4, false); taps.processBlock(frames, notes);
        const auto hit = seq.snapshot().hits[0][4][0];
        require(seq.row(0, 4) == 1 && audio::PatternSequencer::velocity(hit) == 95 && hit.duration == 26,
            "On-screen and computer-keyboard pad events must also record at the callback boundary");
        seq.setStep(0, 7, 1, true);
        for (int block = 0; block < 100; ++block) taps.processBlock(frames, notes);
        require(seq.snapshot().hits[0][7][1].value == 0, "Sequencer playback must not record itself during overdub");
        model::PadData preview; preview.audioData.setSize(1, 4); preview.audioData.writable().clear(); preview.endSample = 4;
        taps.engine.setPreview(preview); taps.processBlock(frames, notes);
        require(seq.row(0, 0) == 0, "Browser preview must not enter live MIDI recording");
    }
    {
        audio::PatternSequencer seq; seq.setHostSync(true); seq.setRecording(true); seq.setQuantize(false);
        seq.beginBlock({true, true, 120, 3.99}, 64, 48000); seq.recordMidi(15, true, 1);
        const auto state = seq.snapshot();
        require(state.rows[0][15] == 1 && audio::PatternSequencer::onsetTicks(state, 0, 15, 0) == 38304,
            "Unquantized notes near the wrap must keep their actual position before the bar boundary");
        const auto file = audio::makePatternMidi(state, 0, 130);
        bool on = false, off = false;
        for (int i = 0; i < file.getTrack(0)->getNumEvents(); ++i) {
            const auto& event = file.getTrack(0)->getEventPointer(i)->message;
            if (event.isNoteOn()) on = event.getTimeStamp() == 38304 && event.getNoteNumber() == 51;
            if (event.isNoteOff()) off = event.getTimeStamp() == 38400;
        }
        require(on && off, "Export must keep late note timing and close notes at the loop end");
    }
}
