#include "DriverEventPublisher.h"

#include <utility>

namespace viper::audio {

DriverEventPublisher &DriverEventPublisher::Instance() {
    static DriverEventPublisher instance;
    return instance;
}

DriverEventPublisher::DriverEventPublisher(std::string socket_name)
    : bridge_(std::move(socket_name)) {
    bridge_.SetRescanProvider([this](std::vector<daemon::DriverEvent> *events) {
        CollectRescanEvents(events);
    });
    bridge_.SetSnapshotHandler([this](
                                   daemon::SnapshotCommandType type,
                                   std::span<const uint8_t> payload
                               ) {
        return ApplySnapshotCommand(type, payload);
    });
}

daemon::DriverEvent DriverEventPublisher::MakeEventLocked(
    daemon::DriverEventType type,
    uint64_t context_instance_id,
    const ContextRecord &record
) const {
    daemon::DriverEvent event{};
    event.type = type;
    event.context_instance_id = context_instance_id;
    event.audio_session_id = record.audio_session_id;
    event.io_id = record.io_id;
    event.sample_rate = record.sample_rate;
    event.channel_mask = record.channel_mask;
    event.enabled = record.enabled;
    event.resource_generation = record.resource_generation;
    event.graph_generation = record.graph_generation;
    event.bypass_reason = record.bypass_reason;
    return event;
}

uint64_t DriverEventPublisher::RegisterContext(
    uint32_t audio_session_id,
    uint32_t io_id,
    SnapshotApplier applier
) {
    daemon::DriverEvent event{};
    uint64_t context_instance_id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) {
            bridge_.Start();
            started_ = true;
        }
        context_instance_id = next_context_instance_id_++;
        ContextRecord record{};
        record.audio_session_id = audio_session_id;
        record.io_id = io_id;
        record.applier = std::move(applier);
        contexts_.emplace(context_instance_id, record);
        event = MakeEventLocked(
            daemon::DriverEventType::CONTEXT_CREATED, context_instance_id, record);
    }
    bridge_.Publish(event);
    return context_instance_id;
}

void DriverEventPublisher::PublishConfigured(
    uint64_t context_instance_id,
    uint32_t sample_rate,
    uint32_t channel_mask
) {
    daemon::DriverEvent event{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto entry = contexts_.find(context_instance_id);
        if (entry == contexts_.end()) return;
        entry->second.sample_rate = sample_rate;
        entry->second.channel_mask = channel_mask;
        entry->second.configured = true;
        event = MakeEventLocked(
            daemon::DriverEventType::CONTEXT_CONFIGURED, context_instance_id, entry->second);
    }
    bridge_.Publish(event);
}

void DriverEventPublisher::PublishEnabled(uint64_t context_instance_id, bool enabled) {
    daemon::DriverEvent event{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto entry = contexts_.find(context_instance_id);
        if (entry == contexts_.end()) return;
        entry->second.enabled = enabled;
        event = MakeEventLocked(
            enabled ? daemon::DriverEventType::CONTEXT_ENABLED
                    : daemon::DriverEventType::CONTEXT_DISABLED,
            context_instance_id,
            entry->second
        );
    }
    bridge_.Publish(event);
}

void DriverEventPublisher::PublishGenerations(
    uint64_t context_instance_id,
    uint64_t resource_generation,
    uint64_t graph_generation
) {
    daemon::DriverEvent event{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto entry = contexts_.find(context_instance_id);
        if (entry == contexts_.end()) return;
        if (entry->second.resource_generation == resource_generation
            && entry->second.graph_generation == graph_generation) {
            return;
        }
        entry->second.resource_generation = resource_generation;
        entry->second.graph_generation = graph_generation;
        event = MakeEventLocked(
            daemon::DriverEventType::RESOURCE_GENERATION_CHANGED,
            context_instance_id,
            entry->second
        );
    }
    bridge_.Publish(event);
}

void DriverEventPublisher::PublishTelemetry(
    uint64_t context_instance_id,
    uint32_t bypass_reason
) {
    daemon::DriverEvent event{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto entry = contexts_.find(context_instance_id);
        if (entry == contexts_.end()) return;
        if (entry->second.bypass_reason == bypass_reason) return;
        entry->second.bypass_reason = bypass_reason;
        event = MakeEventLocked(
            daemon::DriverEventType::TELEMETRY, context_instance_id, entry->second);
    }
    bridge_.Publish(event);
}

void DriverEventPublisher::UnregisterContext(uint64_t context_instance_id) {
    daemon::DriverEvent event{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto entry = contexts_.find(context_instance_id);
        if (entry == contexts_.end()) return;
        entry->second.enabled = false;
        event = MakeEventLocked(
            daemon::DriverEventType::CONTEXT_RELEASED, context_instance_id, entry->second);
        contexts_.erase(entry);
    }
    bridge_.Publish(event);
}

void DriverEventPublisher::CollectRescanEvents(std::vector<daemon::DriverEvent> *events) {
    if (events == nullptr) return;
    std::lock_guard<std::mutex> lock(mutex_);
    events->reserve(contexts_.size() + 1U);
    for (const auto &[context_instance_id, record] : contexts_) {
        events->push_back(MakeEventLocked(
            daemon::DriverEventType::RESCAN_RESPONSE, context_instance_id, record));
    }

    // Close the replay with a context-less RESCAN_RESPONSE. Context ids start at 1,
    // so id 0 cannot collide with a real context and marks the end unambiguously.
    //
    // The daemon needs this to reconcile: until a replay ends it cannot tell a
    // context that is merely not replayed yet from one that no longer exists, so it
    // would keep reporting contexts AudioFlinger has already destroyed. Emitted even
    // with no live contexts, because "the driver has nothing" is exactly the case a
    // reconnecting daemon must be told about.
    daemon::DriverEvent terminator{};
    terminator.type = daemon::DriverEventType::RESCAN_RESPONSE;
    events->push_back(terminator);
}

daemon::DriverDaemonBridge::SnapshotAck DriverEventPublisher::ApplySnapshotCommand(
    daemon::SnapshotCommandType type,
    std::span<const uint8_t> payload
) {
    // Copy the appliers out before calling them: an applier runs driver code that
    // can register or release contexts, and holding mutex_ across that deadlocks.
    std::vector<SnapshotApplier> appliers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        appliers.reserve(contexts_.size());
        for (const auto &[context_instance_id, record] : contexts_) {
            if (record.applier) appliers.push_back(record.applier);
        }
    }

    daemon::DriverDaemonBridge::SnapshotAck ack{};
    if (appliers.empty()) {
        // No context can apply a snapshot yet. NACK with NOT_STAGING so the daemon
        // retries rather than assuming its state landed.
        ack.accepted = false;
        ack.error_code = static_cast<uint32_t>(ApplyError::NOT_STAGING);
        return ack;
    }

    // The first result of the decisive kind wins. A NACK is decisive over any
    // ACK, because a partial apply is not a success. Whichever result is kept,
    // all of its fields travel together: an accepted result reports the applied
    // generations, a rejected one echoes the requested ones plus the error code,
    // and an accepted abort still carries its reason.
    ack.accepted = true;
    bool have_result = false;
    for (const auto &applier : appliers) {
        const daemon::DriverDaemonBridge::SnapshotAck one = applier(type, payload);
        const bool decisive = !one.accepted && ack.accepted;
        if (decisive || !have_result) {
            ack = one;
            have_result = true;
        }
    }
    return ack;
}

} // namespace viper::audio
