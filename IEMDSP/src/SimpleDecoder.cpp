#include "iem/SimpleDecoder.h"

#include <algorithm>

namespace iem {
namespace {

constexpr float kDegreesToRadians = 0.017453292519943295F;
constexpr float kCentreFold = 0.7071067811865476F;
constexpr float kSurroundFold = 0.7071067811865476F;
constexpr float kRearFold = 0.5F;

} // namespace

bool SimpleDecoder::Prepare(uint32_t order) noexcept {
    if (order == 0 || order > kMaxAmbisonicsOrder) {
        prepared_ = false;
        return false;
    }
    order_ = order;
    channels_ = AmbisonicsChannelCount(order);
    matrix_ = {};
    const float normalization = 1.0F / static_cast<float>(channels_);
    for (uint32_t speaker = 0; speaker < kHaloBedChannels; ++speaker) {
        float coefficients[kMaxAmbisonicsChannels]{};
        EvaluateSn3d(order_, kHaloBedAzimuthDegrees[speaker] * kDegreesToRadians,
            0.0F, coefficients);
        for (uint32_t channel = 0; channel < channels_; ++channel) {
            matrix_[speaker][channel] = coefficients[channel] * normalization;
        }
    }
    prepared_ = true;
    return true;
}

bool SimpleDecoder::Process(
    const float *const ambisonics[kMaxAmbisonicsChannels],
    float *const stereo[2],
    std::size_t frames
) noexcept {
    if (!prepared_ || ambisonics == nullptr || stereo == nullptr
        || stereo[0] == nullptr || stereo[1] == nullptr) return false;
    for (uint32_t channel = 0; channel < channels_; ++channel) {
        if (ambisonics[channel] == nullptr) return false;
    }
    for (std::size_t frame = 0; frame < frames; ++frame) {
        float speaker[kHaloBedChannels]{};
        for (uint32_t output = 0; output < kHaloBedChannels; ++output) {
            for (uint32_t channel = 0; channel < channels_; ++channel) {
                speaker[output] += matrix_[output][channel] * ambisonics[channel][frame];
            }
        }
        stereo[0][frame] = speaker[static_cast<uint32_t>(HaloBedChannel::L)]
            + kCentreFold * speaker[static_cast<uint32_t>(HaloBedChannel::C)]
            + kSurroundFold * speaker[static_cast<uint32_t>(HaloBedChannel::Ls)]
            + kRearFold * speaker[static_cast<uint32_t>(HaloBedChannel::Lsr)];
        stereo[1][frame] = speaker[static_cast<uint32_t>(HaloBedChannel::R)]
            + kCentreFold * speaker[static_cast<uint32_t>(HaloBedChannel::C)]
            + kSurroundFold * speaker[static_cast<uint32_t>(HaloBedChannel::Rs)]
            + kRearFold * speaker[static_cast<uint32_t>(HaloBedChannel::Rsr)];
    }
    return true;
}

} // namespace iem
