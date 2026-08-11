#pragma once

#include "iem/PartitionedMatrixConvolver.h"
#include "iem/SphericalHarmonics.h"

#include <cstddef>
#include <cstdint>

namespace iem {

enum class IemResourceError : uint32_t {
    NONE = 0,
    INVALID_ORDER,
    INVALID_EQ,
    RESOURCE_METADATA,
    CONVOLVER_PREPARE,
    PROCESS_NONFINITE,
};

class Ku100Decoder {
public:
    bool Prepare(uint32_t order, uint32_t partition_frames) noexcept;
    bool Process(const float *const input[kMaxAmbisonicsChannels],
        float *const output[2], std::size_t frames) noexcept;
    void Reset() noexcept;

    uint32_t Order() const noexcept { return order_; }
    uint32_t InputChannels() const noexcept { return input_channels_; }
    uint32_t LatencyFrames() const noexcept { return convolver_.LatencyFrames(); }
    IemResourceError Error() const noexcept { return error_; }

private:
    PartitionedMatrixConvolver convolver_{};
    uint32_t order_ = 0;
    uint32_t input_channels_ = 0;
    IemResourceError error_ = IemResourceError::NONE;
};

} // namespace iem
