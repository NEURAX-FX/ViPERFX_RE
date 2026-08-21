#pragma once

#include "AppEventServer.h"
#include "DriverEventServer.h"
#include "OwnerServer.h"
#include "OwnerSupervisor.h"
#include "RouteCache.h"
#include "RouteWatcher.h"
#include "SessionRegistry.h"
#include "SnapshotStore.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace viper::daemon {

struct DaemonConfig {
    std::filesystem::path state_root = "/data/adb/viper4android";
    std::string driver_socket_name = kDriverSocketName;
    std::string app_socket_name = kAppSocketName;
    std::chrono::milliseconds poll_interval{50};
    // 0 means run until stopped; host tests bound the loop.
    uint64_t max_iterations = 0;
    // A route must hold steady for this long before a snapshot is applied. Android
    // reports transient intermediate routes while an accessory settles, and
    // applying each one would thrash the DSP graph.
    std::chrono::milliseconds route_debounce{150};
    // 0 disables restore, leaving the runtime observe-only.
    bool restore_enabled = true;
    std::string owner_socket_name = owner::kOwnerSocketName;
    // Owner support is opt-in: an install without the owner dex must keep the
    // App's legacy backend working rather than binding a socket nothing can use.
    bool owner_enabled = false;
    // Literal path to the owner dex. Empty means the supervisor never spawns and
    // only adopts an owner started some other way.
    std::filesystem::path owner_dex_path;
};

struct DaemonStatus {
    bool driver_connected = false;
    bool route_known = false;
    std::string route_key_hash;
    std::size_t live_contexts = 0;
    uint64_t applied_events = 0;
    uint64_t route_changes = 0;
    uint64_t iterations = 0;
    // Route-restore counters.
    uint64_t restores_attempted = 0;
    uint64_t restores_accepted = 0;
    uint64_t restores_rejected = 0;
    uint64_t restores_bypassed = 0;
    uint64_t route_epoch = 0;
    uint64_t daemon_generation = 0;
    // Route announce outcome. `route_announces_acked` staying 0 while a route is
    // known means the driver never adopted it and is refusing snapshots.
    uint64_t route_announces_acked = 0;
    uint64_t route_announce_failures = 0;
    // App endpoint counters.
    bool app_connected = false;
    uint64_t app_hellos = 0;
    uint64_t app_route_reports = 0;
    uint64_t app_snapshot_commands = 0;
    uint64_t app_rejected_peers = 0;
    uint64_t app_rejected_frames = 0;
    // True when the route came from the App rather than a sysfs probe.
    bool route_from_app = false;
    // Owner process state. `owner_state=absent` while the daemon otherwise looks
    // healthy is what tells an operator nothing holds an effect handle.
    OwnerStatus owner{};
    uint64_t owner_commands_sent = 0;
    uint64_t owner_rejected_peers = 0;
};

/**
 * Daemon control loop.
 *
 * Tracks driver contexts and the current output route, and on a settled route
 * change applies that route's stored snapshot to the driver before the App has
 * started. It never creates an AudioEffect itself: AudioFlinger owns effect
 * lifetime, the owner process holds the client reference, and the driver owns the
 * graph.
 */
