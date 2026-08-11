#pragma once

#include "GraphCrossfade.h"
#include "iem/IemEngine.h"

#include <cstddef>
#include <cstdint>

namespace viper::audio {

struct IemGraphConfig {
    uint32_t sample_rate = 0;
    std::size_t max_block_frames = 0;
    uint64_t generation = 0;
};

class IemGraph final {
public:
    bool Prepare(
        const IemGraphConfig &config,
        const iem::IemParams &params
    ) noexcept;
    bool Process(float *interleaved, std::size_t frame_count) noexcept;
    void ApplyParams(const iem::IemParams &params) noexcept;
    void Reset() noexcept;

    iem::IemEngine &Engine() noexcept { return engine_; }
    const iem::IemEngine &Engine() const noexcept { return engine_; }
    const IemGraphConfig &Config() const noexcept { return config_; }
    GraphCrossfade &Transition() noexcept { return transition_; }
    bool IsPrepared() const noexcept { return prepared_; }
    void SetResourceGeneration(uint64_t generation) noexcept {
        resource_generation_ = generation;
    }
    uint64_t ResourceGeneration() const noexcept { return resource_generation_; }

private:
    IemGraphConfig config_{};
    iem::IemEngine engine_{};
    GraphCrossfade transition_{};
    uint64_t resource_generation_ = 0;
    bool prepared_ = false;
};

} // namespace viper::audio
