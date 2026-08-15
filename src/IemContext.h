#pragma once

#include "IemGraphSlots.h"
#include "IemParameterMailbox.h"
#include "IemResources.h"

#include <cstddef>
#include <atomic>
#include <cstdint>
#include <vector>

namespace viper::audio {

class IemContext final {
public:
    bool Prepare(uint32_t host_sample_rate, std::size_t max_block_frames);
    void RestoreCachedState(
        const iem::IemParams &params,
        uint64_t resource_generation
    ) noexcept;
    bool DispatchRawParam(int param, int val1, int val2, int val3) noexcept;
    bool Process(float *post_viper_interleaved, std::size_t frame_count) noexcept;
    void Reset() noexcept;
    bool ReadTelemetry(iem::IemTelemetrySnapshot &snapshot) const noexcept;

    bool IsPrepared() const noexcept { return prepared_; }
    const iem::IemParams &Params() const noexcept { return parameter_snapshot_; }
    uint64_t ResourceGeneration() const noexcept { return resources_.Generation(); }
    uint64_t StateRevision() const noexcept { return state_revision_; }
    uint64_t GraphGeneration() const noexcept { return graph_generation_; }
    iem::IemPreparationResult LastPreparationResult() const noexcept {
        return static_cast<iem::IemPreparationResult>(
            last_preparation_result_.load(std::memory_order_acquire));
    }
    const IemGraph *ActiveGraphForTest() const noexcept { return graph_slots_.Active(); }
    const IemGraph *PendingGraphForTest() const noexcept { return graph_slots_.Pending(); }

private:
    bool TryPrepareDesiredGraph() noexcept;
    std::vector<float> dry_buffer_;
    std::vector<float> previous_buffer_;
    IemGraphSlots graph_slots_;
    IemResources resources_;
    IemParameterMailbox parameter_mailbox_;
    iem::IemParams parameter_snapshot_{};
    uint64_t applied_parameter_generation_ = 0;
    uint64_t state_revision_ = 0;
    uint64_t graph_generation_ = 0;
    uint32_t host_sample_rate_ = 0;
    std::size_t max_block_frames_ = 0;
    std::atomic<uint32_t> last_preparation_result_{
        static_cast<uint32_t>(iem::IemPreparationResult::NONE)};
    bool structural_dirty_ = false;
    bool prepared_ = false;
};

} // namespace viper::audio
