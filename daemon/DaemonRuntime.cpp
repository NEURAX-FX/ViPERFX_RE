#include "DaemonRuntime.h"

#include "OwnerProcess.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <csignal>

namespace viper::daemon {
namespace {

// Bounded wait for a driver apply result: a driver that never answers must not
// wedge the control loop.
constexpr unsigned kApplyPollAttempts = 200;
constexpr std::chrono::milliseconds kApplyPollInterval{5};

// Mirrors viper::audio::ApplyError::NOT_STAGING. Spelled as a constant here
// because ApplyError lives in the driver headers, which the daemon does not link.
constexpr uint32_t kAppApplyErrorNotStaging = 1;

// Announce retry budget per driver connection. A driver too old to know
// ROUTE_ANNOUNCE drops it silently, so each attempt costs a full
// AwaitApplyOutcome timeout; without a bound the control loop would do nothing else.
constexpr unsigned kMaxAnnounceAttempts = 5;
constexpr std::chrono::seconds kAnnounceRetryDelay{2};

DaemonRuntime *g_signal_target = nullptr;

void HandleStopSignal(int) {
    if (g_signal_target != nullptr) g_signal_target->Stop();
}

// Published in daemon.state, so these strings are a compatibility surface: the
// App matches on them and an older App must be able to ignore an unknown value.
const char *OwnerStateName(OwnerState state) noexcept {
    switch (state) {
        case OwnerState::ABSENT: return "absent";
        case OwnerState::STARTING: return "starting";
        case OwnerState::OWNED: return "owned";
        case OwnerState::FAILED: return "failed";
    }
    return "absent";
}

void SetError(std::string *error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}

} // namespace

DaemonRuntime::DaemonRuntime(
    DaemonConfig config,
    std::unique_ptr<RouteAdapter> route_adapter
)
    : config_(std::move(config)),
      server_(config_.driver_socket_name),
      routes_(nullptr),
      store_(config_.state_root),
      app_server_(config_.app_socket_name),
      owner_server_(config_.owner_socket_name),
      owner_supervisor_(
          MakeOwnerProcessAdapter(config_.owner_dex_path, config_.owner_socket_name)),
      route_cache_(config_.state_root) {
    if (route_adapter != nullptr) {
        routes_ = RouteWatcher(std::move(route_adapter));
    } else {
        // No usable sysfs route source on this class of device, so the App names the
        // route. Seeding from the cache is what makes a boot-time restore possible
        // before the App has run at all.
        DeviceIdentity cached{};
        std::string cache_error;
        std::optional<DeviceIdentity> seed;
        if (route_cache_.Load(&cached, &cache_error)) seed = std::move(cached);

        auto adapter = std::make_unique<AppReportedRouteAdapter>(std::move(seed));
        app_route_adapter_ = adapter.get();
        routes_ = RouteWatcher(std::move(adapter));
    }
    app_server_.SetDelegate(this);
    owner_server_.SetDelegate(this);
}

bool DaemonRuntime::Start(std::string *error) {
    std::error_code code;
    std::filesystem::create_directories(config_.state_root, code);
    if (code) {
        SetError(error, "failed to create daemon state root: " + code.message());
        return false;
    }
    if (!server_.Listen(error)) return false;

    // Resolve the seeded route now rather than on the first RunOnce(): Status() and
    // the state file are read immediately after Start(), and reporting "no route"
    // while a valid cache exists would look like the cache had failed. A settled
    // route here is also what arms the boot-time restore.
    std::string route_error;
    if (routes_.Poll(&route_error)) {
        ++route_changes_;
        ++route_epoch_;
        route_changed_at_ = std::chrono::steady_clock::now();
        route_restore_pending_ = config_.restore_enabled;
    }

    // Owner support is opt-in. Binding the endpoint when the owner is disabled
    // would advertise a socket nothing can answer, and an operator reading
    // daemon.state could not tell that apart from a live owner.
    if (config_.owner_enabled) {
        std::string owner_error;
        if (!owner_server_.Listen(&owner_error)) {
            // Not fatal: without an owner the App's legacy backend still applies
            // state, so the daemon reports the degradation instead of exiting.
            SetError(error, "driver socket bound but owner socket failed: " + owner_error);
        }
        // The owner keeps its effect handle across a daemon restart, so the pid the
        // previous daemon published may still be holding it. Spawning
        // unconditionally here would create a second owner and a duplicate
        // AudioFlinger module; a stale pid is rejected by the supervisor.
        const int survivor = ReadPublishedOwnerPid();
        if (survivor > 0) owner_supervisor_.SeedSurvivingOwner(survivor);
    }

    // A failure to bind the App endpoint must not take the daemon down: route
    // restore from the cache still works, and the App falls back to its direct
    // driver path.
    std::string app_error;
    if (!app_server_.Listen(&app_error)) {
        SetError(error, "driver socket bound but app socket failed: " + app_error);
        return true;
    }
    return true;
}

bool DaemonRuntime::RouteSettled() const {
    if (config_.route_debounce.count() <= 0) return true;
    return std::chrono::steady_clock::now() - route_changed_at_ >= config_.route_debounce;
}

void DaemonRuntime::RunOnce() {
    server_.Poll(&registry_);
    // Polled before the route check so a route the App reports in this pass is
    // acted on immediately rather than one interval later.
    app_server_.Poll();
    // Owner polling precedes restore arbitration: an owner that just created a
    // handle produces a driver context in the same pass, and the restore decision
    // below is what fills it.
    owner_server_.Poll();
    DriveOwner();

    std::string route_error;
    if (routes_.Poll(&route_error)) {
        ++route_changes_;
        // A new route supersedes any restore still waiting to settle, so the epoch
        // moves and the debounce window restarts.
        ++route_epoch_;
        route_changed_at_ = std::chrono::steady_clock::now();
        route_restore_pending_ = config_.restore_enabled;
    }

    // A reconnected driver is a brand new ViperContext with an empty route hash, so
    // the announce is keyed on the connection edge as well as the route. Announcing
    // only on route change would leave a driver that loaded after the last change
    // refusing every snapshot as DEVICE_MISMATCH forever.
    //
    // Retries are bounded per connection and spaced out. A driver too old to know
    // ROUTE_ANNOUNCE drops it without answering, so each attempt costs a full
    // AwaitApplyOutcome timeout; retrying every iteration forever would spend the
    // control loop doing nothing else. Giving up leaves the route unannounced, which
    // is visible as route_announce_failures in the state file.
    const bool driver_connected = server_.Connected();
    const bool new_driver_connection = driver_connected && !driver_was_connected_;
    if (driver_connected) {
        if (new_driver_connection) {
            announced_route_hash_.clear();
            announce_attempts_ = 0;
            announce_next_attempt_ = std::chrono::steady_clock::time_point{};
        }
        const bool announce_needed = announced_route_hash_ != routes_.CurrentKeyHash();
        const bool budget_left = announce_attempts_ < kMaxAnnounceAttempts;
        const bool due = std::chrono::steady_clock::now() >= announce_next_attempt_;
        if (announce_needed && budget_left && due) {
            ++announce_attempts_;
            if (AnnounceRouteToDriver()) {
                announce_attempts_ = 0;
            } else {
                announce_next_attempt_ =
                    std::chrono::steady_clock::now() + kAnnounceRetryDelay;
            }
        }
    } else {
        announced_route_hash_.clear();
        announce_attempts_ = 0;
        announce_next_attempt_ = std::chrono::steady_clock::time_point{};
    }
    // Commit the edge only after announce handling. Omitting this assignment makes
    // every control-loop pass look like a new connection and resets the bounded
    // retry budget forever.
    driver_was_connected_ = driver_connected;
    // A route change arms the initial restore. A new driver connection arms a
    // restore even when the route is unchanged: this is the owner-process boot and
    // respawn path. Registry generation covers a context discovered after the
    // connection edge; the edge itself covers the owner reconnect race.
    const bool new_context_ready =
        registry_.ContextGeneration() != restored_context_generation_
        && registry_.Size() > 0;
    const bool route_announced =
        announced_route_hash_ == routes_.CurrentKeyHash();
    if (config_.restore_enabled
        && (route_restore_pending_ || new_driver_connection || new_context_ready)
        && driver_connected && route_announced && RouteSettled()) {
        route_restore_pending_ = false;
        bool bypassed = false;
        RestoreCurrentRoute(&bypassed);
        restored_context_generation_ = registry_.ContextGeneration();
    }

    ++iterations_;
}

bool DaemonRuntime::Run(std::string *error) {
    if (!server_.Listening()) {
        SetError(error, "daemon started without a bound driver socket");
        return false;
    }

    running_.store(true, std::memory_order_release);
    while (running_.load(std::memory_order_acquire)) {
        RunOnce();
        if (!WriteStateFile(error)) return false;
        if (config_.max_iterations != 0U && iterations_ >= config_.max_iterations) break;
        std::this_thread::sleep_for(config_.poll_interval);
    }
    running_.store(false, std::memory_order_release);
    return true;
}

void DaemonRuntime::Stop() noexcept { running_.store(false, std::memory_order_release); }

DaemonStatus DaemonRuntime::Status() const {
    DaemonStatus status{};
    status.driver_connected = server_.Connected();
    status.route_known = routes_.HasRoute();
    status.route_key_hash = routes_.CurrentKeyHash();
    status.live_contexts = registry_.Size();
    status.applied_events = server_.Statistics().applied_events;
    status.route_changes = route_changes_;
    status.iterations = iterations_;
    status.restores_attempted = restores_attempted_;
    status.restores_accepted = restores_accepted_;
    status.restores_rejected = restores_rejected_;
    status.restores_bypassed = restores_bypassed_;
    status.route_epoch = route_epoch_;
    status.daemon_generation = daemon_generation_;
    status.route_announces_acked = route_announces_;
    status.route_announce_failures = route_announce_failures_;

    const AppEventServer::Stats &app = app_server_.Statistics();
    status.app_connected = app_server_.Connected();
    status.app_hellos = app.hellos;
    status.app_route_reports = app.route_reports;
    status.app_snapshot_commands = app.snapshot_commands;
    status.app_rejected_peers = app.rejected_peers;
    status.app_rejected_frames = app.rejected_frames;
    const OwnerServer::Stats &owner_stats = owner_server_.Statistics();
    status.owner = owner_supervisor_.Status();
    status.owner_commands_sent = owner_stats.commands_sent;
    status.owner_rejected_peers = owner_stats.rejected_peers;
    // A route the App named is the only kind this device can produce, so the App
    // can tell a cached/reported route apart from a sysfs-probed one.
    status.route_from_app = app_route_adapter_ != nullptr && routes_.HasRoute();
    return status;
}

bool DaemonRuntime::WriteStateFile(std::string *error) const {
    const std::filesystem::path path = config_.state_root / "daemon.state";
    const std::filesystem::path temporary = config_.state_root / "daemon.state.tmp";

    const DaemonStatus status = Status();
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            SetError(error, "failed to open daemon state file");
            return false;
        }
        output << "mode=" << (config_.restore_enabled ? "route-restore" : "observe-only") << '\n'
               << "driver_connected=" << (status.driver_connected ? 1 : 0) << '\n'
               << "route_known=" << (status.route_known ? 1 : 0) << '\n'
               << "route_key_hash=" << status.route_key_hash << '\n'
               << "route_epoch=" << status.route_epoch << '\n'
               << "daemon_generation=" << status.daemon_generation << '\n'
               << "live_contexts=" << status.live_contexts << '\n'
               << "applied_events=" << status.applied_events << '\n'
               << "rejected_frames=" << server_.Statistics().rejected_frames << '\n'
               << "rescan_requests=" << server_.Statistics().rescan_requests_sent << '\n'
               << "snapshot_commands=" << server_.Statistics().snapshot_commands_sent << '\n'
               // Route announces are the fix for the driver having no route of its
               // own. If this stays 0 while a route is known, the driver is still
               // refusing snapshots as DEVICE_MISMATCH.
               << "route_announces=" << server_.Statistics().route_announces_sent << '\n'
               << "route_announces_acked=" << route_announces_ << '\n'
               << "route_announce_failures=" << route_announce_failures_ << '\n'
               << "apply_acks=" << server_.Statistics().apply_acks << '\n'
               << "apply_nacks=" << server_.Statistics().apply_nacks << '\n'
               << "restores_attempted=" << status.restores_attempted << '\n'
               << "restores_accepted=" << status.restores_accepted << '\n'
               << "restores_rejected=" << status.restores_rejected << '\n'
               << "restores_bypassed=" << status.restores_bypassed << '\n'
               << "route_from_app=" << (status.route_from_app ? 1 : 0) << '\n'
               << "app_listening=" << (app_server_.Listening() ? 1 : 0) << '\n'
               << "app_connected=" << (status.app_connected ? 1 : 0) << '\n'
               << "app_hellos=" << status.app_hellos << '\n'
               << "app_route_reports=" << status.app_route_reports << '\n'
               << "app_snapshot_commands=" << status.app_snapshot_commands << '\n'
               << "app_rejected_peers=" << status.app_rejected_peers << '\n'
               << "app_rejected_frames=" << status.app_rejected_frames << '\n'
               << "route_changes=" << status.route_changes << '\n'
               // Owner state is the difference between "daemon running" and
               // "something owns an effect handle". Without it a healthy-looking
               // daemon says nothing about whether audio is actually processed.
               << "owner_enabled=" << (config_.owner_enabled ? 1 : 0) << '\n'
               << "owner_listening=" << (owner_server_.Listening() ? 1 : 0) << '\n'
               << "owner_connected=" << (owner_server_.Connected() ? 1 : 0) << '\n'
               << "owner_state=" << OwnerStateName(status.owner.state) << '\n'
               << "owner_pid=" << status.owner.pid << '\n'
               << "owner_effect_id=" << status.owner.effect_id << '\n'
               << "owner_has_control=" << (status.owner.has_control ? 1 : 0) << '\n'
               << "owner_restarts=" << status.owner.restarts << '\n'
               << "owner_spawn_failures=" << status.owner.spawn_failures << '\n'
               << "owner_failure_reason=" << status.owner.last_failure_reason << '\n'
               << "owner_commands_sent=" << status.owner_commands_sent << '\n'
               << "owner_rejected_peers=" << status.owner_rejected_peers << '\n'
               << "tracked_sessions=" << status.owner.tracked_sessions << '\n'
               << "iterations=" << status.iterations << '\n';
        if (!output) {
            SetError(error, "failed to write daemon state file");
            return false;
        }
    }

    std::error_code code;
    std::filesystem::rename(temporary, path, code);
    if (code) {
        SetError(error, "failed to publish daemon state file: " + code.message());
        return false;
    }
    if (error != nullptr) error->clear();
    return true;
}

