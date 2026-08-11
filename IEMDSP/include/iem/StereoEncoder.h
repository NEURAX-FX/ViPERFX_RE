#pragma once

#include "iem/IemEncoder.h"

#include <array>

namespace iem {

class StereoEncoder final : public IemEncoder {
public:
    bool Prepare(const EncoderConfig &config) override;
    void ApplyParams(const IemParams &params) noexcept override;
    void Reset() noexcept override;
    bool Process(
        const float *const stereo[2],
        float *const ambisonics[kMaxAmbisonicsChannels],
        std::size_t frames
    ) noexcept override;

private:
    struct Controls {
        float azimuth = 0.0F;
        float elevation = 0.0F;
        float roll = 0.0F;
        float width = 0.0F;
        bool sample_wise = false;
    };

    using Coefficients = std::array<float, kMaxAmbisonicsChannels>;

    static Controls ConvertControls(const StereoParams &params) noexcept;
    static void EvaluatePair(
        const Controls &controls,
        uint32_t order,
        Coefficients &left,
        Coefficients &right
    ) noexcept;
    static float InterpolateAngle(float start, float end, float amount) noexcept;

    EncoderConfig config_{};
    Controls current_controls_{};
    Controls target_controls_{};
    Coefficients current_left_{};
    Coefficients current_right_{};
    Coefficients target_left_{};
    Coefficients target_right_{};
    bool prepared_ = false;
    bool first_apply_ = true;
};

} // namespace iem
