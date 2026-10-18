#pragma once
#include "model/PadData.h"
#include "dsp/SincInterpolator.h"
#include <algorithm>
#include <cmath>
#include <array>
namespace studiosp::audio {
// Voice state is owned by the audio thread. The engine pins the immutable pad snapshot.
class PadSamplerVoice {
public:
    static constexpr double transitionMs = 3.0;
    // Copies contain only raw snapshot pointers and scalar playback state.
    // The engine pins both states; no shared ownership changes happen here.
    void trigger(const model::PadData& pad, float velocity, double outputRate, bool fadeAttack = false) {
        if (!pad.hasSample()) { stopSmooth(); return; }
        if (playing && data && data->mode == model::PlaybackMode::Loop && pad.mode == model::PlaybackMode::Loop) { stopSmooth(); return; }
        const bool transition = active();
        if (transition) retireCurrent();
        data = &pad; position = static_cast<double>(std::clamp<int64_t>(pad.startSample, 0, pad.audioData.getNumSamples()));
        level = std::clamp(velocity, 0.0f, 1.0f);
        ratio = pad.sampleRate / outputRate * std::pow(2.0, pad.pitchSemitones / 12.0);
        outputSampleRate = outputRate; releasing = false; releaseLevel = 1.0f; looped = false;
        playing = pad.hasSample();
        if (transition || fadeAttack) beginTransition();
    }
    void release() {
        if (playing && data && data->mode == model::PlaybackMode::Gate) {
            if (data->fadeOutMs <= 0) stop();
            else { releasing = true; releaseStep = 1.0f / std::max(1.0f, static_cast<float>(outputSampleRate * data->fadeOutMs / 1000.0)); }
        }
    }
    void stop() { playing = false; data = nullptr; tail = {}; transitionLeft = 0; transitionPending = false; correction.fill(0); lastOutput.fill(0); }
    void stopSmooth() {
        if (!playing) return;
        retireCurrent(); playing = false; data = nullptr; beginTransition();
    }
    bool active() const { return playing || tail.data || transitionLeft > 0; }
    const model::PadData* snapshot() const { return data; }
    const model::PadData* tailSnapshot() const { return tail.data; }
    void renderNextBlock(juce::AudioBuffer<float>& output, int start, int count) {
        for (int i = 0; i < count && active(); ++i) {
            std::array<float, 2> frame{};
            if (playing && data) renderFrame(frame);
            if (transitionLeft > 0) {
                if (tail.data) {
                    PadSamplerVoice old;
                    old.data = tail.data; old.position = tail.position; old.ratio = tail.ratio;
                    old.outputSampleRate = outputSampleRate; old.level = tail.level;
                    old.releaseLevel = tail.releaseLevel; old.releasing = tail.releasing;
                    old.releaseStep = tail.releaseStep; old.looped = tail.looped; old.playing = true;
                    std::array<float, 2> previous{}; old.renderFrame(previous);
                    tail.position = old.position; tail.releaseLevel = old.releaseLevel; tail.looped = old.looped;
                    if (old.playing) tail.lastFrame = previous;
                    else tail.data = nullptr; // Finish the ramp using the last frame if the sample ends early.
                }
                const float mix = static_cast<float>(transitionLeft) / static_cast<float>(transitionLength);
                for (int ch = 0; ch < 2; ++ch) frame[ch] = frame[ch] * (1 - mix) + tail.lastFrame[ch] * mix;
            }
            // If a fast retrigger steals an existing tail, bridge from the exact
            // last mixed frame. This keeps the fixed two-state voice bounded.
            if (transitionPending) {
                for (int ch = 0; ch < 2; ++ch) correction[ch] = lastOutput[ch] - frame[ch];
                transitionPending = false;
            }
            const float mix = transitionLeft > 0 ? static_cast<float>(transitionLeft) / static_cast<float>(transitionLength) : 0;
            for (int ch = 0; ch < std::min(2, output.getNumChannels()); ++ch) {
                lastOutput[ch] = frame[ch] + correction[ch] * mix;
                output.addSample(ch, start + i, lastOutput[ch]);
            }
            if (transitionLeft > 0 && --transitionLeft == 0) { tail = {}; correction.fill(0); }
        }
    }
private:
    struct Tail {
        const model::PadData* data = nullptr;
        double position = 0, ratio = 1;
        float level = 1, releaseLevel = 1, releaseStep = 1;
        bool releasing = false, looped = false;
        std::array<float, 2> lastFrame{};
    } tail;
    std::array<float, 2> lastOutput{}, correction{};
    int transitionLeft = 0, transitionLength = 1;
    bool transitionPending = false;
    void retireCurrent() {
        tail = {data, position, ratio, level, releaseLevel, releaseStep, releasing, looped, lastOutput};
    }
    void beginTransition() {
        transitionLength = std::max(1, static_cast<int>(std::ceil(outputSampleRate * transitionMs / 1000.0)));
        transitionLeft = transitionLength; transitionPending = true;
    }
    void endPlayback() { playing = false; data = nullptr; }
    void renderFrame(std::array<float, 2>& frame) {
        const int64_t size = data->audioData.getNumSamples();
        const auto end = std::clamp<int64_t>(data->endSample > 0 ? data->endSample : size, 0, size);
        const auto loopStart = std::clamp<int64_t>(data->loopStartSample, data->startSample, end);
        const auto loopEnd = std::clamp<int64_t>(data->loopEndSample > loopStart ? data->loopEndSample : end, loopStart, end);
        const auto boundary = data->mode == model::PlaybackMode::Loop ? loopEnd : end;
        if (boundary <= data->startSample || (data->mode == model::PlaybackMode::Loop && loopEnd <= loopStart)) { endPlayback(); return; }
        if (position >= static_cast<double>(boundary)) {
            if (data->mode != model::PlaybackMode::Loop) { endPlayback(); return; }
            position = static_cast<double>(loopStart) + std::fmod(position - static_cast<double>(loopStart), static_cast<double>(loopEnd - loopStart));
            looped = true;
        }
        const double fadeIn = data->sampleRate * data->fadeInMs / 1000.0;
        const double fadeOut = data->sampleRate * data->fadeOutMs / 1000.0;
        const auto regionStart = data->mode == model::PlaybackMode::Loop && looped ? loopStart : data->startSample;
        const double attack = fadeIn > 0 ? std::clamp((position - static_cast<double>(regionStart)) / fadeIn, 0.0, 1.0) : 1;
        const double endFade = fadeOut > 0 ? std::clamp((static_cast<double>(boundary) - position - ratio) / fadeOut, 0.0, 1.0) : 1;
        const float envelope = static_cast<float>(std::min(attack, endFade)) * releaseLevel;
        for (int ch = 0; ch < 2; ++ch) {
            const int source = std::min(ch, data->audioData.getNumChannels() - 1);
            const float panGain = ch == 0 ? 1 - std::max(0.0f, data->pan) : 1 + std::min(0.0f, data->pan);
            // Restrict every interpolation tap to the current trim/loop region.
            const float value = dsp::SincInterpolator::interpolate(data->audioData.getReadPointer(source) + regionStart,
                boundary - regionStart, position - static_cast<double>(regionStart), ratio);
            frame[ch] = value * data->volume * level * panGain * envelope;
        }
        position += ratio;
        if (releasing) { releaseLevel = std::max(0.0f, releaseLevel - releaseStep); if (releaseLevel == 0) endPlayback(); }
    }
    const model::PadData* data = nullptr;
    double position = 0, ratio = 1, outputSampleRate = 48000;
    float level = 1, releaseLevel = 1, releaseStep = 1;
    bool playing = false, releasing = false, looped = false;
};
}
