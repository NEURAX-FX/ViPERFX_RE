#include "iem/HaloEncoder.h"

#include <algorithm>
#include <cmath>

namespace iem {
namespace {

constexpr float kDegreesToRadians = 0.017453292519943295F;
constexpr float kCentreFold = 0.7071067811865476F;
constexpr float kSurroundFold = 0.7071067811865476F;
constexpr float kRearFold = 0.5F;

bool ValidBed(const float *const bed[kHaloBedChannels]) noexcept {
    if (bed == nullptr) return false;
    for (uint32_t channel = 0; channel < kHaloBedChannels; ++channel) {
        if (bed[channel] == nullptr) return false;
    }
    return true;
}

bool ValidMutableBed(float *const bed[kHaloBedChannels]) noexcept {
    if (bed == nullptr) return false;
    for (uint32_t channel = 0; channel < kHaloBedChannels; ++channel) {
        if (bed[channel] == nullptr) return false;
    }
    return true;
}

} // namespace

void EncodeHaloBedToSn3d(
    uint32_t order,
    const float *const bed[kHaloBedChannels],
    float *const ambisonics[kMaxAmbisonicsChannels],
    std::size_t frames
) noexcept {
    if (order == 0 || order > kMaxAmbisonicsOrder || !ValidBed(bed)
        || ambisonics == nullptr) return;
    const uint32_t channels = AmbisonicsChannelCount(order);
    for (uint32_t channel = 0; channel < channels; ++channel) {
        if (ambisonics[channel] == nullptr) return;
        std::fill_n(ambisonics[channel], frames, 0.0F);
    }
    for (uint32_t source = 0; source < kHaloBedChannels; ++source) {
        float coefficients[kMaxAmbisonicsChannels]{};
        EvaluateSn3d(order, kHaloBedAzimuthDegrees[source] * kDegreesToRadians,
            0.0F, coefficients);
        for (uint32_t channel = 0; channel < channels; ++channel) {
            for (std::size_t frame = 0; frame < frames; ++frame) {
                ambisonics[channel][frame] += bed[source][frame] * coefficients[channel];
            }
        }
    }
}

void FoldHaloBedToStereo(
    const float *const bed[kHaloBedChannels],
    float *const stereo[2],
    std::size_t frames
) noexcept {
    if (!ValidBed(bed) || stereo == nullptr || stereo[0] == nullptr || stereo[1] == nullptr) {
        return;
    }
    const uint32_t l = static_cast<uint32_t>(HaloBedChannel::L);
    const uint32_t r = static_cast<uint32_t>(HaloBedChannel::R);
    const uint32_t c = static_cast<uint32_t>(HaloBedChannel::C);
    const uint32_t ls = static_cast<uint32_t>(HaloBedChannel::Ls);
    const uint32_t rs = static_cast<uint32_t>(HaloBedChannel::Rs);
    const uint32_t lsr = static_cast<uint32_t>(HaloBedChannel::Lsr);
    const uint32_t rsr = static_cast<uint32_t>(HaloBedChannel::Rsr);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        stereo[0][frame] = bed[l][frame] + kCentreFold * bed[c][frame]
            + kSurroundFold * bed[ls][frame] + kRearFold * bed[lsr][frame];
        stereo[1][frame] = bed[r][frame] + kCentreFold * bed[c][frame]
            + kSurroundFold * bed[rs][frame] + kRearFold * bed[rsr][frame];
    }
}