bool DaemonRuntime::ApplySnapshotToDriver(
    const Snapshot &snapshot,
    DriverEventServer::ApplyOutcome *outcome
) {
    std::vector<uint8_t> bytes;
    std::string error;
    if (!EncodeSnapshot(snapshot, &bytes, &error)) return false;

    const uint64_t request_id = next_request_id_++;

    SnapshotBegin begin{};
    begin.app_generation = snapshot.app_generation;
    begin.daemon_generation = snapshot.daemon_generation;
    begin.total_size = static_cast<uint32_t>(bytes.size());
    begin.crc32 = Crc32(bytes);
    begin.device_key_hash = snapshot.device_key_hash;

    std::vector<uint8_t> payload;
    if (!EncodeSnapshotBegin(begin, &payload, &error)) return false;
    if (!server_.SendSnapshotCommand(
            SnapshotCommandType::SNAPSHOT_BEGIN, payload, request_id)) {
        return false;
    }
    // BEGIN is answered before streaming: a refusal here means the driver cannot
    // take this snapshot at all, so sending megabytes would be wasted.
    DriverEventServer::ApplyOutcome begin_outcome = AwaitApplyOutcome(request_id);
    if (!begin_outcome.valid || !begin_outcome.accepted) {
        if (outcome != nullptr) *outcome = begin_outcome;
        return false;
    }

    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const std::size_t size =
            std::min<std::size_t>(kMaxSnapshotChunkBytes, bytes.size() - offset);
        SnapshotChunk chunk{};
        chunk.offset = static_cast<uint32_t>(offset);
        chunk.data.assign(
            bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            bytes.begin() + static_cast<std::ptrdiff_t>(offset + size)
        );
        if (!EncodeSnapshotChunk(chunk, &payload, &error)) return false;
        if (!server_.SendSnapshotCommand(
                SnapshotCommandType::SNAPSHOT_CHUNK, payload, request_id)) {
            return false;
        }
        offset += size;
    }

    SnapshotCommit commit{};
    commit.app_generation = snapshot.app_generation;
    commit.daemon_generation = snapshot.daemon_generation;
    if (!EncodeSnapshotCommit(commit, &payload, &error)) return false;
    if (!server_.SendSnapshotCommand(
            SnapshotCommandType::SNAPSHOT_COMMIT, payload, request_id)) {
        return false;
    }

    const DriverEventServer::ApplyOutcome commit_outcome = AwaitApplyOutcome(request_id);
    if (outcome != nullptr) *outcome = commit_outcome;
    return commit_outcome.valid && commit_outcome.accepted;
}

