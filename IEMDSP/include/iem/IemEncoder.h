#pragma once

#include "iem/IemParams.h"
#include "iem/SphericalHarmonics.h"

#include <cstddef>
#include <cstdint>

namespace iem {

struct EncoderConfig {
    uint32_t sample_rate = 96000;
    std::size_t max_frames = 256;
    uint32_t order = 3;
    uint32_t random_seed = 0x6D2B79F5U;
};

class IemEncoder {
public:
    virtual ~IemEncoder() = default;
    virtual bool Prepare(const EncoderConfig &config) = 0;
    virtual void ApplyParams(const IemParams &params) noexcept = 0;
    virtual void Reset() noexcept = 0;
    virtual bool Process(
        const float *const stereo[2],
        float *const ambisonics[kMaxAmbisonicsChannels],
        std::size_t frames
    ) noexcept = 0;
};

} // namespace iem
