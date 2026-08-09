#include "DspGraph.h"
#include "AudioFormat.h"

namespace viper::audio {

bool DspGraph::Prepare(const DspGraphConfig &config) {
    if (!IsSupportedSampleRate(config.sample_rate)
        || config.max_block_frames == 0
        || config.max_block_frames > kMaxBlockFrames) {
        prepared_ = false;
        return false;
    }
    engine_.SetSamplingRate(config.sample_rate);
    engine_.ResetAllEffects();
    config_ = config;
    prepared_ = true;
    return true;
}

void DspGraph::Reset() noexcept {
    engine_.ResetAllEffects();
}

bool DspGraph::Process(float *interleaved, size_t frame_count) noexcept {
    if (!prepared_ || interleaved == nullptr || frame_count == 0
        || frame_count > config_.max_block_frames) {
        return false;
    }
    engine_.Process(interleaved, static_cast<uint32_t>(frame_count));
    return true;
}

ViPER &DspGraph::Engine() noexcept {
    return engine_;
}

const ViPER &DspGraph::Engine() const noexcept {
    return engine_;
}

const DspGraphConfig &DspGraph::Config() const noexcept {
    return config_;
}

bool DspGraph::IsPrepared() const noexcept {
    return prepared_;
}

} // namespace viper::audio
