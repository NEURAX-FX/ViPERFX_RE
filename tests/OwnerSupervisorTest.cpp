#include "OwnerSupervisor.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

using viper::daemon::OwnerProcessAdapter;
using viper::daemon::OwnerState;
using viper::daemon::OwnerSupervisor;

using Clock = std::chrono::steady_clock;

// Deterministic stand-in for fork/exec: the real adapter spawns app_process64,
// which a host test cannot do.
class FakeProcessAdapter final : public OwnerProcessAdapter {
public:
    int Spawn(std::string *error) override {
        ++spawn_calls;
        if (!spawn_succeeds) {
            if (error != nullptr) error->assign("spawn refused by test");
            return -1;
        }
        live_pid = next_pid++;
        return live_pid;
    }

    bool IsAlive(int pid) const override { return pid > 0 && pid == live_pid; }

    void Kill(int pid) override {
        ++kill_calls;
        if (pid == live_pid) live_pid = 0;
    }

    unsigned spawn_calls = 0;
    unsigned kill_calls = 0;
    bool spawn_succeeds = true;
    int next_pid = 1000;
    int live_pid = 0;
};

struct Harness {
    Harness() {
        auto owned = std::make_unique<FakeProcessAdapter>();
        adapter = owned.get();
        supervisor = std::make_unique<OwnerSupervisor>(std::move(owned));
    }

    FakeProcessAdapter *adapter = nullptr;
    std::unique_ptr<OwnerSupervisor> supervisor;
    Clock::time_point now = Clock::time_point{} + std::chrono::seconds(1000);

    void Advance(std::chrono::milliseconds delta) { now += delta; }
    void Poll(bool should_own = true) { supervisor->Poll(should_own, now); }
};

void TestSpawnsOnceWhileStarting() {
    Harness harness;
    harness.Poll();
    assert(harness.adapter->spawn_calls == 1);
    assert(harness.supervisor->Status().state == OwnerState::STARTING);
    assert(harness.supervisor->Status().pid == harness.adapter->live_pid);

    // A pending handshake must not spawn a second owner on every control pass.
    for (int pass = 0; pass < 10; ++pass) {
        harness.Advance(std::chrono::milliseconds(10));
        harness.Poll();
    }
    assert(harness.adapter->spawn_calls == 1);
    assert(!harness.supervisor->Ready());
}

void TestOwnedTransitionRecordsEffect() {
    Harness harness;
    harness.Poll();
    harness.supervisor->OnConnected(harness.adapter->live_pid);

    viper::owner::Owned owned{};
    owned.effect_id = 4915;
    owned.has_control = true;
    harness.supervisor->OnOwned(owned);

    assert(harness.supervisor->Status().state == OwnerState::OWNED);
    assert(harness.supervisor->Status().effect_id == 4915);
    assert(harness.supervisor->Ready());

    // Steady state must not spawn or kill anything.
    for (int pass = 0; pass < 5; ++pass) {
        harness.Advance(std::chrono::seconds(30));
        harness.Poll();
    }
    assert(harness.adapter->spawn_calls == 1);
    assert(harness.adapter->kill_calls == 0);
}

void TestHandshakeTimeoutRespawnsBounded() {
    Harness harness;
    harness.Poll();
    assert(harness.adapter->spawn_calls == 1);

    // Owner never answers: it must be killed and retried, not waited on forever.
    harness.Advance(OwnerSupervisor::kHandshakeTimeout + std::chrono::milliseconds(1));
    harness.Poll();
    assert(harness.adapter->kill_calls == 1);
    assert(harness.supervisor->Status().restarts == 1);

    // Retry is delayed, not immediate: a spawn loop would burn the control thread.
    harness.Poll();
    assert(harness.adapter->spawn_calls == 1);

    harness.Advance(OwnerSupervisor::kSpawnRetryDelay + std::chrono::milliseconds(1));
    harness.Poll();
    assert(harness.adapter->spawn_calls == 2);
}

void TestGivesUpAfterSpawnBudget() {
    Harness harness;
    harness.adapter->spawn_succeeds = false;

    for (unsigned attempt = 0; attempt < OwnerSupervisor::kMaxSpawnAttempts; ++attempt) {
        harness.Poll();
        harness.Advance(OwnerSupervisor::kSpawnRetryDelay + std::chrono::milliseconds(1));
    }
    assert(harness.adapter->spawn_calls == OwnerSupervisor::kMaxSpawnAttempts);
    assert(harness.supervisor->Status().state == OwnerState::FAILED);

    // Exhausted budget must stay exhausted rather than retrying every interval.
    for (int pass = 0; pass < 10; ++pass) {
        harness.Advance(std::chrono::seconds(60));
        harness.Poll();
    }
    assert(harness.adapter->spawn_calls == OwnerSupervisor::kMaxSpawnAttempts);
    assert(!harness.supervisor->Ready());
}

