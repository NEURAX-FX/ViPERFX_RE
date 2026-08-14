#pragma once

#include "iem/SphericalHarmonics.h"

#include <cstddef>
#include <cstdint>

namespace iem {

enum class HaloBedChannel : uint32_t {
    L = 0,
    R = 1,
    C = 2,
    Ls = 3,
    Rs = 4,
    Lsr = 5,
    Rsr = 6,
    kCount = 7,
};

constexpr uint32_t kHaloBedChannels = static_cast<uint32_t>(HaloBedChannel::kCount);
constexpr uint32_t kHaloDirectionalChannels = kHaloBedChannels;
constexpr float kHaloBedAzimuthDegrees[kHaloBedChannels] = {
    -30.0F, 30.0F, 0.0F, -90.0F, 90.0F, -135.0F, 135.0F,
};

struct HaloBedView {
    float *directional[kHaloDirectionalChannels]{};
    float *lfe = nullptr;
};

void EncodeHaloBedToSn3d(
    uint32_t order,
    const float *const bed[kHaloBedChannels],
    float *const ambisonics[kMaxAmbisonicsChannels],
    std::size_t frames
) noexcept;

void FoldHaloBedToStereo(
    const float *const bed[kHaloBedChannels],
    float *const stereo[2],
    std::size_t frames
) noexcept;

} // namespace iem
