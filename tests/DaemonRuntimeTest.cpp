#include "DaemonRuntime.h"

#include "DriverEvent.h"
#include "OwnerProtocol.h"
#include "ViperDaemonProtocol.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

using viper::daemon::DaemonConfig;
using viper::daemon::DaemonRuntime;
using viper::daemon::DeviceIdentity;
using viper::daemon::DriverEvent;
using viper::daemon::DriverEventType;
using viper::daemon::FakeRouteAdapter;
using viper::daemon::FrameHeader;

std::string UniqueSocketName(const char *suffix) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::string("viper4android.daemontest.") + suffix + "."
        + std::to_string(stamp);
}

std::filesystem::path UniqueStateRoot(const char *suffix) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path()
        / ("viper-daemon-test-" + std::string(suffix) + "-" + std::to_string(stamp));
}

// Minimal driver-side client: connects to the daemon and sends framed events.
class FakeDriverClient final {
public:
    ~FakeDriverClient() { Close(); }

    bool Connect(const std::string &socket_name) {
        fd_ = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (fd_ < 0) return false;

        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        address.sun_path[0] = '\0';
        std::memcpy(address.sun_path + 1, socket_name.data(), socket_name.size());
        const socklen_t length = static_cast<socklen_t>(
            offsetof(sockaddr_un, sun_path) + 1U + socket_name.size());
        if (::connect(fd_, reinterpret_cast<sockaddr *>(&address), length) != 0) {
            Close();
            return false;
        }
        return true;
    }

    bool Send(const DriverEvent &event) {
        std::vector<uint8_t> payload;
        std::string error;
        if (!viper::daemon::EncodeDriverEvent(event, &payload, &error)) return false;

        FrameHeader header{};
        header.message_type = static_cast<uint16_t>(event.type);
        header.request_id = event.context_instance_id;
        header.sequence = event.event_sequence;
        const std::string_view view(
            reinterpret_cast<const char *>(payload.data()), payload.size());
        std::vector<uint8_t> frame;
        if (!viper::daemon::EncodeFrame(header, view, &frame, &error)) return false;
        return ::send(fd_, frame.data(), frame.size(), 0)
            == static_cast<ssize_t>(frame.size());
    }

    // Sends a frame whose header disagrees with its payload.
    bool SendMismatchedFrame(const DriverEvent &event) {
        std::vector<uint8_t> payload;
        std::string error;
        if (!viper::daemon::EncodeDriverEvent(event, &payload, &error)) return false;

        FrameHeader header{};
        header.message_type = static_cast<uint16_t>(DriverEventType::TELEMETRY);
        header.sequence = event.event_sequence + 100U;
        const std::string_view view(
            reinterpret_cast<const char *>(payload.data()), payload.size());
        std::vector<uint8_t> frame;
        if (!viper::daemon::EncodeFrame(header, view, &frame, &error)) return false;
        return ::send(fd_, frame.data(), frame.size(), 0)
            == static_cast<ssize_t>(frame.size());
    }

    bool SendRaw(const std::vector<uint8_t> &bytes) {
        return ::send(fd_, bytes.data(), bytes.size(), 0)
            == static_cast<ssize_t>(bytes.size());
    }

    void Close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
};

// Minimal owner-side client. The real owner is an ART process, so the host test
// speaks the same wire format from C++.
class FakeOwnerClient final {
public:
    ~FakeOwnerClient() { Close(); }

    bool Connect(const std::string &socket_name) {
        fd_ = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (fd_ < 0) return false;

        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        address.sun_path[0] = '\0';
        std::memcpy(address.sun_path + 1, socket_name.data(), socket_name.size());
        const socklen_t length = static_cast<socklen_t>(
            offsetof(sockaddr_un, sun_path) + 1U + socket_name.size());
        if (::connect(fd_, reinterpret_cast<sockaddr *>(&address), length) != 0) {
            Close();
            return false;
        }
        return true;
    }

    bool SendHello(uint64_t pid, uint64_t boot_id) {
        viper::owner::OwnerHello hello{};
        hello.owner_pid = pid;
        hello.boot_id = boot_id;
        std::vector<uint8_t> payload;
        std::string error;
        if (!viper::owner::EncodeOwnerHello(hello, &payload, &error)) return false;
        return SendOwnerFrame(viper::owner::OwnerMessage::OWNER_HELLO, payload);
    }

