#include "DriverDaemonBridge.h"

#include "FakeDaemonServer.h"
#include "ViperDaemonProtocol.h"

#include <cassert>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

using viper::daemon::DriverDaemonBridge;
using viper::daemon::DriverEvent;
using viper::daemon::DriverEventType;
using viper::test::FakeDaemonServer;
using viper::test::UniqueSocketName;

DriverEvent MakeEvent(DriverEventType type, uint64_t context_instance_id) {
    DriverEvent event{};
    event.type = type;
    event.context_instance_id = context_instance_id;
    event.audio_session_id = 0;
    event.io_id = 42;
    event.sample_rate = 48000;
    event.channel_mask = 3;
    event.enabled = true;
    event.session_generation = 5;
    event.resource_generation = 6;
    event.graph_generation = 7;
    event.bypass_reason = 0;
    return event;
}

void TestPublishSerializesAndAssignsMonotonicSequences() {
    const auto name = UniqueSocketName("publish");
    FakeDaemonServer server(name);
    assert(server.Listen());

    DriverDaemonBridge bridge(name);
    bridge.Start();
    assert(server.Accept(std::chrono::milliseconds(2000)));

    assert(bridge.Publish(MakeEvent(DriverEventType::CONTEXT_CREATED, 11)));
    assert(bridge.Publish(MakeEvent(DriverEventType::CONTEXT_CONFIGURED, 11)));

    DriverEvent first{};
    DriverEvent second{};
    assert(server.ReceiveEvent(&first, std::chrono::milliseconds(2000)));
    assert(server.ReceiveEvent(&second, std::chrono::milliseconds(2000)));

    assert(first.type == DriverEventType::CONTEXT_CREATED);
    assert(second.type == DriverEventType::CONTEXT_CONFIGURED);
    assert(first.context_instance_id == 11);
    assert(first.io_id == 42);
    assert(first.sample_rate == 48000);
    assert(first.channel_mask == 3);
    assert(first.enabled);
    assert(first.boot_id != 0);
    assert(first.boot_id == second.boot_id);
    assert(second.event_sequence == first.event_sequence + 1U);

    bridge.Stop();
}

void TestPublishDoesNotBlockWithoutDaemon() {
    DriverDaemonBridge bridge(UniqueSocketName("absent"));
    bridge.Start();
    assert(!bridge.Connected());

    const auto start = std::chrono::steady_clock::now();
    for (int index = 0; index < 512; ++index) {
        bridge.Publish(MakeEvent(DriverEventType::CONTEXT_CREATED,
            static_cast<uint64_t>(index) + 1U));
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    assert(elapsed < std::chrono::milliseconds(200));
    assert(bridge.DroppedEvents() > 0);

    bridge.Stop();
}

void TestBoundedQueueDropsInsteadOfGrowing() {
    DriverDaemonBridge bridge(UniqueSocketName("bounded"));
    bridge.Start();

    const std::size_t attempts = DriverDaemonBridge::kQueueCapacity * 4U;
    std::size_t accepted = 0;
    for (std::size_t index = 0; index < attempts; ++index) {
        if (bridge.Publish(MakeEvent(DriverEventType::TELEMETRY, index + 1U))) ++accepted;
    }
    assert(accepted <= DriverDaemonBridge::kQueueCapacity);
    assert(bridge.DroppedEvents() >= attempts - DriverDaemonBridge::kQueueCapacity);

    bridge.Stop();
}

void TestReconnectsAfterDaemonRestart() {
    const auto name = UniqueSocketName("reconnect");
    FakeDaemonServer server(name);
    assert(server.Listen());

    DriverDaemonBridge bridge(name);
    bridge.Start();
    assert(server.Accept(std::chrono::milliseconds(2000)));
    assert(bridge.Publish(MakeEvent(DriverEventType::CONTEXT_CREATED, 1)));

    DriverEvent event{};
    assert(server.ReceiveEvent(&event, std::chrono::milliseconds(2000)));

    server.DropClient();
    assert(server.Accept(std::chrono::milliseconds(4000)));

    bool delivered = false;
    for (int attempt = 0; attempt < 40 && !delivered; ++attempt) {
        bridge.Publish(MakeEvent(DriverEventType::CONTEXT_RELEASED, 1));
        delivered = server.ReceiveEvent(&event, std::chrono::milliseconds(200));
    }
    assert(delivered);
    assert(event.type == DriverEventType::CONTEXT_RELEASED);

    bridge.Stop();
}

void TestRescanProviderRepublishesLiveContexts() {
    const auto name = UniqueSocketName("rescan");
    FakeDaemonServer server(name);
    assert(server.Listen());

    DriverDaemonBridge bridge(name);
    bridge.SetRescanProvider([](std::vector<DriverEvent> *events) {
        events->push_back(MakeEvent(DriverEventType::RESCAN_RESPONSE, 21));
        events->push_back(MakeEvent(DriverEventType::RESCAN_RESPONSE, 22));
    });
    bridge.Start();
    assert(server.Accept(std::chrono::milliseconds(2000)));

    bridge.RequestRescan();

    DriverEvent first{};
    DriverEvent second{};
    assert(server.ReceiveEvent(&first, std::chrono::milliseconds(2000)));
    assert(server.ReceiveEvent(&second, std::chrono::milliseconds(2000)));
    assert(first.type == DriverEventType::RESCAN_RESPONSE);
    assert(second.type == DriverEventType::RESCAN_RESPONSE);
    assert(first.context_instance_id == 21);
    assert(second.context_instance_id == 22);

    bridge.Stop();
}

void TestEventCodecRejectsTruncatedPayload() {
    std::vector<uint8_t> encoded;
    std::string error;
    assert(viper::daemon::EncodeDriverEvent(
        MakeEvent(DriverEventType::CONTEXT_ENABLED, 3), &encoded, &error));
    assert(encoded.size() == viper::daemon::kDriverEventWireSize);

    DriverEvent decoded{};
    encoded.pop_back();
    assert(!viper::daemon::DecodeDriverEvent(encoded, &decoded, &error));
}

} // namespace

int main() {
    TestEventCodecRejectsTruncatedPayload();
    TestPublishSerializesAndAssignsMonotonicSequences();
    TestPublishDoesNotBlockWithoutDaemon();
    TestBoundedQueueDropsInsteadOfGrowing();
    TestReconnectsAfterDaemonRestart();
    TestRescanProviderRepublishesLiveContexts();
    return 0;
}
