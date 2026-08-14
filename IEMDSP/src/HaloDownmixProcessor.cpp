#include "iem/HaloDownmixProcessor.h"

#include <algorithm>
#include <cmath>

namespace iem {
namespace {

constexpr float kInverseSqrtTwo = 0.70710678118654752440F;

uint32_t RoleIndex(HaloDownmixRole role) noexcept {
    return static_cast<uint32_t>(role);
}

void ProcessMidSide(float &left, float &right, float mid_gain, float side_gain) noexcept {
    const float mid = (left + right) * kInverseSqrtTwo * mid_gain;
    const float side = (left - right) * kInverseSqrtTwo * side_gain;
    left = (mid + side) * kInverseSqrtTwo;
    right = (mid - side) * kInverseSqrtTwo;
}

} // namespace

float HaloDownmixProcessor::BiquadState::Process(
    float input,
    const HaloDownmixBiquadCoefficients &coefficients
) noexcept {
    const float output = coefficients.b0 * input
        + coefficients.b1 * x1
        + coefficients.b2 * x2
        - coefficients.a1 * y1
        - coefficients.a2 * y2;
    x2 = x1;
    x1 = input;
    y2 = y1;
    y1 = output;
    return output;
}

void HaloDownmixProcessor::BiquadState::Reset() noexcept {
    x1 = 0.0F;
    x2 = 0.0F;
    y1 = 0.0F;
    y2 = 0.0F;
}

void HaloDownmixProcessor::Ramp::Set(
    float value,
    uint32_t frames,
    bool immediate
) noexcept {
    target = value;
    if (immediate || frames == 0U) {
        current = target;
        step = 0.0F;
        remaining = 0U;
        return;
    }
    if (target == current) return;
    remaining = frames;
    step = (target - current) / static_cast<float>(frames);
}

float HaloDownmixProcessor::Ramp::Next() noexcept {
    if (remaining != 0U) {
        current += step;
        --remaining;
        if (remaining == 0U) current = target;
    }
    return current;
}

void HaloDownmixProcessor::Ramp::Snap() noexcept {
    current = target;
    step = 0.0F;
    remaining = 0U;
}

void HaloDownmixProcessor::DelayState::Set(uint32_t frames, bool immediate) noexcept {
    target = frames;
    if (immediate || target == current) {
        current = target;
        mix = 1.0F;
        active = false;
        return;
    }
    mix = 0.0F;
    active = true;
}

void HaloDownmixProcessor::DelayState::Reset() noexcept {
    current = target;
    mix = 1.0F;
    active = false;
}

bool HaloDownmixProcessor::Prepare(std::size_t max_frames) noexcept {
    if (max_frames == 0U) return false;
    max_frames_ = max_frames;
    delay_ring_.assign(static_cast<std::size_t>(kRoleCount) * kLatencyFrames, 0.0F);
    prepared_ = true;
    params_ = HaloDownmixParams{};
    ApplyParams(params_);
    Reset();
    return true;
}

void HaloDownmixProcessor::ApplyParams(const HaloDownmixParams &params) noexcept {
    const bool immediate = !started_;
    if (params_.side_shelf_enable && !params.side_shelf_enable) {
        side_shelf_[0].Reset();
        side_shelf_[1].Reset();
    }
    if (params_.rear_shelf_enable && !params.rear_shelf_enable) {
        rear_shelf_[0].Reset();
        rear_shelf_[1].Reset();
    }
    if (params_.lfe_lpf_enable && !params.lfe_lpf_enable) lfe_low_pass_.Reset();
    if (params_.output_hpf_enable && !params.output_hpf_enable) {
        output_high_pass_[0].Reset();
        output_high_pass_[1].Reset();
    }
    params_ = params;

    delays_[RoleIndex(HaloDownmixRole::L)].Set(kLatencyFrames, immediate);
    delays_[RoleIndex(HaloDownmixRole::R)].Set(kLatencyFrames, immediate);
    delays_[RoleIndex(HaloDownmixRole::C)].Set(kLatencyFrames, immediate);
    delays_[RoleIndex(HaloDownmixRole::LFE)].Set(kLatencyFrames, immediate);
    delays_[RoleIndex(HaloDownmixRole::Ls)].Set(
        DelayTarget(params_.ls_delay_us, params_.delay_enable), immediate);
    delays_[RoleIndex(HaloDownmixRole::Rs)].Set(
        DelayTarget(params_.rs_delay_us, params_.delay_enable), immediate);
    delays_[RoleIndex(HaloDownmixRole::Lsr)].Set(
        DelayTarget(params_.lsr_delay_us, params_.delay_enable), immediate);
    delays_[RoleIndex(HaloDownmixRole::Rsr)].Set(
        DelayTarget(params_.rsr_delay_us, params_.delay_enable), immediate);
    SetRampTargets(immediate);
}

void HaloDownmixProcessor::Reset() noexcept {
    std::fill(delay_ring_.begin(), delay_ring_.end(), 0.0F);
    write_index_ = 0U;
    for (auto &delay : delays_) delay.Reset();
    ClearFilters();
    divergence_.Snap();
    front_mid_.Snap();
    front_side_.Snap();
    center_.Snap();
    surround_mid_.Snap();
    surround_side_.Snap();
    rear_mid_.Snap();
    rear_side_.Snap();
    lfe_.Snap();
    output_left_.Snap();
    output_right_.Snap();
    started_ = false;
}

bool HaloDownmixProcessor::Process(
    const float *const inputs[kRoleCount],
    float *const outputs[2],
    std::size_t frames
) noexcept {
    if (!prepared_ || inputs == nullptr || outputs == nullptr
        || outputs[0] == nullptr || outputs[1] == nullptr
        || frames > max_frames_) return false;
    for (uint32_t role = 0; role < kRoleCount; ++role) {
        if (inputs[role] == nullptr) return false;
    }

    started_ = true;
    const auto &balance = params_.derived.balance;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        float role[kRoleCount]{};
        for (uint32_t index = 0; index < kRoleCount; ++index) {
            role[index] = ProcessDelayed(index, inputs[index][frame]);
        }
        write_index_ = (write_index_ + 1U) % kLatencyFrames;

        if (params_.side_shelf_enable) {
            role[RoleIndex(HaloDownmixRole::Ls)] = side_shelf_[0].Process(
                role[RoleIndex(HaloDownmixRole::Ls)], params_.derived.side_shelf);
            role[RoleIndex(HaloDownmixRole::Rs)] = side_shelf_[1].Process(
                role[RoleIndex(HaloDownmixRole::Rs)], params_.derived.side_shelf);
        }
        if (params_.rear_shelf_enable) {
            role[RoleIndex(HaloDownmixRole::Lsr)] = rear_shelf_[0].Process(
                role[RoleIndex(HaloDownmixRole::Lsr)], params_.derived.rear_shelf);
            role[RoleIndex(HaloDownmixRole::Rsr)] = rear_shelf_[1].Process(
                role[RoleIndex(HaloDownmixRole::Rsr)], params_.derived.rear_shelf);
        }

        auto balance_pair = [&](HaloDownmixRole left_role, HaloDownmixRole right_role) {
            const uint32_t left_index = RoleIndex(left_role);
            const uint32_t right_index = RoleIndex(right_role);
            const float left = role[left_index];
            const float right = role[right_index];
            role[left_index] = balance.left_from_left * left
                + balance.left_from_right * right;
            role[right_index] = balance.right_from_left * left
                + balance.right_from_right * right;
        };
        balance_pair(HaloDownmixRole::L, HaloDownmixRole::R);
        balance_pair(HaloDownmixRole::Ls, HaloDownmixRole::Rs);
        balance_pair(HaloDownmixRole::Lsr, HaloDownmixRole::Rsr);

        const float divergence = divergence_.Next();
        const float center_input = role[RoleIndex(HaloDownmixRole::C)];
        role[RoleIndex(HaloDownmixRole::L)] += center_input * divergence * 0.5F;
        role[RoleIndex(HaloDownmixRole::R)] += center_input * divergence * 0.5F;
        role[RoleIndex(HaloDownmixRole::C)] = center_input * (1.0F - divergence);

        if (params_.lfe_lpf_enable) {
            role[RoleIndex(HaloDownmixRole::LFE)] = lfe_low_pass_.Process(
                role[RoleIndex(HaloDownmixRole::LFE)],
                params_.derived.lfe_low_pass);
        }

        ProcessMidSide(
            role[RoleIndex(HaloDownmixRole::L)],
            role[RoleIndex(HaloDownmixRole::R)],
            front_mid_.Next(), front_side_.Next());
        ProcessMidSide(
            role[RoleIndex(HaloDownmixRole::Ls)],
            role[RoleIndex(HaloDownmixRole::Rs)],
            surround_mid_.Next(), surround_side_.Next());
        ProcessMidSide(
            role[RoleIndex(HaloDownmixRole::Lsr)],
            role[RoleIndex(HaloDownmixRole::Rsr)],
            rear_mid_.Next(), rear_side_.Next());
        role[RoleIndex(HaloDownmixRole::C)] *= center_.Next();
        role[RoleIndex(HaloDownmixRole::LFE)] *= lfe_.Next();

        const float shared_scale = params_.scale_input_by_output_count ? 0.5F : 1.0F;
        const float shared = (role[RoleIndex(HaloDownmixRole::C)]
            + role[RoleIndex(HaloDownmixRole::LFE)]) * shared_scale;
        float left = role[RoleIndex(HaloDownmixRole::L)]
            + role[RoleIndex(HaloDownmixRole::Ls)]
            + role[RoleIndex(HaloDownmixRole::Lsr)] + shared;
        float right = role[RoleIndex(HaloDownmixRole::R)]
            + role[RoleIndex(HaloDownmixRole::Rs)]
            + role[RoleIndex(HaloDownmixRole::Rsr)] + shared;

        if (params_.output_hpf_enable) {
            left = output_high_pass_[0].Process(left,
                params_.derived.output_high_pass);
            right = output_high_pass_[1].Process(right,
                params_.derived.output_high_pass);
        }
        outputs[0][frame] = left * output_left_.Next();
        outputs[1][frame] = right * output_right_.Next();
    }
    return true;
}

