#include "iem/HaloDownmixParams.h"

#include <algorithm>
#include <cmath>

namespace iem {
namespace {

constexpr float kPi = 3.14159265358979323846F;
constexpr float kInverseSqrtTwo = 0.70710678118654752440F;

float Millionths(int32_t value) noexcept {
    return static_cast<float>(std::clamp(value, 0, 1000000)) / 1000000.0F;
}

HaloDownmixBiquadCoefficients MakeLowHighPass(
    uint32_t sample_rate,
    int32_t frequency_millionths,
    bool high_pass
) noexcept {
    if (sample_rate == 0U) return {};
    const double frequency_hz = static_cast<double>(
        HaloDownmixFrequencyHz(frequency_millionths));
    const double w0 = 2.0 * static_cast<double>(kPi) * frequency_hz
        / static_cast<double>(sample_rate);
    const double cosine = std::cos(w0);
    const double alpha = std::sin(w0) / 2.0;
    const double denominator = 1.0 + alpha;
    const double sign = high_pass ? -1.0 : 1.0;
    const double b0 = (1.0 - sign * cosine) / (2.0 * denominator);
    return {
        static_cast<float>(b0),
        static_cast<float>(sign * (1.0 - sign * cosine) / denominator),
        static_cast<float>(b0),
        static_cast<float>(-2.0 * cosine / denominator),
        static_cast<float>((1.0 - alpha) / denominator),
    };
}

} // namespace

HaloDownmixParams::HaloDownmixParams() noexcept {
    RefreshHaloDownmixDerived(*this);
}

float HaloDownmixFrequencyHz(int32_t normalized_millionths) noexcept {
    return 20.0F * std::exp(std::log(1100.0F) * Millionths(normalized_millionths));
}

float HaloDownmixGainDb(int32_t normalized_millionths) noexcept {
    return 90.0F * Millionths(normalized_millionths) - 70.0F;
}

float HaloDownmixGainLinear(int32_t normalized_millionths) noexcept {
    return std::pow(10.0F, HaloDownmixGainDb(normalized_millionths) / 20.0F);
}

HaloDownmixBalanceCoefficients MakeHaloDownmixBalance(
    int32_t left_millionths,
    int32_t right_millionths
) noexcept {
    const float left_angle = kPi * Millionths(left_millionths) / 4.0F;
    const float right_angle = kPi * Millionths(right_millionths) / 4.0F;
    const float left_a = (std::sin(left_angle) + std::cos(left_angle))
        * kInverseSqrtTwo;
    const float left_b = (std::sin(left_angle) - std::cos(left_angle))
        * kInverseSqrtTwo;
    const float right_a = (std::sin(right_angle) + std::cos(right_angle))
        * kInverseSqrtTwo;
    const float right_b = (std::sin(right_angle) - std::cos(right_angle))
        * kInverseSqrtTwo;
    return {left_a, right_b, left_b, right_a};
}

HaloDownmixBiquadCoefficients MakeHaloDownmixHighShelf(
    uint32_t sample_rate,
    int32_t frequency_millionths,
    int32_t gain_millionths
) noexcept {
    if (sample_rate == 0U) return {};
    const float frequency_hz = HaloDownmixFrequencyHz(frequency_millionths);
    const float gain_db = HaloDownmixGainDb(gain_millionths);
    const float amplitude = std::pow(10.0F, gain_db / 40.0F);
    const float w0 = 2.0F * kPi * frequency_hz / static_cast<float>(sample_rate);
    const float cosine = std::cos(w0);
    const float sine = std::sin(w0);
    const float alpha = sine * kInverseSqrtTwo;
    const float beta = 2.0F * std::sqrt(amplitude) * alpha;
    const float a0 = (amplitude + 1.0F)
        - (amplitude - 1.0F) * cosine + beta;
    return {
        amplitude * ((amplitude + 1.0F)
            + (amplitude - 1.0F) * cosine + beta) / a0,
        -2.0F * amplitude * ((amplitude - 1.0F)
            + (amplitude + 1.0F) * cosine) / a0,
        amplitude * ((amplitude + 1.0F)
            + (amplitude - 1.0F) * cosine - beta) / a0,
        2.0F * ((amplitude - 1.0F)
            - (amplitude + 1.0F) * cosine) / a0,
        ((amplitude + 1.0F)
            - (amplitude - 1.0F) * cosine - beta) / a0,
    };
}

HaloDownmixBiquadCoefficients MakeHaloDownmixLowPass(
    uint32_t sample_rate,
    int32_t frequency_millionths
) noexcept {
    return MakeLowHighPass(sample_rate, frequency_millionths, false);
}

HaloDownmixBiquadCoefficients MakeHaloDownmixHighPass(
    uint32_t sample_rate,
    int32_t frequency_millionths
) noexcept {
    return MakeLowHighPass(sample_rate, frequency_millionths, true);
}

void RefreshHaloDownmixDerived(HaloDownmixParams &params) noexcept {
    params.derived.side_shelf = MakeHaloDownmixHighShelf(
        96000U, params.side_shelf_frequency_millionths,
        params.side_shelf_gain_millionths);
    params.derived.rear_shelf = MakeHaloDownmixHighShelf(
        96000U, params.rear_shelf_frequency_millionths,
        params.rear_shelf_gain_millionths);
    params.derived.lfe_low_pass = MakeHaloDownmixLowPass(
        96000U, params.lfe_lpf_frequency_millionths);
    params.derived.output_high_pass = MakeHaloDownmixHighPass(
        96000U, params.output_hpf_frequency_millionths);
    params.derived.balance = MakeHaloDownmixBalance(
        params.pan_left_millionths, params.pan_right_millionths);
    params.derived.front_mid_gain = HaloDownmixGainLinear(
        params.front_mid_trim_millionths);
    params.derived.front_side_gain = HaloDownmixGainLinear(
        params.front_side_trim_millionths);
    params.derived.center_gain = HaloDownmixGainLinear(
        params.center_trim_millionths);
    params.derived.surround_mid_gain = HaloDownmixGainLinear(
        params.surround_mid_trim_millionths);
    params.derived.surround_side_gain = HaloDownmixGainLinear(
        params.surround_side_trim_millionths);
    params.derived.rear_mid_gain = HaloDownmixGainLinear(
        params.rear_mid_trim_millionths);
    params.derived.rear_side_gain = HaloDownmixGainLinear(
        params.rear_side_trim_millionths);
    params.derived.lfe_gain = HaloDownmixGainLinear(params.lfe_trim_millionths);
    params.derived.output_left_gain = HaloDownmixGainLinear(
        params.output_left_trim_millionths);
    params.derived.output_right_gain = HaloDownmixGainLinear(
        params.output_right_trim_millionths);
}

} // namespace iem
