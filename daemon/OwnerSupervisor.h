#pragma once

#include "OwnerProtocol.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <set>
#include <string>

namespace viper::daemon {

enum class OwnerState {
    // No owner process and none being started.
    ABSENT,
    // Spawned, waiting for hello/OWNED.
    STARTING,
    // Owner holds the session-0 effect handle.
    OWNED,
    // Owner cannot be started, or reported it cannot create the handle. The App's
    // legacy backend stays in charge in this state.
    FAILED,
};

struct OwnerStatus {
    OwnerState state = OwnerState::ABSENT;
    int pid = 0;
    uint32_t effect_id = 0;
    bool has_control = false;
    uint64_t restarts = 0;
    uint64_t spawn_failures = 0;
    uint32_t last_failure_reason = 0;
    std::size_t tracked_sessions = 0;
};

/**
 * Spawns the owner process. Separated behind an interface because a host test
 * cannot fork app_process64, and because the spawn command is the only part that
 * depends on the module layout.
 */
class OwnerProcessAdapter {
public:
    virtual ~OwnerProcessAdapter() = default;

    // Returns the child pid, or -1 with `error` set.
    virtual int Spawn(std::string *error) = 0;
    virtual bool IsAlive(int pid) const = 0;
    virtual void Kill(int pid) = 0;
};

/**
 * Decides when an owner process should exist and keeps that decision bounded.
 *
 * The supervisor never touches the effect itself: it starts a process, watches
 * the handshake, and records what the owner reported. Every failure path ends in
 * a state the daemon can publish, because "no owner" must be visible rather than
 * looking like a healthy daemon with silent audio.
 */
class OwnerSupervisor final {
public:
    // A spawned owner has to connect and answer within this window. It only has
    // to open a socket and create one effect, so a long timeout would just delay
    // recovery from a wedged owner.
    static constexpr std::chrono::milliseconds kHandshakeTimeout{4000};
    // Spacing between spawn attempts. Without it a failing exec would spin the
    // control loop.
    static constexpr std::chrono::milliseconds kSpawnRetryDelay{2000};
    // Bounded so a permanently broken install cannot respawn forever.
    static constexpr unsigned kMaxSpawnAttempts = 3;

    explicit OwnerSupervisor(std::unique_ptr<OwnerProcessAdapter> adapter);

    OwnerSupervisor(const OwnerSupervisor &) = delete;
    OwnerSupervisor &operator=(const OwnerSupervisor &) = delete;

    // Drives spawn/timeout decisions. `should_own` is the daemon's policy: false
    // means no handle is wanted, which must also tear down a pending owner.
    void Poll(bool should_own, std::chrono::steady_clock::time_point now);

    // The owner socket connected. `pid` is the peer pid, which may differ from a
    // spawned child when a restarted daemon adopts a running owner.
    void OnConnected(int pid);

    // Adopts an owner that outlived the previous daemon.
    //
    // The owner keeps its effect handle across a daemon restart, so a replacement
    // daemon that spawned unconditionally would create a second owner and a
    // duplicate AudioFlinger module. `pid` comes from the state file the dead
    // daemon published. Returns false when it names no live process, in which case
    // normal spawning proceeds.
    bool SeedSurvivingOwner(int pid);
    void OnOwned(const owner::Owned &owned);
    void OnOwnerFailed(const owner::OwnerFailed &failed);
    void OnReleased(const owner::Released &released);
    void OnSessionDelta(const owner::SessionDelta &delta);
    void OnDisconnected();

    OwnerStatus Status() const;

    // True only in OWNED: the daemon uses this to decide whether the App may
    // stop creating its own effect.
    bool Ready() const noexcept { return state_ == OwnerState::OWNED; }

private:
    void ResetOwnerObservations();
    void KillIfSpawned();

    std::unique_ptr<OwnerProcessAdapter> adapter_;
    OwnerState state_ = OwnerState::ABSENT;
    int pid_ = 0;
    bool pid_is_ours_ = false;
    uint32_t effect_id_ = 0;
    bool has_control_ = false;
    uint64_t restarts_ = 0;
    uint64_t spawn_failures_ = 0;
    uint32_t last_failure_reason_ = 0;
    unsigned spawn_attempts_ = 0;
    std::chrono::steady_clock::time_point next_spawn_attempt_{};
    std::chrono::steady_clock::time_point handshake_deadline_{};
    // Both flags defer a deadline to Poll(), which owns the caller's clock.
    // Reading steady_clock at the callback would ignore an injected clock and, for
    // the handshake, leave a zero deadline that Poll() reads as already expired.
    bool respawn_backoff_pending_ = false;
    bool handshake_arm_pending_ = false;
    std::set<uint64_t> tracked_sessions_;
};

} // namespace viper::daemon
