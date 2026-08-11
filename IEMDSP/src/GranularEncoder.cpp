#include "iem/GranularEncoder.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace iem {

namespace {

constexpr float kDegreesToRadians = 0.017453292519943295F;
constexpr float kPi = 3.14159265358979323846F;
constexpr float kTwoPi = 6.2831853071795864769F;

float CentidegreesToRadians(int32_t value) noexcept {
    return static_cast<float>(value) * 0.01F * kDegreesToRadians;
}

} // namespace

bool GranularEncoder::Prepare(const EncoderConfig &config) {
    if (config.sample_rate == 0 || config.max_frames == 0
        || config.order == 0 || config.order > kMaxAmbisonicsOrder) {
        return false;
    }
    const uint64_t history_frames = static_cast<uint64_t>(config.sample_rate)
        * kHistorySeconds;
    if (history_frames == 0
        || history_frames > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
        return false;
    }
    config_ = config;
    history_.assign(static_cast<std::size_t>(history_frames) * 2U, 0.0F);
    random_ = FixedRandom(config.random_seed);
    prepared_ = true;
    first_apply_ = true;
    const IemParams defaults{};
    params_ = defaults.granular;
    ClampParams();
    EvaluateCenter();
    current_center_ = target_center_;
    current_center_weights_ = target_center_weights_;
    Reset();
    first_apply_ = true;
    return true;
}

void GranularEncoder::ApplyParams(const IemParams &params) noexcept {
    params_ = params.granular;
    ClampParams();
    EvaluateCenter();
    if (first_apply_) {
        current_center_ = target_center_;
        current_center_weights_ = target_center_weights_;
        first_apply_ = false;
    }
}

void GranularEncoder::Reset() noexcept {
    std::fill(history_.begin(), history_.end(), 0.0F);
    for (Grain &grain : grains_) grain.Reset();
    free_count_ = kMaxGrains;
    active_count_ = 0;
    for (std::size_t index = 0; index < kMaxGrains; ++index) {
        free_indices_[index] = static_cast<uint16_t>(kMaxGrains - index - 1U);
    }
    random_ = FixedRandom(config_.random_seed);
    write_head_ = 0;
    valid_history_frames_ = 0;
    grain_counter_ = 0;
    delta_samples_ = 0;
    pool_exhaustion_count_ = 0;
    frozen_ = false;
    last_spawned_direction_ = {1.0F, 0.0F, 0.0F};
    current_center_ = target_center_;
    current_center_weights_ = target_center_weights_;
}

void GranularEncoder::SetFreeze(bool freeze) noexcept {
    if (!freeze) {
        frozen_ = false;
        return;
    }
    if (valid_history_frames_ != 0) frozen_ = true;
}

