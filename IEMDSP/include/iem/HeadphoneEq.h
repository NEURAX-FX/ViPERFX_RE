#pragma once

#include "iem/Ku100Decoder.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace iem {

class HeadphoneEq {
public:
    bool Prepare(int32_t id, uint32_t partition_frames) noexcept;
    bool Process(const float *const input[2], float *const output[2],
        std::size_t frames) noexcept;
    void Reset() noexcept;

    int32_t Id() const noexcept { return id_; }
    uint32_t LatencyFrames() const noexcept { return partition_frames_; }
    bool UsesConvolution() const noexcept { return id_ >= 0; }
    IemResourceError Error() const noexcept { return error_; }

private:
    PartitionedMatrixConvolver convolver_{};
    std::vector<float> delay_{};
    uint32_t partition_frames_ = 0;
    uint32_t delay_index_ = 0;
    int32_t id_ = -1;
    IemResourceError error_ = IemResourceError::NONE;
};

} // namespace iem
