#ifndef VIPER_DSP_GRAPH_H
#define VIPER_DSP_GRAPH_H

#include "viper/ViPER.h"
#include <cstddef>
#include <cstdint>

namespace viper::audio {

struct DspGraphConfig {
    uint32_t sample_rate = 0;
    size_t max_block_frames = 0;
    uint64_t generation = 0;
};

class DspGraph final {
public:
    bool Prepare(const DspGraphConfig &config);
    void Reset() noexcept;
    bool Process(float *interleaved, size_t frame_count) noexcept;

    ViPER &Engine() noexcept;
    const ViPER &Engine() const noexcept;
    const DspGraphConfig &Config() const noexcept;
    bool IsPrepared() const noexcept;

private:
    DspGraphConfig config_{};
    ViPER engine_{};
    bool prepared_ = false;
};

} // namespace viper::audio

#endif
