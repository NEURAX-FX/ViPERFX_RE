#pragma once

#include "DriverDaemonBridge.h"
#include "SnapshotApplyController.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace viper::audio {

/**
 * Process-wide owner of the daemon bridge and the live driver-context registry.
 *
 * Every method here runs on AudioFlinger command/lifecycle threads only. The
 * real-time Process() path must never call into this class.
 */
class DriverEventPublisher final {
public:
    // Process-wide instance used by the driver.
    static DriverEventPublisher &Instance();

    explicit DriverEventPublisher(std::string socket_name = daemon::kDriverSocketName);

    DriverEventPublisher(const DriverEventPublisher &) = delete;
    DriverEventPublisher &operator=(const DriverEventPublisher &) = delete;

    // Applies one snapshot command to a single driver context. Invoked on the
    // daemon bridge thread; ViperContext serializes internally.
    using SnapshotApplier = std::function<daemon::DriverDaemonBridge::SnapshotAck(
        daemon::SnapshotCommandType type,
        std::span<const uint8_t> payload
    )>;

    // Returns the driver-local context identity used by the daemon registry.
    // `applier` receives daemon snapshot commands for this context; a null
    // applier registers an observe-only context.
    uint64_t RegisterContext(
        uint32_t audio_session_id,
        uint32_t io_id,
        SnapshotApplier applier = nullptr
    );

    void PublishConfigured(
        uint64_t context_instance_id,
        uint32_t sample_rate,
        uint32_t channel_mask
    );

    void PublishEnabled(uint64_t context_instance_id, bool enabled);

    void PublishGenerations(
        uint64_t context_instance_id,
        uint64_t resource_generation,
        uint64_t graph_generation
    );

    // Bounded health telemetry. `bypass_reason` is the driver DisableReason code.
    void PublishTelemetry(uint64_t context_instance_id, uint32_t bypass_reason);

    void UnregisterContext(uint64_t context_instance_id);

    // Builds RESCAN_RESPONSE events so a restarted daemon can rebuild state.
    void CollectRescanEvents(std::vector<daemon::DriverEvent> *events);

    // Fans a snapshot command out to every registered context. Accepted only when
    // all of them accept, so the daemon never sees a partial apply as success.
    daemon::DriverDaemonBridge::SnapshotAck ApplySnapshotCommand(
        daemon::SnapshotCommandType type,
        std::span<const uint8_t> payload
    );

private:
    struct ContextRecord {
        uint32_t audio_session_id = 0;
        uint32_t io_id = 0;
        uint32_t sample_rate = 0;
        uint32_t channel_mask = 0;
        bool enabled = false;
        bool configured = false;
        uint64_t resource_generation = 0;
        uint64_t graph_generation = 0;
        uint32_t bypass_reason = 0;
        SnapshotApplier applier;
    };

    daemon::DriverEvent MakeEventLocked(
        daemon::DriverEventType type,
        uint64_t context_instance_id,
        const ContextRecord &record
    ) const;

    daemon::DriverDaemonBridge bridge_;
    std::mutex mutex_;
    std::unordered_map<uint64_t, ContextRecord> contexts_;
    uint64_t next_context_instance_id_ = 1;
    bool started_ = false;
};

} // namespace viper::audio
