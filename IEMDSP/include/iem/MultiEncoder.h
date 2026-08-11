#pragma once

#include "iem/IemEncoder.h"

#include <array>

namespace iem {

class MultiEncoder final : public IemEncoder {
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
    using Coefficients = std::array<float, kMaxAmbisonicsChannels>;
    using SourceCoefficients = std::array<Coefficients, 2>;

    void EvaluateTargets(const MultiParams &params) noexcept;

    EncoderConfig config_{};
    SourceCoefficients current_{};
    SourceCoefficients target_{};
    bool prepared_ = false;
    bool first_apply_ = true;
};

} // namespace iem
