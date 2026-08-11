#pragma once

#include "IemGraph.h"
#include "IemResources.h"

#include <array>
#include <atomic>

namespace viper::audio {

struct IemGraphSwapResult {
    IemGraph *active = nullptr;
    IemGraph *previous = nullptr;
    bool changed = false;
    bool sample_rate_changed = false;
};

class IemGraphSlots final {
public:
    bool PrepareInitial(
        const IemGraphConfig &config,
        const iem::IemParams &params,
        const IemResources &resources
    ) noexcept;
    bool PreparePending(
        const IemGraphConfig &config,
        const iem::IemParams &params,
        const IemResources &resources
    ) noexcept;

    IemGraphSwapResult ConsumePending() noexcept;
    bool CancelPending() noexcept;
    void ReleasePrevious() noexcept;

    IemGraph *Active() noexcept;
    const IemGraph *Active() const noexcept;
    IemGraph *Pending() noexcept;
    const IemGraph *Pending() const noexcept;
    IemGraph *Previous() noexcept;
    const IemGraph *Previous() const noexcept;

private:
    std::array<IemGraph, 3> graphs_{};
    std::atomic<uint32_t> state_{0x3FU};
};

} // namespace viper::audio
