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
    if (params.render_mode != RenderMode::OFF && !rotator_.Prepare(encoder_config)) return false;
    if (params.render_mode != RenderMode::KU100 && !simple_decoder_.Prepare(params.order)) {
        return false;
    }
    if (params.render_mode == RenderMode::KU100) {
        if (!decoder_.Prepare(params.order, profile_config_.partition_frames)) {
            error_ = decoder_.Error();
            return false;
        }
        if (!headphone_eq_.Prepare(static_cast<int32_t>(params.decoder.headphone_eq),
                profile_config_.partition_frames)) {
            error_ = headphone_eq_.Error();
            return false;
        }
    }
    if (!limiter_.Prepare(96000)
        || !encoded_.Prepare(kMaxAmbisonicsChannels, max_frames)
        || !halo_bed_.Prepare(kHaloBedChannels, max_frames)
        || !halo_lfe_.Prepare(1, max_frames)
        || !rotated_.Prepare(kMaxAmbisonicsChannels, max_frames)
        || !decoded_.Prepare(2, max_frames)
        || !equalized_.Prepare(2, max_frames)
        || !delayed_dry_.Prepare(2, max_frames)
        || !mix_.Prepare(2, max_frames)) {
        return false;
    }
    wet_latency_frames_ = Halo() != nullptr ? Halo()->StftLatencyFrames() : 0U;
    if (params.render_mode == RenderMode::KU100) {
        wet_latency_frames_ += decoder_.LatencyFrames() + headphone_eq_.LatencyFrames();
    }
    total_latency_frames_ = wet_latency_frames_ + limiter_.LatencyFrames();
    if (total_latency_frames_ > profile_config_.maximum_latency_ms * 96U) return false;
    dry_delay_.assign(static_cast<std::size_t>(2U) * wet_latency_frames_, 0.0F);
    dry_delay_index_ = 0;
    lfe_delay_frames_ = Halo() != nullptr && params.render_mode == RenderMode::KU100
        ? decoder_.LatencyFrames() : 0U;
    lfe_delay_.assign(std::max(1U, lfe_delay_frames_), 0.0F);
    lfe_delay_index_ = 0;
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
    params_.halo = params.halo;
    params_.rotation = params.rotation;
    if (IemEncoder *encoder = Encoder()) encoder->ApplyParams(params_);
    if (HaloEncoder *halo = Halo()) halo->ApplyParams(params_);
    if (params_.render_mode != RenderMode::OFF) rotator_.ApplyParams(params_);
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
        || output[1] == nullptr || (Encoder() == nullptr && Halo() == nullptr)) return false;
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
    float *rendered_outputs[2]{decoded_.ChannelData(0), decoded_.ChannelData(1)};
    float *wet_outputs[2]{rendered_outputs[0], rendered_outputs[1]};
    const float *halo_lfe = nullptr;
    if (HaloEncoder *halo = Halo()) {
        std::array<float *, kHaloBedChannels> bed_pointers{};
        std::array<const float *, kHaloBedChannels> bed_inputs{};
        HaloBedView bed_view{};
        for (uint32_t channel = 0; channel < kHaloBedChannels; ++channel) {
            bed_pointers[channel] = halo_bed_.ChannelData(channel);
            bed_inputs[channel] = bed_pointers[channel];
            bed_view.directional[channel] = bed_pointers[channel];
        }
        bed_view.lfe = halo_lfe_.ChannelData(0);
        halo_lfe = bed_view.lfe;
        if (!halo->ProcessBed(stereo, bed_view, frames)) return false;
        if (params_.render_mode == RenderMode::OFF) {
            FoldHaloBedToStereo(bed_inputs.data(), rendered_outputs, frames);
        } else {
            EncodeHaloBedToSn3d(params_.order, bed_inputs.data(), encoded_pointers.data(), frames);
        }
    } else if (!Encoder()->Process(stereo, encoded_pointers.data(), frames)) {
        return false;
    }

    if (params_.render_mode != RenderMode::OFF) {
        if (!rotator_.Process(encoded_inputs.data(), rotated_pointers.data(), frames)) return false;
    }
    if (params_.render_mode == RenderMode::SIMPLE) {
        if (!simple_decoder_.Process(rotated_inputs.data(), rendered_outputs, frames)) return false;
    } else if (params_.render_mode == RenderMode::OFF && Halo() == nullptr) {
        if (!simple_decoder_.Process(encoded_inputs.data(), rendered_outputs, frames)) return false;
    } else if (params_.render_mode == RenderMode::KU100) {
        if (!decoder_.Process(rotated_inputs.data(), rendered_outputs, frames)) {
            error_ = decoder_.Error();
            return false;
        }
    }

    if (halo_lfe != nullptr) {
        MixDelayedLfe(rendered_outputs[0], rendered_outputs[1], halo_lfe, frames);
    }

    if (params_.render_mode == RenderMode::KU100) {
        const float *decoded_inputs[2]{rendered_outputs[0], rendered_outputs[1]};
        float *equalized_outputs[2]{equalized_.ChannelData(0), equalized_.ChannelData(1)};
        if (!headphone_eq_.Process(decoded_inputs, equalized_outputs, frames)) {
            error_ = headphone_eq_.Error();
            return false;
        }
        wet_outputs[0] = equalized_outputs[0];
        wet_outputs[1] = equalized_outputs[1];
    }

    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (uint32_t channel = 0; channel < 2; ++channel) {
            if (wet_latency_frames_ == 0) {
                delayed_dry_.ChannelData(channel)[frame] = stereo[channel][frame];
            } else {
                float *ring = dry_delay_.data()
                    + static_cast<std::size_t>(channel) * wet_latency_frames_;
                delayed_dry_.ChannelData(channel)[frame] = ring[dry_delay_index_];
                ring[dry_delay_index_] = stereo[channel][frame];
            }
        }
        if (wet_latency_frames_ != 0) {
            dry_delay_index_ = (dry_delay_index_ + 1U) % wet_latency_frames_;
        }
        const float wet = wet_smoother_.Next();
        const float dry_gain = std::cos(wet * kHalfPi);
        const float wet_gain = std::sin(wet * kHalfPi);
        const float output_gain = gain_smoother_.Next();
        for (uint32_t channel = 0; channel < 2; ++channel) {
            mix_.ChannelData(channel)[frame] = (delayed_dry_.ChannelData(channel)[frame] * dry_gain
                + wet_outputs[channel][frame] * wet_gain) * output_gain;
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
    if (HaloEncoder *halo = Halo()) halo->Reset();
    if (params_.render_mode != RenderMode::OFF) rotator_.Reset();
    if (params_.render_mode == RenderMode::KU100) {
        decoder_.Reset();
        headphone_eq_.Reset();
    }
    limiter_.Reset();
    ClearRuntimeBuffers();
    wet_smoother_.Reset(std::clamp(params_.wet, 0.0F, 1.0F));
    gain_smoother_.Reset(std::pow(10.0F,
        std::clamp(params_.output_gain_db, -24.0F, 24.0F) / 20.0F));
    if (error_ == IemResourceError::PROCESS_NONFINITE) error_ = IemResourceError::NONE;
}

