#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace iem {

class LinkedLookaheadLimiter {
public:
    static constexpr uint32_t kLookaheadFrames = 96;

    bool Prepare(uint32_t sample_rate) noexcept;
    void SetEnabled(bool enabled) noexcept { enabled_ = enabled; }
    void SetCeilingCentidb(int32_t ceiling_centidb) noexcept;
    bool Process(const float *const input[2], float *const output[2],
        std::size_t frames) noexcept;
    void Reset() noexcept;

    uint32_t LatencyFrames() const noexcept { return kLookaheadFrames; }
    float Gain() const noexcept { return gain_; }
    float GainReductionDb() const noexcept;

private:
    std::vector<float> delay_{};
    uint32_t delay_index_ = 0;
    uint32_t hold_frames_ = 0;
    float ceiling_ = 0.9660508789F;
    float gain_ = 1.0F;
    float release_coefficient_ = 0.0F;
    bool enabled_ = true;
};

} // namespace iem
