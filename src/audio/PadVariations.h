#pragma once
#include "model/PadData.h"
#include "dsp/Classic12Bit.h"
#include <array>

namespace studiosp::audio {
struct VariationOptions {
    bool pitch = true, trim = true, fades = true, classic = false;
    float amount = 0.5f;
};
inline std::array<model::PadData, 4> makePadVariations(const model::PadData& source,
    const VariationOptions& options, int64_t seed, const std::function<bool()>& cancelled = {}) {
    if (!source.hasSample() || !std::isfinite(options.amount)) throw std::invalid_argument("Carga un sample antes de generar variaciones.");
    if (source.startSample < 0 || source.startSample >= source.endSample || source.endSample > source.audioData.getNumSamples())
        throw std::invalid_argument("Selecciona una region IN/OUT valida.");
    if (cancelled && cancelled()) throw std::runtime_error("Procesamiento cancelado.");
    const float amount = juce::jlimit(0.0f, 1.0f, options.amount);
    auto classic = source.classicAudioData;
    if (options.classic && amount > 0 && !source.classic12Bit && classic.getNumSamples() == 0)
        classic = dsp::makeClassicSample(source.stretchedAudioData.getNumSamples() > 0 ? source.stretchedAudioData : source.audioData,
                                        source.sampleRate, cancelled);
    std::array<model::PadData, 4> result{source, source, source, source};
    juce::Random random(seed);
    constexpr std::array<float, 4> intervals{-12, -5, 5, 12};
    for (int i = 0; i < 4; ++i) {
        if (cancelled && cancelled()) throw std::runtime_error("Procesamiento cancelado.");
        auto& candidate = result[static_cast<size_t>(i)];
        const float strength = amount * (0.75f + 0.25f * random.nextFloat());
        candidate.sampleName += " / V" + std::to_string(i + 1);
        if (options.pitch) candidate.pitchSemitones = juce::jlimit(-24.0f, 24.0f, source.pitchSemitones + intervals[i] * strength);
        if (options.trim && amount > 0) {
            const auto length = source.endSample - source.startSample;
            candidate.startSample += static_cast<int64_t>(length * strength * random.nextFloat() * 0.12);
            candidate.endSample -= static_cast<int64_t>(length * strength * (0.1 + random.nextFloat() * 0.35));
            candidate.endSample = std::max(candidate.startSample + 1, candidate.endSample);
            candidate.loopStartSample = candidate.startSample; candidate.loopEndSample = candidate.endSample;
        }
        if (options.fades) {
            candidate.fadeInMs = juce::jlimit(0.0f, 100.0f, source.fadeInMs + strength * random.nextFloat() * 20);
            candidate.fadeOutMs = juce::jlimit(0.0f, 100.0f, source.fadeOutMs + strength * (20 + random.nextFloat() * 80));
        }
        if (options.classic && amount > 0 && i % 2 == 1) {
            candidate.classic12Bit = !source.classic12Bit;
            if (candidate.classic12Bit) candidate.classicAudioData = classic;
        }
    }
    return result;
}
}