DriverEventServer::ApplyOutcome DaemonRuntime::AwaitApplyOutcome(uint64_t request_id) {
    // Bounded: a driver that never answers must not wedge the control loop.
    for (unsigned attempt = 0; attempt < kApplyPollAttempts; ++attempt) {
        server_.Poll(&registry_);
        const DriverEventServer::ApplyOutcome outcome = server_.TakeApplyOutcome();
        if (outcome.valid && outcome.request_id == request_id) return outcome;
        std::this_thread::sleep_for(kApplyPollInterval);
    }
    return DriverEventServer::ApplyOutcome{};
}

bool DaemonRuntime::AnnounceRouteToDriver() {
    RouteAnnounce announce{};
    announce.device_key_hash = routes_.CurrentKeyHash();

    std::vector<uint8_t> payload;
    std::string error;
    if (!EncodeRouteAnnounce(announce, &payload, &error)) {
        ++route_announce_failures_;
        return false;
    }

    const uint64_t request_id = next_request_id_++;
    if (!server_.SendSnapshotCommand(
            SnapshotCommandType::ROUTE_ANNOUNCE, payload, request_id)) {
        ++route_announce_failures_;
        return false;
    }

    const DriverEventServer::ApplyOutcome outcome = AwaitApplyOutcome(request_id);
    if (!outcome.valid || !outcome.accepted) {
        // Left unrecorded so the next RunOnce() retries: a driver that refused or
        // never answered still has no route, and silently giving up would strand it
        // rejecting every snapshot.
        ++route_announce_failures_;
        return false;
    }

    announced_route_hash_ = announce.device_key_hash;
    ++route_announces_;
    return true;
}

