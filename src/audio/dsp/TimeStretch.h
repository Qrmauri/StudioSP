#pragma once
#include "model/SampleBuffer.h"
#include <rubberband/RubberBandStretcher.h>
#include <array>
#include <cmath>
#include <functional>
#include <stdexcept>

namespace studiosp::audio::dsp {
// Offline only. Both passes, allocation, peaks and destruction belong to a worker.
inline model::SampleBuffer makeStretchedSample(const model::SampleBuffer& original, double rate,
    double timeRatio, const std::function<bool()>& cancelled = {}) {
    if (!std::isfinite(rate) || rate < 8000 || rate > 192000 || !std::isfinite(timeRatio)
        || timeRatio < 0.5 || timeRatio > 2 || original.getNumChannels() < 1 || original.getNumChannels() > 2
        || original.getNumSamples() < 1 || original.getNumSamples() * timeRatio > rate * 300)
        throw std::invalid_argument("Stretch: ratio 0.5-2, maximo cinco minutos de salida.");
    if (timeRatio == 1) return original;
    if (cancelled && cancelled()) throw std::runtime_error("Procesamiento cancelado.");
    // R3 3.3 can exhaust a tiny offline target while discarding startup padding.
    // Supply a short silent tail to complete its analysis window, then retain
    // exactly the requested original duration. This is not a rate conversion.
    model::SampleBuffer padded;
    const model::SampleBuffer* source = &original;
    const int minimumInput = static_cast<int>(std::ceil(rate * 0.25));
    if (original.getNumSamples() < minimumInput) {
        padded.setSize(original.getNumChannels(), minimumInput);
        auto& paddedAudio = padded.writable(); paddedAudio.clear();
        for (int ch = 0; ch < original.getNumChannels(); ++ch)
            paddedAudio.copyFrom(ch, 0, original.getReadPointer(ch), original.getNumSamples());
        source = &padded;
    }
    using Stretcher = RubberBand::RubberBandStretcher;
    Stretcher stretcher(static_cast<size_t>(std::lround(rate)), static_cast<size_t>(original.getNumChannels()),
        Stretcher::OptionProcessOffline | Stretcher::OptionEngineFiner | Stretcher::OptionChannelsTogether
        | Stretcher::OptionThreadingNever, timeRatio, 1.0);
    constexpr int blockSize = 1024;
    stretcher.setMaxProcessSize(blockSize);
    stretcher.setExpectedInputDuration(static_cast<size_t>(source->getNumSamples()));
    std::array<const float*, 2> input{};
    const auto checkCancel = [&] { if (cancelled && cancelled()) throw std::runtime_error("Procesamiento cancelado."); };
    for (int offset = 0; offset < source->getNumSamples(); offset += blockSize) {
        checkCancel(); const int count = std::min(blockSize, source->getNumSamples() - offset);
        for (int ch = 0; ch < original.getNumChannels(); ++ch) input[ch] = source->getReadPointer(ch) + offset;
        stretcher.study(input.data(), static_cast<size_t>(count), offset + count == source->getNumSamples());
    }
    const int expected = std::max(1, static_cast<int>(std::lround(original.getNumSamples() * timeRatio)));
    model::SampleBuffer result; result.setSize(original.getNumChannels(), expected);
    auto& output = result.writable(); output.clear();
    juce::AudioBuffer<float> scratch(original.getNumChannels(), blockSize);
    std::array<float*, 2> destination{};
    for (int ch = 0; ch < original.getNumChannels(); ++ch) destination[ch] = scratch.getWritePointer(ch);
    int written = 0;
    const auto drain = [&] {
        while (stretcher.available() > 0) {
            checkCancel();
            const int available = std::min(blockSize, stretcher.available());
            const int received = static_cast<int>(stretcher.retrieve(destination.data(), static_cast<size_t>(available)));
            if (received == 0) throw std::runtime_error("Stretch: no se pudo extraer audio.");
            const int copy = std::min(received, expected - written);
            for (int ch = 0; ch < output.getNumChannels(); ++ch) {
                for (int i = 0; i < received; ++i) if (!std::isfinite(destination[ch][i])) throw std::runtime_error("Stretch: audio no valido.");
                if (copy > 0) output.copyFrom(ch, written, destination[ch], copy);
            }
            written += copy;
        }
    };
    for (int offset = 0; offset < source->getNumSamples(); offset += blockSize) {
        checkCancel(); const int count = std::min(blockSize, source->getNumSamples() - offset);
        for (int ch = 0; ch < original.getNumChannels(); ++ch) input[ch] = source->getReadPointer(ch) + offset;
        stretcher.process(input.data(), static_cast<size_t>(count), offset + count == source->getNumSamples());
        drain();
    }
    drain();
    if (written < expected - 1) throw std::runtime_error("Stretch: salida incompleta.");
    result.buildPeaks(); return result;
}
}
