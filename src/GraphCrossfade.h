#ifndef VIPER_GRAPH_CROSSFADE_H
#define VIPER_GRAPH_CROSSFADE_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace viper::audio {

class GraphCrossfade final {
public:
    bool Prepare(uint32_t sample_rate, float duration_ms = 5.0F);
    void Reset() noexcept;
    void StartDryToWet() noexcept;
    void Apply(float *wet, const float *dry, size_t frame_count) noexcept;

    bool IsActive() const noexcept;
    size_t TransitionFrames() const noexcept;
    size_t RemainingFrames() const noexcept;

private:
    std::vector<float> dry_gains_;
    std::vector<float> wet_gains_;
    size_t position_ = 0;
    bool active_ = false;
};

} // namespace viper::audio

#endif
