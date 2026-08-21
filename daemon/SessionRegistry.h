#pragma once

#include "DriverEvent.h"

#include <cstdint>
#include <map>
#include <vector>

namespace viper::daemon {

enum class ContextState {
    CREATED,
    CONFIGURED,
    ACTIVE,
    INACTIVE,
    RELEASED,
};

// Context identity is (boot_id, context_instance_id): instance ids restart at 1
// on every driver load, so the boot id keeps stale entries from colliding.
struct ContextKey {
    uint64_t boot_id = 0;
    uint64_t context_instance_id = 0;

    friend bool operator<(const ContextKey &left, const ContextKey &right) noexcept {
        if (left.boot_id != right.boot_id) return left.boot_id < right.boot_id;
        return left.context_instance_id < right.context_instance_id;
    }

    friend bool operator==(const ContextKey &left, const ContextKey &right) noexcept {
        return left.boot_id == right.boot_id
            && left.context_instance_id == right.context_instance_id;
    }
};

struct ContextEntry {
    ContextKey key;
    ContextState state = ContextState::CREATED;
    uint32_t audio_session_id = 0;
    uint32_t io_id = 0;
    uint32_t sample_rate = 0;
    uint32_t channel_mask = 0;
    uint64_t resource_generation = 0;
    uint64_t graph_generation = 0;
    uint32_t bypass_reason = 0;
    uint64_t last_sequence = 0;
    // Sequence of the event that introduced this entry. The daemon compares this
    // against the point it last considered for restore, so a context that appears
    // after a restore can be given the route's state without waiting for a route
    // change.
    uint64_t created_sequence = 0;
    bool stale = false;
};

enum class ApplyResult {
    APPLIED,
    // A sequence gap means events were dropped; the registry needs a rescan.
    APPLIED_WITH_GAP,
    // Replays of already-observed sequences are ignored.
    DUPLICATE,
    IGNORED,
};

/**
 * Logical view of driver contexts, rebuilt from bridge events.
 *
 * The registry never creates or releases an AudioEffect; AudioFlinger owns
 * effect lifetime. Entries here are observations only.
 */
class SessionRegistry final {
public:
    ApplyResult Apply(const DriverEvent &event);

    // Marks every entry observed at or before `sequence` as needing confirmation
    // by a rescan. Used after a detected gap or reconnect.
    void MarkStaleAfter(uint64_t sequence);

    // Drops entries not refreshed by the rescan that ended at `sequence`.
    void RescanComplete(uint64_t sequence);

    std::vector<ContextEntry> Snapshot() const;

    bool RescanNeeded() const noexcept { return rescan_needed_; }
    uint64_t HighestSequence() const noexcept { return highest_sequence_; }
    std::size_t Size() const noexcept { return contexts_.size(); }

    // Number of live-stream CONTEXT_CREATED events observed. Unlike the driver's
    // event sequence this is daemon-local and does not reset when the driver
    // reconnects, so it is safe for restore arbitration across owner restarts.
    uint64_t ContextGeneration() const noexcept { return context_generation_; }

    // Sequence of the most recently introduced context, or 0 when none was ever
    // seen. Kept for diagnostics and tests; restore arbitration uses the daemon-local
    // generation above because driver sequence numbers restart on reconnect.
    uint64_t NewestCreatedSequence() const noexcept { return newest_created_sequence_; }

private:
    std::map<ContextKey, ContextEntry> contexts_;
    uint64_t highest_sequence_ = 0;
    uint64_t rescan_started_sequence_ = 0;
    bool rescan_needed_ = false;
    uint64_t context_generation_ = 0;
    uint64_t newest_created_sequence_ = 0;
};

} // namespace viper::daemon
