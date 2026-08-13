#pragma once

#include "iem/HaloBed.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace iem {

class SimpleDecoder {
public:
    bool Prepare(uint32_t order) noexcept;
    bool Process(
        float *const ambisonics[kMaxAmbisonicsChannels],
        float *const stereo[2],
        std::size_t frames
    ) noexcept;

private:
    using Row = std::array<float, kMaxAmbisonicsChannels>;
    std::array<Row, kHaloBedChannels> matrix_{};
    uint32_t order_ = 0;
    uint32_t channels_ = 0;
    bool prepared_ = false;
};

} // namespace iem