bool DaemonRuntime::RestoreCurrentRoute(bool *bypassed) {
    if (bypassed != nullptr) *bypassed = false;
    if (!routes_.HasRoute()) {
        // Without a route key there is no snapshot to choose; bypass rather than
        // guess and apply another device's state.
        ++restores_bypassed_;
        if (bypassed != nullptr) *bypassed = true;
        return false;
    }

    // current.snapshot first, then previous.snapshot. The store keeps both files so
    // an interrupted write cannot cost the user their settings; loading only the
    // current one would throw that guarantee away and look like the daemon had
    // simply forgotten the route.
    Snapshot snapshot{};
    std::string error;
    const std::string_view route_hash = routes_.CurrentKeyHash();
    bool loaded = store_.LoadCurrent(route_hash, &snapshot, &error);
    if (!loaded || snapshot.device_key_hash != route_hash) {
        Snapshot fallback{};
        std::string fallback_error;
        if (store_.LoadPrevious(route_hash, &fallback, &fallback_error)
            && fallback.device_key_hash == route_hash) {
            snapshot = std::move(fallback);
            loaded = true;
        } else {
            loaded = false;
        }
    }
    if (!loaded) {
        // Nothing usable for this route: the driver keeps its defaults, which is the
        // safe bypass. The device-hash re-check above also runs on the fallback, so a
        // forged previous.snapshot cannot become a second inheritance path.
        ++restores_bypassed_;
        if (bypassed != nullptr) *bypassed = true;
        return false;
    }

    // The daemon owns the generation it applies under, so the App can detect that
    // the daemon moved ahead and reconcile instead of overwriting blindly.
    ++daemon_generation_;
    snapshot.daemon_generation = daemon_generation_;

    ++restores_attempted_;
    DriverEventServer::ApplyOutcome outcome{};
    if (!ApplySnapshotToDriver(snapshot, &outcome)) {
        ++restores_rejected_;
        return false;
    }
    ++restores_accepted_;
    return true;
}

