#include "iem/HaloDialogExtractor.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace iem {
namespace {

constexpr float kLn10 = 2.302585092994046F;
constexpr float kPowerDbScale = kLn10 / 20.0F;
constexpr float kFloorLinear = 0.0316227766F;
constexpr float kCentreScale = 0.7071067811865476F;
constexpr int kNeighborOffsets[] = {-3, -2, -1, 1, 2, 3};

float PowerDb(float power) noexcept {
    if (power < 1.0e-10F) return -1.0e10F;
    return std::log(power) / kPowerDbScale;
}

float Thousandths(int32_t value) noexcept {
    return static_cast<float>(value) * 0.001F;
}

float Clamp01(float value) noexcept {
    return std::clamp(value, 0.0F, 1.0F);
}

float Mag2(float re, float im) noexcept {
    return re * re + im * im;
}

} // namespace

void ComputeHaloDialogAd(
    const float left_re[HaloStft::kBins],
    const float left_im[HaloStft::kBins],
    const float right_re[HaloStft::kBins],
    const float right_im[HaloStft::kBins],
    HaloDialogFeatureFrame &out
) noexcept {
    float pmax_sum = 0.0F;
    float pdif_sum = 0.0F;
    float a_min = 0.0F;
    float a_max = 0.0F;
    for (uint32_t bin = 0; bin < HaloStft::kBins; ++bin) {
        const float p_left = Mag2(left_re[bin], left_im[bin]);
        const float p_right = Mag2(right_re[bin], right_im[bin]);
        const float p_max = std::max(p_left, p_right);
        const float dif_re = right_re[bin] - left_re[bin];
        const float dif_im = right_im[bin] - left_im[bin];
        const float p_dif = Mag2(dif_re, dif_im);
        out.a[bin] = PowerDb(p_max);
        out.d[bin] = PowerDb(p_dif);
        pmax_sum += p_max;
        pdif_sum += p_dif;
        if (bin == 0 || out.a[bin] < a_min) a_min = out.a[bin];
        if (bin == 0 || out.a[bin] > a_max) a_max = out.a[bin];
    }
    out.a[513] = PowerDb(pmax_sum / static_cast<float>(HaloStft::kBins));
    out.d[513] = PowerDb(pdif_sum / static_cast<float>(HaloStft::kBins));
    out.a[514] = a_min;
    out.a[515] = a_max;
    out.d[514] = 0.0F;
    out.d[515] = 0.0F;
}

void BuildHaloDialogFeatures(
    const HaloDialogFeatureFrame slots[HaloStft::kHistoryFrames],
    uint32_t bin,
    float features[kDialogNetInputs]
) noexcept {
    float lo = slots[0].a[514];
    float hi = slots[0].a[515];
    for (uint32_t slot = 1; slot < HaloStft::kHistoryFrames; ++slot) {
        lo = std::min(lo, slots[slot].a[514]);
        hi = std::max(hi, slots[slot].a[515]);
    }
    const float span = hi - lo;
    const auto norm = [lo, span](float value) {
        return span == 0.0F ? 0.0F : (value - lo) / span;
    };

    uint32_t cursor = 0;
    for (uint32_t slot = 0; slot < HaloStft::kHistoryFrames; ++slot) {
        features[cursor++] = norm(slots[slot].a[bin]);
        features[cursor++] = norm(slots[slot].d[bin]);
    }
    const HaloDialogFeatureFrame &target = slots[HaloStft::kHistoryFrames - 2U];
    for (int offset : kNeighborOffsets) {
        const int neighbor = static_cast<int>(bin) + offset;
        if (neighbor < 0 || neighbor >= static_cast<int>(HaloStft::kBins)) {
            features[cursor++] = 0.0F;
            features[cursor++] = 0.0F;
            continue;
        }
        features[cursor++] = norm(target.a[neighbor]);
        features[cursor++] = norm(target.d[neighbor]);
    }
    for (uint32_t slot = 0; slot < HaloStft::kHistoryFrames; ++slot) {
        features[cursor++] = norm(slots[slot].a[513]);
        features[cursor++] = norm(slots[slot].d[513]);
    }

    if (bin == 0) {
        features[36] = 0.0F;
        return;
    }
    const float hz = static_cast<float>(bin) * 48000.0F / static_cast<float>(HaloStft::kFftSize);
    features[36] = Clamp01((std::log(hz) - std::log(10.0F)) / (std::log(24000.0F) - std::log(10.0F)));
}

bool HaloDialogExtractor::Prepare() noexcept {
    Reset();
    prepared_ = net_.Prepare();
    return prepared_;
}