void TestOwnerReportedFailureDoesNotRespawnBlindly() {
    Harness harness;
    harness.Poll();
    harness.supervisor->OnConnected(harness.adapter->live_pid);

    viper::owner::OwnerFailed failed{};
    failed.reason_code = 2;
    harness.supervisor->OnOwnerFailed(failed);

    assert(harness.supervisor->Status().state == OwnerState::FAILED);
    assert(harness.supervisor->Status().last_failure_reason == 2);

    for (int pass = 0; pass < 5; ++pass) {
        harness.Advance(std::chrono::seconds(30));
        harness.Poll();
    }
    // The owner process exists and cannot create the effect; spawning more copies
    // would only multiply the failure.
    assert(harness.adapter->spawn_calls == 1);
}

void TestOwnerDeathRespawnsWithFreshBudget() {
    Harness harness;
    harness.Poll();
    harness.supervisor->OnConnected(harness.adapter->live_pid);
    viper::owner::Owned owned{};
    owned.effect_id = 77;
    harness.supervisor->OnOwned(owned);
    assert(harness.supervisor->Ready());

    // AudioFlinger destroys the module when the owner dies, so the daemon must
    // notice and bring a new owner up.
    harness.adapter->live_pid = 0;
    harness.supervisor->OnDisconnected();
    harness.Poll();
    assert(harness.supervisor->Status().state == OwnerState::ABSENT
        || harness.supervisor->Status().state == OwnerState::STARTING);

    harness.Advance(OwnerSupervisor::kSpawnRetryDelay + std::chrono::milliseconds(1));
    harness.Poll();
    assert(harness.adapter->spawn_calls == 2);
    assert(harness.supervisor->Status().restarts >= 1);
    assert(!harness.supervisor->Ready());
}

void TestAdoptsExistingOwnerWithoutSpawning() {
    Harness harness;
    // A restarted daemon finds an owner already connected: killing it would drop
    // a working effect module, so it must be adopted instead.
    harness.supervisor->OnConnected(31337);
    viper::owner::Owned owned{};
    owned.effect_id = 4242;
    owned.has_control = true;
    harness.supervisor->OnOwned(owned);

    assert(harness.supervisor->Status().state == OwnerState::OWNED);
    assert(harness.supervisor->Status().pid == 31337);
    assert(harness.supervisor->Status().effect_id == 4242);

    for (int pass = 0; pass < 5; ++pass) {
        harness.Advance(std::chrono::seconds(30));
        harness.Poll();
    }
    assert(harness.adapter->spawn_calls == 0);
    assert(harness.adapter->kill_calls == 0);
}

// A connection starts a fresh handshake window. Leaving the deadline unset makes
// the next Poll() read a zero time_point as already expired and kill an owner
// that only just connected, which loses the effect module it is about to create.
void TestConnectionArmsHandshakeWindow() {
    Harness harness;
    harness.supervisor->OnConnected(2024);
    assert(harness.supervisor->Status().state == OwnerState::STARTING);

    // Well inside the window: no kill, no respawn.
    harness.Advance(OwnerSupervisor::kHandshakeTimeout / 2);
    harness.Poll();
    assert(harness.adapter->kill_calls == 0);
    assert(harness.supervisor->Status().pid == 2024);
    assert(harness.supervisor->Status().restarts == 0);

    // Past the window an owner that never answered is still abandoned.
    harness.Advance(OwnerSupervisor::kHandshakeTimeout);
    harness.Poll();
    assert(harness.supervisor->Status().restarts == 1);
    // Adopted owners are not ours to kill: a restarted daemon must not terminate
    // a process it did not spawn.
    assert(harness.adapter->kill_calls == 0);
}

void TestDisabledOwnershipNeverSpawns() {
    Harness harness;
    for (int pass = 0; pass < 10; ++pass) {
        harness.Advance(std::chrono::seconds(5));
        harness.Poll(false);
    }
    assert(harness.adapter->spawn_calls == 0);
    assert(harness.supervisor->Status().state == OwnerState::ABSENT);

    // A pending handshake is abandoned when ownership is switched off.
    harness.Poll(true);
    assert(harness.adapter->spawn_calls == 1);
    harness.Poll(false);
    assert(harness.adapter->kill_calls == 1);
    assert(harness.supervisor->Status().state == OwnerState::ABSENT);
}