AppHelloAck DaemonRuntime::OnHello(const AppHello &hello) {
    AppHelloAck ack{};
    ack.daemon_generation = daemon_generation_;
    ack.route_epoch = route_epoch_;
    ack.route_key_hash = routes_.CurrentKeyHash();
    if (config_.restore_enabled) ack.flags |= kAppFlagRestoreEnabled;
    if (server_.Connected()) ack.flags |= kAppFlagDriverConnected;
    if (routes_.HasRoute()) ack.flags |= kAppFlagRouteKnown;

    // The App's generation is reported, not adopted: the daemon's own generation is
    // the arbiter, and letting a client push it forward would let a stale App
    // invalidate a restore the daemon already performed.
    if (hello.app_generation > 0U) {
        app_reported_generation_ = hello.app_generation;
    }
    return ack;
}

bool DaemonRuntime::OnRouteReport(
    const AppRouteReport &report,
    AppRouteAck *ack,
    std::string *error
) {
    DeviceIdentity identity{};
    identity.route_type = report.route_type;
    identity.stable_address_or_port = report.stable_address_or_port;
    identity.product_name = report.product_name;
    identity.encoding = report.encoding;
    identity.sample_rate = report.sample_rate;
    identity.channel_mask = report.channel_mask;
    identity.output_flags = report.output_flags;

    if (ack != nullptr) {
        ack->daemon_generation = daemon_generation_;
        ack->route_epoch = route_epoch_;
        ack->route_key_hash = routes_.CurrentKeyHash();
    }

    if (!IsValidDeviceIdentity(identity)) {
        SetError(error, "app reported an unusable route identity");
        return false;
    }
    if (app_route_adapter_ == nullptr) {
        // A caller supplied its own adapter, so the local route is not the App's to
        // set. Caching still helps the next boot, but claiming acceptance would
        // promise a restore that cannot happen.
        SetError(error, "daemon is not using the app-reported route source");
        return false;
    }

    app_route_adapter_->SetReportedRoute(identity);

    // Persist before the route takes effect: a reboot right after this must find
    // the same route the App just named, which is the whole point of the cache.
    std::string cache_error;
    if (!route_cache_.Store(identity, &cache_error)) {
        // The live route still applies; only the boot-time memory is lost.
        SetError(error, "route accepted but not cached: " + cache_error);
    }

    std::string route_error;
    if (routes_.Poll(&route_error)) {
        ++route_changes_;
        ++route_epoch_;
        route_changed_at_ = std::chrono::steady_clock::now();
        route_restore_pending_ = config_.restore_enabled;
    }

    if (ack != nullptr) {
        ack->accepted = true;
        ack->route_epoch = route_epoch_;
        ack->route_key_hash = routes_.CurrentKeyHash();
    }
    return true;
}