void IemPipeline::MixDelayedLfe(
    float *left,
    float *right,
    const float *lfe,
    std::size_t frames
) noexcept {
    for (std::size_t frame = 0; frame < frames; ++frame) {
        float aligned = lfe[frame];
        if (lfe_delay_frames_ != 0U) {
            aligned = lfe_delay_[lfe_delay_index_];
            lfe_delay_[lfe_delay_index_] = lfe[frame];
            lfe_delay_index_ = (lfe_delay_index_ + 1U) % lfe_delay_frames_;
        }
        left[frame] += aligned;
        right[frame] += aligned;
    }
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
        if constexpr (std::is_same_v<Type, StereoEncoder>
            || std::is_same_v<Type, MultiEncoder>
            || std::is_same_v<Type, GranularEncoder>) return &encoder;
        else return nullptr;
    }, encoder_);
}

const IemEncoder *IemPipeline::Encoder() const noexcept {
    return std::visit([](const auto &encoder) -> const IemEncoder * {
        using Type = std::decay_t<decltype(encoder)>;
        if constexpr (std::is_same_v<Type, StereoEncoder>
            || std::is_same_v<Type, MultiEncoder>
            || std::is_same_v<Type, GranularEncoder>) return &encoder;
        else return nullptr;
    }, encoder_);
}

HaloEncoder *IemPipeline::Halo() noexcept {
    return std::get_if<HaloEncoder>(&encoder_);
}

const HaloEncoder *IemPipeline::Halo() const noexcept {
    return std::get_if<HaloEncoder>(&encoder_);
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
    case EncoderMode::HALO:
        encoder_.emplace<HaloEncoder>();
        return Halo()->Prepare(config);
    }
    return Encoder() != nullptr && Encoder()->Prepare(config);
}

void IemPipeline::ClearRuntimeBuffers() noexcept {
    encoded_.Clear();
    halo_bed_.Clear();
    halo_lfe_.Clear();
    rotated_.Clear();
    decoded_.Clear();
    equalized_.Clear();
    delayed_dry_.Clear();
    mix_.Clear();
    std::fill(dry_delay_.begin(), dry_delay_.end(), 0.0F);
    std::fill(lfe_delay_.begin(), lfe_delay_.end(), 0.0F);
    dry_delay_index_ = 0;
    lfe_delay_index_ = 0;
}

} // namespace iem
