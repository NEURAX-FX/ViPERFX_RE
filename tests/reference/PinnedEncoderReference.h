#pragma once

#include <array>
#include <cmath>

namespace iem_test {

inline std::array<float, 4> FirstOrderSn3d(float azimuth, float elevation) {
    const float cos_elevation = std::cos(elevation);
    return {
        1.0F,
        cos_elevation * std::sin(azimuth),
        std::sin(elevation),
        cos_elevation * std::cos(azimuth),
    };
}

} // namespace iem_test
