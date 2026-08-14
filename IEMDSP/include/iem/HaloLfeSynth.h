#pragma once

#include "iem/IemParams.h"

#include <cstddef>
#include <cstdint>

namespace iem {

float HaloLfeCutoffHz(int32_t normalized_millionths) noexcept;
float HaloLfeGainDb(int32_t normalized_millionths) noexcept;
float HaloLfeGainLinear(int32_t normalized_millionths) noexcept;
HaloLfeCoefficients MakeHaloLfeLowPass(
    uint32_t sample_rate,
    int32_t normalized_millionths
) noexcept;

class HaloLfeSynth {
public:
    bool Prepare(uint32_t sample_rate) noexcept;
    void ApplyParams(const HaloLfeParams &params) noexcept;
    void Reset() noexcept;
    void Process(
        float *left,
        float *right,
        float *centre,
        float *lfe,
        std::size_t frames
    ) noexcept;

private:
    struct BiquadState {
        float x1 = 0.0F;
        float x2 = 0.0F;
        float y1 = 0.0F;
        float y2 = 0.0F;

        float Process(float input, const HaloLfeCoefficients &coefficients) noexcept;
        void Reset() noexcept;
    };

    bool prepared_ = false;
    bool started_ = false;
    HaloLfeCoefficients coefficients_{};
    BiquadState left_filter_{};
    BiquadState right_filter_{};
    BiquadState centre_filter_{};
    float gain_linear_ = 0.0316227766F;
    float split_ = 0.0F;
    float enable_mix_ = 1.0F;
    float enable_target_ = 1.0F;
};

} // namespace iem