std::size_t HaloDownmixProcessor::PreparedBytes() const noexcept {
    return delay_ring_.capacity() * sizeof(float);
}

uint32_t HaloDownmixProcessor::DelayTarget(
    int32_t relative_delay_us,
    bool relative_enabled
) const noexcept {
    if (!relative_enabled) return kLatencyFrames;
    const uint32_t relative_frames = static_cast<uint32_t>(
        std::clamp(relative_delay_us, 0, 32000)) * 96U / 1000U;
    return kLatencyFrames - std::min(relative_frames, kLatencyFrames);
}

float HaloDownmixProcessor::ReadDelayed(
    uint32_t role,
    float input,
    uint32_t delay_frames
) const noexcept {
    if (delay_frames == 0U) return input;
    const uint32_t read_index = (write_index_ + kLatencyFrames - delay_frames)
        % kLatencyFrames;
    return delay_ring_[static_cast<std::size_t>(role) * kLatencyFrames + read_index];
}

float HaloDownmixProcessor::ProcessDelayed(uint32_t role, float input) noexcept {
    DelayState &delay = delays_[role];
    const float old_value = ReadDelayed(role, input, delay.current);
    float output = old_value;
    if (delay.active) {
        const float new_value = ReadDelayed(role, input, delay.target);
        output = old_value + (new_value - old_value) * delay.mix;
        delay.mix = std::min(1.0F, delay.mix + kDelayCrossfadeStep);
        if (delay.mix >= 1.0F) {
            delay.current = delay.target;
            delay.active = false;
        }
    }
    delay_ring_[static_cast<std::size_t>(role) * kLatencyFrames + write_index_] = input;
    return output;
}

