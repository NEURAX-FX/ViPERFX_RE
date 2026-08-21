// Exercises the real DriverDaemonBridge (driver side, inside libv4a_re.so)
// against the real DaemonRuntime (daemon side). Every other test in this area
// replaces one of the two peers with a fake, so this is the only coverage that
// both implementations agree on the private @viper4android.driver.v1 protocol.
#include "DaemonRuntime.h"
#include "DriverEventPublisher.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace {

using viper::daemon::ContextState;
using viper::daemon::DaemonConfig;
using viper::daemon::DaemonRuntime;
using viper::daemon::DeviceIdentity;
using viper::daemon::FakeRouteAdapter;

std::string UniqueSocketName(const char *suffix) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::string("viper4android.e2e.") + suffix + "." + std::to_string(stamp);
}

std::filesystem::path UniqueStateRoot(const char *suffix) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path()
        / ("viper-e2e-" + std::string(suffix) + "-" + std::to_string(stamp));
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

// The bridge thread sends asynchronously, so the daemon loop must be pumped.
template <typename Predicate>
bool PumpUntil(DaemonRuntime &runtime, Predicate predicate, int attempts = 400) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        runtime.RunOnce();
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

DaemonConfig MakeConfig(const char *suffix) {
    DaemonConfig config{};
    config.state_root = UniqueStateRoot(suffix);
    config.driver_socket_name = UniqueSocketName(suffix);
    return config;
}

void TestDriverLifecycleReachesDaemonRegistry() {
    const DaemonConfig config = MakeConfig("lifecycle");
    auto adapter = std::make_unique<FakeRouteAdapter>();
    adapter->SetRoute(SpeakerRoute());
    DaemonRuntime runtime(config, std::move(adapter));

    std::string error;
    assert(runtime.Start(&error));

    viper::audio::DriverEventPublisher publisher(config.driver_socket_name);
    const uint64_t context = publisher.RegisterContext(4242, 9);
    assert(context != 0);

    // The context must arrive over the real socket and be decoded by the daemon.
    // The bridge replays live contexts on connect, and that replay is stamped
    // after the already-queued CONTEXT_CREATED, so whichever frame lands first
    // the identity fields must match; the disabled context is not ACTIVE yet.
    assert(PumpUntil(runtime, [&] { return runtime.Registry().Size() == 1; }));
    auto entry = runtime.Registry().Snapshot().at(0);
    assert(entry.audio_session_id == 4242);
    assert(entry.io_id == 9);
    assert(entry.state != ContextState::ACTIVE);

    publisher.PublishConfigured(context, 44100, 3);
    assert(PumpUntil(runtime, [&] {
        const auto entries = runtime.Registry().Snapshot();
        return !entries.empty() && entries.at(0).sample_rate == 44100;
    }));

    publisher.PublishEnabled(context, true);
    assert(PumpUntil(runtime, [&] {
        const auto entries = runtime.Registry().Snapshot();
        return !entries.empty() && entries.at(0).state == ContextState::ACTIVE;
    }));

    publisher.PublishGenerations(context, 11, 12);
    assert(PumpUntil(runtime, [&] {
        const auto entries = runtime.Registry().Snapshot();
        return !entries.empty() && entries.at(0).resource_generation == 11
            && entries.at(0).graph_generation == 12;
    }));
    // Generation events must not disturb the lifecycle state.
    assert(runtime.Registry().Snapshot().at(0).state == ContextState::ACTIVE);

    publisher.PublishTelemetry(context, 3);
    assert(PumpUntil(runtime, [&] {
        const auto entries = runtime.Registry().Snapshot();
        return !entries.empty() && entries.at(0).bypass_reason == 3;
    }));

    publisher.UnregisterContext(context);
    assert(PumpUntil(runtime, [&] { return runtime.Registry().Size() == 0; }));

    // No frame from the real bridge may be rejected by the real daemon.
    assert(runtime.Server().Statistics().rejected_frames == 0);
    assert(runtime.Server().Statistics().applied_events >= 5);

    std::filesystem::remove_all(config.state_root);
}

void TestDaemonStartedAfterDriverStillLearnsContexts() {
    const DaemonConfig config = MakeConfig("late-daemon");

    // Driver first: the bridge reconnects until the daemon binds its socket.
    viper::audio::DriverEventPublisher publisher(config.driver_socket_name);
    const uint64_t first = publisher.RegisterContext(1, 1);
    const uint64_t second = publisher.RegisterContext(2, 2);
    publisher.PublishConfigured(first, 48000, 3);
    publisher.PublishEnabled(first, true);

    auto adapter = std::make_unique<FakeRouteAdapter>();
    adapter->SetRoute(SpeakerRoute());
    DaemonRuntime runtime(config, std::move(adapter));

    std::string error;
    assert(runtime.Start(&error));

    // On connect the bridge replays live contexts, so a late daemon converges
    // without the driver being restarted.
    assert(PumpUntil(runtime, [&] { return runtime.Registry().Size() == 2; }));
    const auto entries = runtime.Registry().Snapshot();
    bool saw_active = false;
    for (const auto &entry : entries) {
        if (entry.state == ContextState::ACTIVE) saw_active = true;
    }
    assert(saw_active);
    assert(second != 0);
    assert(runtime.Server().Statistics().rejected_frames == 0);

    std::filesystem::remove_all(config.state_root);
}

void TestDaemonRestartRecoversRegistryViaReplay() {
    const DaemonConfig config = MakeConfig("daemon-restart");

    auto adapter = std::make_unique<FakeRouteAdapter>();
    adapter->SetRoute(SpeakerRoute());
    std::string error;

    viper::audio::DriverEventPublisher publisher(config.driver_socket_name);
    uint64_t context = 0;
    {
        DaemonRuntime first(config, std::move(adapter));
        assert(first.Start(&error));
        context = publisher.RegisterContext(77, 5);
        publisher.PublishEnabled(context, true);
        assert(PumpUntil(first, [&] { return first.Registry().Size() == 1; }));
    }

    // The daemon died; its registry is gone but the driver keeps running.
    auto second_adapter = std::make_unique<FakeRouteAdapter>();
    second_adapter->SetRoute(SpeakerRoute());
    DaemonRuntime restarted(config, std::move(second_adapter));
    assert(restarted.Start(&error));

    assert(PumpUntil(restarted, [&] { return restarted.Registry().Size() == 1; }));
    const auto entry = restarted.Registry().Snapshot().at(0);
    assert(entry.audio_session_id == 77);
    // Replay carries the driver's current enable state, not a stale transition.
    assert(entry.state == ContextState::ACTIVE);
    assert(restarted.Server().Statistics().rejected_frames == 0);

    std::filesystem::remove_all(config.state_root);
}

} // namespace

int main() {
    TestDriverLifecycleReachesDaemonRegistry();
    TestDaemonStartedAfterDriverStillLearnsContexts();
    TestDaemonRestartRecoversRegistryViaReplay();
    std::puts("daemon/driver end-to-end tests passed");
    return 0;
}