bool GranularEncoder::Process(
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

    const std::size_t history_frames = history_.size() / 2U;
    float *left_history = history_.data();
    float *right_history = history_.data() + history_frames;
    const float mix = static_cast<float>(params_.mix_tenths_percent) * 0.001F;
    const float dry_gain = std::sqrt(std::max(0.0F, 1.0F - mix));
    const float wet_gain = std::sqrt(std::max(0.0F, mix));

    for (std::size_t frame = 0; frame < frames; ++frame) {
        const float amount = static_cast<float>(frame + 1U) / static_cast<float>(frames);
        std::array<float, kMaxAmbisonicsChannels> dry_weights{};
        if (params_.sample_wise) {
            CenterControls center{};
            center.azimuth = InterpolateAngle(
                current_center_.azimuth, target_center_.azimuth, amount
            );
            center.elevation = current_center_.elevation
                + (target_center_.elevation - current_center_.elevation) * amount;
            center.roll = InterpolateAngle(current_center_.roll, target_center_.roll, amount);
            const Quaternion direction = Quaternion::FromYawPitchRoll(
                center.azimuth,
                -center.elevation,
                center.roll,
                RotationSequence::ROLL_PITCH_YAW
            );
            const Vec3 vector = direction.Rotate({1.0F, 0.0F, 0.0F});
            EvaluateSn3d(
                config_.order,
                std::atan2(vector.y, vector.x),
                std::atan2(vector.z, std::hypot(vector.x, vector.y)),
                dry_weights.data()
            );
        } else {
            for (uint32_t channel = 0; channel < channels; ++channel) {
                dry_weights[channel] = current_center_weights_[channel]
                    + (target_center_weights_[channel] - current_center_weights_[channel])
                        * amount;
            }
        }

        if (!frozen_) {
            left_history[write_head_] = stereo[0][frame];
            right_history[write_head_] = stereo[1][frame];
        }

        if (grain_counter_ >= delta_samples_) {
            grain_counter_ = 0;
            delta_samples_ = RandomDeltaSamples();
            if (!SpawnGrain()) ++pool_exhaustion_count_;
        } else {
            ++grain_counter_;
        }

        std::array<float, kMaxAmbisonicsChannels> wet{};
        std::size_t active_index = 0;
        while (active_index < active_count_) {
            const uint16_t grain_index = active_indices_[active_index];
            Grain &grain = grains_[grain_index];
            const float sample = grain.RenderSample(
                left_history, right_history, history_frames
            );
            const auto &weights = grain.ChannelWeights();
            for (uint32_t channel = 0; channel < channels; ++channel) {
                wet[channel] += sample * weights[channel];
            }
            if (!grain.IsActive()) {
                free_indices_[free_count_++] = grain_index;
                active_indices_[active_index] = active_indices_[--active_count_];
            } else {
                ++active_index;
            }
        }

        const float dry_sample = stereo[0][frame] + stereo[1][frame];
        for (uint32_t channel = 0; channel < channels; ++channel) {
            ambisonics[channel][frame] = dry_sample * dry_weights[channel] * dry_gain
                + wet[channel] * wet_gain;
        }
        if (!frozen_) {
            write_head_ = static_cast<uint32_t>((write_head_ + 1U) % history_frames);
            if (valid_history_frames_ < history_frames) ++valid_history_frames_;
        }
    }

    current_center_ = target_center_;
    current_center_weights_ = target_center_weights_;
    return true;
}

void GranularEncoder::ClampParams() noexcept {
    params_.azimuth_centidegrees = std::clamp(
        params_.azimuth_centidegrees, -18000, 18000
    );
    params_.elevation_centidegrees = std::clamp(
        params_.elevation_centidegrees, -18000, 18000
    );
    params_.shape_tenths = std::clamp(params_.shape_tenths, -100, 100);
    params_.size_centidegrees = std::clamp(params_.size_centidegrees, 0, 36000);
    params_.roll_centidegrees = std::clamp(params_.roll_centidegrees, -18000, 18000);
    params_.width_centidegrees = std::clamp(params_.width_centidegrees, -36000, 36000);
    params_.delta_time_us = std::clamp(params_.delta_time_us, 1000, 2000000);
    params_.delta_time_mod_tenths_percent = std::clamp(
        params_.delta_time_mod_tenths_percent, 0, 1000
    );
    params_.grain_length_us = std::clamp(params_.grain_length_us, 1000, 2000000);
    params_.grain_length_mod_tenths_percent = std::clamp(
        params_.grain_length_mod_tenths_percent, 0, 1000
    );
    params_.read_position_us = std::clamp(params_.read_position_us, 0, 4000000);
    params_.position_mod_us = std::clamp(params_.position_mod_us, 0, 4000000);
    params_.pitch_millisem = std::clamp(params_.pitch_millisem, -12000, 12000);
    params_.pitch_mod_millisem = std::clamp(params_.pitch_mod_millisem, 0, 12000);
    params_.attack_tenths_percent = std::clamp(
        params_.attack_tenths_percent, 0, 500
    );
    params_.attack_mod_tenths_percent = std::clamp(
        params_.attack_mod_tenths_percent, 0, 1000
    );
    params_.decay_tenths_percent = std::clamp(params_.decay_tenths_percent, 0, 500);
    params_.decay_mod_tenths_percent = std::clamp(
        params_.decay_mod_tenths_percent, 0, 1000
    );
    params_.mix_tenths_percent = std::clamp(params_.mix_tenths_percent, 0, 1000);
    params_.source_probability_hundredths = std::clamp(
        params_.source_probability_hundredths, -100, 100
    );
    if (params_.spatial_mode != GranularSpatialMode::THREE_D
        && params_.spatial_mode != GranularSpatialMode::TWO_D) {
        params_.spatial_mode = GranularSpatialMode::THREE_D;
    }
}