bool DaemonRuntime::OnSnapshotCommand(
    SnapshotCommandType type,
    std::span<const uint8_t> payload,
    AppApplyResult *result
) {
    if (result != nullptr) {
        result->daemon_generation = daemon_generation_;
    }

    // Staged alongside the relay so the daemon ends up owning a copy of whatever the
    // driver accepted. Without this the App's state would live only in the driver's
    // memory and every reboot would come back to defaults, which is exactly what
    // route restore exists to prevent.
    StageAppSnapshotCommand(type, payload);

    if (!server_.Connected()) {
        // No driver to apply to. Reported as NOT_STAGING so the App retries later
        // instead of believing its state landed.
        if (result != nullptr) {
            result->error_code = kAppApplyErrorNotStaging;
        }
        return false;
    }

    const uint64_t request_id = next_request_id_++;
    if (!server_.SendSnapshotCommand(type, payload, request_id)) {
        if (result != nullptr) {
            result->error_code = kAppApplyErrorNotStaging;
        }
        return false;
    }

    // A chunk that the driver accepts produces no reply, so waiting for one would
    // stall the whole control loop for every chunk of a multi-megabyte snapshot.
    if (type == SnapshotCommandType::SNAPSHOT_CHUNK) {
        if (result != nullptr) result->accepted = true;
        return true;
    }

    const DriverEventServer::ApplyOutcome outcome = AwaitApplyOutcome(request_id);
    const bool accepted = outcome.valid && outcome.accepted;
    if (result != nullptr) {
        result->accepted = accepted;
        result->error_code = outcome.error_code;
        result->app_generation = outcome.app_generation;
        result->resource_generation = outcome.resource_generation;
        result->graph_generation = outcome.graph_generation;
        result->daemon_generation = daemon_generation_;
    }

    if (accepted && type == SnapshotCommandType::SNAPSHOT_COMMIT) {
        // An App apply that the driver accepted moves the daemon's generation too, so
        // a later daemon restore cannot silently regress past what the user applied.
        ++daemon_generation_;
        if (result != nullptr) result->daemon_generation = daemon_generation_;
        CommitStagedAppSnapshot();
    }
    if (!accepted || type == SnapshotCommandType::SNAPSHOT_ABORT) {
        // A refused or abandoned transfer must not leave bytes that a later commit
        // could accidentally persist.
        app_staged_bytes_.clear();
        app_staging_ = false;
    }
    return accepted;
}

