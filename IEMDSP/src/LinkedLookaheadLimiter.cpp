#include "iem/LinkedLookaheadLimiter.h"

#include <algorithm>
#include <cmath>

namespace iem {

bool LinkedLookaheadLimiter::Prepare(uint32_t sample_rate) noexcept {
    if (sample_rate != 96000) return false;
    delay_.assign(static_cast<std::size_t>(2U) * kLookaheadFrames, 0.0F);
    release_coefficient_ = std::exp(-1.0F / (0.050F * static_cast<float>(sample_rate)));
    Reset();
    return true;
}

void LinkedLookaheadLimiter::SetCeilingCentidb(int32_t ceiling_centidb) noexcept {
    ceiling_centidb = std::clamp(ceiling_centidb, -1200, 0);
    ceiling_ = std::pow(10.0F, static_cast<float>(ceiling_centidb) / 2000.0F);
}

bool LinkedLookaheadLimiter::Process(const float *const input[2], float *const output[2],
    std::size_t frames) noexcept {
    if (delay_.size() != static_cast<std::size_t>(2U) * kLookaheadFrames
        || input == nullptr || output == nullptr || input[0] == nullptr
        || input[1] == nullptr || output[0] == nullptr || output[1] == nullptr) {
        return false;
    }
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const float left = input[0][frame];
        const float right = input[1][frame];
        if (!std::isfinite(left) || !std::isfinite(right)) return false;
        float *left_delay = delay_.data();
        float *right_delay = delay_.data() + kLookaheadFrames;
        const float delayed_left = left_delay[delay_index_];
        const float delayed_right = right_delay[delay_index_];
        left_delay[delay_index_] = left;
        right_delay[delay_index_] = right;

        if (enabled_) {
            const float peak = std::max(std::abs(left), std::abs(right));
            const float required_gain = peak > ceiling_ ? ceiling_ / peak : 1.0F;
            if (required_gain < gain_) {
                gain_ = required_gain;
                hold_frames_ = kLookaheadFrames;
            } else if (hold_frames_ != 0) {
                --hold_frames_;
            } else {
                gain_ = 1.0F - (1.0F - gain_) * release_coefficient_;
                if (gain_ > 0.999999F) gain_ = 1.0F;
            }
        } else {
            gain_ = 1.0F;
            hold_frames_ = 0;
        }
        output[0][frame] = delayed_left * gain_;
        output[1][frame] = delayed_right * gain_;
        delay_index_ = (delay_index_ + 1U) % kLookaheadFrames;
    }
    return true;
}

void LinkedLookaheadLimiter::Reset() noexcept {
    std::fill(delay_.begin(), delay_.end(), 0.0F);
    delay_index_ = 0;
    hold_frames_ = 0;
    gain_ = 1.0F;
}

float LinkedLookaheadLimiter::GainReductionDb() const noexcept {
    return gain_ > 0.0F ? -20.0F * std::log10(gain_) : 120.0F;
}

} // namespace iem