    bool SendOwned(uint32_t effect_id, bool has_control) {
        viper::owner::Owned owned{};
        owned.effect_id = effect_id;
        owned.has_control = has_control;
        std::vector<uint8_t> payload;
        std::string error;
        if (!viper::owner::EncodeOwned(owned, &payload, &error)) return false;
        return SendOwnerFrame(viper::owner::OwnerMessage::OWNED, payload);
    }

    bool SendSessionDelta(uint32_t session_id, uint32_t client_uid, bool appeared) {
        viper::owner::SessionDelta delta{};
        delta.audio_session_id = session_id;
        delta.client_uid = client_uid;
        delta.appeared = appeared;
        std::vector<uint8_t> payload;
        std::string error;
        if (!viper::owner::EncodeSessionDelta(delta, &payload, &error)) return false;
        return SendOwnerFrame(viper::owner::OwnerMessage::SESSION_DELTA, payload);
    }

    bool ReceiveOwnerMessage(uint16_t *message_type, std::vector<uint8_t> *payload) {
        std::vector<uint8_t> buffer(viper::daemon::kMaxFrameSize, 0U);
        for (int attempt = 0; attempt < 200; ++attempt) {
            const ssize_t received = ::recv(fd_, buffer.data(), buffer.size(), MSG_DONTWAIT);
            if (received > 0) {
                FrameHeader header{};
                std::string error;
                if (!viper::daemon::DecodeFrame(
                        std::span<const uint8_t>(
                            buffer.data(), static_cast<std::size_t>(received)),
                        &header,
                        payload,
                        &error)) {
                    return false;
                }
                *message_type = header.message_type;
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }

    void Close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    bool SendOwnerFrame(
        viper::owner::OwnerMessage type,
        const std::vector<uint8_t> &payload
    ) {
        FrameHeader header{};
        header.message_type = static_cast<uint16_t>(type);
        const std::string_view view(
            reinterpret_cast<const char *>(payload.data()), payload.size());
        std::vector<uint8_t> frame;
        std::string error;
        if (!viper::daemon::EncodeFrame(header, view, &frame, &error)) return false;
        return ::send(fd_, frame.data(), frame.size(), 0)
            == static_cast<ssize_t>(frame.size());
    }

    int fd_ = -1;
};

DriverEvent Event(DriverEventType type, uint64_t context, uint64_t sequence) {
    DriverEvent event{};
    event.type = type;
    event.boot_id = 0xD1AB10ULL;
    event.event_sequence = sequence;
    event.context_instance_id = context;
    event.audio_session_id = 55;
    event.io_id = 3;
    return event;
}

DeviceIdentity SpeakerRoute() {
    DeviceIdentity identity{};
    identity.route_type = "speaker";
    identity.stable_address_or_port = "builtin";
    identity.product_name = "internal";
    identity.sample_rate = 48000;
    identity.channel_mask = 3;
    identity.encoding = "pcm_16";
    return identity;
}

DeviceIdentity BluetoothRoute() {
    DeviceIdentity identity{};
    identity.route_type = "bluetooth_a2dp";
    identity.stable_address_or_port = "AA:BB:CC:DD:EE:FF";
    identity.product_name = "Buds";
    identity.sample_rate = 44100;
    identity.channel_mask = 3;
    identity.encoding = "pcm_16";
    return identity;
}

// Drains the daemon loop until `predicate` holds or the attempt budget runs out.
template <typename Predicate>
bool PumpUntil(DaemonRuntime &runtime, Predicate predicate, int attempts = 200) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        runtime.RunOnce();
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

std::string ReadStateFile(const std::filesystem::path &root) {
    std::ifstream input(root / "daemon.state");
    return std::string(
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void TestStartCreatesStateRootAndBindsSocket() {
    DaemonConfig config{};
    config.state_root = UniqueStateRoot("start");
    config.driver_socket_name = UniqueSocketName("start");
    // Both endpoints need unique names: the defaults are the production socket
    // names, so leaving app_socket_name alone collides with a daemon actually
    // running on the machine and fails the test for the wrong reason.
    config.app_socket_name = UniqueSocketName("start") + ".app";

    auto adapter = std::make_unique<FakeRouteAdapter>();
    adapter->SetRoute(SpeakerRoute());
    DaemonRuntime runtime(config, std::move(adapter));

    std::string error;
    assert(runtime.Start(&error));
    assert(error.empty());
    assert(std::filesystem::is_directory(config.state_root));
    assert(runtime.Server().Listening());
    assert(!runtime.Server().Connected());

    std::filesystem::remove_all(config.state_root);
}

void TestDriverEventsPopulateRegistry() {
    DaemonConfig config{};
    config.state_root = UniqueStateRoot("events");
    config.driver_socket_name = UniqueSocketName("events");
    config.app_socket_name = UniqueSocketName("events") + ".app";

    auto adapter = std::make_unique<FakeRouteAdapter>();
    adapter->SetRoute(SpeakerRoute());
    DaemonRuntime runtime(config, std::move(adapter));

    std::string error;
    assert(runtime.Start(&error));

    FakeDriverClient client;
    assert(client.Connect(config.driver_socket_name));
    assert(client.Send(Event(DriverEventType::DRIVER_HELLO, 0, 1)));
    assert(client.Send(Event(DriverEventType::CONTEXT_CREATED, 1, 2)));

    DriverEvent configured = Event(DriverEventType::CONTEXT_CONFIGURED, 1, 3);
    configured.sample_rate = 48000;
    configured.channel_mask = 3;
    assert(client.Send(configured));
    assert(client.Send(Event(DriverEventType::CONTEXT_ENABLED, 1, 4)));

    assert(PumpUntil(runtime, [&] {
        const auto entries = runtime.Registry().Snapshot();
        return entries.size() == 1
            && entries.at(0).state == viper::daemon::ContextState::ACTIVE;
    }));

    const auto entry = runtime.Registry().Snapshot().at(0);
    assert(entry.sample_rate == 48000);
    assert(entry.channel_mask == 3);
    assert(runtime.Server().Connected());
    // DRIVER_HELLO is not a context and must not create a registry entry.
    assert(runtime.Registry().Size() == 1);

    // Release removes the context; the daemon never owns effect lifetime.
    assert(client.Send(Event(DriverEventType::CONTEXT_RELEASED, 1, 5)));
    assert(PumpUntil(runtime, [&] { return runtime.Registry().Size() == 0; }));

    std::filesystem::remove_all(config.state_root);
}

void TestMalformedFramesAreRejectedWithoutStalling() {
    DaemonConfig config{};
    config.state_root = UniqueStateRoot("malformed");
    config.driver_socket_name = UniqueSocketName("malformed");
    config.app_socket_name = UniqueSocketName("malformed") + ".app";

    auto adapter = std::make_unique<FakeRouteAdapter>();
    adapter->SetRoute(SpeakerRoute());
    DaemonRuntime runtime(config, std::move(adapter));

    std::string error;
    assert(runtime.Start(&error));

    FakeDriverClient client;
    assert(client.Connect(config.driver_socket_name));

    // Garbage bytes: no valid frame header.
    assert(client.SendRaw(std::vector<uint8_t>(64, 0xAB)));
    // Header/payload disagreement must be rejected, not applied.
    assert(client.SendMismatchedFrame(Event(DriverEventType::CONTEXT_CREATED, 7, 1)));

    assert(PumpUntil(runtime, [&] {
        return runtime.Server().Statistics().rejected_frames >= 2;
    }));
    assert(runtime.Registry().Size() == 0);

    // A valid event after the bad ones still lands: the stream is not poisoned.
    assert(client.Send(Event(DriverEventType::CONTEXT_CREATED, 8, 2)));
    assert(PumpUntil(runtime, [&] { return runtime.Registry().Size() == 1; }));

    std::filesystem::remove_all(config.state_root);
}

void TestSequenceGapTriggersRescanRequest() {
    DaemonConfig config{};
    config.state_root = UniqueStateRoot("gap");
    config.driver_socket_name = UniqueSocketName("gap");
    config.app_socket_name = UniqueSocketName("gap") + ".app";

    auto adapter = std::make_unique<FakeRouteAdapter>();
    adapter->SetRoute(SpeakerRoute());
    DaemonRuntime runtime(config, std::move(adapter));

    std::string error;
    assert(runtime.Start(&error));

    FakeDriverClient client;
    assert(client.Connect(config.driver_socket_name));
    assert(client.Send(Event(DriverEventType::CONTEXT_CREATED, 1, 1)));
    assert(PumpUntil(runtime, [&] { return runtime.Registry().Size() == 1; }));

    // Sequence 2 is lost.
    assert(client.Send(Event(DriverEventType::CONTEXT_CREATED, 2, 3)));
    assert(PumpUntil(runtime, [&] {
        return runtime.Server().Statistics().rescan_requests_sent >= 1;
    }));
    assert(runtime.Registry().RescanNeeded());

    std::filesystem::remove_all(config.state_root);
}

void TestDriverReconnectIsAccepted() {
    DaemonConfig config{};
    config.state_root = UniqueStateRoot("reconnect");
    config.driver_socket_name = UniqueSocketName("reconnect");
    config.app_socket_name = UniqueSocketName("reconnect") + ".app";

    auto adapter = std::make_unique<FakeRouteAdapter>();
    adapter->SetRoute(SpeakerRoute());
    DaemonRuntime runtime(config, std::move(adapter));

    std::string error;
    assert(runtime.Start(&error));

    {
        FakeDriverClient first;
        assert(first.Connect(config.driver_socket_name));
        assert(first.Send(Event(DriverEventType::CONTEXT_CREATED, 1, 1)));
        assert(PumpUntil(runtime, [&] { return runtime.Registry().Size() == 1; }));
    }

    assert(PumpUntil(runtime, [&] { return !runtime.Server().Connected(); }));

    FakeDriverClient second;
    assert(second.Connect(config.driver_socket_name));
    assert(second.Send(Event(DriverEventType::CONTEXT_CREATED, 9, 20)));
    assert(PumpUntil(runtime, [&] {
        return runtime.Server().Connected()
            && runtime.Server().Statistics().accepted_connections >= 2;
    }));

    std::filesystem::remove_all(config.state_root);
}

void TestRouteChangeIsObserved() {
    DaemonConfig config{};
    config.state_root = UniqueStateRoot("route");
    config.driver_socket_name = UniqueSocketName("route");
    config.app_socket_name = UniqueSocketName("route") + ".app";

    auto adapter = std::make_unique<FakeRouteAdapter>();
    adapter->SetRoute(SpeakerRoute());
    FakeRouteAdapter *route_adapter = adapter.get();
    DaemonRuntime runtime(config, std::move(adapter));

    std::string error;
    assert(runtime.Start(&error));

    runtime.RunOnce();
    assert(runtime.Routes().HasRoute());
    const std::string speaker_hash = runtime.Routes().CurrentKeyHash();
    assert(speaker_hash.size() == 64U);
    assert(runtime.Status().route_changes == 1);

    // An unchanged route must not count as a change.
    runtime.RunOnce();
    assert(runtime.Status().route_changes == 1);

    route_adapter->SetRoute(BluetoothRoute());
    runtime.RunOnce();
    assert(runtime.Status().route_changes == 2);
    assert(runtime.Routes().CurrentKeyHash() != speaker_hash);

    // An unavailable route keeps the last known key instead of inventing one.
    route_adapter->SetUnavailable("no route source");
    runtime.RunOnce();
    assert(runtime.Status().route_changes == 2);
    assert(runtime.Routes().HasRoute());

    std::filesystem::remove_all(config.state_root);
}

void TestStateFileReportsStatus() {
    DaemonConfig config{};
    config.state_root = UniqueStateRoot("state");
    config.driver_socket_name = UniqueSocketName("state");
    config.app_socket_name = UniqueSocketName("state") + ".app";
    // Restore off: this runtime only observes, and the state file must say so.
    config.restore_enabled = false;

    auto adapter = std::make_unique<FakeRouteAdapter>();
    adapter->SetRoute(SpeakerRoute());
    DaemonRuntime runtime(config, std::move(adapter));

    std::string error;
    assert(runtime.Start(&error));

    FakeDriverClient client;
    assert(client.Connect(config.driver_socket_name));
    assert(client.Send(Event(DriverEventType::CONTEXT_CREATED, 1, 1)));
    assert(PumpUntil(runtime, [&] { return runtime.Registry().Size() == 1; }));
    assert(runtime.WriteStateFile(&error));

    const std::string contents = ReadStateFile(config.state_root);
    assert(contents.find("mode=observe-only") != std::string::npos);
    assert(contents.find("driver_connected=1") != std::string::npos);
    assert(contents.find("route_known=1") != std::string::npos);
    assert(contents.find("live_contexts=1") != std::string::npos);
    assert(
        contents.find("route_key_hash=" + runtime.Routes().CurrentKeyHash())
        != std::string::npos);
    // Observe-only must never have applied anything.
    assert(contents.find("restores_attempted=0") != std::string::npos);
    assert(contents.find("snapshot_commands=0") != std::string::npos);
    // The temporary file must not survive an atomic publish.
    assert(!std::filesystem::exists(config.state_root / "daemon.state.tmp"));

    std::filesystem::remove_all(config.state_root);

    // With restore enabled the mode line changes, so an operator can tell which
    // build is running from the state file alone.
    DaemonConfig restoring{};
    restoring.state_root = UniqueStateRoot("state-restore");
    restoring.driver_socket_name = UniqueSocketName("state-restore");
    restoring.app_socket_name = UniqueSocketName("state-restore") + ".app";
    auto restoring_adapter = std::make_unique<FakeRouteAdapter>();
    restoring_adapter->SetRoute(SpeakerRoute());
    DaemonRuntime restoring_runtime(restoring, std::move(restoring_adapter));
    assert(restoring_runtime.Start(&error));
    assert(restoring_runtime.WriteStateFile(&error));
    const std::string restoring_contents = ReadStateFile(restoring.state_root);
    assert(restoring_contents.find("mode=route-restore") != std::string::npos);
    assert(restoring_contents.find("route_epoch=") != std::string::npos);
    assert(restoring_contents.find("daemon_generation=") != std::string::npos);

    std::filesystem::remove_all(restoring.state_root);
}

void TestRunStopsAtIterationBoundAndOnStop() {
    DaemonConfig config{};
    config.state_root = UniqueStateRoot("run");
    config.driver_socket_name = UniqueSocketName("run");
    config.app_socket_name = UniqueSocketName("run") + ".app";
    config.poll_interval = std::chrono::milliseconds(1);
    config.max_iterations = 3;

    auto adapter = std::make_unique<FakeRouteAdapter>();
    adapter->SetRoute(SpeakerRoute());
    DaemonRuntime runtime(config, std::move(adapter));

    std::string error;
    assert(runtime.Start(&error));
    assert(runtime.Run(&error));
    assert(runtime.Status().iterations == 3);
    assert(!ReadStateFile(config.state_root).empty());

    // Stop() must break an unbounded loop. The first runtime still owns its
    // socket and state root, so the second needs its own.
    DaemonConfig unbounded = config;
    unbounded.max_iterations = 0;
    unbounded.state_root = UniqueStateRoot("run-loop");
    unbounded.driver_socket_name = UniqueSocketName("run-loop");
    unbounded.app_socket_name = UniqueSocketName("run-loop") + ".app";
    auto second_adapter = std::make_unique<FakeRouteAdapter>();
    second_adapter->SetRoute(SpeakerRoute());
    DaemonRuntime looping(unbounded, std::move(second_adapter));
    assert(looping.Start(&error));

    std::thread stopper([&looping] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        looping.Stop();
    });
    assert(looping.Run(&error));
    stopper.join();
    assert(looping.Status().iterations >= 1);

    std::filesystem::remove_all(unbounded.state_root);
    std::filesystem::remove_all(config.state_root);
}

void TestRunRequiresStart() {
    DaemonConfig config{};
    config.state_root = UniqueStateRoot("nostart");
    config.driver_socket_name = UniqueSocketName("nostart");
    config.app_socket_name = UniqueSocketName("nostart") + ".app";
    config.max_iterations = 1;

    auto adapter = std::make_unique<FakeRouteAdapter>();
    adapter->SetRoute(SpeakerRoute());
    DaemonRuntime runtime(config, std::move(adapter));

    std::string error;
    assert(!runtime.Run(&error));
    assert(!error.empty());
}

// The daemon must ask a connected owner to hold session 0, and must publish owner
// state. Without the published state "daemon running" says nothing about whether
// anything owns a handle, which is exactly the failure this whole change fixes.
void TestOwnerIsCommandedAndReported() {
    DaemonConfig config{};
    config.state_root = UniqueStateRoot("owner");
    config.driver_socket_name = UniqueSocketName("owner");
    config.app_socket_name = UniqueSocketName("owner") + ".app";
    config.owner_socket_name = UniqueSocketName("owner") + ".owner";
    config.owner_enabled = true;

    auto adapter = std::make_unique<FakeRouteAdapter>();
    adapter->SetRoute(SpeakerRoute());
    DaemonRuntime runtime(config, std::move(adapter));

    std::string error;
    assert(runtime.Start(&error));
    // No owner yet: that must be visible rather than looking healthy.
    assert(runtime.WriteStateFile(&error));
    assert(ReadStateFile(config.state_root).find("owner_state=absent")
        != std::string::npos);

    FakeOwnerClient owner;
    assert(owner.Connect(config.owner_socket_name));
    assert(owner.SendHello(4242, 0xB007ULL));
    assert(PumpUntil(runtime, [&] { return runtime.OwnerLink().Connected(); }));

    // Hello is acked, then exactly one own-session command is issued.
    assert(PumpUntil(runtime, [&] {
        return runtime.OwnerLink().Statistics().commands_sent == 1;
    }));
    uint16_t message_type = 0;
    std::vector<uint8_t> payload;
    assert(owner.ReceiveOwnerMessage(&message_type, &payload));
    assert(message_type
        == static_cast<uint16_t>(viper::owner::OwnerMessage::OWNER_HELLO_ACK));
    assert(owner.ReceiveOwnerMessage(&message_type, &payload));
    assert(message_type == static_cast<uint16_t>(viper::owner::OwnerMessage::OWN_SESSION));
    viper::owner::OwnSession request{};
    assert(viper::owner::DecodeOwnSession(payload, &request, &error));
    assert(request.audio_session_id == 0);

    // Repeated polls must not re-issue the command for the same owner connection.
    for (int pass = 0; pass < 10; ++pass) runtime.RunOnce();
    assert(runtime.OwnerLink().Statistics().commands_sent == 1);

    assert(owner.SendOwned(5150, true));
    assert(PumpUntil(runtime, [&] { return runtime.OwnerReady(); }));
    assert(runtime.WriteStateFile(&error));
    const std::string owned_state = ReadStateFile(config.state_root);
    assert(owned_state.find("owner_state=owned") != std::string::npos);
    assert(owned_state.find("owner_effect_id=5150") != std::string::npos);
    assert(owned_state.find("owner_pid=4242") != std::string::npos);

    // Session deltas the owner observes are counted, not turned into effects.
    assert(owner.SendSessionDelta(9001, 10438, true));
    assert(PumpUntil(runtime, [&] {
        return runtime.OwnerState().tracked_sessions == 1;
    }));
    assert(runtime.WriteStateFile(&error));
    assert(ReadStateFile(config.state_root).find("tracked_sessions=1")
        != std::string::npos);

    // Owner death must be observed: AudioFlinger dropped the module with it.
    owner.Close();
    assert(PumpUntil(runtime, [&] { return !runtime.OwnerReady(); }));
    assert(runtime.WriteStateFile(&error));
    const std::string dead_state = ReadStateFile(config.state_root);
    assert(dead_state.find("owner_state=owned") == std::string::npos);
    assert(dead_state.find("owner_effect_id=0") != std::string::npos);

    std::filesystem::remove_all(config.state_root);
}

// Owner support must be opt-in: an install without the owner dex has to keep the
// App's legacy backend working rather than binding a socket nothing can use.
void TestOwnerDisabledBindsNoSocket() {
    DaemonConfig config{};
    config.state_root = UniqueStateRoot("owner-off");
    config.driver_socket_name = UniqueSocketName("owner-off");
    config.app_socket_name = UniqueSocketName("owner-off") + ".app";
    config.owner_socket_name = UniqueSocketName("owner-off") + ".owner";
    config.owner_enabled = false;

    auto adapter = std::make_unique<FakeRouteAdapter>();
    adapter->SetRoute(SpeakerRoute());
    DaemonRuntime runtime(config, std::move(adapter));

    std::string error;
    assert(runtime.Start(&error));
    assert(!runtime.OwnerLink().Listening());

    FakeOwnerClient owner;
    assert(!owner.Connect(config.owner_socket_name));

    for (int pass = 0; pass < 5; ++pass) runtime.RunOnce();
    assert(runtime.OwnerState().state == viper::daemon::OwnerState::ABSENT);
    assert(runtime.WriteStateFile(&error));
    assert(ReadStateFile(config.state_root).find("owner_state=absent")
        != std::string::npos);

    std::filesystem::remove_all(config.state_root);
}

} // namespace

int main() {
    TestStartCreatesStateRootAndBindsSocket();
    TestDriverEventsPopulateRegistry();
    TestMalformedFramesAreRejectedWithoutStalling();
    TestSequenceGapTriggersRescanRequest();
    TestDriverReconnectIsAccepted();
    TestRouteChangeIsObserved();
    TestStateFileReportsStatus();
    TestRunStopsAtIterationBoundAndOnStop();
    TestRunRequiresStart();
    TestOwnerIsCommandedAndReported();
    TestOwnerDisabledBindsNoSocket();
    std::puts("daemon runtime tests passed");
    return 0;
}
