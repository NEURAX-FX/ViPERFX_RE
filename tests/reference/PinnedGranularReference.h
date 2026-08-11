#pragma once

#include <algorithm>
#include <cmath>

namespace iem_test {

inline float PinnedWindowValue(float progress, float attack, float decay) {
    constexpr float kHalfPi = 1.5707963267948966F;
    progress = std::clamp(progress, 0.0F, 1.0F);
    if (attack > 0.0F && progress < attack) {
        const float sine = std::sin(progress / attack * kHalfPi);
        return sine * sine;
    }
    if (decay > 0.0F && progress > 1.0F - decay) {
        const float cosine = std::cos((progress - (1.0F - decay)) / decay * kHalfPi);
        return cosine * cosine;
    }
    return 1.0F;
}

inline float PinnedInterpolatedSample(
    const float *history,
    std::size_t history_frames,
    float position
) {
    const auto base_unwrapped = static_cast<unsigned long long>(std::floor(position));
    const std::size_t base = static_cast<std::size_t>(base_unwrapped % history_frames);
    const std::size_t next = (base + 1U) % history_frames;
    const float fraction = position - std::floor(position);
    return history[base] + (history[next] - history[base]) * fraction;
}

} // namespace iem_test