void GranularEncoder::EvaluateCenter() noexcept {
    target_center_ = {
        CentidegreesToRadians(params_.azimuth_centidegrees),
        CentidegreesToRadians(params_.elevation_centidegrees),
        CentidegreesToRadians(params_.roll_centidegrees),
    };
    const Quaternion direction = Quaternion::FromYawPitchRoll(
        target_center_.azimuth,
        -target_center_.elevation,
        target_center_.roll,
        RotationSequence::ROLL_PITCH_YAW
    );
    const Vec3 vector = direction.Rotate({1.0F, 0.0F, 0.0F});
    EvaluateSn3d(
        config_.order,
        std::atan2(vector.y, vector.x),
        std::atan2(vector.z, std::hypot(vector.x, vector.y)),
        target_center_weights_.data()
    );
}

Vec3 GranularEncoder::RandomDirection() noexcept {
    const float spread = SymmetricSpreadSample();
    const float size_factor = static_cast<float>(params_.size_centidegrees) / 36000.0F;
    const float sized_spread = spread * size_factor;
    if (params_.spatial_mode == GranularSpatialMode::TWO_D) {
        const float sign = random_.NextUnit() > 0.5F ? 1.0F : -1.0F;
        const float azimuth = target_center_.azimuth + kPi * sized_spread * sign;
        const float cos_elevation = std::cos(target_center_.elevation);
        return {
            std::cos(azimuth) * cos_elevation,
            std::sin(azimuth) * cos_elevation,
            std::sin(target_center_.elevation),
        };
    }

    const float cos_theta = std::clamp(1.0F - 2.0F * sized_spread, -1.0F, 1.0F);
    const float sin_theta = std::sqrt(std::max(0.0F, 1.0F - cos_theta * cos_theta));
    const float phi = random_.NextUnit() * kTwoPi;
    const Vec3 local{
        cos_theta,
        sin_theta * std::cos(phi),
        sin_theta * std::sin(phi),
    };
    const Quaternion direction = Quaternion::FromYawPitchRoll(
        target_center_.azimuth,
        -target_center_.elevation,
        target_center_.roll,
        RotationSequence::ROLL_PITCH_YAW
    );
    return direction.Rotate(local);
}

float GranularEncoder::SymmetricSpreadSample() noexcept {
    const float magnitude = std::pow(2.0F, std::abs(params_.shape_tenths) * 0.1F);
    const float edge_distance = std::pow(
        std::abs(random_.NextBipolar()), magnitude
    );
    return params_.shape_tenths < 0 ? 1.0F - edge_distance : edge_distance;
}

uint32_t GranularEncoder::RandomDeltaSamples() noexcept {
    const float modulation = static_cast<float>(params_.delta_time_mod_tenths_percent)
        * 0.001F;
    const float seconds = static_cast<float>(params_.delta_time_us) * 1.0e-6F
        * (1.0F + modulation * random_.NextBipolar());
    const float clamped = std::clamp(seconds, 0.001F, 2.0F);
    return std::max(1U, static_cast<uint32_t>(std::lround(clamped * config_.sample_rate)));
}

