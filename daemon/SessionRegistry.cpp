#include "SessionRegistry.h"

namespace viper::daemon {
namespace {

bool IsLifecycleEvent(DriverEventType type) noexcept {
    switch (type) {
        case DriverEventType::CONTEXT_CREATED:
        case DriverEventType::CONTEXT_CONFIGURED:
        case DriverEventType::CONTEXT_ENABLED:
        case DriverEventType::CONTEXT_DISABLED:
        case DriverEventType::CONTEXT_RELEASED:
        case DriverEventType::RESOURCE_GENERATION_CHANGED:
        case DriverEventType::TELEMETRY:
        case DriverEventType::RESCAN_RESPONSE:
            return true;
        default:
            return false;
    }
}

// Only these types may introduce a context the registry has not seen. Everything
// else describes an existing context and is dropped when the context is unknown.
bool CreatesContext(DriverEventType type) noexcept {
    return type == DriverEventType::CONTEXT_CREATED
        || type == DriverEventType::RESCAN_RESPONSE;
}

void ApplyObservations(ContextEntry &entry, const DriverEvent &event) noexcept {
    entry.audio_session_id = event.audio_session_id;
    entry.io_id = event.io_id;
    if (event.sample_rate != 0U) entry.sample_rate = event.sample_rate;
    if (event.channel_mask != 0U) entry.channel_mask = event.channel_mask;
    entry.resource_generation = event.resource_generation;
    entry.graph_generation = event.graph_generation;
    entry.bypass_reason = event.bypass_reason;
}

} // namespace

ApplyResult SessionRegistry::Apply(const DriverEvent &event) {
    if (!IsLifecycleEvent(event.type)) return ApplyResult::IGNORED;

    // A RESCAN_RESPONSE carrying no context id ends a replay. Context ids start at
    // 1 (DriverEventPublisher::next_context_instance_id_), so 0 is never a real
    // context and is safe as the terminator.
    //
    // Without an explicit end of replay the registry cannot distinguish "not
    // replayed yet" from "gone", so entries marked stale stay stale forever and the
    // registry reports contexts AudioFlinger no longer has.
    if (event.type == DriverEventType::RESCAN_RESPONSE
        && event.context_instance_id == 0U) {
        if (event.event_sequence > highest_sequence_) {
            highest_sequence_ = event.event_sequence;
        }
        RescanComplete(highest_sequence_);
        return ApplyResult::APPLIED;
    }

    const ContextKey key{event.boot_id, event.context_instance_id};
    auto entry = contexts_.find(key);
    if (entry == contexts_.end() && !CreatesContext(event.type)) {
        return ApplyResult::IGNORED;
    }

    // Replays carry a sequence already observed for this context.
    if (entry != contexts_.end() && event.event_sequence != 0U
        && event.event_sequence <= entry->second.last_sequence) {
        return ApplyResult::DUPLICATE;
    }

    bool gap = false;
    if (event.event_sequence != 0U) {
        // A rescan replays historical contexts with fresh sequences, so gaps are
        // only meaningful for the live event stream.
        if (highest_sequence_ != 0U && event.event_sequence > highest_sequence_ + 1U
            && event.type != DriverEventType::RESCAN_RESPONSE) {
            gap = true;
            rescan_needed_ = true;
        }
        if (event.event_sequence > highest_sequence_) {
            highest_sequence_ = event.event_sequence;
        }
    }

    if (event.type == DriverEventType::CONTEXT_RELEASED) {
        if (entry != contexts_.end()) contexts_.erase(entry);
        return gap ? ApplyResult::APPLIED_WITH_GAP : ApplyResult::APPLIED;
    }

    const bool inserted = entry == contexts_.end();
    if (inserted) {
        ContextEntry created{};
        created.key = key;
        created.created_sequence = event.event_sequence;
        entry = contexts_.emplace(key, created).first;
        // Both live CONTEXT_CREATED and RESCAN_RESPONSE can introduce an entry:
        // the latter is how a daemon that restarted rediscovers an existing owner.
        // Count either so both boot and daemon-restart restores are triggered.
        ++context_generation_;
        newest_created_sequence_ = event.event_sequence;
    }

    ContextEntry &record = entry->second;
    record.last_sequence = event.event_sequence;
    ApplyObservations(record, event);

    switch (event.type) {
        case DriverEventType::CONTEXT_CREATED:
            record.state = ContextState::CREATED;
            break;
        case DriverEventType::CONTEXT_CONFIGURED:
            record.state = ContextState::CONFIGURED;
            break;
        case DriverEventType::CONTEXT_ENABLED:
            record.state = ContextState::ACTIVE;
            break;
        case DriverEventType::CONTEXT_DISABLED:
            record.state = ContextState::INACTIVE;
            break;
        case DriverEventType::RESCAN_RESPONSE:
            // Replay carries the driver's current enable state, not a transition.
            record.state = event.enabled ? ContextState::ACTIVE : ContextState::INACTIVE;
            record.stale = false;
            break;
        case DriverEventType::RESOURCE_GENERATION_CHANGED:
        case DriverEventType::TELEMETRY:
            // Observation-only: the lifecycle state is untouched.
            break;
        default:
            break;
    }

    return gap ? ApplyResult::APPLIED_WITH_GAP : ApplyResult::APPLIED;
}

void SessionRegistry::MarkStaleAfter(uint64_t sequence) {
    rescan_started_sequence_ = sequence;
    // A rescan is outstanding from here until a terminator arrives. Callers gate
    // reconciliation on this, so leaving it clear made a requested rescan invisible
    // and its stale entries uncollectable.
    rescan_needed_ = true;
    for (auto &[key, entry] : contexts_) {
        if (entry.last_sequence <= sequence) entry.stale = true;
    }
}

void SessionRegistry::RescanComplete(uint64_t sequence) {
    for (auto entry = contexts_.begin(); entry != contexts_.end();) {
        // Anything still stale was not replayed, so the context no longer exists.
        if (entry->second.stale && entry->second.last_sequence <= sequence) {
            entry = contexts_.erase(entry);
        } else {
            entry->second.stale = false;
            ++entry;
        }
    }
    rescan_started_sequence_ = 0;
    rescan_needed_ = false;
}

std::vector<ContextEntry> SessionRegistry::Snapshot() const {
    std::vector<ContextEntry> entries;
    entries.reserve(contexts_.size());
    for (const auto &[key, entry] : contexts_) entries.push_back(entry);
    return entries;
}

} // namespace viper::daemon
