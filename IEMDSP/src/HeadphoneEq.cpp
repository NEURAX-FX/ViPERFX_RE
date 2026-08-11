#include "iem/HeadphoneEq.h"

#include "IemResourceManifest.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace iem {

bool HeadphoneEq::Prepare(int32_t id, uint32_t partition_frames) noexcept {
    convolver_ = PartitionedMatrixConvolver{};
    delay_.clear();
    partition_frames_ = 0;
    delay_index_ = 0;
    id_ = -2;
    error_ = IemResourceError::NONE;
    if (partition_frames < 64 || (partition_frames & (partition_frames - 1U)) != 0) {
        error_ = IemResourceError::CONVOLVER_PREPARE;
        return false;
    }
    if (id < -1 || id >= static_cast<int32_t>(resources::kHeadphoneEqResourceCount)) {
        error_ = IemResourceError::INVALID_EQ;
        return false;
    }
    if (id >= 0) {
        const resources::HeadphoneEqResource *resource = resources::FindHeadphoneEq(id);
        if (resource == nullptr || resource->id != id || resource->channels != 2
            || resource->frames == 0 || resource->impulse == nullptr) {
            error_ = IemResourceError::RESOURCE_METADATA;
            return false;
        }
        std::vector<float> matrix(static_cast<std::size_t>(4U) * resource->frames, 0.0F);
        std::copy_n(resource->impulse, resource->frames, matrix.data());
        std::copy_n(resource->impulse + resource->frames, resource->frames,
            matrix.data() + static_cast<std::size_t>(3U) * resource->frames);
        if (!convolver_.Prepare(2, resource->frames, partition_frames, matrix.data())) {
            error_ = IemResourceError::CONVOLVER_PREPARE;
            return false;
        }
    }
    id_ = id;
    partition_frames_ = partition_frames;
    delay_.assign(static_cast<std::size_t>(2U) * partition_frames_, 0.0F);
    delay_index_ = 0;
    return true;
}

bool HeadphoneEq::Process(const float *const input[2], float *const output[2],
    std::size_t frames) noexcept {
    if (input == nullptr || output == nullptr || input[0] == nullptr || input[1] == nullptr
        || output[0] == nullptr || output[1] == nullptr || partition_frames_ == 0) {
        return false;
    }
    for (uint32_t channel = 0; channel < 2; ++channel) {
        for (std::size_t frame = 0; frame < frames; ++frame) {
            if (!std::isfinite(input[channel][frame])) {
                error_ = IemResourceError::PROCESS_NONFINITE;
                return false;
            }
        }
    }
    if (id_ >= 0) {
        if (!convolver_.Process(input, output, frames)) return false;
    } else {
        for (std::size_t frame = 0; frame < frames; ++frame) {
            for (uint32_t channel = 0; channel < 2; ++channel) {
                float *ring = delay_.data() + static_cast<std::size_t>(channel)
                    * partition_frames_;
                output[channel][frame] = ring[delay_index_];
                ring[delay_index_] = input[channel][frame];
            }
            delay_index_ = (delay_index_ + 1U) % partition_frames_;
        }
    }
    for (uint32_t channel = 0; channel < 2; ++channel) {
        for (std::size_t frame = 0; frame < frames; ++frame) {
            if (!std::isfinite(output[channel][frame])) {
                error_ = IemResourceError::PROCESS_NONFINITE;
                return false;
            }
        }
    }
    return true;
}

void HeadphoneEq::Reset() noexcept {
    convolver_.Reset();
    std::fill(delay_.begin(), delay_.end(), 0.0F);
    delay_index_ = 0;
    if (error_ == IemResourceError::PROCESS_NONFINITE) {
        error_ = IemResourceError::NONE;
    }
}

} // namespace iem
