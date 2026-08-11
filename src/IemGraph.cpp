#include "IemGraph.h"

#include "AudioFormat.h"

namespace viper::audio {

bool IemGraph::Prepare(
    const IemGraphConfig &config,
    const iem::IemParams &params
) noexcept {
    if (!IsSupportedSampleRate(config.sample_rate)
        || config.max_block_frames == 0
        || config.max_block_frames > kMaxBlockFrames
        || config.generation == 0) {
        return false;
    }
    iem::IemEngineConfig engine_config{
        config.sample_rate,
        config.max_block_frames,
        96000,
        256,
        16,
    };
    if (!engine_.Prepare(engine_config, params)
        || !transition_.Prepare(config.sample_rate)) {
        return false;
    }
    config_ = config;
    resource_generation_ = 0;
    prepared_ = true;
    return true;
}

bool IemGraph::Process(float *interleaved, std::size_t frame_count) noexcept {
    return prepared_ && engine_.Process(interleaved, frame_count);
}

void IemGraph::ApplyParams(const iem::IemParams &params) noexcept {
    if (!prepared_) return;
    iem::IemParams dynamic = params;
    const iem::IemParams &structural = engine_.Params();
    dynamic.order = structural.order;
    dynamic.encoder_mode = structural.encoder_mode;
    dynamic.latency_profile = structural.latency_profile;
    dynamic.decoder.headphone_eq = structural.decoder.headphone_eq;
    engine_.ApplyParams(dynamic);
}

void IemGraph::Reset() noexcept {
    if (!prepared_) return;
    engine_.Reset();
    transition_.Reset();
}

} // namespace viper::audio