class DaemonRuntime final : private AppEventServer::Delegate,
                            private OwnerServer::Delegate {
public:
    // Route adapter may be null: this device has no usable sysfs route source, so
    // the runtime falls back to the App-reported route seeded from RouteCache.
    DaemonRuntime(DaemonConfig config, std::unique_ptr<RouteAdapter> route_adapter);

    DaemonRuntime(const DaemonRuntime &) = delete;
    DaemonRuntime &operator=(const DaemonRuntime &) = delete;

    // Binds the driver socket. Returns false with `error` set on failure.
    bool Start(std::string *error);

    // Runs until Stop() or the configured iteration bound.
    bool Run(std::string *error);

    void Stop() noexcept;

    // Single control-loop pass; exposed for deterministic host tests.
    void RunOnce();

    // Applies the stored snapshot for the current route, if any. Returns false when
    // nothing was applied; `bypassed` distinguishes "no snapshot" from "refused".
    bool RestoreCurrentRoute(bool *bypassed);

    // Monotonic per-route counter; increments on every settled route change.
    uint64_t RouteEpoch() const noexcept { return route_epoch_; }

    DaemonStatus Status() const;
    bool WriteStateFile(std::string *error) const;

    const SessionRegistry &Registry() const noexcept { return registry_; }
    const DriverEventServer &Server() const noexcept { return server_; }
    const RouteWatcher &Routes() const noexcept { return routes_; }
    const AppEventServer &AppServer() const noexcept { return app_server_; }
    const class OwnerServer &OwnerLink() const noexcept { return owner_server_; }

    // Owner ownership state. `OwnerReady()` is what the App uses to stop creating
    // its own effect, so it must mean "a handle exists", not "a socket exists".
    struct OwnerStatus OwnerState() const { return owner_supervisor_.Status(); }
    bool OwnerReady() const noexcept { return owner_supervisor_.Ready(); }

private:
    DaemonConfig config_;
    DriverEventServer server_;
    SessionRegistry registry_;
    RouteWatcher routes_;
    SnapshotStore store_;
    AppEventServer app_server_;
    OwnerServer owner_server_;
    OwnerSupervisor owner_supervisor_;
    RouteCache route_cache_;
    // Non-owning: `routes_` owns the adapter. Null when a caller supplied its own
    // adapter, in which case App route reports are cached but not applied locally.
    AppReportedRouteAdapter *app_route_adapter_ = nullptr;
    std::atomic<bool> running_{false};
    uint64_t iterations_ = 0;
    uint64_t route_changes_ = 0;
    // Owner connection epoch: OWN_SESSION is issued once per connection, so a
    // reconnected owner is commanded again but a live one is left alone.
    uint64_t owner_connections_seen_ = 0;
    uint64_t owner_commanded_connection_ = 0;

    // Streams a snapshot to the driver and waits for its ACK/NACK.
    bool ApplySnapshotToDriver(
        const Snapshot &snapshot,
        DriverEventServer::ApplyOutcome *outcome
    );

    // Polls until the driver answers `request_id`, or the bounded budget expires.
    DriverEventServer::ApplyOutcome AwaitApplyOutcome(uint64_t request_id);

    // True once the current route has been stable for config_.route_debounce.
    bool RouteSettled() const;

    // Tells the driver which route the daemon believes is live.
    //
    // The driver cannot derive this: it sees an AudioFlinger effect instance, not
    // a mixer route, so without an announce its route hash stays empty and it
    // refuses every snapshot as DEVICE_MISMATCH.
    bool AnnounceRouteToDriver();

    // AppEventServer::Delegate. Called from Poll() on the control loop thread.
    AppHelloAck OnHello(const AppHello &hello) override;
    bool OnRouteReport(
        const AppRouteReport &report,
        AppRouteAck *ack,
        std::string *error
    ) override;
    bool OnSnapshotCommand(
        SnapshotCommandType type,
        std::span<const uint8_t> payload,
        AppApplyResult *result
    ) override;

    // OwnerServer::Delegate. Called from Poll() on the control loop thread.
    owner::OwnerHelloAck OnOwnerHello(const owner::OwnerHello &hello) override;
    void OnOwned(const owner::Owned &owned) override;
    void OnOwnerFailed(const owner::OwnerFailed &failed) override;
    void OnReleased(const owner::Released &released) override;
    void OnSessionDelta(const owner::SessionDelta &delta) override;
    void OnOwnerDisconnected() override;

    // Issues OWN_SESSION once per owner connection. Re-issuing on every pass would
    // make the owner tear down and recreate a working handle.
    void DriveOwner();

    // Reads `owner_pid` from the state file this daemon's predecessor wrote, or 0
    // when there is none. Used only at startup, to find an owner that outlived the
    // previous daemon rather than replacing it with a duplicate.
    int ReadPublishedOwnerPid() const;

    // Reassembles the snapshot the App streams, so the daemon can persist whatever
    // the driver accepted. Nothing is written until the driver has ACKed the commit.
    void StageAppSnapshotCommand(
        SnapshotCommandType type,
        std::span<const uint8_t> payload
    );
    void CommitStagedAppSnapshot();

    std::vector<uint8_t> app_staged_bytes_;
    std::uint32_t app_staged_total_ = 0;
    std::uint32_t app_staged_crc_ = 0;
    bool app_staging_ = false;
    uint64_t app_store_failures_ = 0;

    std::chrono::steady_clock::time_point route_changed_at_{};
    bool route_restore_pending_ = false;
    // Latest daemon-local context generation already considered for restore. A
    // changed value means a newly created AudioEffect needs the current snapshot;
    // unlike driver event_sequence this survives reconnects.
    uint64_t restored_context_generation_ = 0;
    uint64_t route_epoch_ = 0;
    uint64_t daemon_generation_ = 1;
    // Last generation the App announced. Recorded for diagnostics only: the
    // daemon's own generation is the arbiter.
    uint64_t app_reported_generation_ = 0;
    uint64_t next_request_id_ = 1;

    // Route hash the driver was last told, and whether it still holds. A reconnected
    // driver is a fresh ViperContext with an empty hash, so the announce has to be
    // resent per connection, not per route change.
    std::string announced_route_hash_;
    bool driver_was_connected_ = false;
    // Retry budget for the announce, reset on each new driver connection.
    unsigned announce_attempts_ = 0;
    std::chrono::steady_clock::time_point announce_next_attempt_{};
    uint64_t route_announces_ = 0;
    uint64_t route_announce_failures_ = 0;
    uint64_t restores_attempted_ = 0;
    uint64_t restores_accepted_ = 0;
    uint64_t restores_rejected_ = 0;
    uint64_t restores_bypassed_ = 0;
};

// Installs SIGTERM/SIGINT handlers that stop `runtime`. Not thread safe; call
// once from main() before Run().
void InstallSignalHandlers(DaemonRuntime *runtime);

} // namespace viper::daemon
