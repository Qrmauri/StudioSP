#pragma once
#include "PatternSequencer.h"
#include <juce_audio_basics/juce_audio_basics.h>

namespace studiosp::audio {
// Called outside the audio callback from a captured immutable pattern state.
inline juce::MidiFile makePatternMidi(const PatternSequencer::State& state, int pattern, double tempo) {
    juce::MidiFile file; file.setTicksPerQuarterNote(PatternSequencer::ticksPerQuarter);
    juce::MidiMessageSequence track;
    pattern = juce::jlimit(0, 3, pattern);
    if (!std::isfinite(tempo) || tempo < 1 || tempo > 1000) tempo = state.bpm;
    // MIDI tempo meta events are 24-bit microseconds/quarter, so very slow host tempos need this clamp.
    track.addEvent(juce::MidiMessage::tempoMetaEvent(juce::jlimit(1, 0xffffff, juce::roundToInt(60000000.0 / tempo))));
    track.addEvent(juce::MidiMessage::timeSignatureMetaEvent(4, 4));
    track.addEvent(juce::MidiMessage::textMetaEvent(3, "StudioSP Pattern " + juce::String::charToString(static_cast<juce::juce_wchar>('A' + pattern))));
    for (int pad = 0; pad < 16; ++pad) for (int step = 0; step < 16; ++step) if (state.rows[pattern][pad] & (1u << step)) {
        const int start = PatternSequencer::onsetTicks(state, pattern, pad, step);
        const int end = std::min(PatternSequencer::cycleTicks, start + PatternSequencer::lengthTicks(state, pattern, pad, step));
        auto on = juce::MidiMessage::noteOn(10, 36 + pad, static_cast<juce::uint8>(PatternSequencer::velocity(state.hits[pattern][pad][step])));
        auto off = juce::MidiMessage::noteOff(10, 36 + pad);
        on.setTimeStamp(start); off.setTimeStamp(end); track.addEvent(on); track.addEvent(off);
    }
    auto end = juce::MidiMessage::endOfTrack(); end.setTimeStamp(PatternSequencer::cycleTicks); track.addEvent(end);
    file.addTrack(track); return file;
}
}
