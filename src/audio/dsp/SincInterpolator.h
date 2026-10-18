#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace studiosp::audio::dsp {
class SincInterpolator {
public:
    static float interpolate(const float* data, int64_t size, double position, double ratio = 1.0) noexcept {
        if (data == nullptr || size <= 0 || !std::isfinite(position)) return 0;
        constexpr double pi = 3.14159265358979323846;
        const auto centre = static_cast<int64_t>(std::floor(position));
        const double cutoff = 1.0 / std::max(1.0, ratio);
        double sum = 0, weights = 0;
        for (int tap = -15; tap <= 16; ++tap) {
            const auto index = centre + tap;
            const double distance = static_cast<double>(index) - position;
            const double w = (distance + 16.0) / 32.0;
            if (index < 0 || index >= size || w < 0 || w > 1) continue;
            const double x = pi * distance * cutoff;
            const double sinc = std::abs(x) < 1.0e-12 ? 1.0 : std::sin(x) / x;
            const double weight = cutoff * sinc * (0.42 - 0.5 * std::cos(2 * pi * w) + 0.08 * std::cos(4 * pi * w));
            sum += data[index] * weight; weights += weight;
        }
        return std::abs(weights) > 1.0e-9 ? static_cast<float>(sum / weights) : 0;
    }
};
}
