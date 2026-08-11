#pragma once

#include <cstdint>

namespace iem {

constexpr uint32_t kMaxAmbisonicsOrder = 3;
constexpr uint32_t kMaxAmbisonicsChannels = 16;

constexpr uint32_t AmbisonicsChannelCount(uint32_t order) noexcept {
    return (order + 1U) * (order + 1U);
}

void EvaluateSn3d(
    uint32_t order,
    float azimuth_radians,
    float elevation_radians,
    float out[kMaxAmbisonicsChannels]
) noexcept;

} // namespace iem