void TestSessionCountIsTracked() {
    Harness harness;
    harness.supervisor->OnConnected(500);
    viper::owner::Owned owned{};
    owned.effect_id = 5;
    harness.supervisor->OnOwned(owned);

    viper::owner::SessionDelta appeared{};
    appeared.audio_session_id = 111;
    appeared.client_uid = 10001;
    appeared.appeared = true;
    harness.supervisor->OnSessionDelta(appeared);
    assert(harness.supervisor->Status().tracked_sessions == 1);

    // Repeats must not inflate the count.
    harness.supervisor->OnSessionDelta(appeared);
    assert(harness.supervisor->Status().tracked_sessions == 1);

    viper::owner::SessionDelta vanished = appeared;
    vanished.appeared = false;
    harness.supervisor->OnSessionDelta(vanished);
    assert(harness.supervisor->Status().tracked_sessions == 0);

    // Losing the owner clears observations: they came from that process.
    harness.supervisor->OnSessionDelta(appeared);
    harness.supervisor->OnDisconnected();
    assert(harness.supervisor->Status().tracked_sessions == 0);
}

// A daemon killed by an update or a crash leaves its owner running: the owner now
// keeps its effect and reconnects. The replacement daemon knows that pid from the
// state file the dead one published, so it must wait for the survivor instead of
// spawning a second owner and creating a duplicate AudioFlinger module.
void TestSeededSurvivorIsAwaitedInsteadOfSpawning() {
    Harness harness;
    harness.adapter->live_pid = 4242;

    assert(harness.supervisor->SeedSurvivingOwner(4242));
    assert(harness.supervisor->Status().state == OwnerState::STARTING);
    assert(harness.supervisor->Status().pid == 4242);

    // Inside the handshake window nothing is spawned: the survivor is expected.
    harness.Advance(OwnerSupervisor::kHandshakeTimeout / 2);
    harness.Poll();
    assert(harness.adapter->spawn_calls == 0);

    harness.supervisor->OnConnected(4242);
    viper::owner::Owned owned{};
    owned.effect_id = 909;
    owned.has_control = true;
    harness.supervisor->OnOwned(owned);

    for (int pass = 0; pass < 5; ++pass) {
        harness.Advance(std::chrono::seconds(30));
        harness.Poll();
    }
    // Adopted, not replaced, and never killed: this daemon did not spawn it.
    assert(harness.adapter->spawn_calls == 0);
    assert(harness.adapter->kill_calls == 0);
    assert(harness.supervisor->Status().pid == 4242);
    assert(harness.supervisor->Status().effect_id == 909);
}

// A pid from a stale state file is not an owner. Trusting it would leave the
// daemon waiting forever for a process that no longer exists.
void TestSeedRejectsDeadOrMissingPid() {
    Harness harness;
    harness.adapter->live_pid = 0;

    assert(!harness.supervisor->SeedSurvivingOwner(9999));
    assert(!harness.supervisor->SeedSurvivingOwner(0));
    assert(harness.supervisor->Status().state == OwnerState::ABSENT);

    // Normal startup still happens: a fresh boot has no survivor to wait for.
    harness.adapter->live_pid = 0;
    harness.Poll();
    assert(harness.adapter->spawn_calls == 1);
}

// The seeded survivor may itself be wedged. The existing handshake timeout has to
// still apply, otherwise a broken owner would block its own replacement.
void TestSeededSurvivorThatNeverConnectsIsReplaced() {
    Harness harness;
    harness.adapter->live_pid = 777;
    assert(harness.supervisor->SeedSurvivingOwner(777));

    // The window is armed against the caller's clock, so the first pass sets the
    // deadline and does not spawn.
    harness.Poll();
    assert(harness.adapter->spawn_calls == 0);
    assert(harness.supervisor->Status().pid == 777);

    harness.Advance(OwnerSupervisor::kHandshakeTimeout + std::chrono::milliseconds(1));
    harness.Poll();
    // Not ours to kill, but it does count as a restart and frees the state.
    assert(harness.adapter->kill_calls == 0);
    assert(harness.supervisor->Status().restarts == 1);

    harness.Advance(OwnerSupervisor::kSpawnRetryDelay + std::chrono::milliseconds(1));
    harness.Poll();
    assert(harness.adapter->spawn_calls == 1);
}

} // namespace

int main() {
    TestSpawnsOnceWhileStarting();
    TestOwnedTransitionRecordsEffect();
    TestHandshakeTimeoutRespawnsBounded();
    TestGivesUpAfterSpawnBudget();
    TestOwnerReportedFailureDoesNotRespawnBlindly();
    TestOwnerDeathRespawnsWithFreshBudget();
    TestAdoptsExistingOwnerWithoutSpawning();
    TestConnectionArmsHandshakeWindow();
    TestDisabledOwnershipNeverSpawns();
    TestSessionCountIsTracked();
    TestSeededSurvivorIsAwaitedInsteadOfSpawning();
    TestSeedRejectsDeadOrMissingPid();
    TestSeededSurvivorThatNeverConnectsIsReplaced();
    std::puts("owner supervisor tests passed");
    return 0;
}
