#include "iem/MultiEncoder.h"

#include <algorithm>
#include <cmath>

namespace iem {

namespace {

constexpr float kDegreesToRadians = 0.017453292519943295F;

} // namespace

bool MultiEncoder::Prepare(const EncoderConfig &config) {
    if (config.sample_rate == 0 || config.max_frames == 0
        || config.order == 0 || config.order > kMaxAmbisonicsOrder) {
        return false;
    }
    config_ = config;
    prepared_ = true;
    first_apply_ = true;
    const IemParams defaults{};
    EvaluateTargets(defaults.multi);
    current_ = target_;
    return true;
}

void MultiEncoder::ApplyParams(const IemParams &params) noexcept {
    EvaluateTargets(params.multi);
    if (first_apply_) {
        current_ = target_;
        first_apply_ = false;
    }
}

void MultiEncoder::Reset() noexcept {
    current_ = target_;
}

bool MultiEncoder::Process(
    const float *const stereo[2],
    float *const ambisonics[kMaxAmbisonicsChannels],
    std::size_t frames
) noexcept {
    const uint32_t channels = AmbisonicsChannelCount(config_.order);
    if (!prepared_ || frames > config_.max_frames || stereo == nullptr
        || stereo[0] == nullptr || stereo[1] == nullptr || ambisonics == nullptr) {
        return false;
    }
    for (uint32_t channel = 0; channel < channels; ++channel) {
        if (ambisonics[channel] == nullptr) return false;
    }
    for (uint32_t channel = 0; channel < kMaxAmbisonicsChannels; ++channel) {
        if (ambisonics[channel] != nullptr) {
            std::fill_n(ambisonics[channel], frames, 0.0F);
        }
    }
    if (frames == 0) return true;

    for (uint32_t channel = 0; channel < channels; ++channel) {
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const float amount = static_cast<float>(frame + 1)
                / static_cast<float>(frames);
            float output = 0.0F;
            for (std::size_t source = 0; source < 2; ++source) {
                const float coefficient = current_[source][channel]
                    + (target_[source][channel] - current_[source][channel]) * amount;
                output += stereo[source][frame] * coefficient;
            }
            ambisonics[channel][frame] = output;
        }
    }
    current_ = target_;
    return true;
}

void MultiEncoder::EvaluateTargets(const MultiParams &params) noexcept {
    for (std::size_t source = 0; source < 2; ++source) {
        EvaluateSn3d(
            config_.order,
            static_cast<float>(params.azimuth_centidegrees[source])
                * 0.01F * kDegreesToRadians,
            static_cast<float>(params.elevation_centidegrees[source])
                * 0.01F * kDegreesToRadians,
            target_[source].data()
        );
        const float gain = params.mute[source]
            ? 0.0F
            : std::pow(10.0F, static_cast<float>(params.gain_decidb[source]) / 200.0F);
        for (float &coefficient : target_[source]) coefficient *= gain;
    }
}

} // namespace iem
