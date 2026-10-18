#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <memory>
#include <vector>

namespace studiosp::model {
// Copies share immutable audio. Writers detach on the non-audio thread.
class SampleBuffer {
public:
    struct Peak { float low = 0, high = 0; };
    int getNumSamples() const { return storage ? storage->audio.getNumSamples() : 0; }
    int getNumChannels() const { return storage ? storage->audio.getNumChannels() : 0; }
    const float* getReadPointer(int channel) const { return storage->audio.getReadPointer(channel); }
    float getSample(int channel, int sample) const { return storage->audio.getSample(channel, sample); }
    void setSize(int channels, int samples) { writable().setSize(channels, samples); }
    void setSample(int channel, int sample, float value) { writable().setSample(channel, sample, value); }
    void copyFrom(int channel, int destination, const juce::AudioBuffer<float>& source, int sourceChannel, int offset, int count) {
        writable().copyFrom(channel, destination, source, sourceChannel, offset, count);
    }
    juce::AudioBuffer<float>& writable() {
        if (!storage) storage = std::make_shared<Storage>();
        else if (storage.use_count() != 1) storage = std::make_shared<Storage>(*storage);
        storage->peaks.clear();
        return storage->audio;
    }
    const void* identity() const { return storage.get(); }
    size_t memoryBytes() const {
        return storage ? sizeof(Storage) + static_cast<size_t>(getNumChannels()) * getNumSamples() * sizeof(float)
            + storage->peaks.capacity() * sizeof(Peak) : 0;
    }
    void buildPeaks() {
        if (!storage || !storage->peaks.empty() || getNumSamples() == 0) return;
        if (storage.use_count() != 1) storage = std::make_shared<Storage>(*storage);
        const int count = getNumSamples();
        storage->peaks.reserve(static_cast<size_t>((count + bucketSize - 1) / bucketSize));
        for (int first = 0; first < count; first += bucketSize)
            storage->peaks.push_back(scan(first, std::min(count, first + bucketSize)));
    }
    Peak range(int first, int last) const {
        first = juce::jlimit(0, getNumSamples(), first); last = juce::jlimit(first, getNumSamples(), last);
        Peak result;
        while (first < last) {
            if (first % bucketSize == 0 && first + bucketSize <= last && !storage->peaks.empty()) {
                const auto value = storage->peaks[static_cast<size_t>(first / bucketSize)];
                result.low = std::min(result.low, value.low); result.high = std::max(result.high, value.high);
                first += bucketSize;
            } else {
                const int end = std::min(last, (first / bucketSize + 1) * bucketSize);
                const auto value = scan(first, end);
                result.low = std::min(result.low, value.low); result.high = std::max(result.high, value.high); first = end;
            }
        }
        return result;
    }
private:
    static constexpr int bucketSize = 256;
    struct Storage { juce::AudioBuffer<float> audio; std::vector<Peak> peaks; };
    std::shared_ptr<Storage> storage;
    Peak scan(int first, int last) const {
        Peak result;
        for (int ch = 0; ch < getNumChannels(); ++ch) {
            const auto* data = getReadPointer(ch);
            for (int i = first; i < last; ++i) { result.low = std::min(result.low, data[i]); result.high = std::max(result.high, data[i]); }
        }
        return result;
    }
};
}
