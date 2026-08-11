#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace iem {

enum class IemBypassReason : uint32_t {
    NONE = 0,
    DISABLED,
    NOT_PREPARED,
    INVALID_BLOCK,
    RESAMPLER_FAILURE,
    OUTPUT_UNDERFLOW,
    NON_FINITE,
    PROCESS_FAILURE,
};

struct IemTelemetrySnapshot {
    uint64_t sequence = 0;
    uint64_t processed_frames = 0;
    uint64_t latest_process_ns = 0;
    uint64_t average_process_ns = 0;
    uint64_t max_process_ns = 0;
    uint64_t deadline_misses = 0;
    uint64_t output_underflows = 0;
    uint32_t host_sample_rate = 0;
    uint32_t internal_sample_rate = 96000;
    uint32_t latency_frames = 0;
    IemBypassReason bypass_reason = IemBypassReason::DISABLED;
    bool enabled = false;
    bool prepared = false;
};

class IemTelemetryPublisher final {
public:
    IemTelemetryPublisher() = default;
    IemTelemetryPublisher(IemTelemetryPublisher &&other) noexcept;
    IemTelemetryPublisher &operator=(IemTelemetryPublisher &&other) noexcept;
    IemTelemetryPublisher(const IemTelemetryPublisher &) = delete;
    IemTelemetryPublisher &operator=(const IemTelemetryPublisher &) = delete;

    void Configure(uint32_t host_sample_rate, uint32_t internal_sample_rate) noexcept;
    void RecordBlock(
        uint64_t frames,
        uint64_t process_ns,
        uint64_t budget_ns,
        uint32_t latency_frames,
        bool enabled,
        IemBypassReason reason = IemBypassReason::NONE
    ) noexcept;
    void RecordFailure(IemBypassReason reason) noexcept;
    bool Read(IemTelemetrySnapshot &snapshot) const noexcept;

private:
    struct Slot {
        IemTelemetrySnapshot snapshot{};
    };

    void Publish() noexcept;
    void MoveFrom(IemTelemetryPublisher &other) noexcept;

    std::array<Slot, 3> slots_{};
    mutable std::atomic<int> reading_index_{-1};
    std::atomic<int> published_index_{0};
    IemTelemetrySnapshot writer_state_{};
    uint64_t total_process_ns_ = 0;
    uint64_t block_count_ = 0;
    uint64_t next_sequence_ = 0;
};

} // namespace iem
