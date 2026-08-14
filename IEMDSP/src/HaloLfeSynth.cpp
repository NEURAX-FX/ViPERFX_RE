#include "iem/HaloLfeSynth.h"

#include <algorithm>
#include <cmath>

namespace iem {
namespace {

constexpr float kPi = 3.14159265358979323846F;

float Millionths(int32_t value) noexcept {
    return static_cast<float>(std::clamp(value, 0, 1000000)) / 1000000.0F;
}

} // namespace

float HaloLfeCutoffHz(int32_t normalized_millionths) noexcept {
    const float normalized = Millionths(normalized_millionths);
    return std::exp(std::log(10.0F)
        + normalized * (std::log(200.0F) - std::log(10.0F)));
}

float HaloLfeGainDb(int32_t normalized_millionths) noexcept {
    return 55.0F * Millionths(normalized_millionths) - 45.0F;
}

float HaloLfeGainLinear(int32_t normalized_millionths) noexcept {
    return std::pow(10.0F, HaloLfeGainDb(normalized_millionths) / 20.0F);
}

HaloLfeCoefficients MakeHaloLfeLowPass(
    uint32_t sample_rate,
    int32_t normalized_millionths
) noexcept {
    if (sample_rate == 0) return {};
    const float cutoff_hz = HaloLfeCutoffHz(normalized_millionths);
    const float w0 = 2.0F * kPi * cutoff_hz / static_cast<float>(sample_rate);
    const float alpha = std::sin(w0) / 2.0F;
    const float cosine = std::cos(w0);
    const float denominator = 1.0F + alpha;
    const float b0 = (1.0F - cosine) / (2.0F * denominator);
    return {
        b0,
        (1.0F - cosine) / denominator,
        b0,
        2.0F * cosine / denominator,
        -(1.0F - alpha) / denominator,
    };
}

float HaloLfeSynth::BiquadState::Process(
    float input,
    const HaloLfeCoefficients &coefficients
) noexcept {
    const float output = coefficients.b0 * input
        + coefficients.b1 * x1
        + coefficients.b2 * x2
        + coefficients.fb1 * y1
        + coefficients.fb2 * y2;
    x2 = x1;
    x1 = input;
    y2 = y1;
    y1 = output;
    return output;
}

void HaloLfeSynth::BiquadState::Reset() noexcept {
    x1 = 0.0F;
    x2 = 0.0F;
    y1 = 0.0F;
    y2 = 0.0F;
}

bool HaloLfeSynth::Prepare(uint32_t sample_rate) noexcept {
    if (sample_rate != 96000U) return false;
    prepared_ = true;
    coefficients_ = HaloLfeParams{}.coefficients_96k;
    gain_linear_ = HaloLfeParams{}.gain_linear;
    split_ = 0.0F;
    enable_mix_ = 1.0F;
    enable_target_ = 1.0F;
    started_ = false;
    Reset();
    return true;
}

void HaloLfeSynth::ApplyParams(const HaloLfeParams &params) noexcept {
    coefficients_ = params.coefficients_96k;
    gain_linear_ = params.gain_linear;
    split_ = Millionths(params.split_millionths);
    enable_target_ = params.enabled ? 1.0F : 0.0F;
    if (!started_) enable_mix_ = enable_target_;
}

void HaloLfeSynth::Reset() noexcept {
    left_filter_.Reset();
    right_filter_.Reset();
    centre_filter_.Reset();
    enable_mix_ = enable_target_;
    started_ = false;
}

void HaloLfeSynth::Process(
    float *left,
    float *right,
    float *centre,
    float *lfe,
    std::size_t frames
) noexcept {
    if (!prepared_ || left == nullptr || right == nullptr || centre == nullptr
        || lfe == nullptr || frames == 0) return;

    started_ = true;
    if (enable_mix_ == 0.0F && enable_target_ == 0.0F) {
        std::fill_n(lfe, frames, 0.0F);
        return;
    }

    const float enable_step = (enable_target_ - enable_mix_)
        / static_cast<float>(frames);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const float active = enable_mix_;
        const float filtered_left = left_filter_.Process(left[frame], coefficients_);
        const float filtered_right = right_filter_.Process(right[frame], coefficients_);
        const float filtered_centre = centre_filter_.Process(centre[frame], coefficients_);
        const float active_split = split_ * active;
        lfe[frame] = (filtered_left + filtered_right + filtered_centre)
            * gain_linear_ * active;
        left[frame] -= filtered_left * active_split;
        right[frame] -= filtered_right * active_split;
        centre[frame] -= filtered_centre * active_split;
        enable_mix_ += enable_step;
    }
    enable_mix_ = enable_target_;
    if (enable_mix_ == 0.0F) {
        left_filter_.Reset();
        right_filter_.Reset();
        centre_filter_.Reset();
    }
}

} // namespace iem
