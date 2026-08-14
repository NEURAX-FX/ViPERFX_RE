#pragma once

#include "iem/AlignedPlanarBuffer.h"
#include "iem/HaloBed.h"
#include "iem/HaloDownmixProcessor.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace iem {

class HaloDownmixDecoder {
public:
    bool Prepare(
        uint32_t order,
        uint32_t sample_rate,
        std::size_t max_frames
    ) noexcept;
    void ApplyParams(const HaloDownmixParams &params) noexcept;
    void Reset() noexcept;
    bool Process(
        const float *const ambisonics[kMaxAmbisonicsChannels],
        const float *lfe,
        float *const stereo[2],
        std::size_t frames
    ) noexcept;

    uint32_t LatencyFrames() const noexcept {
        return HaloDownmixProcessor::kLatencyFrames;
    }
    std::size_t PreparedBytes() const noexcept;

private:
    using Row = std::array<float, kMaxAmbisonicsChannels>;

    std::array<Row, kHaloDirectionalChannels> matrix_{};
    AlignedPlanarBuffer logical_bed_{};
    HaloDownmixProcessor processor_{};
    uint32_t order_ = 0U;
    uint32_t channels_ = 0U;
    std::size_t max_frames_ = 0U;
    bool prepared_ = false;
};

} // namespace iem