bool HaloEncoder::Prepare(const EncoderConfig &config) noexcept {
    if (config.sample_rate != 96000 || config.max_frames == 0
        || config.order == 0 || config.order > kMaxAmbisonicsOrder) return false;
    config_ = config;
    ring_frames_ = HaloStft::kReportedLatency + HaloStft::kFftSize
        + 4U * config.max_frames;
    if (!analysis_.Prepare(config.max_frames)
        || !dialog_.Prepare()
        || !surround_.Prepare()
        || !diffusion_.Prepare(config.sample_rate)
        || !frame_time_.Prepare(kHaloBedChannels, HaloStft::kFftSize)
        || !output_ring_.Prepare(kHaloBedChannels, ring_frames_)) {
        prepared_ = false;
        return false;
    }
    for (auto &stft : synthesis_) {
        if (!stft.Prepare(HaloStft::kFftSize)) {
            prepared_ = false;
            return false;
        }
    }
    prepared_ = true;
    Reset();
    ApplyParams(IemParams{});
    return true;
}

void HaloEncoder::ApplyParams(const IemParams &params) noexcept {
    dialog_.ApplyParams(params.halo);
    surround_.ApplyParams(params.halo);
    diffusion_.ApplyParams(params.halo);
}

void HaloEncoder::Reset() noexcept {
    analysis_.Reset();
    for (auto &stft : synthesis_) stft.Reset();
    dialog_.Reset();
    surround_.Reset();
    diffusion_.Reset();
    frame_time_.Clear();
    output_ring_.Clear();
    read_frame_ = 0;
    synthesis_frame_ = HaloStft::kReportedLatency;
}

void HaloEncoder::OnAnalysisFrame(
    const float left_re[HaloStft::kBins],
    const float left_im[HaloStft::kBins],
    const float right_re[HaloStft::kBins],
    const float right_im[HaloStft::kBins],
    void *user
) noexcept {
    static_cast<HaloEncoder *>(user)->RenderFrame(left_re, left_im, right_re, right_im);
}

void HaloEncoder::RenderFrame(
    const float left_re[HaloStft::kBins],
    const float left_im[HaloStft::kBins],
    const float right_re[HaloStft::kBins],
    const float right_im[HaloStft::kBins]
) noexcept {
    HaloDialogFrame dialog_frame{};
    dialog_.ProcessFrame(left_re, left_im, right_re, right_im, dialog_frame);
    surround_.ProcessFrame(dialog_frame, bed_re_, bed_im_);
    float *frame[kHaloBedChannels]{};
    for (uint32_t channel = 0; channel < kHaloBedChannels; ++channel) {
        frame[channel] = frame_time_.ChannelData(channel);
        synthesis_[channel].InverseAdd(bed_re_[channel], bed_im_[channel],
            frame[channel], HaloStft::kFftSize);
    }
    diffusion_.Process(frame, HaloStft::kFftSize);
    for (uint32_t channel = 0; channel < kHaloBedChannels; ++channel) {
        float *ring = output_ring_.ChannelData(channel);
        for (uint32_t offset = 0; offset < HaloStft::kFftSize; ++offset) {
            ring[(synthesis_frame_ + offset) % ring_frames_] += frame[channel][offset];
        }
    }
    synthesis_frame_ += HaloStft::kHop;
}

bool HaloEncoder::ProcessBed(
    const float *const stereo[2],
    float *const bed[kHaloBedChannels],
    std::size_t frames
) noexcept {
    if (!prepared_ || frames > config_.max_frames || stereo == nullptr
        || stereo[0] == nullptr || stereo[1] == nullptr || !ValidMutableBed(bed)) return false;
    if (!analysis_.Process(stereo[0], stereo[1], frames, OnAnalysisFrame, this)) return false;
    for (uint32_t channel = 0; channel < kHaloBedChannels; ++channel) {
        float *ring = output_ring_.ChannelData(channel);
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const std::size_t index = (read_frame_ + frame) % ring_frames_;
            bed[channel][frame] = ring[index];
            ring[index] = 0.0F;
        }
    }
    read_frame_ += frames;
    return true;
}

std::size_t HaloEncoder::PreparedBytes() const noexcept {
    return kHaloBedChannels * (ring_frames_ + HaloStft::kFftSize) * sizeof(float)
        + 2U * kHaloBedChannels * HaloStft::kBins * sizeof(float);
}

} // namespace iem
