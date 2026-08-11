#include "iem/Ku100Decoder.h"

#include "IemResourceManifest.h"

#include <cmath>

namespace iem {

bool Ku100Decoder::Prepare(uint32_t order, uint32_t partition_frames) noexcept {
    convolver_ = PartitionedMatrixConvolver{};
    order_ = 0;
    input_channels_ = 0;
    error_ = IemResourceError::NONE;
    const resources::Ku100Resource *resource = resources::FindKu100(order);
    if (resource == nullptr) {
        error_ = IemResourceError::INVALID_ORDER;
        return false;
    }
    const uint32_t expected_inputs = AmbisonicsChannelCount(order);
    if (resource->order != order || resource->input_channels != expected_inputs
        || resource->frames == 0 || resource->ir == nullptr) {
        error_ = IemResourceError::RESOURCE_METADATA;
        return false;
    }
    if (!convolver_.Prepare(expected_inputs, resource->frames,
            partition_frames, resource->ir)) {
        error_ = IemResourceError::CONVOLVER_PREPARE;
        return false;
    }
    order_ = order;
    input_channels_ = expected_inputs;
    return true;
}

bool Ku100Decoder::Process(const float *const input[kMaxAmbisonicsChannels],
    float *const output[2], std::size_t frames) noexcept {
    if (input == nullptr || output == nullptr || output[0] == nullptr
        || output[1] == nullptr || input_channels_ == 0) return false;
    for (uint32_t channel = 0; channel < input_channels_; ++channel) {
        if (input[channel] == nullptr) return false;
        for (std::size_t frame = 0; frame < frames; ++frame) {
            if (!std::isfinite(input[channel][frame])) {
                error_ = IemResourceError::PROCESS_NONFINITE;
                return false;
            }
        }
    }
    if (!convolver_.Process(input, output, frames)) return false;
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

void Ku100Decoder::Reset() noexcept {
    convolver_.Reset();
    if (error_ == IemResourceError::PROCESS_NONFINITE) {
        error_ = IemResourceError::NONE;
    }
}

} // namespace iem
