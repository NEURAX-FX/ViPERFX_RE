#pragma once

#include <cstdint>

namespace iem {

struct HaloDownmixBiquadCoefficients {
    float b0 = 1.0F;
    float b1 = 0.0F;
    float b2 = 0.0F;
    float a1 = 0.0F;
    float a2 = 0.0F;
};

struct HaloDownmixBalanceCoefficients {
    float left_from_left = 1.0F;
    float left_from_right = 0.0F;
    float right_from_left = 0.0F;
    float right_from_right = 1.0F;
};

struct HaloDownmixDerived {
    HaloDownmixBiquadCoefficients side_shelf{};
    HaloDownmixBiquadCoefficients rear_shelf{};
    HaloDownmixBiquadCoefficients lfe_low_pass{};
    HaloDownmixBiquadCoefficients output_high_pass{};
    HaloDownmixBalanceCoefficients balance{};
    float front_mid_gain = 1.0F;
    float front_side_gain = 1.0F;
    float center_gain = 0.7079458F;
    float surround_mid_gain = 0.7079458F;
    float surround_side_gain = 0.7079458F;
    float rear_mid_gain = 0.5011872F;
    float rear_side_gain = 0.5011872F;
    float lfe_gain = 1.0F;
    float output_left_gain = 1.0F;
    float output_right_gain = 1.0F;
};

struct HaloDownmixParams {
    bool delay_enable = true;
    int32_t ls_delay_us = 0;
    int32_t rs_delay_us = 0;
    int32_t lsr_delay_us = 0;
    int32_t rsr_delay_us = 0;
    bool side_shelf_enable = false;
    int32_t side_shelf_frequency_millionths = 229819;
    int32_t side_shelf_gain_millionths = 777778;
    bool rear_shelf_enable = false;
    int32_t rear_shelf_frequency_millionths = 229819;
    int32_t rear_shelf_gain_millionths = 777778;
    int32_t pan_left_millionths = 1000000;
    int32_t pan_right_millionths = 1000000;
    int32_t center_divergence_millionths = 0;
    int32_t front_mid_trim_millionths = 777778;
    int32_t front_side_trim_millionths = 777778;
    int32_t center_trim_millionths = 744444;
    int32_t surround_mid_trim_millionths = 744444;
    int32_t surround_side_trim_millionths = 744444;
    int32_t rear_mid_trim_millionths = 711111;
    int32_t rear_side_trim_millionths = 711111;
    int32_t lfe_trim_millionths = 777778;
    bool lfe_lpf_enable = false;
    int32_t lfe_lpf_frequency_millionths = 328797;
    bool scale_input_by_output_count = false;
    bool output_hpf_enable = false;
    int32_t output_hpf_frequency_millionths = 57898;
    int32_t output_left_trim_millionths = 777778;
    int32_t output_right_trim_millionths = 777778;
    HaloDownmixDerived derived{};

    HaloDownmixParams() noexcept;
};

float HaloDownmixFrequencyHz(int32_t normalized_millionths) noexcept;
float HaloDownmixGainDb(int32_t normalized_millionths) noexcept;
float HaloDownmixGainLinear(int32_t normalized_millionths) noexcept;
HaloDownmixBalanceCoefficients MakeHaloDownmixBalance(
    int32_t left_millionths,
    int32_t right_millionths
) noexcept;
HaloDownmixBiquadCoefficients MakeHaloDownmixHighShelf(
    uint32_t sample_rate,
    int32_t frequency_millionths,
    int32_t gain_millionths
) noexcept;
HaloDownmixBiquadCoefficients MakeHaloDownmixLowPass(
    uint32_t sample_rate,
    int32_t frequency_millionths
) noexcept;
HaloDownmixBiquadCoefficients MakeHaloDownmixHighPass(
    uint32_t sample_rate,
    int32_t frequency_millionths
) noexcept;
void RefreshHaloDownmixDerived(HaloDownmixParams &params) noexcept;

} // namespace iem
