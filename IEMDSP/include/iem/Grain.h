#pragma once

#include "iem/SphericalHarmonics.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace iem {

struct GrainStart {
    uint32_t start_position = 0;
    uint32_t length_samples = 1;
    float pitch_factor = 1.0F;
    std::array<float, kMaxAmbisonicsChannels> channel_weights{};
    float gain = 1.0F;
    float attack_fraction = 0.5F;
    float decay_fraction = 0.5F;
    uint32_t source_channel = 0;
};

class Grain {
public:
    void Start(const GrainStart &start) noexcept;
    void Reset() noexcept;
    float RenderSample(
        const float *left_history,
        const float *right_history,
        std::size_t history_frames
    ) noexcept;

    bool IsActive() const noexcept { return active_; }
    const std::array<float, kMaxAmbisonicsChannels> &ChannelWeights() const noexcept {
        return start_.channel_weights;
    }

private:
    float WindowValue(float progress) const noexcept;

    GrainStart start_{};
    uint32_t current_index_ = 0;
    bool active_ = false;
};

} // namespace iem
