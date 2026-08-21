#include "DriverEventPublisher.h"

#include "FakeDaemonServer.h"

#include <cassert>
#include <chrono>
#include <cstdio>

namespace {

using viper::daemon::DriverEvent;
using viper::daemon::DriverEventType;

constexpr auto kTimeout = std::chrono::milliseconds(2000);

// Skips RESCAN_RESPONSE replay frames the bridge emits on every fresh connection.
bool ReceiveSkippingReplay(viper::test::FakeDaemonServer &server, DriverEvent *event) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (!server.ReceiveEvent(event, kTimeout)) return false;
        if (event->type != DriverEventType::RESCAN_RESPONSE) return true;
    }
    return false;
}

// The publisher owns a process-wide bridge on the default socket name, so these
// tests drive the bridge directly through the publisher's registry semantics.
// Every replay ends with a terminator (context id 0), so a replay of N live
// contexts is N+1 events.
void TestGenerationChangeEmitsExactlyOnce() {
    viper::audio::DriverEventPublisher publisher;
    const uint64_t context = publisher.RegisterContext(11, 22);
    assert(context != 0);

    std::vector<DriverEvent> events;
    publisher.CollectRescanEvents(&events);
    assert(events.size() == 2);
    assert(events[0].resource_generation == 0);
    assert(events[0].graph_generation == 0);

    publisher.PublishGenerations(context, 3, 4);
    events.clear();
    publisher.CollectRescanEvents(&events);
    assert(events.size() == 2);
    assert(events[0].resource_generation == 3);
    assert(events[0].graph_generation == 4);

    publisher.UnregisterContext(context);
    events.clear();
    publisher.CollectRescanEvents(&events);
    // No contexts left, but the terminator still tells the daemon so.
    assert(events.size() == 1);
    assert(events[0].context_instance_id == 0);
}

void TestTelemetryTracksBypassReason() {
    viper::audio::DriverEventPublisher publisher;
    const uint64_t context = publisher.RegisterContext(33, 44);

    publisher.PublishTelemetry(context, 2);
    std::vector<DriverEvent> events;
    publisher.CollectRescanEvents(&events);
    assert(events.size() == 2);
    assert(events[0].bypass_reason == 2);

    publisher.PublishTelemetry(context, 0);
    events.clear();
    publisher.CollectRescanEvents(&events);
    assert(events[0].bypass_reason == 0);
}

void TestUnknownContextIsIgnored() {
    viper::audio::DriverEventPublisher publisher;
    publisher.PublishGenerations(9999, 1, 1);
    publisher.PublishTelemetry(9999, 1);
    publisher.PublishConfigured(9999, 48000, 3);
    publisher.PublishEnabled(9999, true);
    publisher.UnregisterContext(9999);

    std::vector<DriverEvent> events;
    publisher.CollectRescanEvents(&events);
    // Terminator only: nothing was ever registered.
    assert(events.size() == 1);
    assert(events[0].context_instance_id == 0);
}

// End-to-end: generation and telemetry events must reach a listening daemon.
void TestGenerationAndTelemetryReachDaemon() {
    const auto name = viper::test::UniqueSocketName("publisher");
    viper::test::FakeDaemonServer server(name);
    assert(server.Listen());

    viper::audio::DriverEventPublisher publisher(name);
    const uint64_t context = publisher.RegisterContext(55, 66);
    assert(server.Accept(kTimeout));

    // A fresh connection replays live contexts as RESCAN_RESPONSE before the queued
    // lifecycle events drain, so skip replay frames.
    DriverEvent created{};
    assert(ReceiveSkippingReplay(server, &created));
    assert(created.type == DriverEventType::CONTEXT_CREATED);
    assert(created.context_instance_id == context);

    publisher.PublishGenerations(context, 7, 8);
    DriverEvent generations{};
    assert(ReceiveSkippingReplay(server, &generations));
    assert(generations.type == DriverEventType::RESOURCE_GENERATION_CHANGED);
    assert(generations.resource_generation == 7);
    assert(generations.graph_generation == 8);

    publisher.PublishTelemetry(context, 4);
    DriverEvent telemetry{};
    assert(ReceiveSkippingReplay(server, &telemetry));
    assert(telemetry.type == DriverEventType::TELEMETRY);
    assert(telemetry.bypass_reason == 4);
    // Telemetry carries the current generation pair alongside the bypass reason.
    assert(telemetry.resource_generation == 7);
    assert(telemetry.graph_generation == 8);

    // Repeated identical values must not produce another frame.
    publisher.PublishGenerations(context, 7, 8);
    publisher.PublishTelemetry(context, 4);
    publisher.PublishEnabled(context, true);
    DriverEvent next{};
    assert(ReceiveSkippingReplay(server, &next));
    assert(next.type == DriverEventType::CONTEXT_ENABLED);
}

// The daemon can only reconcile its registry if a replay has a visible end, so the
// driver must close every rescan with a terminator. Context ids start at 1, so the
// terminator is a RESCAN_RESPONSE carrying context id 0.
void TestRescanReplayIsTerminated() {
    viper::audio::DriverEventPublisher publisher;
    publisher.RegisterContext(11, 22);
    publisher.RegisterContext(33, 44);

    std::vector<DriverEvent> events;
    publisher.CollectRescanEvents(&events);

    // Two live contexts plus one terminator, and the terminator comes last: a
    // terminator in the middle would collect contexts not yet replayed.
    assert(events.size() == 3);
    assert(events[0].context_instance_id != 0);
    assert(events[1].context_instance_id != 0);
    assert(events[2].type == DriverEventType::RESCAN_RESPONSE);
    assert(events[2].context_instance_id == 0);
}

// An empty driver must still terminate, otherwise a daemon that reconnects to a
// driver with no contexts keeps every phantom entry it had before.
void TestEmptyRescanReplayIsTerminated() {
    viper::audio::DriverEventPublisher publisher;

    std::vector<DriverEvent> events;
    publisher.CollectRescanEvents(&events);

    assert(events.size() == 1);
    assert(events[0].type == DriverEventType::RESCAN_RESPONSE);
    assert(events[0].context_instance_id == 0);
}

} // namespace

int main() {
    TestGenerationChangeEmitsExactlyOnce();
    TestTelemetryTracksBypassReason();
    TestUnknownContextIsIgnored();
    TestGenerationAndTelemetryReachDaemon();
    TestRescanReplayIsTerminated();
    TestEmptyRescanReplayIsTerminated();
    std::puts("driver event publisher tests passed");
    return 0;
}
