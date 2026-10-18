#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace studiosp::ui {
class WaveformViewport {
public:
    void setSize(int64_t frames) { size = std::max<int64_t>(0, frames); fit(); }
    void fit() { first = 0; length = static_cast<double>(std::max<int64_t>(1, size)); }
    void fitSelection(int64_t start, int64_t end) {
        first = static_cast<double>(start); length = static_cast<double>(end - start); constrain();
    }
    void zoom(double factor, double anchor) {
        anchor = std::clamp(anchor, 0.0, 1.0);
        const double sample = first + length * anchor;
        length /= std::max(0.01, factor); constrainLength(); first = sample - length * anchor; constrain();
    }
    void pan(double fraction) { first += length * fraction; constrain(); }
    int64_t sampleAt(double fraction) const {
        return std::clamp<int64_t>(static_cast<int64_t>(std::llround(first + length * std::clamp(fraction, 0.0, 1.0))), 0, size);
    }
    double fractionAt(int64_t sample) const { return (static_cast<double>(sample) - first) / length; }
    double zoomFactor() const { return static_cast<double>(std::max<int64_t>(1, size)) / length; }
    double visibleStart() const { return first; }
    double visibleLength() const { return length; }
    static void moveMarker(bool startMarker, int64_t sample, int64_t size, int64_t& start, int64_t& end) {
        if (size <= 0) { start = end = 0; return; }
        start = std::clamp<int64_t>(start, 0, size - 1); end = std::clamp<int64_t>(end, start + 1, size);
        if (startMarker) start = std::clamp<int64_t>(sample, 0, end - 1);
        else end = std::clamp<int64_t>(sample, start + 1, size);
    }
private:
    int64_t size = 0;
    double first = 0, length = 1;
    void constrainLength() { length = std::clamp(length, static_cast<double>(std::min<int64_t>(16, std::max<int64_t>(1, size))), static_cast<double>(std::max<int64_t>(1, size))); }
    void constrain() { constrainLength(); first = std::clamp(first, 0.0, std::max(0.0, static_cast<double>(size) - length)); }
};
}