void DaemonRuntime::StageAppSnapshotCommand(
    SnapshotCommandType type,
    std::span<const uint8_t> payload
) {
    std::string error;
    switch (type) {
        case SnapshotCommandType::SNAPSHOT_BEGIN: {
            SnapshotBegin begin{};
            if (!DecodeSnapshotBegin(payload, &begin, &error)) return;
            app_staged_bytes_.clear();
            app_staged_bytes_.reserve(begin.total_size);
            app_staged_total_ = begin.total_size;
            app_staged_crc_ = begin.crc32;
            app_staging_ = true;
            return;
        }
        case SnapshotCommandType::SNAPSHOT_CHUNK: {
            if (!app_staging_) return;
            SnapshotChunk chunk{};
            if (!DecodeSnapshotChunk(payload, &chunk, &error)) {
                app_staging_ = false;
                return;
            }
            // Strictly sequential, exactly as the driver requires: a gap would stage
            // uninitialized bytes and persist a corrupt snapshot.
            if (chunk.offset != app_staged_bytes_.size()) {
                app_staging_ = false;
                return;
            }
            app_staged_bytes_.insert(
                app_staged_bytes_.end(), chunk.data.begin(), chunk.data.end());
            return;
        }
        default:
            return;
    }
}

void DaemonRuntime::CommitStagedAppSnapshot() {
    if (!app_staging_) return;
    app_staging_ = false;

    // Validate the reassembled bytes independently of the driver: the store is the
    // thing a future boot trusts blindly, so it must never hold something unverified.
    if (app_staged_bytes_.size() != app_staged_total_
        || Crc32(app_staged_bytes_) != app_staged_crc_) {
        app_staged_bytes_.clear();
        return;
    }

    Snapshot snapshot{};
    std::string error;
    if (!DecodeSnapshot(app_staged_bytes_, &snapshot, &error)) {
        app_staged_bytes_.clear();
        return;
    }
    app_staged_bytes_.clear();

    // Only the route the daemon believes is live may be written, otherwise a stale
    // or hostile App could file state under another device's key.
    if (snapshot.device_key_hash != routes_.CurrentKeyHash()) return;

    // Stored under the generation it was applied at, so the next boot restores at a
    // generation the App will not immediately consider stale.
    snapshot.daemon_generation = daemon_generation_;
    if (!store_.Commit(snapshot.device_key_hash, snapshot, &error)) {
        ++app_store_failures_;
    }
}

