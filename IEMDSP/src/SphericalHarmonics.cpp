#include "iem/SphericalHarmonics.h"

#include <algorithm>
#include <array>
#include <cmath>

void SHEval0(float x, float y, float z, float *coefficients);
void SHEval1(float x, float y, float z, float *coefficients);
void SHEval2(float x, float y, float z, float *coefficients);
void SHEval3(float x, float y, float z, float *coefficients);

namespace iem {

namespace {

constexpr float kSqrtFourPi = 3.544907701811032F;
constexpr std::array<float, 4> kN3dToSn3d{
    1.0F,
    0.5773502691896258F,
    0.4472135954999579F,
    0.3779644730092272F,
};

void EvaluateN3d(
    uint32_t order,
    float x,
    float y,
    float z,
    float *coefficients
) noexcept {
    switch (order) {
    case 0:
        SHEval0(x, y, z, coefficients);
        break;
    case 1:
        SHEval1(x, y, z, coefficients);
        break;
    case 2:
        SHEval2(x, y, z, coefficients);
        break;
    case 3:
        SHEval3(x, y, z, coefficients);
        break;
    default:
        break;
    }
}

} // namespace

void EvaluateSn3d(
    uint32_t order,
    float azimuth_radians,
    float elevation_radians,
    float out[kMaxAmbisonicsChannels]
) noexcept {
    if (out == nullptr) return;
    std::fill_n(out, kMaxAmbisonicsChannels, 0.0F);
    if (order > kMaxAmbisonicsOrder
        || !std::isfinite(azimuth_radians)
        || !std::isfinite(elevation_radians)) {
        return;
    }

    const float cos_elevation = std::cos(elevation_radians);
    const float x = cos_elevation * std::cos(azimuth_radians);
    const float y = cos_elevation * std::sin(azimuth_radians);
    const float z = std::sin(elevation_radians);
    EvaluateN3d(order, x, y, z, out);

    uint32_t channel = 0;
    for (uint32_t degree = 0; degree <= order; ++degree) {
        const float scale = kSqrtFourPi * kN3dToSn3d[degree];
        const uint32_t degree_channels = degree * 2U + 1U;
        for (uint32_t index = 0; index < degree_channels; ++index) {
            out[channel++] *= scale;
        }
    }
}

} // namespace iem
