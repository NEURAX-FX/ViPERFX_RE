#include "iem/HaloSurroundAssigner.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace iem {
namespace {

constexpr float kPi = 3.14159265358979323846F;
constexpr float kDeRampS = 0.5F;
constexpr float kRampSentinel = 1.0e8F;

float Thousandths(int32_t value) noexcept {
    return static_cast<float>(value) * 0.001F;
}

float Clamp01(float value) noexcept {
    return std::clamp(value, 0.0F, 1.0F);
}

float Mag2(float re, float im) noexcept {
    return re * re + im * im;
}

float PhaseCoherence(float l_re, float l_im, float r_re, float r_im, float energy) noexcept {
    if (energy < 1.0e-7F) return 1.0F;
    float phase_delta = std::fabs(std::atan2(l_im, l_re) - std::atan2(r_im, r_re));
    float p = 1.0F - (2.0F / kPi) * phase_delta;
    if (p < -1.0F) p = -2.0F - p;
    return p;
}

float DialogAlpha(float isolate, float aggress) noexcept {
    if (isolate == 0.0F) return 0.99F;
    return 1.0F - std::exp(std::log(0.01F) + aggress * (std::log(0.2F) - std::log(0.01F)));
}

float Ramped(float p, float fade, float ramp) noexcept {
    if (ramp == kRampSentinel) return fade > 0.0F ? 0.0F : 1.0F;
    return Clamp01(0.5F + ramp * (p - fade));
}

} // namespace

void HaloSurroundAssigner::AlphaAverager::Reset() noexcept {
    std::memset(history, 0, sizeof(history));
    filled = 0;
    write = 0;
}

float HaloSurroundAssigner::AlphaAverager::Push(float value) noexcept {
    history[write] = value;
    write = (write + 1U) % 8U;
    if (filled < 8U) ++filled;
    float sum = 0.0F;
    for (uint32_t index = 0; index < filled; ++index) sum += history[index];
    return sum / static_cast<float>(filled);
}

bool HaloSurroundAssigner::Prepare() noexcept {
    Reset();
    prepared_ = true;
    return true;
}

void HaloSurroundAssigner::ApplyParams(const HaloParams &params) noexcept {
    params_ = params;
}

void HaloSurroundAssigner::Reset() noexcept {
    std::memset(energy_prev_, 0, sizeof(energy_prev_));
    std::memset(transient_, 0, sizeof(transient_));
    std::memset(c_prev_, 0, sizeof(c_prev_));
    std::memset(f_prev_, 0, sizeof(f_prev_));
    std::memset(b_prev_, 0, sizeof(b_prev_));
    dialog_alpha_prev_ = 0.0F;
    alpha_.Reset();
}