owner::OwnerHelloAck DaemonRuntime::OnOwnerHello(const owner::OwnerHello &hello) {
    owner::OwnerHelloAck ack{};
    // The generation is never zero, so the owner can tell a real ack from a
    // zero-initialised buffer.
    ack.daemon_generation = daemon_generation_;
    ack.accepted = config_.owner_enabled;
    if (ack.accepted) {
        ++owner_connections_seen_;
        owner_supervisor_.OnConnected(static_cast<int>(hello.owner_pid));
    }
    return ack;
}

void DaemonRuntime::OnOwned(const owner::Owned &owned) {
    owner_supervisor_.OnOwned(owned);
}

void DaemonRuntime::OnOwnerFailed(const owner::OwnerFailed &failed) {
    owner_supervisor_.OnOwnerFailed(failed);
}

void DaemonRuntime::OnReleased(const owner::Released &released) {
    owner_supervisor_.OnReleased(released);
    // A released handle can be asked for again on the same connection, so the
    // command epoch is rewound rather than the connection being counted as new.
    owner_commanded_connection_ = 0;
}

void DaemonRuntime::OnSessionDelta(const owner::SessionDelta &delta) {
    owner_supervisor_.OnSessionDelta(delta);
}

void DaemonRuntime::OnOwnerDisconnected() {
    owner_supervisor_.OnDisconnected();
    owner_commanded_connection_ = 0;
}

void DaemonRuntime::DriveOwner() {
    if (!config_.owner_enabled) {
        // Keep the supervisor in ABSENT so a disabled owner never looks pending.
        owner_supervisor_.Poll(false, std::chrono::steady_clock::now());
        return;
    }

    owner_supervisor_.Poll(true, std::chrono::steady_clock::now());

    // One OWN_SESSION per owner connection. Re-issuing every pass would make the
    // owner release and recreate a working handle, which AudioFlinger would see as
    // the effect module disappearing and coming back.
    if (!owner_server_.Connected()) return;
    if (owner_commanded_connection_ == owner_connections_seen_) return;
    if (owner_supervisor_.Ready()) return;

    const uint64_t request_id = next_request_id_++;
    if (owner_server_.RequestOwnSession(
            0, owner::EffectTypeSelector::HIDL, request_id)) {
        owner_commanded_connection_ = owner_connections_seen_;
    }
}

int DaemonRuntime::ReadPublishedOwnerPid() const {
    // The predecessor's own state file. Read before this daemon overwrites it, so
    // Start() is the only valid caller.
    std::ifstream input(config_.state_root / "daemon.state", std::ios::binary);
    if (!input) return 0;

    static constexpr std::string_view kKey = "owner_pid=";
    std::string line;
    while (std::getline(input, line)) {
        if (line.compare(0, kKey.size(), kKey) != 0) continue;
        const std::string value = line.substr(kKey.size());
        // strtol, not stol: this translation unit is built with exceptions
        // disabled. A malformed or out-of-range value reads as "no owner", which
        // the supervisor would conclude anyway once it checked liveness.
        errno = 0;
        char *end = nullptr;
        const long parsed = ::strtol(value.c_str(), &end, 10);
        if (errno != 0 || end == value.c_str()) return 0;
        if (parsed <= 0 || parsed > std::numeric_limits<int>::max()) return 0;
        return static_cast<int>(parsed);
    }
    return 0;
}

void InstallSignalHandlers(DaemonRuntime *runtime) {
    g_signal_target = runtime;

    struct sigaction action{};
    action.sa_handler = HandleStopSignal;
    ::sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    ::sigaction(SIGTERM, &action, nullptr);
    ::sigaction(SIGINT, &action, nullptr);

    // A dead peer socket must not kill the daemon; send() reports EPIPE instead.
    struct sigaction ignore{};
    ignore.sa_handler = SIG_IGN;
    ::sigemptyset(&ignore.sa_mask);
    ::sigaction(SIGPIPE, &ignore, nullptr);
}

} // namespace viper::daemon