bool GranularEncoder::SpawnGrain() noexcept {
    if (free_count_ == 0) return false;
    const uint16_t index = free_indices_[--free_count_];
    const std::size_t history_frames = history_.size() / 2U;

    const float position_seconds = static_cast<float>(params_.read_position_us) * 1.0e-6F
        + static_cast<float>(params_.position_mod_us) * 1.0e-6F * random_.NextUnit();
    const uint32_t position_samples = static_cast<uint32_t>(
        std::lround(position_seconds * config_.sample_rate)
    );
    const uint32_t start_position = static_cast<uint32_t>(
        (static_cast<uint64_t>(write_head_) + history_frames
            - (position_samples % history_frames)) % history_frames
    );

    const float length_modulation = static_cast<float>(
        params_.grain_length_mod_tenths_percent
    ) * 0.001F;
    const float length_seconds = std::clamp(
        static_cast<float>(params_.grain_length_us) * 1.0e-6F
            * (1.0F + length_modulation * random_.NextBipolar()),
        0.001F,
        2.0F
    );
    const float pitch_modulation = static_cast<float>(params_.pitch_mod_millisem)
        * 0.001F * random_.NextBipolar();
    float pitch_semitones = static_cast<float>(params_.pitch_millisem) * 0.001F
        - pitch_modulation;
    if (!frozen_) pitch_semitones = std::clamp(pitch_semitones, -12.0F, 0.0F);
    const float pitch_factor = std::exp2(pitch_semitones / 12.0F);
    const float stretched_seconds = std::min(length_seconds / pitch_factor, 4.0F);
    const uint32_t length_samples = std::max(
        1U,
        static_cast<uint32_t>(stretched_seconds * config_.sample_rate)
    );

    const float attack = std::clamp(
        static_cast<float>(params_.attack_tenths_percent) * 0.001F
            + static_cast<float>(params_.attack_mod_tenths_percent) * 0.001F
                * random_.NextBipolar(),
        0.0F,
        0.5F
    );
    const float decay = std::clamp(
        static_cast<float>(params_.decay_tenths_percent) * 0.001F
            + static_cast<float>(params_.decay_mod_tenths_percent) * 0.001F
                * random_.NextBipolar(),
        0.0F,
        0.5F
    );
    const float source_threshold = static_cast<float>(
        params_.source_probability_hundredths
    ) * 0.005F + 0.5F;
    const uint32_t source = random_.NextUnit() > source_threshold ? 0U : 1U;

    last_spawned_direction_ = RandomDirection();
    std::array<float, kMaxAmbisonicsChannels> weights{};
    EvaluateSn3d(
        config_.order,
        std::atan2(last_spawned_direction_.y, last_spawned_direction_.x),
        std::atan2(
            last_spawned_direction_.z,
            std::hypot(last_spawned_direction_.x, last_spawned_direction_.y)
        ),
        weights.data()
    );
    GrainStart start{};
    start.start_position = start_position;
    start.length_samples = length_samples;
    start.pitch_factor = pitch_factor;
    start.channel_weights = weights;
    start.gain = GrainGain();
    start.attack_fraction = attack;
    start.decay_fraction = decay;
    start.source_channel = source;
    grains_[index].Start(start);
    active_indices_[active_count_++] = index;
    return true;
}

float GranularEncoder::MeanWindowPower() const noexcept {
    constexpr int kResolution = 256;
    const float attack = static_cast<float>(params_.attack_tenths_percent) * 0.001F;
    const float decay = static_cast<float>(params_.decay_tenths_percent) * 0.001F;
    float sum = 0.0F;
    for (int index = 0; index < kResolution; ++index) {
        const float progress = static_cast<float>(index)
            / static_cast<float>(kResolution - 1);
        float value = 1.0F;
        if (attack > 0.0F && progress < attack) {
            const float sine = std::sin(progress / attack * kPi * 0.5F);
            value = sine * sine;
        } else if (decay > 0.0F && progress > 1.0F - decay) {
            const float cosine = std::cos(
                (progress - (1.0F - decay)) / decay * kPi * 0.5F
            );
            value = cosine * cosine;
        }
        sum += value * value;
    }
    return std::max(sum / static_cast<float>(kResolution), 1.0e-6F);
}

float GranularEncoder::GrainGain() const noexcept {
    const float overlap = std::min(
        static_cast<float>(params_.grain_length_us)
            / static_cast<float>(params_.delta_time_us),
        static_cast<float>(kMaxGrains)
    );
    const float window_power = MeanWindowPower();
    const float normalization = params_.position_mod_us > 0
        ? std::sqrt(1.0F / std::max(overlap * window_power, 1.0e-6F))
        : 1.0F / std::max(overlap * window_power, 1.0e-6F);
    return std::min(normalization, 1.0F) * 1.41F;
}

float GranularEncoder::InterpolateAngle(float start, float end, float amount) noexcept {
    float difference = std::fmod(end - start, kTwoPi);
    if (difference > kPi) difference -= kTwoPi;
    if (difference < -kPi) difference += kTwoPi;
    return start + difference * amount;
}

} // namespace iem
