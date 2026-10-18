#pragma once
#include "model/SampleBuffer.h"
#include "SincInterpolator.h"
#include <functional>
#include <stdexcept>

namespace studiosp::audio::dsp {
inline constexpr double classicSampleRate = 26040.0;
inline float quantize12Bit(float sample) {
    return std::clamp(std::round(sample * 2048.0f), -2048.0f, 2047.0f) / 2048.0f;
}
// Convert off the audio thread; preserve the original buffer for reversible mode changes.
inline model::SampleBuffer makeClassicSample(const model::SampleBuffer& original, double rate, const std::function<bool()>& cancelled = {}) {
    model::SampleBuffer converted;
    if (original.getNumSamples() == 0 || rate <= 0) return converted;
    const int count = std::max(1, static_cast<int>(std::ceil(original.getNumSamples() * classicSampleRate / rate)));
    converted.setSize(original.getNumChannels(), count);
    auto& output = converted.writable();
    const double ratio = rate / classicSampleRate;
    for (int ch = 0; ch < output.getNumChannels(); ++ch)
        for (int i = 0; i < count; ++i) {
            if (i % 4096 == 0 && cancelled && cancelled()) throw std::runtime_error("Procesamiento cancelado.");
            output.setSample(ch, i, quantize12Bit(SincInterpolator::interpolate(original.getReadPointer(ch),
                original.getNumSamples(), i * ratio, ratio)));
        }
    converted.buildPeaks();
    return converted;
}
}
