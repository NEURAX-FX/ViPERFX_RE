#include "OwnerSupervisor.h"

#include <utility>

namespace viper::daemon {
namespace {

// Session deltas are keyed by (session, uid): the same session id can be reported
// by more than one client, and dropping one client must not clear the other.
uint64_t SessionKey(const owner::SessionDelta &delta) noexcept {
    return (static_cast<uint64_t>(delta.audio_session_id) << 32U)
        | static_cast<uint64_t>(delta.client_uid);
}

} // namespace

OwnerSupervisor::OwnerSupervisor(std::unique_ptr<OwnerProcessAdapter> adapter)
    : adapter_(std::move(adapter)) {}

void OwnerSupervisor::Poll(bool should_own, std::chrono::steady_clock::time_point now) {
    if (!should_own) {
        // Ownership was switched off. A pending or live owner must go, otherwise a
        // handle would outlive the policy that asked for it.
        if (state_ != OwnerState::ABSENT) {
            KillIfSpawned();
            ResetOwnerObservations();
            state_ = OwnerState::ABSENT;
            spawn_attempts_ = 0;
            next_spawn_attempt_ = {};
            handshake_deadline_ = {};
        }
        respawn_backoff_pending_ = false;
        return;
    }

    // A connection since the last pass arms its handshake window here, so the
    // deadline is measured against the clock the caller actually advances.
    if (handshake_arm_pending_) {
        handshake_arm_pending_ = false;
        handshake_deadline_ = now + kHandshakeTimeout;
    }

    // The owner vanished since the last pass. Convert that into a delay against
    // the caller's clock so a crash-looping owner cannot spin the control loop.
    if (respawn_backoff_pending_) {
        respawn_backoff_pending_ = false;
        next_spawn_attempt_ = now + kSpawnRetryDelay;
    }

    switch (state_) {
        case OwnerState::OWNED:
            // Nothing to do while the owner holds the handle. Death arrives as a
            // socket disconnect, not as a poll result, so this path stays quiet.
            return;
        case OwnerState::FAILED:
            // Terminal until something external changes: either the owner told us
            // it cannot create the effect, or the spawn budget is exhausted.
            // Retrying here would multiply a known failure.
            return;
        case OwnerState::STARTING:
            if (now < handshake_deadline_) return;
            // The owner never answered. Kill it and count a restart: a wedged
            // owner holds no usable handle but does hold a socket slot.
            KillIfSpawned();
            ResetOwnerObservations();
            ++restarts_;
            state_ = OwnerState::ABSENT;
            next_spawn_attempt_ = now + kSpawnRetryDelay;
            return;
        case OwnerState::ABSENT:
            break;
    }

    if (now < next_spawn_attempt_) return;
    if (spawn_attempts_ >= kMaxSpawnAttempts) {
        state_ = OwnerState::FAILED;
        return;
    }

    ++spawn_attempts_;
    std::string error;
    const int pid = adapter_->Spawn(&error);
    if (pid <= 0) {
        ++spawn_failures_;
        next_spawn_attempt_ = now + kSpawnRetryDelay;
        if (spawn_attempts_ >= kMaxSpawnAttempts) state_ = OwnerState::FAILED;
        return;
    }

    pid_ = pid;
    pid_is_ours_ = true;
    state_ = OwnerState::STARTING;
    handshake_deadline_ = now + kHandshakeTimeout;
}

void OwnerSupervisor::OnConnected(int pid) {
    pid_ = pid;
    // A daemon restart finds an owner already running. It was not spawned by this
    // supervisor, so it must not be killed as if it were a stale child.
    if (state_ == OwnerState::ABSENT) pid_is_ours_ = false;
    if (state_ != OwnerState::OWNED) {
        state_ = OwnerState::STARTING;
        // The window is armed in Poll() against the caller's clock. Reading
        // steady_clock here would ignore an injected clock, and leaving the
        // deadline at its zero value makes Poll() treat it as already expired and
        // abandon an owner that has not had a chance to answer yet.
        handshake_arm_pending_ = true;
    }
}

bool OwnerSupervisor::SeedSurvivingOwner(int pid) {
    // Only meaningful at startup: a supervisor that already tracks an owner has a
    // live opinion that a state file cannot improve on.
    if (state_ != OwnerState::ABSENT) return false;
    if (pid <= 0) return false;
    // A stale pid from an old state file is not an owner. Waiting on one would
    // block the spawn that the daemon actually needs.
    if (!adapter_->IsAlive(pid)) return false;

    pid_ = pid;
    // Not ours: this supervisor did not spawn it, so it must never be killed as a
    // stale child.
    pid_is_ours_ = false;
    state_ = OwnerState::STARTING;
    // Reuse the handshake window. A survivor that never reconnects is wedged and
    // gets replaced by the existing timeout path rather than blocking forever.
    handshake_arm_pending_ = true;
    return true;
}

void OwnerSupervisor::OnOwned(const owner::Owned &owned) {
    effect_id_ = owned.effect_id;
    has_control_ = owned.has_control;
    last_failure_reason_ = 0;
    state_ = OwnerState::OWNED;
    // The handshake completed, so the next owner death starts from a full budget
    // rather than inheriting attempts spent before this one worked.
    spawn_attempts_ = 0;
    handshake_deadline_ = {};
}

void OwnerSupervisor::OnOwnerFailed(const owner::OwnerFailed &failed) {
    last_failure_reason_ = failed.reason_code;
    effect_id_ = 0;
    has_control_ = false;
    // The process is alive and told us it cannot create the effect. Spawning more
    // copies would hit the same wall, so this is reported rather than retried.
    state_ = OwnerState::FAILED;
    handshake_deadline_ = {};
}

void OwnerSupervisor::OnReleased(const owner::Released &) {
    effect_id_ = 0;
    has_control_ = false;
    tracked_sessions_.clear();
    // Released on request: the owner is still connected, so it can be asked again.
    if (state_ == OwnerState::OWNED) state_ = OwnerState::STARTING;
}

void OwnerSupervisor::OnSessionDelta(const owner::SessionDelta &delta) {
    if (delta.appeared) {
        tracked_sessions_.insert(SessionKey(delta));
    } else {
        tracked_sessions_.erase(SessionKey(delta));
    }
}

void OwnerSupervisor::OnDisconnected() {
    ResetOwnerObservations();
    if (state_ == OwnerState::OWNED || state_ == OwnerState::STARTING) ++restarts_;
    pid_ = 0;
    pid_is_ours_ = false;
    state_ = OwnerState::ABSENT;
    handshake_deadline_ = {};
    // Delay the replacement so a crash-looping owner cannot spin the control loop.
    // The delay is applied in Poll() against the caller's clock: reading
    // steady_clock here would ignore an injected or paused clock.
    respawn_backoff_pending_ = true;
}

OwnerStatus OwnerSupervisor::Status() const {
    OwnerStatus status{};
    status.state = state_;
    status.pid = pid_;
    status.effect_id = effect_id_;
    status.has_control = has_control_;
    status.restarts = restarts_;
    status.spawn_failures = spawn_failures_;
    status.last_failure_reason = last_failure_reason_;
    status.tracked_sessions = tracked_sessions_.size();
    return status;
}

void OwnerSupervisor::ResetOwnerObservations() {
    effect_id_ = 0;
    has_control_ = false;
    tracked_sessions_.clear();
}

void OwnerSupervisor::KillIfSpawned() {
    if (pid_ > 0 && pid_is_ours_) adapter_->Kill(pid_);
    pid_ = 0;
    pid_is_ours_ = false;
}

} // namespace viper::daemon