void HaloSurroundAssigner::ProcessFrame(
    const HaloDialogFrame &in,
    float bed_re[7][HaloStft::kBins],
    float bed_im[7][HaloStft::kBins]
) noexcept {
    if (!prepared_) return;

    const float isolate = Thousandths(params_.dialog_isolate_thousandths);
    const float aggress = Thousandths(params_.dialog_aggress_thousandths);
    const float divergence = Thousandths(params_.divergence_thousandths);
    const float fade_fs = 2.0F * Thousandths(params_.fade_thousandths) - 1.0F;
    const float fade_sb = 2.0F * Thousandths(params_.fade_rears_thousandths) - 1.0F;
    const float raw_alpha = DialogAlpha(isolate, aggress);
    const float s = 0.5F * raw_alpha + 0.5F * dialog_alpha_prev_;
    dialog_alpha_prev_ = s;

    float ramp_fs = (kDeRampS == 0.0F) ? kRampSentinel : (0.5F / kDeRampS);
    float ramp_sb = 0.5F;
    if (fade_fs == -1.0F || fade_fs == 1.0F) ramp_fs = kRampSentinel;
    if (fade_sb == -1.0F || fade_sb == 1.0F) ramp_sb = kRampSentinel;

    for (uint32_t bin = 0; bin < HaloStft::kBins; ++bin) {
        const float l_re = in.residual_l_re[bin];
        const float l_im = in.residual_l_im[bin];
        const float r_re = in.residual_r_re[bin];
        const float r_im = in.residual_r_im[bin];
        const float energy = Mag2(l_re, l_im) + Mag2(r_re, r_im);
        const float energy_smooth = 0.5F * energy_prev_[bin] + 0.5F * energy;
        energy_prev_[bin] = energy_smooth;

        float t = std::max(transient_[bin] - 1.0F, 0.0F);
        if (energy > 100.0F * energy_smooth && energy > 0.001F) t = 15.0F;
        transient_[bin] = t;

        const float p = PhaseCoherence(l_re, l_im, r_re, r_im, energy);
        float alpha_in = s;
        if (t > 0.0F && t < 14.0F) alpha_in = std::min(0.9F, s);
        const float alpha = alpha_.Push(alpha_in);

        const float c_raw = Clamp01(2.0F * (1.0F - divergence) * (p - divergence));
        const float c = alpha * c_prev_[bin] + (1.0F - alpha) * c_raw;
        const float f = alpha * f_prev_[bin] + (1.0F - alpha) * Ramped(p, fade_fs, ramp_fs);
        const float b = alpha * b_prev_[bin] + (1.0F - alpha) * Ramped(p, fade_sb, ramp_sb);
        c_prev_[bin] = c;
        f_prev_[bin] = f;
        b_prev_[bin] = b;

        const float front_gain = f;
        const float rear_gain = (1.0F - f) * (params_.back_boost ? 1.0F : b);
        const float side_gain = (1.0F - f) - rear_gain;

        const float mid_re = 0.5F * (l_re + r_re);
        const float mid_im = 0.5F * (l_im + r_im);
        const float side_re = 0.5F * (l_re - r_re);
        const float side_im = 0.5F * (l_im - r_im);
        const float c_re = in.centre_re[bin] + c * mid_re;
        const float c_im = in.centre_im[bin] + c * mid_im;
        const float l_base_re = (1.0F - c) * mid_re + side_re;
        const float l_base_im = (1.0F - c) * mid_im + side_im;
        const float r_base_re = (1.0F - c) * mid_re - side_re;
        const float r_base_im = (1.0F - c) * mid_im - side_im;

        bed_re[static_cast<uint32_t>(HaloBedChannel::L)][bin] = front_gain * l_base_re;
        bed_im[static_cast<uint32_t>(HaloBedChannel::L)][bin] = front_gain * l_base_im;
        bed_re[static_cast<uint32_t>(HaloBedChannel::R)][bin] = front_gain * r_base_re;
        bed_im[static_cast<uint32_t>(HaloBedChannel::R)][bin] = front_gain * r_base_im;
        bed_re[static_cast<uint32_t>(HaloBedChannel::C)][bin] = c_re;
        bed_im[static_cast<uint32_t>(HaloBedChannel::C)][bin] = c_im;
        bed_re[static_cast<uint32_t>(HaloBedChannel::Ls)][bin] = side_gain * l_base_re;
        bed_im[static_cast<uint32_t>(HaloBedChannel::Ls)][bin] = side_gain * l_base_im;
        bed_re[static_cast<uint32_t>(HaloBedChannel::Rs)][bin] = side_gain * r_base_re;
        bed_im[static_cast<uint32_t>(HaloBedChannel::Rs)][bin] = side_gain * r_base_im;
        bed_re[static_cast<uint32_t>(HaloBedChannel::Lsr)][bin] = rear_gain * l_base_re;
        bed_im[static_cast<uint32_t>(HaloBedChannel::Lsr)][bin] = rear_gain * l_base_im;
        bed_re[static_cast<uint32_t>(HaloBedChannel::Rsr)][bin] = rear_gain * r_base_re;
        bed_im[static_cast<uint32_t>(HaloBedChannel::Rsr)][bin] = rear_gain * r_base_im;
    }
}

} // namespace iem
