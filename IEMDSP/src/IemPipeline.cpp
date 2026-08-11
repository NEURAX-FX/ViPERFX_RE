#include "iem/IemPipeline.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <type_traits>

namespace iem {
namespace {

constexpr float kHalfPi = 1.5707963267948966F;

uint32_t ProfileIndex(LatencyProfile profile) noexcept {
    const uint32_t index = static_cast<uint32_t>(profile);
    return index <= 2 ? index : 1U;
}

} // namespace

bool IemPipeline::Prepare(const IemParams &params, std::size_t max_frames) noexcept {
    prepared_ = false;
    error_ = IemResourceError::NONE;
    if (max_frames == 0 || params.order == 0 || params.order > kMaxAmbisonicsOrder) {
        error_ = IemResourceError::INVALID_ORDER;
        return false;
    }
    params_ = params;
    max_frames_ = max_frames;
    profile_config_ = kLatencyProfiles[ProfileIndex(params.latency_profile)];
    if (!PrepareEncoder(params, max_frames)) return false;
    const EncoderConfig encoder_config{96000, max_frames, params.order, 0x6D2B79F5U};
    if (!rotator_.Prepare(encoder_config)) return false;
    if (!decoder_.Prepare(params.order, profile_config_.partition_frames)) {
        error_ = decoder_.Error();
        return false;
    }
    if (!headphone_eq_.Prepare(static_cast<int32_t>(params.decoder.headphone_eq),
            profile_config_.partition_frames)) {
        error_ = headphone_eq_.Error();
        return false;
    }
    if (!limiter_.Prepare(96000)
        || !encoded_.Prepare(kMaxAmbisonicsChannels, max_frames)
        || !rotated_.Prepare(kMaxAmbisonicsChannels, max_frames)
        || !decoded_.Prepare(2, max_frames)
        || !equalized_.Prepare(2, max_frames)
        || !delayed_dry_.Prepare(2, max_frames)
        || !mix_.Prepare(2, max_frames)) {
        return false;
    }
    wet_latency_frames_ = decoder_.LatencyFrames() + headphone_eq_.LatencyFrames();
    total_latency_frames_ = wet_latency_frames_ + limiter_.LatencyFrames();
    if (total_latency_frames_ > profile_config_.maximum_latency_ms * 96U) return false;
    dry_delay_.assign(static_cast<std::size_t>(2U) * wet_latency_frames_, 0.0F);
    dry_delay_index_ = 0;
    wet_smoother_.Reset(std::clamp(params.wet, 0.0F, 1.0F));
    gain_smoother_.Reset(std::pow(10.0F,
        std::clamp(params.output_gain_db, -24.0F, 24.0F) / 20.0F));
    prepared_ = true;
    ApplyParams(params);
    Reset();
    return true;
}

void IemPipeline::ApplyParams(const IemParams &params) noexcept {
    params_.enable = params.enable;
    params_.wet = params.wet;
    params_.output_gain_db = params.output_gain_db;
    params_.limiter = params.limiter;
    params_.stereo = params.stereo;
    params_.multi = params.multi;
    params_.granular = params.granular;
    params_.rotation = params.rotation;
    if (IemEncoder *encoder = Encoder()) encoder->ApplyParams(params_);
    rotator_.ApplyParams(params_);
    limiter_.SetEnabled(params_.limiter.enabled);
    limiter_.SetCeilingCentidb(params_.limiter.ceiling_centidb);
    wet_smoother_.SetTarget(std::clamp(params_.wet, 0.0F, 1.0F), max_frames_);
    gain_smoother_.SetTarget(std::pow(10.0F,
        std::clamp(params_.output_gain_db, -24.0F, 24.0F) / 20.0F), max_frames_);
}

bool IemPipeline::Process(const float *const stereo[2], float *const output[2],
    std::size_t frames) noexcept {
    if (!prepared_ || frames > max_frames_ || stereo == nullptr || output == nullptr
        || stereo[0] == nullptr || stereo[1] == nullptr || output[0] == nullptr
        || output[1] == nullptr || Encoder() == nullptr) return false;
    std::array<float *, kMaxAmbisonicsChannels> encoded_pointers{};
    std::array<float *, kMaxAmbisonicsChannels> rotated_pointers{};
    std::array<const float *, kMaxAmbisonicsChannels> encoded_inputs{};
    std::array<const float *, kMaxAmbisonicsChannels> rotated_inputs{};
    for (uint32_t channel = 0; channel < kMaxAmbisonicsChannels; ++channel) {
        encoded_pointers[channel] = encoded_.ChannelData(channel);
        rotated_pointers[channel] = rotated_.ChannelData(channel);
        encoded_inputs[channel] = encoded_pointers[channel];
        rotated_inputs[channel] = rotated_pointers[channel];
    }
    if (!Encoder()->Process(stereo, encoded_pointers.data(), frames)
        || !rotator_.Process(encoded_inputs.data(), rotated_pointers.data(), frames)) {
        return false;
    }
    float *decoded_outputs[2]{decoded_.ChannelData(0), decoded_.ChannelData(1)};
    if (!decoder_.Process(rotated_inputs.data(), decoded_outputs, frames)) {
        error_ = decoder_.Error();
        return false;
    }
    const float *decoded_inputs[2]{decoded_outputs[0], decoded_outputs[1]};
    float *equalized_outputs[2]{equalized_.ChannelData(0), equalized_.ChannelData(1)};
    if (!headphone_eq_.Process(decoded_inputs, equalized_outputs, frames)) {
        error_ = headphone_eq_.Error();
        return false;
    }

    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (uint32_t channel = 0; channel < 2; ++channel) {
            float *ring = dry_delay_.data()
                + static_cast<std::size_t>(channel) * wet_latency_frames_;
            delayed_dry_.ChannelData(channel)[frame] = ring[dry_delay_index_];
            ring[dry_delay_index_] = stereo[channel][frame];
        }
        dry_delay_index_ = (dry_delay_index_ + 1U) % wet_latency_frames_;
        const float wet = wet_smoother_.Next();
        const float dry_gain = std::cos(wet * kHalfPi);
        const float wet_gain = std::sin(wet * kHalfPi);
        const float output_gain = gain_smoother_.Next();
        for (uint32_t channel = 0; channel < 2; ++channel) {
            mix_.ChannelData(channel)[frame] = (delayed_dry_.ChannelData(channel)[frame] * dry_gain
                + equalized_outputs[channel][frame] * wet_gain) * output_gain;
        }
    }
    const float *mix_inputs[2]{mix_.ChannelData(0), mix_.ChannelData(1)};
    if (!limiter_.Process(mix_inputs, output, frames)) return false;
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

void IemPipeline::SetFreeze(bool freeze) noexcept {
    if (auto *granular = std::get_if<GranularEncoder>(&encoder_)) {
        granular->SetFreeze(freeze);
    }
}

void IemPipeline::ResetAngles() noexcept {
    rotator_.ResetAngles();
}

void IemPipeline::Reset() noexcept {
    if (IemEncoder *encoder = Encoder()) encoder->Reset();
    rotator_.Reset();
    decoder_.Reset();
    headphone_eq_.Reset();
    limiter_.Reset();
    ClearRuntimeBuffers();
    wet_smoother_.Reset(std::clamp(params_.wet, 0.0F, 1.0F));
    gain_smoother_.Reset(std::pow(10.0F,
        std::clamp(params_.output_gain_db, -24.0F, 24.0F) / 20.0F));
    if (error_ == IemResourceError::PROCESS_NONFINITE) error_ = IemResourceError::NONE;
}

uint32_t IemPipeline::ActiveGrainCount() const noexcept {
    if (const auto *granular = std::get_if<GranularEncoder>(&encoder_)) {
        return granular->ActiveGrainCount();
    }
    return 0;
}

uint64_t IemPipeline::GrainPoolExhaustionCount() const noexcept {
    if (const auto *granular = std::get_if<GranularEncoder>(&encoder_)) {
        return granular->PoolExhaustionCount();
    }
    return 0;
}

bool IemPipeline::IsFrozen() const noexcept {
    if (const auto *granular = std::get_if<GranularEncoder>(&encoder_)) {
        return granular->IsFrozen();
    }
    return false;
}

IemEncoder *IemPipeline::Encoder() noexcept {
    return std::visit([](auto &encoder) -> IemEncoder * {
        using Type = std::decay_t<decltype(encoder)>;
        if constexpr (std::is_same_v<Type, std::monostate>) return nullptr;
        else return &encoder;
    }, encoder_);
}

const IemEncoder *IemPipeline::Encoder() const noexcept {
    return std::visit([](const auto &encoder) -> const IemEncoder * {
        using Type = std::decay_t<decltype(encoder)>;
        if constexpr (std::is_same_v<Type, std::monostate>) return nullptr;
        else return &encoder;
    }, encoder_);
}

bool IemPipeline::PrepareEncoder(const IemParams &params, std::size_t max_frames) noexcept {
    const EncoderConfig config{96000, max_frames, params.order, 0x6D2B79F5U};
    switch (params.encoder_mode) {
    case EncoderMode::STEREO:
        encoder_.emplace<StereoEncoder>();
        break;
    case EncoderMode::MULTI:
        encoder_.emplace<MultiEncoder>();
        break;
    case EncoderMode::GRANULAR:
        encoder_.emplace<GranularEncoder>();
        break;
    }
    return Encoder() != nullptr && Encoder()->Prepare(config);
}

void IemPipeline::ClearRuntimeBuffers() noexcept {
    encoded_.Clear();
    rotated_.Clear();
    decoded_.Clear();
    equalized_.Clear();
    delayed_dry_.Clear();
    mix_.Clear();
    std::fill(dry_delay_.begin(), dry_delay_.end(), 0.0F);
    dry_delay_index_ = 0;
}

} // namespace iem
