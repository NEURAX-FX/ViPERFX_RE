#include "iem/HaloDownmixDecoder.h"

#include <algorithm>

namespace iem {
namespace {

constexpr float kDegreesToRadians = 0.017453292519943295F;

} // namespace

bool HaloDownmixDecoder::Prepare(
    uint32_t order,
    uint32_t sample_rate,
    std::size_t max_frames
) noexcept {
    if (order == 0U || order > kMaxAmbisonicsOrder
        || sample_rate != HaloDownmixProcessor::kSampleRate || max_frames == 0U
        || !logical_bed_.Prepare(HaloDownmixProcessor::kRoleCount, max_frames)
        || !processor_.Prepare(max_frames)) {
        prepared_ = false;
        return false;
    }

    order_ = order;
    channels_ = AmbisonicsChannelCount(order);
    max_frames_ = max_frames;
    matrix_ = {};
    const float normalization = 1.0F / static_cast<float>(channels_);
    for (uint32_t speaker = 0; speaker < kHaloDirectionalChannels; ++speaker) {
        float coefficients[kMaxAmbisonicsChannels]{};
        EvaluateSn3d(order_, kHaloBedAzimuthDegrees[speaker] * kDegreesToRadians,
            0.0F, coefficients);
        for (uint32_t channel = 0; channel < channels_; ++channel) {
            matrix_[speaker][channel] = coefficients[channel] * normalization;
        }
    }
    prepared_ = true;
    Reset();
    return true;
}

void HaloDownmixDecoder::ApplyParams(const HaloDownmixParams &params) noexcept {
    processor_.ApplyParams(params);
}

void HaloDownmixDecoder::Reset() noexcept {
    logical_bed_.Clear();
    processor_.Reset();
}

bool HaloDownmixDecoder::Process(
    const float *const ambisonics[kMaxAmbisonicsChannels],
    const float *lfe,
    float *const stereo[2],
    std::size_t frames
) noexcept {
    if (!prepared_ || frames > max_frames_ || ambisonics == nullptr
        || stereo == nullptr || stereo[0] == nullptr || stereo[1] == nullptr) {
        return false;
    }
    for (uint32_t channel = 0; channel < channels_; ++channel) {
        if (ambisonics[channel] == nullptr) return false;
    }

    for (uint32_t speaker = 0; speaker < kHaloDirectionalChannels; ++speaker) {
        float *output = logical_bed_.ChannelData(speaker);
        for (std::size_t frame = 0; frame < frames; ++frame) {
            float sample = 0.0F;
            for (uint32_t channel = 0; channel < channels_; ++channel) {
                sample += matrix_[speaker][channel] * ambisonics[channel][frame];
            }
            output[frame] = sample;
        }
    }

    const uint32_t lfe_role = static_cast<uint32_t>(HaloDownmixRole::LFE);
    if (lfe == nullptr) {
        std::fill_n(logical_bed_.ChannelData(lfe_role), frames, 0.0F);
    }
    std::array<const float *, HaloDownmixProcessor::kRoleCount> inputs{};
    inputs[static_cast<uint32_t>(HaloDownmixRole::L)] =
        logical_bed_.ChannelData(static_cast<uint32_t>(HaloBedChannel::L));
    inputs[static_cast<uint32_t>(HaloDownmixRole::R)] =
        logical_bed_.ChannelData(static_cast<uint32_t>(HaloBedChannel::R));
    inputs[static_cast<uint32_t>(HaloDownmixRole::C)] =
        logical_bed_.ChannelData(static_cast<uint32_t>(HaloBedChannel::C));
    inputs[lfe_role] = lfe != nullptr ? lfe : logical_bed_.ChannelData(lfe_role);
    inputs[static_cast<uint32_t>(HaloDownmixRole::Ls)] =
        logical_bed_.ChannelData(static_cast<uint32_t>(HaloBedChannel::Ls));
    inputs[static_cast<uint32_t>(HaloDownmixRole::Rs)] =
        logical_bed_.ChannelData(static_cast<uint32_t>(HaloBedChannel::Rs));
    inputs[static_cast<uint32_t>(HaloDownmixRole::Lsr)] =
        logical_bed_.ChannelData(static_cast<uint32_t>(HaloBedChannel::Lsr));
    inputs[static_cast<uint32_t>(HaloDownmixRole::Rsr)] =
        logical_bed_.ChannelData(static_cast<uint32_t>(HaloBedChannel::Rsr));
    return processor_.Process(inputs.data(), stereo, frames);
}

std::size_t HaloDownmixDecoder::PreparedBytes() const noexcept {
    return static_cast<std::size_t>(HaloDownmixProcessor::kRoleCount)
        * max_frames_ * sizeof(float) + processor_.PreparedBytes();
}

} // namespace iem
