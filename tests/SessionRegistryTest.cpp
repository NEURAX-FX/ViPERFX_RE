#include "SessionRegistry.h"

#include <cassert>
#include <cstdio>

namespace {

using viper::daemon::ApplyResult;
using viper::daemon::ContextState;
using viper::daemon::DriverEvent;
using viper::daemon::DriverEventType;
using viper::daemon::SessionRegistry;

constexpr uint64_t kBoot = 0xB0071DULL;

DriverEvent Event(
    DriverEventType type,
    uint64_t context_instance_id,
    uint64_t sequence,
    uint64_t boot_id = kBoot
) {
    DriverEvent event{};
    event.type = type;
    event.boot_id = boot_id;
    event.event_sequence = sequence;
    event.context_instance_id = context_instance_id;
    event.audio_session_id = 100;
    event.io_id = 7;
    return event;
}

void TestLifecycleTransitions() {
    SessionRegistry registry;
    assert(registry.Apply(Event(DriverEventType::CONTEXT_CREATED, 1, 1))
        == ApplyResult::APPLIED);
    assert(registry.Snapshot().at(0).state == ContextState::CREATED);

    DriverEvent configured = Event(DriverEventType::CONTEXT_CONFIGURED, 1, 2);
    configured.sample_rate = 48000;
    configured.channel_mask = 3;
    assert(registry.Apply(configured) == ApplyResult::APPLIED);
    auto entries = registry.Snapshot();
    assert(entries.at(0).state == ContextState::CONFIGURED);
    assert(entries.at(0).sample_rate == 48000);
    assert(entries.at(0).channel_mask == 3);

    assert(registry.Apply(Event(DriverEventType::CONTEXT_ENABLED, 1, 3))
        == ApplyResult::APPLIED);
    assert(registry.Snapshot().at(0).state == ContextState::ACTIVE);

    assert(registry.Apply(Event(DriverEventType::CONTEXT_DISABLED, 1, 4))
        == ApplyResult::APPLIED);
    assert(registry.Snapshot().at(0).state == ContextState::INACTIVE);

    // A released context leaves the registry: AudioFlinger owns effect lifetime,
    // and the daemon must not keep referencing a dead context.
    assert(registry.Apply(Event(DriverEventType::CONTEXT_RELEASED, 1, 5))
        == ApplyResult::APPLIED);
    assert(registry.Snapshot().empty());
}

void TestIdentityIsBootIdPlusInstanceId() {
    SessionRegistry registry;
    registry.Apply(Event(DriverEventType::CONTEXT_CREATED, 1, 1, 111));
    registry.Apply(Event(DriverEventType::CONTEXT_CREATED, 1, 2, 222));
    // Same instance id, different boot: two distinct contexts.
    assert(registry.Size() == 2);

    const auto entries = registry.Snapshot();
    assert(entries.at(0).key.boot_id == 111);
    assert(entries.at(1).key.boot_id == 222);
}

void TestGenerationAndTelemetryUpdates() {
    SessionRegistry registry;
    registry.Apply(Event(DriverEventType::CONTEXT_CREATED, 5, 1));

    DriverEvent generations = Event(DriverEventType::RESOURCE_GENERATION_CHANGED, 5, 2);
    generations.resource_generation = 9;
    generations.graph_generation = 12;
    assert(registry.Apply(generations) == ApplyResult::APPLIED);

    DriverEvent telemetry = Event(DriverEventType::TELEMETRY, 5, 3);
    telemetry.resource_generation = 9;
    telemetry.graph_generation = 12;
    telemetry.bypass_reason = 4;
    assert(registry.Apply(telemetry) == ApplyResult::APPLIED);

    const auto entry = registry.Snapshot().at(0);
    assert(entry.resource_generation == 9);
    assert(entry.graph_generation == 12);
    assert(entry.bypass_reason == 4);
    // Generation and telemetry events must not rewrite the lifecycle state.
    assert(entry.state == ContextState::CREATED);
}

void TestSequenceGapRequestsRescan() {
    SessionRegistry registry;
    assert(registry.Apply(Event(DriverEventType::CONTEXT_CREATED, 1, 1))
        == ApplyResult::APPLIED);
    assert(!registry.RescanNeeded());

    // Sequence 2 never arrived.
    assert(registry.Apply(Event(DriverEventType::CONTEXT_CREATED, 2, 3))
        == ApplyResult::APPLIED_WITH_GAP);
    assert(registry.RescanNeeded());
    assert(registry.HighestSequence() == 3);
}

void TestDuplicateSequencesAreIgnored() {
    SessionRegistry registry;
    registry.Apply(Event(DriverEventType::CONTEXT_CREATED, 1, 1));
    registry.Apply(Event(DriverEventType::CONTEXT_ENABLED, 1, 2));

    // A replayed frame must not move the state backwards.
    assert(registry.Apply(Event(DriverEventType::CONTEXT_CREATED, 1, 1))
        == ApplyResult::DUPLICATE);
    assert(registry.Snapshot().at(0).state == ContextState::ACTIVE);
}

void TestUnknownEventTypesAreIgnored() {
    SessionRegistry registry;
    assert(registry.Apply(Event(DriverEventType::DRIVER_HELLO, 0, 1))
        == ApplyResult::IGNORED);
    assert(registry.Size() == 0);

    // Events for a context the registry never observed are ignored, not invented.
    assert(registry.Apply(Event(DriverEventType::CONTEXT_ENABLED, 42, 2))
        == ApplyResult::IGNORED);
    assert(registry.Size() == 0);
}

void TestRescanReplacesStaleEntries() {
    SessionRegistry registry;
    registry.Apply(Event(DriverEventType::CONTEXT_CREATED, 1, 1));
    registry.Apply(Event(DriverEventType::CONTEXT_CREATED, 2, 2));
    assert(registry.Size() == 2);

    registry.MarkStaleAfter(registry.HighestSequence());
    for (const auto &entry : registry.Snapshot()) assert(entry.stale);

    // Only context 1 is confirmed by the rescan; context 2 disappeared while the
    // daemon was disconnected.
    DriverEvent replay = Event(DriverEventType::RESCAN_RESPONSE, 1, 10);
    replay.sample_rate = 44100;
    replay.enabled = true;
    assert(registry.Apply(replay) == ApplyResult::APPLIED);
    registry.RescanComplete(registry.HighestSequence());

    const auto entries = registry.Snapshot();
    assert(entries.size() == 1);
    assert(entries.at(0).key.context_instance_id == 1);
    assert(!entries.at(0).stale);
    assert(entries.at(0).sample_rate == 44100);
    assert(entries.at(0).state == ContextState::ACTIVE);
    assert(!registry.RescanNeeded());
}

void TestRescanResponseCreatesUnknownContexts() {
    SessionRegistry registry;
    // After a daemon restart the registry is empty; replay must repopulate it.
    DriverEvent replay = Event(DriverEventType::RESCAN_RESPONSE, 3, 1);
    replay.enabled = false;
    replay.sample_rate = 48000;
    assert(registry.Apply(replay) == ApplyResult::APPLIED);

    const auto entries = registry.Snapshot();
    assert(entries.size() == 1);
    assert(entries.at(0).state == ContextState::INACTIVE);
    assert(entries.at(0).sample_rate == 48000);
}

void TestRescanTerminatorCollectsPhantomContexts() {
    SessionRegistry registry;
    registry.Apply(Event(DriverEventType::CONTEXT_CREATED, 1, 1));
    registry.Apply(Event(DriverEventType::CONTEXT_CREATED, 2, 2));
    assert(registry.Size() == 2);

    // A reconnect or gap starts a rescan: every known entry needs confirmation.
    registry.MarkStaleAfter(registry.HighestSequence());
    assert(registry.RescanNeeded());

    // The driver replays only context 1; context 2 died while we were not looking.
    DriverEvent replay = Event(DriverEventType::RESCAN_RESPONSE, 1, 10);
    replay.enabled = true;
    assert(registry.Apply(replay) == ApplyResult::APPLIED);

    // The terminator is what makes reconciliation possible: without an explicit end
    // of replay the registry can never tell "not replayed yet" from "gone", so it
    // keeps phantom contexts forever. Context id 0 is never issued by the driver
    // (ids start at 1), so it is a safe sentinel.
    DriverEvent terminator = Event(DriverEventType::RESCAN_RESPONSE, 0, 11);
    assert(registry.Apply(terminator) == ApplyResult::APPLIED);

    const auto entries = registry.Snapshot();
    assert(entries.size() == 1);
    assert(entries.at(0).key.context_instance_id == 1);
    assert(!entries.at(0).stale);
    assert(!registry.RescanNeeded());
}

void TestRescanTerminatorNeverBecomesAContext() {
    SessionRegistry registry;
    // A terminator with nothing outstanding is a no-op, not a new context.
    DriverEvent terminator = Event(DriverEventType::RESCAN_RESPONSE, 0, 1);
    registry.Apply(terminator);
    assert(registry.Size() == 0);
}

} // namespace

int main() {
    TestLifecycleTransitions();
    TestIdentityIsBootIdPlusInstanceId();
    TestGenerationAndTelemetryUpdates();
    TestSequenceGapRequestsRescan();
    TestDuplicateSequencesAreIgnored();
    TestUnknownEventTypesAreIgnored();
    TestRescanReplacesStaleEntries();
    TestRescanResponseCreatesUnknownContexts();
    TestRescanTerminatorCollectsPhantomContexts();
    TestRescanTerminatorNeverBecomesAContext();
    std::puts("session registry tests passed");
    return 0;
}
