#pragma once
#include "model/PadData.h"
#include <atomic>
#include <memory>
#include <stdexcept>

namespace studiosp::audio {
inline model::PadData trimRescuedAudio(model::PadData data) {
    const int start = static_cast<int>(data.startSample), end = static_cast<int>(data.endSample);
    if (!data.hasSample() || start < 0 || end > data.audioData.getNumSamples() || start >= end)
        throw std::invalid_argument("Seleccion de audio invalida.");
    model::SampleBuffer selected; selected.setSize(data.audioData.getNumChannels(), end - start);
    auto& audio = selected.writable();
    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
        audio.copyFrom(ch, 0, data.audioData.getReadPointer(ch) + start, end - start);
    selected.buildPeaks(); data.audioData = std::move(selected);
    data.startSample = data.loopStartSample = 0; data.endSample = data.loopEndSample = end - start;
    return data;
}
// One audio writer, snapshots on a worker. Prepare is serialized with readers and
// runs only while the host has stopped audio. Atomic samples avoid data races even
// when an overrun forces the reader to discard a copy.
class RecentAudio {
public:
    struct Mark { uint64_t end = 0, generation = 0; };
    void prepare(double sampleRate) {
        if (!std::isfinite(sampleRate) || sampleRate <= 0 || sampleRate > 192000) throw std::invalid_argument("Frecuencia de captura invalida.");
        rate = sampleRate; capacity = static_cast<uint64_t>(std::ceil(rate * 32));
        samples = std::make_unique<std::atomic<float>[]>(static_cast<size_t>(capacity * 2));
        ++requestedReset; completed = 0; reserved = 0; appliedReset = requestedReset.load();
    }
    void requestReset() { ++requestedReset; }
    bool resetIfRequested() {
        const auto generation = requestedReset.load();
        if (appliedReset.load() == generation) return false;
        completed = 0; reserved = 0; appliedReset = generation; return true;
    }
    void write(const juce::AudioBuffer<float>& block, bool applyPendingReset = true) {
        if (applyPendingReset) resetIfRequested();
        if (!samples || block.getNumChannels() == 0) return;
        const auto first = completed.load();
        const auto last = first + static_cast<uint64_t>(block.getNumSamples());
        reserved.store(last, std::memory_order_release);
        for (int i = 0; i < block.getNumSamples(); ++i) {
            const auto slot = static_cast<size_t>((first + i) % capacity) * 2;
            for (int ch = 0; ch < 2; ++ch)
                samples[slot + ch].store(juce::jlimit(-0.988553f, 0.988553f,
                    block.getSample(std::min(ch, block.getNumChannels() - 1), i)), std::memory_order_release);
        }
        completed.store(last, std::memory_order_release);
    }
    Mark mark() const {
        const auto generation = requestedReset.load();
        if (appliedReset.load() != generation) return {0, generation};
        const auto end = completed.load(std::memory_order_acquire);
        return requestedReset.load() == generation ? Mark{end, generation} : Mark{};
    }
    model::PadData snapshot() const {
        for (int attempt = 0; attempt < 3; ++attempt) {
            auto data = snapshot(mark()); if (data.hasSample()) return data;
        }
        return {};
    }
    model::PadData snapshot(Mark position) const {
        const auto generation = position.generation, end = position.end;
        if (!samples || appliedReset.load() != generation || requestedReset.load() != generation
            || end > completed.load(std::memory_order_acquire)) return {};
        const auto count = std::min(end, static_cast<uint64_t>(std::floor(rate * 30)));
        if (count == 0) return {};
        const auto start = end - count;
        model::PadData data; data.sampleName = "Improvisacion"; data.sampleRate = rate;
        data.audioData.setSize(2, static_cast<int>(count));
        auto& audio = data.audioData.writable();
        for (uint64_t i = 0; i < count; ++i) {
            const auto slot = static_cast<size_t>((start + i) % capacity) * 2;
            for (int ch = 0; ch < 2; ++ch)
                audio.setSample(ch, static_cast<int>(i), samples[slot + ch].load(std::memory_order_acquire));
        }
        // A writer reserves its entire block before touching samples. If it
        // reaches this window, discard the copy rather than returning torn audio.
        if (requestedReset.load() != generation || appliedReset.load() != generation) return {};
        if (reserved.load(std::memory_order_acquire) - start > capacity) return {};
        data.endSample = data.loopEndSample = static_cast<int64_t>(count);
        data.audioData.buildPeaks(); return data;
    }
private:
    static_assert(std::atomic<float>::is_always_lock_free);
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    std::unique_ptr<std::atomic<float>[]> samples;
    double rate = 48000;
    uint64_t capacity = 0;
    std::atomic<uint64_t> completed{0}, reserved{0}, requestedReset{0}, appliedReset{0};
};
}
