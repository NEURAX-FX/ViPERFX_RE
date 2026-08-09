#ifndef VIPER_DSP_GRAPH_SLOTS_H
#define VIPER_DSP_GRAPH_SLOTS_H

#include "DspGraph.h"
#include "DspResources.h"
#include <array>
#include <atomic>

namespace viper::audio {

struct DspGraphSwapResult {
    DspGraph *active = nullptr;
    DspGraph *previous = nullptr;
    bool changed = false;
    bool sample_rate_changed = false;
};

class DspGraphSlots final {
public:
    bool PrepareInitial(
        const DspGraphConfig &config,
        const viper::ViPERParams &params,
        const DspResources &resources
    );
    bool PreparePending(
        const DspGraphConfig &config,
        const viper::ViPERParams &params,
        const DspResources &resources
    );

    DspGraphSwapResult ConsumePending() noexcept;
    void ReleasePrevious() noexcept;

    DspGraph *Active() noexcept;
    DspGraph *Pending() noexcept;
    DspGraph *Previous() noexcept;

private:
    std::array<DspGraph, 2> graphs_{};
    std::atomic<uint32_t> state_{0x3FU};
};

} // namespace viper::audio

#endif
