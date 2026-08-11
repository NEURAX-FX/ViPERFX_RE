#include "iem/StereoEncoder.h"

#include "iem/Quaternion.h"

#include <algorithm>
#include <cmath>

namespace iem {

namespace {

constexpr float kDegreesToRadians = 0.017453292519943295F;
constexpr float kTwoPi = 6.2831853071795864769F;

bool ValidBuffers(
    const float *const stereo[2],
    float *const ambisonics[kMaxAmbisonicsChannels],
    uint32_t active_channels
) noexcept {
    if (stereo == nullptr || stereo[0] == nullptr || stereo[1] == nullptr
        || ambisonics == nullptr) {
        return false;
    }
    for (uint32_t channel = 0; channel < active_channels; ++channel) {
        if (ambisonics[channel] == nullptr) return false;
    }
    return true;
}

} // namespace

bool StereoEncoder::Prepare(const EncoderConfig &config) {
    if (config.sample_rate == 0 || config.max_frames == 0
        || config.order == 0 || config.order > kMaxAmbisonicsOrder) {
        return false;
    }
    config_ = config;
    prepared_ = true;
    first_apply_ = true;
    const IemParams defaults{};
    target_controls_ = ConvertControls(defaults.stereo);
    current_controls_ = target_controls_;
    EvaluatePair(target_controls_, config_.order, target_left_, target_right_);
    current_left_ = target_left_;
    current_right_ = target_right_;
    return true;
}

void StereoEncoder::ApplyParams(const IemParams &params) noexcept {
    target_controls_ = ConvertControls(params.stereo);
    EvaluatePair(target_controls_, config_.order, target_left_, target_right_);
    if (first_apply_) {
        current_controls_ = target_controls_;
        current_left_ = target_left_;
        current_right_ = target_right_;
        first_apply_ = false;
    }
}

void StereoEncoder::Reset() noexcept {
    current_controls_ = target_controls_;
    current_left_ = target_left_;
    current_right_ = target_right_;
}

bool StereoEncoder::Process(
    const float *const stereo[2],
    float *const ambisonics[kMaxAmbisonicsChannels],
    std::size_t frames
) noexcept {
    const uint32_t channels = AmbisonicsChannelCount(config_.order);
    if (!prepared_ || frames > config_.max_frames
        || !ValidBuffers(stereo, ambisonics, channels)) {
        return false;
    }
    for (uint32_t channel = 0; channel < kMaxAmbisonicsChannels; ++channel) {
        if (ambisonics[channel] != nullptr) {
            std::fill_n(ambisonics[channel], frames, 0.0F);
        }
    }
    if (frames == 0) return true;

    if (target_controls_.sample_wise) {
        Coefficients left{};
        Coefficients right{};
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const float amount = static_cast<float>(frame + 1)
                / static_cast<float>(frames);
            Controls controls{};
            controls.azimuth = InterpolateAngle(
                current_controls_.azimuth, target_controls_.azimuth, amount
            );
            controls.elevation = current_controls_.elevation
                + (target_controls_.elevation - current_controls_.elevation) * amount;
            controls.roll = InterpolateAngle(
                current_controls_.roll, target_controls_.roll, amount
            );
            controls.width = current_controls_.width
                + (target_controls_.width - current_controls_.width) * amount;
            controls.sample_wise = true;
            EvaluatePair(controls, config_.order, left, right);
            for (uint32_t channel = 0; channel < channels; ++channel) {
                ambisonics[channel][frame] = stereo[0][frame] * left[channel]
                    + stereo[1][frame] * right[channel];
            }
        }
    } else {
        for (uint32_t channel = 0; channel < channels; ++channel) {
            const float left_start = current_left_[channel];
            const float right_start = current_right_[channel];
            const float left_delta = target_left_[channel] - left_start;
            const float right_delta = target_right_[channel] - right_start;
            for (std::size_t frame = 0; frame < frames; ++frame) {
                const float amount = static_cast<float>(frame + 1)
                    / static_cast<float>(frames);
                ambisonics[channel][frame] = stereo[0][frame]
                        * (left_start + left_delta * amount)
                    + stereo[1][frame] * (right_start + right_delta * amount);
            }
        }
    }

    current_controls_ = target_controls_;
    current_left_ = target_left_;
    current_right_ = target_right_;
    return true;
}

StereoEncoder::Controls StereoEncoder::ConvertControls(
    const StereoParams &params
) noexcept {
    return {
        static_cast<float>(params.azimuth_centidegrees) * 0.01F * kDegreesToRadians,
        static_cast<float>(params.elevation_centidegrees) * 0.01F * kDegreesToRadians,
        static_cast<float>(params.roll_centidegrees) * 0.01F * kDegreesToRadians,
        static_cast<float>(params.width_centidegrees) * 0.01F * kDegreesToRadians,
        params.sample_wise,
    };
}

void StereoEncoder::EvaluatePair(
    const Controls &controls,
    uint32_t order,
    Coefficients &left,
    Coefficients &right
) noexcept {
    const Quaternion direction = Quaternion::FromYawPitchRoll(
        controls.azimuth,
        -controls.elevation,
        controls.roll,
        RotationSequence::ROLL_PITCH_YAW
    );
    const Quaternion width_rotation = Quaternion::FromAxisAngle(
        {0.0F, 0.0F, 1.0F}, controls.width * 0.5F
    );
    const Vec3 left_direction = (direction * width_rotation).Rotate({1.0F, 0.0F, 0.0F});
    const Vec3 right_direction = (direction * width_rotation.Conjugate()).Rotate(
        {1.0F, 0.0F, 0.0F}
    );
    EvaluateSn3d(
        order,
        std::atan2(left_direction.y, left_direction.x),
        std::atan2(
            left_direction.z,
            std::hypot(left_direction.x, left_direction.y)
        ),
        left.data()
    );
    EvaluateSn3d(
        order,
        std::atan2(right_direction.y, right_direction.x),
        std::atan2(
            right_direction.z,
            std::hypot(right_direction.x, right_direction.y)
        ),
        right.data()
    );
}

float StereoEncoder::InterpolateAngle(float start, float end, float amount) noexcept {
    float difference = std::fmod(end - start, kTwoPi);
    if (difference > kTwoPi * 0.5F) difference -= kTwoPi;
    if (difference < -kTwoPi * 0.5F) difference += kTwoPi;
    return start + difference * amount;
}

} // namespace iem
