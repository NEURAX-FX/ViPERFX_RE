#include "GraphCrossfade.h"
#include "AudioFormat.h"
#include <algorithm>
#include <cmath>

namespace viper::audio {
namespace {

constexpr float kHalfPi = 1.57079632679489661923F;

} // namespace

bool GraphCrossfade::Prepare(uint32_t sample_rate, float duration_ms) {
    if (!IsSupportedSampleRate(sample_rate) || !std::isfinite(duration_ms)
        || duration_ms <= 0.0F) {
        return false;
    }
    const size_t transition_frames = std::max<size_t>(
        1,
        static_cast<size_t>(std::llround(
            static_cast<double>(sample_rate) * duration_ms / 1000.0
        ))
    );
    dry_gains_.resize(transition_frames);
    wet_gains_.resize(transition_frames);
    if (transition_frames == 1) {
        dry_gains_[0] = 0.0F;
        wet_gains_[0] = 1.0F;
    } else {
        for (size_t i = 0; i < transition_frames; ++i) {
            const float progress = static_cast<float>(i)
                / static_cast<float>(transition_frames - 1);
            const float dry_gain = std::cos(progress * kHalfPi);
            const float wet_gain = std::sin(progress * kHalfPi);
            const float normalization = dry_gain + wet_gain;
            dry_gains_[i] = dry_gain / normalization;
            wet_gains_[i] = wet_gain / normalization;
        }
    }
    Reset();
    return true;
}

void GraphCrossfade::Reset() noexcept {
    position_ = wet_gains_.size();
    active_ = false;
}

void GraphCrossfade::StartDryToWet() noexcept {
    if (wet_gains_.empty()) {
        active_ = false;
        position_ = 0;
        return;
    }
    position_ = 0;
    active_ = true;
}

void GraphCrossfade::Apply(
    float *wet,
    const float *dry,
    size_t frame_count
) noexcept {
    if (!active_ || wet == nullptr || dry == nullptr || frame_count == 0) return;
    const size_t frames = std::min(frame_count, RemainingFrames());
    for (size_t frame = 0; frame < frames; ++frame) {
        const float dry_gain = dry_gains_[position_];
        const float wet_gain = wet_gains_[position_];
        const size_t sample = frame * kChannelCount;
        wet[sample] = dry[sample] * dry_gain + wet[sample] * wet_gain;
        wet[sample + 1] = dry[sample + 1] * dry_gain + wet[sample + 1] * wet_gain;
        ++position_;
    }
    if (position_ >= wet_gains_.size()) active_ = false;
}

bool GraphCrossfade::IsActive() const noexcept {
    return active_;
}

size_t GraphCrossfade::TransitionFrames() const noexcept {
    return wet_gains_.size();
}

size_t GraphCrossfade::RemainingFrames() const noexcept {
    if (!active_ || position_ >= wet_gains_.size()) return 0;
    return wet_gains_.size() - position_;
}

} // namespace viper::audio