void HaloDownmixProcessor::SetRampTargets(bool immediate) noexcept {
    divergence_.Set(static_cast<float>(params_.center_divergence_millionths)
        / 1000000.0F, kDivergenceRampFrames, immediate);
    front_mid_.Set(params_.derived.front_mid_gain, kGainRampFrames, immediate);
    front_side_.Set(params_.derived.front_side_gain, kGainRampFrames, immediate);
    center_.Set(params_.derived.center_gain, kGainRampFrames, immediate);
    surround_mid_.Set(params_.derived.surround_mid_gain, kGainRampFrames, immediate);
    surround_side_.Set(params_.derived.surround_side_gain, kGainRampFrames, immediate);
    rear_mid_.Set(params_.derived.rear_mid_gain, kGainRampFrames, immediate);
    rear_side_.Set(params_.derived.rear_side_gain, kGainRampFrames, immediate);
    lfe_.Set(params_.derived.lfe_gain, kGainRampFrames, immediate);
    output_left_.Set(params_.derived.output_left_gain, kGainRampFrames, immediate);
    output_right_.Set(params_.derived.output_right_gain, kGainRampFrames, immediate);
}

void HaloDownmixProcessor::ClearFilters() noexcept {
    for (auto &filter : side_shelf_) filter.Reset();
    for (auto &filter : rear_shelf_) filter.Reset();
    lfe_low_pass_.Reset();
    for (auto &filter : output_high_pass_) filter.Reset();
}

} // namespace iem
