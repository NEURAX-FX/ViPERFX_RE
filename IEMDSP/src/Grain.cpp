#include "iem/Grain.h"

#include <algorithm>
#include <cmath>

namespace iem {

namespace {

constexpr float kHalfPi = 1.5707963267948966F;

} // namespace

void Grain::Start(const GrainStart &start) noexcept {
    start_ = start;
    start_.length_samples = std::max(start_.length_samples, 1U);
    start_.pitch_factor = std::max(start_.pitch_factor, 0.0001F);
    start_.attack_fraction = std::clamp(start_.attack_fraction, 0.0F, 0.5F);
    start_.decay_fraction = std::clamp(start_.decay_fraction, 0.0F, 0.5F);
    start_.source_channel = std::min(start_.source_channel, 1U);
    current_index_ = 0;
    active_ = true;
}

void Grain::Reset() noexcept {
    start_ = {};
    current_index_ = 0;
    active_ = false;
}

float Grain::RenderSample(
    const float *left_history,
    const float *right_history,
    std::size_t history_frames
) noexcept {
    if (!active_ || left_history == nullptr || right_history == nullptr
        || history_frames == 0) {
        return 0.0F;
    }
    if (current_index_ >= start_.length_samples) {
        active_ = false;
        return 0.0F;
    }

    const float read_position = static_cast<float>(start_.start_position)
        + static_cast<float>(current_index_) * start_.pitch_factor;
    const auto base_unwrapped = static_cast<uint64_t>(std::floor(read_position));
    const std::size_t base = static_cast<std::size_t>(base_unwrapped % history_frames);
    const std::size_t next = (base + 1U) % history_frames;
    const float fraction = read_position - std::floor(read_position);
    const float *history = start_.source_channel == 0 ? left_history : right_history;
    const float sample = history[base] + (history[next] - history[base]) * fraction;
    const float progress = start_.length_samples <= 1
        ? 1.0F
        : static_cast<float>(current_index_)
            / static_cast<float>(start_.length_samples - 1U);
    const float output = sample * WindowValue(progress) * start_.gain;

    ++current_index_;
    if (current_index_ >= start_.length_samples) active_ = false;
    return output;
}

float Grain::WindowValue(float progress) const noexcept {
    progress = std::clamp(progress, 0.0F, 1.0F);
    if (start_.attack_fraction > 0.0F && progress < start_.attack_fraction) {
        const float sine = std::sin(progress / start_.attack_fraction * kHalfPi);
        return sine * sine;
    }
    const float decay_start = 1.0F - start_.decay_fraction;
    if (start_.decay_fraction > 0.0F && progress > decay_start) {
        const float cosine = std::cos(
            (progress - decay_start) / start_.decay_fraction * kHalfPi
        );
        return cosine * cosine;
    }
    return 1.0F;
}

} // namespace iem