void HaloDialogExtractor::ApplyParams(const HaloParams &params) noexcept {
    params_ = params;
}

void HaloDialogExtractor::Reset() noexcept {
    for (auto &slot : slots_) slot = HaloDialogFeatureFrame{};
    for (uint32_t index = 0; index < HaloStft::kHistoryFrames; ++index) {
        map_[index] = index;
    }
    std::memset(envelope_, 0, sizeof(envelope_));
    std::memset(left_history_re_, 0, sizeof(left_history_re_));
    std::memset(left_history_im_, 0, sizeof(left_history_im_));
    std::memset(right_history_re_, 0, sizeof(right_history_re_));
    std::memset(right_history_im_, 0, sizeof(right_history_im_));
    have_target_ = false;
}

void HaloDialogExtractor::RotateHistory() noexcept {
    const uint32_t first = map_[0];
    for (uint32_t index = 0; index + 1U < HaloStft::kHistoryFrames; ++index) {
        map_[index] = map_[index + 1U];
    }
    map_[HaloStft::kHistoryFrames - 1U] = first;
}

void HaloDialogExtractor::ProcessFrame(
    const float left_re[HaloStft::kBins],
    const float left_im[HaloStft::kBins],
    const float right_re[HaloStft::kBins],
    const float right_im[HaloStft::kBins],
    HaloDialogFrame &out
) noexcept {
    if (!prepared_) return;

    const uint32_t newest = map_[HaloStft::kHistoryFrames - 1U];
    ComputeHaloDialogAd(left_re, left_im, right_re, right_im, slots_[newest]);

    HaloDialogFeatureFrame ordered[HaloStft::kHistoryFrames];
    for (uint32_t index = 0; index < HaloStft::kHistoryFrames; ++index) {
        ordered[index] = slots_[map_[index]];
    }

    const float isolate = Thousandths(params_.dialog_isolate_thousandths);
    const float aggress = Thousandths(params_.dialog_aggress_thousandths);
    const float isolate_prime = isolate * std::min(2.0F * aggress, 1.0F);
    const float attack = Thousandths(params_.dialog_attack_thousandths);
    const float release = Thousandths(params_.dialog_release_thousandths);
    const float mix_in = Thousandths(params_.dialog_mix_in_thousandths);

    if (have_target_) {
        float mask_prev = 0.0F;
        for (uint32_t bin = 0; bin < HaloStft::kBins; ++bin) {
            float features[kDialogNetInputs]{};
            BuildHaloDialogFeatures(ordered, bin, features);
            const float q = net_.Infer(features);
            float d = 0.0F;
            if (q > kFloorLinear) {
                d = 2.0F * (q - kFloorLinear);
                if (d > envelope_[bin]) {
                    d = std::min(d, (1.0F - attack) * envelope_[bin] + attack * d);
                }
            }
            if (envelope_[bin] > d) {
                d = std::max(d, release * envelope_[bin]);
            }
            envelope_[bin] = d;
            const float mask = Clamp01(0.5F * (mix_in + envelope_[bin]) + 0.5F * mask_prev);
            mask_prev = mask;
            const float gain = isolate_prime * mask;
            const float mid_re = left_history_re_[bin] + right_history_re_[bin];
            const float mid_im = left_history_im_[bin] + right_history_im_[bin];
            out.residual_l_re[bin] = left_history_re_[bin] - 0.5F * gain * mid_re;
            out.residual_l_im[bin] = left_history_im_[bin] - 0.5F * gain * mid_im;
            out.residual_r_re[bin] = right_history_re_[bin] - 0.5F * gain * mid_re;
            out.residual_r_im[bin] = right_history_im_[bin] - 0.5F * gain * mid_im;
            out.centre_re[bin] = (0.5F / kCentreScale) * gain * mid_re;
            out.centre_im[bin] = (0.5F / kCentreScale) * gain * mid_im;
        }
    } else {
        // HaloDialogFrame has default member initializers, so it is not trivially
        // copyable and memset on it is ill-formed (-Werror=class-memaccess).
        // Value-initialization zeroes every float array member identically.
        out = HaloDialogFrame{};
    }

    std::copy(left_re, left_re + HaloStft::kBins, left_history_re_);
    std::copy(left_im, left_im + HaloStft::kBins, left_history_im_);
    std::copy(right_re, right_re + HaloStft::kBins, right_history_re_);
    std::copy(right_im, right_im + HaloStft::kBins, right_history_im_);
    have_target_ = true;
    RotateHistory();
}

} // namespace iem
