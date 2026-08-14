#pragma once

#include "iem/HaloDownmixParams.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace iem {

enum class HaloDownmixRole : uint32_t {
    L = 0,
    R = 1,
    C = 2,
    LFE = 3,
    Ls = 4,
    Rs = 5,
    Lsr = 6,
    Rsr = 7,
    kCount = 8,
};

class HaloDownmixProcessor {
public:
    static constexpr uint32_t kSampleRate = 96000U;
    static constexpr uint32_t kLatencyFrames = 3072U;
    static constexpr uint32_t kRoleCount =
        static_cast<uint32_t>(HaloDownmixRole::kCount);

    bool Prepare(std::size_t max_frames) noexcept;
    void ApplyParams(const HaloDownmixParams &params) noexcept;
    void Reset() noexcept;
    bool Process(
        const float *const inputs[kRoleCount],
        float *const outputs[2],
        std::size_t frames
    ) noexcept;
    std::size_t PreparedBytes() const noexcept;

private:
    struct BiquadState {
        float x1 = 0.0F;
        float x2 = 0.0F;
        float y1 = 0.0F;
        float y2 = 0.0F;

        float Process(float input,
            const HaloDownmixBiquadCoefficients &coefficients) noexcept;
        void Reset() noexcept;
    };

    struct Ramp {
        float current = 0.0F;
        float target = 0.0F;
        float step = 0.0F;
        uint32_t remaining = 0U;

        void Set(float value, uint32_t frames, bool immediate) noexcept;
        float Next() noexcept;
        void Snap() noexcept;
    };

    struct DelayState {
        uint32_t current = kLatencyFrames;
        uint32_t target = kLatencyFrames;
        float mix = 1.0F;
        bool active = false;

        void Set(uint32_t frames, bool immediate) noexcept;
        void Reset() noexcept;
    };

    static constexpr uint32_t kDivergenceRampFrames = 100U;
    static constexpr uint32_t kGainRampFrames = 1024U;
    static constexpr float kDelayCrossfadeStep = 0.0001F;

    uint32_t DelayTarget(int32_t relative_delay_us, bool relative_enabled) const noexcept;
    float ReadDelayed(uint32_t role, float input, uint32_t delay_frames) const noexcept;
    float ProcessDelayed(uint32_t role, float input) noexcept;
    void SetRampTargets(bool immediate) noexcept;
    void ClearFilters() noexcept;

    bool prepared_ = false;
    bool started_ = false;
    std::size_t max_frames_ = 0;
    uint32_t write_index_ = 0U;
    HaloDownmixParams params_{};
    std::vector<float> delay_ring_{};
    std::array<DelayState, kRoleCount> delays_{};
    std::array<BiquadState, 2> side_shelf_{};
    std::array<BiquadState, 2> rear_shelf_{};
    BiquadState lfe_low_pass_{};
    std::array<BiquadState, 2> output_high_pass_{};
    Ramp divergence_{};
    Ramp front_mid_{};
    Ramp front_side_{};
    Ramp center_{};
    Ramp surround_mid_{};
    Ramp surround_side_{};
    Ramp rear_mid_{};
    Ramp rear_side_{};
    Ramp lfe_{};
    Ramp output_left_{};
    Ramp output_right_{};
};

} // namespace iem
