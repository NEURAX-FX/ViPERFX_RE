#include "iem/IemTelemetry.h"

#include <algorithm>
#include <limits>

namespace iem {

IemTelemetryPublisher::IemTelemetryPublisher(
    IemTelemetryPublisher &&other
) noexcept {
    MoveFrom(other);
}

IemTelemetryPublisher &IemTelemetryPublisher::operator=(
    IemTelemetryPublisher &&other
) noexcept {
    if (this != &other) MoveFrom(other);
    return *this;
}

void IemTelemetryPublisher::Configure(
    uint32_t host_sample_rate,
    uint32_t internal_sample_rate
) noexcept {
    writer_state_ = {};
    writer_state_.host_sample_rate = host_sample_rate;
    writer_state_.internal_sample_rate = internal_sample_rate;
    writer_state_.bypass_reason = IemBypassReason::DISABLED;
    writer_state_.prepared = true;
    total_process_ns_ = 0;
    block_count_ = 0;
    next_sequence_ = 0;
    Publish();
}

void IemTelemetryPublisher::RecordBlock(
    uint64_t frames,
    uint64_t process_ns,
    uint64_t budget_ns,
    uint32_t latency_frames,
    bool enabled,
    IemBypassReason reason
) noexcept {
    writer_state_.processed_frames += frames;
    writer_state_.latest_process_ns = process_ns;
    if (block_count_ >= 1000000U
        || total_process_ns_ > std::numeric_limits<uint64_t>::max() - process_ns) {
        block_count_ = block_count_ / 2U + 1U;
        total_process_ns_ = total_process_ns_ / 2U + process_ns;
    } else {
        ++block_count_;
        total_process_ns_ += process_ns;
    }
    writer_state_.average_process_ns = total_process_ns_ / block_count_;
    writer_state_.max_process_ns = std::max(
        writer_state_.max_process_ns, process_ns
    );
    if (budget_ns != 0 && process_ns > budget_ns) {
        ++writer_state_.deadline_misses;
    }
    writer_state_.latency_frames = latency_frames;
    writer_state_.latency_ms = writer_state_.host_sample_rate != 0
        ? static_cast<float>(latency_frames) * 1000.0F
            / static_cast<float>(writer_state_.host_sample_rate)
        : 0.0F;
    writer_state_.bypass_reason = reason;
    writer_state_.enabled = enabled;
    Publish();
}

void IemTelemetryPublisher::RecordQueueOverflow(bool input_queue) noexcept {
    if (input_queue) ++writer_state_.input_overflows;
    else ++writer_state_.output_overflows;
    Publish();
}

void IemTelemetryPublisher::RecordSpatialState(
    uint32_t encoder_mode,
    uint32_t ambisonics_order,
    uint32_t active_grains,
    uint64_t grain_pool_exhaustions,
    uint32_t fault_code,
    float limiter_gain_reduction_db
) noexcept {
    writer_state_.encoder_mode = encoder_mode;
    writer_state_.ambisonics_order = ambisonics_order;
    writer_state_.active_grains = active_grains;
    writer_state_.grain_pool_exhaustions = grain_pool_exhaustions;
    writer_state_.fault_code = fault_code;
    writer_state_.limiter_gain_reduction_db = limiter_gain_reduction_db;
}

void IemTelemetryPublisher::RecordFailure(IemBypassReason reason) noexcept {
    writer_state_.bypass_reason = reason;
    if (reason == IemBypassReason::OUTPUT_UNDERFLOW) {
        ++writer_state_.output_underflows;
    }
    Publish();
}

bool IemTelemetryPublisher::Read(
    IemTelemetrySnapshot &snapshot
) const noexcept {
    for (;;) {
        const int published = published_index_.load(std::memory_order_acquire);
        reading_index_.store(published, std::memory_order_release);
        if (published != published_index_.load(std::memory_order_acquire)) {
            reading_index_.store(-1, std::memory_order_release);
            continue;
        }
        snapshot = slots_[published].snapshot;
        reading_index_.store(-1, std::memory_order_release);
        return snapshot.prepared;
    }
}

void IemTelemetryPublisher::Publish() noexcept {
    const int published = published_index_.load(std::memory_order_acquire);
    const int reading = reading_index_.load(std::memory_order_acquire);
    int target = 0;
    while (target == published || target == reading) ++target;
    writer_state_.sequence = ++next_sequence_;
    slots_[target].snapshot = writer_state_;
    published_index_.store(target, std::memory_order_release);
}

void IemTelemetryPublisher::MoveFrom(IemTelemetryPublisher &other) noexcept {
    writer_state_ = other.writer_state_;
    total_process_ns_ = other.total_process_ns_;
    block_count_ = other.block_count_;
    next_sequence_ = other.next_sequence_;
    slots_ = other.slots_;
    const int published = other.published_index_.load(std::memory_order_relaxed);
    published_index_.store(published, std::memory_order_relaxed);
    reading_index_.store(-1, std::memory_order_relaxed);
}

} // namespace iem
