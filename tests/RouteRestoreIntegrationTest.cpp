// End-to-end route-first restore: a real DaemonRuntime reads a stored snapshot
// for the settled route and applies it to a real ViperContext over the real
// @viper4android.driver.v1 socket, before any App has run.
//
// Everything here is the production code path: SnapshotStore -> RouteWatcher ->
// DriverEventServer -> DriverDaemonBridge -> SnapshotApplyController ->
// DspGraphSlots. Only the route adapter is faked, because host tests cannot move
// a real audio route.
#include "DaemonRuntime.h"

#include "AudioFormat.h"
#include "DeviceKey.h"
#include "DriverEventPublisher.h"
#include "ParameterStream.h"
#include "SnapshotSchema.h"
#include "SnapshotStore.h"
#include "ViPERParams.h"
#include "ViperContext.h"
#include "ViperDaemonProtocol.h"
#include "viper/constants.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using viper::daemon::DaemonConfig;
using viper::daemon::DaemonRuntime;
using viper::daemon::DeviceIdentity;
using viper::daemon::FakeRouteAdapter;
using viper::daemon::Snapshot;
using viper::daemon::SnapshotStore;

std::string UniqueSocketName(const char *suffix) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::string("viper4android.restore.") + suffix + "." + std::to_string(stamp);
}

std::filesystem::path UniqueStateRoot(const char *suffix) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path()
        / ("viper-restore-" + std::string(suffix) + "-" + std::to_string(stamp));
}

DeviceIdentity SpeakerIdentity() {
    DeviceIdentity identity{};
    identity.route_type = "speaker";
    identity.stable_address_or_port = "builtin";
    identity.product_name = "speaker";
    identity.sample_rate = viper::daemon::kRouteIdentitySampleRate;
    identity.channel_mask = viper::daemon::kRouteIdentityChannelMask;
    identity.encoding = viper::daemon::kRouteIdentityEncoding;
    return identity;
}

DeviceIdentity BluetoothIdentity() {
    DeviceIdentity identity{};
    identity.route_type = "bluetooth_a2dp";
    identity.stable_address_or_port = "aa:bb:cc:dd:ee:ff";
    identity.product_name = "bluetooth_a2dp";
    identity.sample_rate = viper::daemon::kRouteIdentitySampleRate;
    identity.channel_mask = viper::daemon::kRouteIdentityChannelMask;
    identity.encoding = viper::daemon::kRouteIdentityEncoding;
    return identity;
}

std::string HashOf(const DeviceIdentity &identity) {
    return viper::daemon::HashDeviceKey(viper::daemon::NormalizeDeviceKey(identity));
}

// A snapshot whose only observable effect is the convolver kernel id, so the test
// can prove which route's state reached the graph.
Snapshot SnapshotFor(
    const DeviceIdentity &identity,
    int32_t kernel_id,
    uint64_t app_generation = 4,
    uint64_t daemon_generation = 1,
    bool master_enabled = true
) {
    using namespace viper::params;
    const std::size_t samples = 64;
    std::vector<float> kernel(samples, 0.0F);
    kernel[0] = 1.0F;

    viper::daemon::RawParamRecord fill{};
    fill.param = kParamConvolverSetBuffer;
    fill.arr_size = static_cast<uint32_t>(samples);
    fill.payload.assign(samples * sizeof(float), 0U);
    std::memcpy(fill.payload.data(), kernel.data(), fill.payload.size());
    const uint32_t crc = viper::daemon::Crc32(fill.payload);

    viper::daemon::RawParamRecord enable{};
    enable.param = kParamConvolverEnable;
    enable.val1 = 1;

    viper::daemon::RawParamRecord prepare{};
    prepare.param = kParamConvolverPrepareBuffer;
    prepare.val1 = static_cast<int32_t>(samples);
    prepare.val2 = 2;

    viper::daemon::RawParamRecord commit{};
    commit.param = kParamConvolverCommitBuffer;
    commit.val1 = static_cast<int32_t>(samples);
    commit.val2 = static_cast<int32_t>(crc);
    commit.val3 = kernel_id;

    std::vector<uint8_t> parameters;
    std::string error;
    const bool encoded = viper::daemon::EncodeParameterStream(
        {enable, prepare, fill, commit}, &parameters, &error);
    assert(encoded);

    Snapshot snapshot{};
    snapshot.device_key = viper::daemon::NormalizeDeviceKey(identity);
    snapshot.device_key_hash = viper::daemon::HashDeviceKey(snapshot.device_key);
    snapshot.boot_id = 0xB007ULL;
    snapshot.daemon_generation = daemon_generation;
    snapshot.app_generation = app_generation;
    snapshot.created_at_millis = 1700000000000ULL;
    snapshot.master_enabled = master_enabled;
    snapshot.parameters = parameters;
    return snapshot;
}

void StoreSnapshot(const std::filesystem::path &root, const Snapshot &snapshot) {
    SnapshotStore store(root);
    std::string error;
    const bool committed = store.Commit(snapshot.device_key_hash, snapshot, &error);
    assert(committed);
}

// Files a self-consistent snapshot into another route's directory, by copying the
// bytes rather than going through Commit().
//
// Commit() cannot express this: it requires device_hash == snapshot.device_key_hash,
// and ValidateSnapshot() requires that hash to match device_key. So the store already
// makes a self-inconsistent snapshot unwritable, and the only way this state arises
// on a real device is on-disk tampering or corruption. That is precisely what the
// daemon's load-time re-check defends against, so the test has to forge it directly.
void PlantForeignSnapshot(
    const std::filesystem::path &root,
    const std::string &victim_hash,
    const Snapshot &snapshot
) {
    // Written correctly under its own hash first, so the bytes are a valid snapshot.
    StoreSnapshot(root, snapshot);

    const auto source = root / "routes" / snapshot.device_key_hash / "current.snapshot";
    const auto victim_directory = root / "routes" / victim_hash;
    std::error_code code;
    std::filesystem::create_directories(victim_directory, code);
    assert(!code);
    std::filesystem::copy_file(
        source,
        victim_directory / "current.snapshot",
        std::filesystem::copy_options::overwrite_existing,
        code
    );
    assert(!code);
}

void Configure(ViperContext &context) {
    effect_config_t config{};
    config.input_cfg.buffer.frame_count = viper::audio::kMaxBlockFrames;
    config.input_cfg.sampling_rate = viper::daemon::kRouteIdentitySampleRate;
    config.input_cfg.channels = AUDIO_CHANNEL_OUT_STEREO;
    config.input_cfg.format = AUDIO_FORMAT_PCM_16_BIT;
    config.input_cfg.access_mode = EFFECT_BUFFER_ACCESS_WRITE;
    config.input_cfg.mask = EFFECT_CONFIG_ALL & ~EFFECT_CONFIG_PROVIDER;
    config.output_cfg = config.input_cfg;

    int32_t reply = -1;
    uint32_t reply_size = sizeof(reply);
    const int32_t status = context.HandleCommand(
        EFFECT_CMD_SET_CONFIG, sizeof(config), &config, &reply_size, &reply);
    assert(status == 0);
}

void Enable(ViperContext &context) {
    int32_t reply = -1;
    uint32_t reply_size = sizeof(reply);
    const int32_t status =
        context.HandleCommand(EFFECT_CMD_ENABLE, 0, nullptr, &reply_size, &reply);
    assert(status == 0);
}

int32_t QueryInt(ViperContext &context, int32_t param) {
    struct {
        effect_param_t header;
        int32_t param;
        int32_t value;
    } request{}, response{};
    request.header.psize = sizeof(int32_t);
    request.header.vsize = sizeof(int32_t);
    request.param = param;
    uint32_t reply_size = sizeof(response);
    const int32_t status = context.HandleCommand(
        EFFECT_CMD_GET_PARAM, sizeof(request), &request, &reply_size, &response);
    assert(status == 0);
    return response.value;
}

// A prepared graph becomes active only when Process() consumes the pending slot.
void PumpAudio(ViperContext &context, std::size_t frames = 256) {
    std::vector<int16_t> input(frames * viper::audio::kChannelCount, 0);
    std::vector<int16_t> output(frames * viper::audio::kChannelCount, 0);
    audio_buffer_t in_buffer{};
    audio_buffer_t out_buffer{};
    in_buffer.frame_count = frames;
    in_buffer.raw = input.data();
    out_buffer.frame_count = frames;
    out_buffer.raw = output.data();
    context.Process(&in_buffer, &out_buffer);
}

constexpr int32_t kParamGetEnabled = 1;
constexpr int32_t kParamGetConvolutionKernelId = 5;

// Everything a live driver instance needs: publisher owning the bridge plus a
// context registered as the snapshot applier.
struct DriverHarness {
    explicit DriverHarness(const std::string &socket_name, const DeviceIdentity &route)
        : publisher(socket_name) {
        Configure(context);
        context.SetDaemonRoute(HashOf(route));
        instance = publisher.RegisterContext(
            77,
            5,
            [this](
                viper::daemon::SnapshotCommandType type,
                std::span<const uint8_t> payload
            ) { return context.HandleSnapshotCommand(type, payload); }
        );
    }

    viper::audio::DriverEventPublisher publisher;
    ViperContext context;
    uint64_t instance = 0;
};

// A driver that refuses ROUTE_ANNOUNCE, standing in for one built before the command
// existed. Everything else behaves like DriverHarness.
//
// The refusal arrives as a NACK rather than silence: DriverDaemonBridge always
// answers a snapshot command, so a handler returning a default ack produces
// SNAPSHOT_APPLIED_NACK. A genuinely old driver was rejected one layer earlier, in
// IsSnapshotCommandType, and answered nothing at all. Both leave the route
// unannounced and must not spin the control loop; this reproduces the cheaper of the
// two, and the silent case is covered by the timeout in AwaitApplyOutcome.
struct LegacyDriverHarness {
    explicit LegacyDriverHarness(const std::string &socket_name)
        : publisher(socket_name) {
        Configure(context);
        instance = publisher.RegisterContext(
            78,
            6,
            [this](
                viper::daemon::SnapshotCommandType type,
                std::span<const uint8_t> payload
            ) -> viper::daemon::DriverDaemonBridge::SnapshotAck {
                if (type == viper::daemon::SnapshotCommandType::ROUTE_ANNOUNCE) {
                    ++announces_seen;
                    // Default ack == refusal, which the bridge turns into a NACK.
                    return {};
                }
                return context.HandleSnapshotCommand(type, payload);
            }
        );
    }

    viper::audio::DriverEventPublisher publisher;
    ViperContext context;
    uint64_t instance = 0;
    unsigned announces_seen = 0;
};

template <typename Predicate>
bool PumpUntil(DaemonRuntime &runtime, Predicate predicate, int attempts = 400) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        runtime.RunOnce();
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

DaemonConfig MakeConfig(const char *suffix, std::chrono::milliseconds debounce) {
    DaemonConfig config{};
    config.state_root = UniqueStateRoot(suffix);
    config.driver_socket_name = UniqueSocketName(suffix);
    // Both endpoints must be unique, not just the driver one. The defaults are the
    // production socket names, so leaving app_socket_name alone makes a test collide
    // with a daemon actually running on the machine and fail for the wrong reason.
    config.app_socket_name = UniqueSocketName(suffix) + ".app";
    config.route_debounce = debounce;
    return config;
}

void TestStoredSnapshotIsAppliedOnRouteDetection() {
    const DaemonConfig config = MakeConfig("apply", std::chrono::milliseconds(0));
    StoreSnapshot(config.state_root, SnapshotFor(SpeakerIdentity(), 4242));

    auto adapter = std::make_unique<FakeRouteAdapter>();
    adapter->SetRoute(SpeakerIdentity());
    DaemonRuntime runtime(config, std::move(adapter));
    std::string error;
    assert(runtime.Start(&error));

    DriverHarness driver(config.driver_socket_name, SpeakerIdentity());

    // Daemon-first: the restore happens without any App involvement.
    assert(PumpUntil(runtime, [&] { return runtime.Status().restores_accepted == 1; }));
    assert(runtime.Status().restores_rejected == 0);
    assert(runtime.Status().restores_bypassed == 0);
    assert(runtime.Server().Statistics().apply_acks >= 2); // BEGIN + COMMIT

    // The snapshot really reached the live DSP graph.
    Enable(driver.context);
    PumpAudio(driver.context);
    assert(QueryInt(driver.context, kParamGetConvolutionKernelId) == 4242);
    assert(QueryInt(driver.context, kParamGetEnabled) == 1);

    std::filesystem::remove_all(config.state_root);
}

void TestMissingSnapshotBypassesSafely() {
    // No snapshot stored for this route at all.
    const DaemonConfig config = MakeConfig("bypass", std::chrono::milliseconds(0));

    auto adapter = std::make_unique<FakeRouteAdapter>();
    adapter->SetRoute(SpeakerIdentity());
    DaemonRuntime runtime(config, std::move(adapter));
    std::string error;
    assert(runtime.Start(&error));

    DriverHarness driver(config.driver_socket_name, SpeakerIdentity());

    assert(PumpUntil(runtime, [&] { return runtime.Status().restores_bypassed == 1; }));
    // Bypass, not a rejected apply: nothing was sent to the driver.
    assert(runtime.Status().restores_attempted == 0);
    assert(runtime.Server().Statistics().snapshot_commands_sent == 0);

    // The driver keeps its defaults and stays usable.
    Enable(driver.context);
    PumpAudio(driver.context);
    assert(QueryInt(driver.context, kParamGetConvolutionKernelId) == 0);

    std::filesystem::remove_all(config.state_root);
}

void TestOtherDeviceSnapshotIsNotInherited() {
    const DaemonConfig config = MakeConfig("no-inherit", std::chrono::milliseconds(0));
    // Only the Bluetooth route has stored state; the live route is the speaker.
    StoreSnapshot(config.state_root, SnapshotFor(BluetoothIdentity(), 9999));

    auto adapter = std::make_unique<FakeRouteAdapter>();
    adapter->SetRoute(SpeakerIdentity());
    DaemonRuntime runtime(config, std::move(adapter));
    std::string error;
    assert(runtime.Start(&error));

    DriverHarness driver(config.driver_socket_name, SpeakerIdentity());

    assert(PumpUntil(runtime, [&] { return runtime.Status().restores_bypassed == 1; }));
    // Cross-device inheritance is the failure this whole key scheme prevents.
    assert(runtime.Status().restores_attempted == 0);
    Enable(driver.context);
    PumpAudio(driver.context);
    assert(QueryInt(driver.context, kParamGetConvolutionKernelId) != 9999);

    std::filesystem::remove_all(config.state_root);
}

void TestRouteChangeSelectsThatRoutesSnapshot() {
    const DaemonConfig config = MakeConfig("switch", std::chrono::milliseconds(0));
    StoreSnapshot(config.state_root, SnapshotFor(SpeakerIdentity(), 111));
    StoreSnapshot(config.state_root, SnapshotFor(BluetoothIdentity(), 222));

    auto adapter = std::make_unique<FakeRouteAdapter>();
    adapter->SetRoute(SpeakerIdentity());
    FakeRouteAdapter *route = adapter.get();
    DaemonRuntime runtime(config, std::move(adapter));
    std::string error;
    assert(runtime.Start(&error));

    DriverHarness driver(config.driver_socket_name, SpeakerIdentity());

    assert(PumpUntil(runtime, [&] { return runtime.Status().restores_accepted == 1; }));
    Enable(driver.context);
    PumpAudio(driver.context);
    assert(QueryInt(driver.context, kParamGetConvolutionKernelId) == 111);
    const uint64_t first_epoch = runtime.Status().route_epoch;

    // Move to Bluetooth: the driver must adopt that route's snapshot, not keep the
    // speaker's.
    route->SetRoute(BluetoothIdentity());
    driver.context.SetDaemonRoute(HashOf(BluetoothIdentity()));
    assert(PumpUntil(runtime, [&] { return runtime.Status().restores_accepted == 2; }));
    assert(runtime.Status().route_epoch > first_epoch);
    PumpAudio(driver.context);
    assert(QueryInt(driver.context, kParamGetConvolutionKernelId) == 222);

    std::filesystem::remove_all(config.state_root);
}

void TestTransientRouteIsDebounced() {
    const DaemonConfig config = MakeConfig("debounce", std::chrono::milliseconds(200));
    StoreSnapshot(config.state_root, SnapshotFor(SpeakerIdentity(), 555));
    StoreSnapshot(config.state_root, SnapshotFor(BluetoothIdentity(), 666));

    auto adapter = std::make_unique<FakeRouteAdapter>();
    adapter->SetRoute(SpeakerIdentity());
    FakeRouteAdapter *route = adapter.get();
    DaemonRuntime runtime(config, std::move(adapter));
    std::string error;
    assert(runtime.Start(&error));

    DriverHarness driver(config.driver_socket_name, BluetoothIdentity());

    // Within the debounce window the speaker route is superseded by Bluetooth, so
    // only the settled route is ever applied. Applying the transient one would
    // rebuild the DSP graph for a route the user never landed on.
    runtime.RunOnce();
    assert(runtime.Status().restores_attempted == 0);
    route->SetRoute(BluetoothIdentity());
    runtime.RunOnce();
    assert(runtime.Status().restores_attempted == 0);
    assert(runtime.Status().route_epoch == 2);

    assert(PumpUntil(runtime, [&] { return runtime.Status().restores_accepted == 1; }));
    // Exactly one apply: the speaker snapshot was never sent.
    assert(runtime.Status().restores_attempted == 1);
    Enable(driver.context);
    PumpAudio(driver.context);
    assert(QueryInt(driver.context, kParamGetConvolutionKernelId) == 666);

    std::filesystem::remove_all(config.state_root);
}

void TestDaemonGenerationAdvancesPerRestore() {
    const DaemonConfig config = MakeConfig("generation", std::chrono::milliseconds(0));
    StoreSnapshot(config.state_root, SnapshotFor(SpeakerIdentity(), 777));
    StoreSnapshot(config.state_root, SnapshotFor(BluetoothIdentity(), 888));

    auto adapter = std::make_unique<FakeRouteAdapter>();
    adapter->SetRoute(SpeakerIdentity());
    FakeRouteAdapter *route = adapter.get();
    DaemonRuntime runtime(config, std::move(adapter));
    std::string error;
    assert(runtime.Start(&error));

    DriverHarness driver(config.driver_socket_name, SpeakerIdentity());
    assert(PumpUntil(runtime, [&] { return runtime.Status().restores_accepted == 1; }));
    const uint64_t first = runtime.Status().daemon_generation;

    route->SetRoute(BluetoothIdentity());
    driver.context.SetDaemonRoute(HashOf(BluetoothIdentity()));
    assert(PumpUntil(runtime, [&] { return runtime.Status().restores_accepted == 2; }));

    // A monotonic daemon generation is what lets the App detect that the daemon
    // moved ahead and reconcile instead of overwriting the restore.
    assert(runtime.Status().daemon_generation > first);

    std::filesystem::remove_all(config.state_root);
}

// The daemon is the route authority, so a driver that connected without a route (or
// with a stale one) must be corrected by the announce rather than left refusing
// everything as DEVICE_MISMATCH. That refusal is exactly what happened on device:
// the driver only ever sees an AudioFlinger effect instance, never a mixer route.
void TestDriverRouteIsCorrectedByTheDaemonAnnounce() {
    const DaemonConfig config = MakeConfig("announce", std::chrono::milliseconds(0));
    StoreSnapshot(config.state_root, SnapshotFor(SpeakerIdentity(), 1234));

    auto adapter = std::make_unique<FakeRouteAdapter>();
    adapter->SetRoute(SpeakerIdentity());
    DaemonRuntime runtime(config, std::move(adapter));
    std::string error;
    assert(runtime.Start(&error));

    // The driver believes it is on Bluetooth; the daemon knows it is on speaker.
    DriverHarness driver(config.driver_socket_name, BluetoothIdentity());

    assert(PumpUntil(runtime, [&] { return runtime.Status().restores_accepted == 1; }));
    assert(runtime.Status().restores_rejected == 0);
    assert(runtime.Server().Statistics().route_announces_sent >= 1);
    assert(runtime.Server().Statistics().apply_nacks == 0);

    // The speaker snapshot reached the graph, which only happens if the driver
    // adopted the announced route before the transfer.
    Enable(driver.context);
    PumpAudio(driver.context);
    assert(QueryInt(driver.context, kParamGetConvolutionKernelId) == 1234);

    std::filesystem::remove_all(config.state_root);
}

// A driver that refuses the announce must be retried a bounded number of times and
// then left alone. On device this was unbounded: 459 failed announces accumulated
// while an old driver was connected, each one blocking the control loop for a full
// AwaitApplyOutcome timeout, so the daemon did nothing else.
void TestRefusedAnnounceStopsRetryingAfterTheBudget() {
    const DaemonConfig config = MakeConfig("announce-budget", std::chrono::milliseconds(0));
    StoreSnapshot(config.state_root, SnapshotFor(SpeakerIdentity(), 555));

    auto adapter = std::make_unique<FakeRouteAdapter>();
    adapter->SetRoute(SpeakerIdentity());
    DaemonRuntime runtime(config, std::move(adapter));
    std::string error;
    assert(runtime.Start(&error));

    LegacyDriverHarness driver(config.driver_socket_name);

    // Pump well past the budget. The retry delay means most iterations are no-ops.
    for (int attempt = 0; attempt < 60; ++attempt) {
        runtime.RunOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    const uint64_t sent = runtime.Server().Statistics().route_announces_sent;
    // Bounded, and the driver saw exactly what was sent.
    assert(sent >= 1);
    assert(sent <= 5);
    assert(driver.announces_seen == sent);
    // Never acked, so the route stays unannounced and that is visible.
    assert(runtime.Status().route_announces_acked == 0);

    // Pumping further must not resume: the budget is per connection, not per second.
    for (int attempt = 0; attempt < 60; ++attempt) {
        runtime.RunOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(runtime.Server().Statistics().route_announces_sent == sent);

    std::filesystem::remove_all(config.state_root);
}

// Second line of defence: a snapshot sitting in the wrong route's directory must
// never be applied, even though the announce has told the driver the right route.
// Only the daemon can catch this — the driver would see a device hash matching
// whatever it was just announced, so the payload would look legitimate to it.
void TestPlantedForeignSnapshotIsNotApplied() {
    const DaemonConfig config = MakeConfig("planted", std::chrono::milliseconds(0));
    // A valid Bluetooth snapshot, copied byte-for-byte into the speaker directory.
    PlantForeignSnapshot(
        config.state_root, HashOf(SpeakerIdentity()), SnapshotFor(BluetoothIdentity(), 4321));

    auto adapter = std::make_unique<FakeRouteAdapter>();
    adapter->SetRoute(SpeakerIdentity());
    DaemonRuntime runtime(config, std::move(adapter));
    std::string error;
    assert(runtime.Start(&error));

    DriverHarness driver(config.driver_socket_name, SpeakerIdentity());

    assert(PumpUntil(runtime, [&] { return runtime.Status().restores_bypassed >= 1; }));
    assert(runtime.Status().restores_accepted == 0);
    // Bypassed before any transfer: the driver was never asked to apply it.
    assert(runtime.Server().Statistics().snapshot_commands_sent == 0);

    Enable(driver.context);
    PumpAudio(driver.context);
    assert(QueryInt(driver.context, kParamGetConvolutionKernelId) == 0);

    std::filesystem::remove_all(config.state_root);
}

void TestRestoreWaitsForTheDriverToConnect() {
    const DaemonConfig config = MakeConfig("no-driver", std::chrono::milliseconds(0));
    StoreSnapshot(config.state_root, SnapshotFor(SpeakerIdentity(), 321));

    auto adapter = std::make_unique<FakeRouteAdapter>();
    adapter->SetRoute(SpeakerIdentity());
    DaemonRuntime runtime(config, std::move(adapter));
    std::string error;
    assert(runtime.Start(&error));

    // This is the real boot order: the late_start daemon detects its route long
    // before AudioFlinger loads the effect. Nothing may be attempted yet, and the
    // pending restore must not be discarded.
    for (int pass = 0; pass < 10; ++pass) runtime.RunOnce();
    assert(runtime.Status().route_known);
    assert(runtime.Status().restores_attempted == 0);
    assert(runtime.Status().restores_rejected == 0);
    assert(runtime.Status().restores_bypassed == 0);
    assert(runtime.Server().Statistics().snapshot_commands_sent == 0);

    // The driver appears; the still-pending restore now runs without any further
    // route change.
    DriverHarness driver(config.driver_socket_name, SpeakerIdentity());
    assert(PumpUntil(runtime, [&] { return runtime.Status().restores_accepted == 1; }));
    assert(runtime.Status().restores_attempted == 1);

    Enable(driver.context);
    PumpAudio(driver.context);
    assert(QueryInt(driver.context, kParamGetConvolutionKernelId) == 321);

    std::filesystem::remove_all(config.state_root);
}

void TestObserveOnlyRuntimeNeverApplies() {
    DaemonConfig config = MakeConfig("observe", std::chrono::milliseconds(0));
    config.restore_enabled = false;
    StoreSnapshot(config.state_root, SnapshotFor(SpeakerIdentity(), 4321));

    auto adapter = std::make_unique<FakeRouteAdapter>();
    adapter->SetRoute(SpeakerIdentity());
    DaemonRuntime runtime(config, std::move(adapter));
    std::string error;
    assert(runtime.Start(&error));

    DriverHarness driver(config.driver_socket_name, SpeakerIdentity());

    for (int pass = 0; pass < 20; ++pass) runtime.RunOnce();
    // Restore disabled must mean exactly that: no command reaches the driver.
    assert(runtime.Status().restores_attempted == 0);
    assert(runtime.Status().restores_bypassed == 0);
    assert(runtime.Server().Statistics().snapshot_commands_sent == 0);

    Enable(driver.context);
    PumpAudio(driver.context);
    assert(QueryInt(driver.context, kParamGetConvolutionKernelId) == 0);

    std::filesystem::remove_all(config.state_root);
}

// A context that appears without a route change must still get the route's state.
//
// This is the boot path once the daemon owns the effect handle: the owner process
// creates a handle, AudioFlinger loads the driver, and a fresh ViperContext appears
// with default parameters. The route did not move, so keying restore on route change
// alone leaves that context unconfigured - silently wrong audio, not silence.
//
// The second harness reuses the same route and socket, standing in for the owner
// re-creating its handle (respawn, or AudioFlinger rebuilding the effect chain).
void TestNewDriverContextTriggersRestore() {
    const DaemonConfig config = MakeConfig("newctx", std::chrono::milliseconds(0));
    StoreSnapshot(config.state_root, SnapshotFor(SpeakerIdentity(), 1717));

    auto adapter = std::make_unique<FakeRouteAdapter>();
    adapter->SetRoute(SpeakerIdentity());
    DaemonRuntime runtime(config, std::move(adapter));
    std::string error;
    assert(runtime.Start(&error));

    {
        DriverHarness first(config.driver_socket_name, SpeakerIdentity());
        assert(PumpUntil(runtime, [&] {
            return runtime.Status().restores_accepted == 1;
        }));
    }

    // The first driver is gone. Its context release must reach the registry before
    // the replacement connects, otherwise the next restore cannot be attributed.
    assert(PumpUntil(runtime, [&] { return !runtime.Server().Connected(); }));

    DriverHarness second(config.driver_socket_name, SpeakerIdentity());

    // No route change happened between the two drivers: only the new context can
    // arm this restore.
    const uint64_t epoch_before = runtime.RouteEpoch();
    assert(PumpUntil(runtime, [&] {
        return runtime.Status().restores_accepted == 2;
    }));
    assert(runtime.RouteEpoch() == epoch_before);
    assert(runtime.Status().restores_rejected == 0);

    // And it reached the new context's live graph, not just the counters.
    Enable(second.context);
    PumpAudio(second.context);
    assert(QueryInt(second.context, kParamGetConvolutionKernelId) == 1717);
    assert(QueryInt(second.context, kParamGetEnabled) == 1);

    std::filesystem::remove_all(config.state_root);
}

// A replacement driver can connect before the dead one's CONTEXT_RELEASED has been
// processed, so the registry briefly holds two contexts. The restore must still
// happen exactly once for the new context, and must not re-fire on every later
// poll: repeated applies would rebuild the DSP graph under live audio.
void TestReconnectBeforeReleaseRestoresExactlyOnce() {
    const DaemonConfig config = MakeConfig("reconnect-race", std::chrono::milliseconds(0));
    StoreSnapshot(config.state_root, SnapshotFor(SpeakerIdentity(), 2626));

    auto adapter = std::make_unique<FakeRouteAdapter>();
    adapter->SetRoute(SpeakerIdentity());
    DaemonRuntime runtime(config, std::move(adapter));
    std::string error;
    assert(runtime.Start(&error));

    auto first = std::make_unique<DriverHarness>(config.driver_socket_name, SpeakerIdentity());
    assert(PumpUntil(runtime, [&] { return runtime.Status().restores_accepted == 1; }));

    // Drop the first driver and connect the replacement without pumping in between,
    // so the release and the new connection are observed in the same batch.
    first.reset();
    DriverHarness second(config.driver_socket_name, SpeakerIdentity());

    const uint64_t epoch_before = runtime.RouteEpoch();
    assert(PumpUntil(runtime, [&] { return runtime.Status().restores_accepted == 2; }));

    // Settle: any per-poll re-fire would show up as a third restore here.
    for (int pass = 0; pass < 60; ++pass) runtime.RunOnce();
    assert(runtime.Status().restores_accepted == 2);
    assert(runtime.Status().restores_attempted == 2);
    assert(runtime.Status().restores_rejected == 0);
    // No route change was involved: only the new context armed this restore.
    assert(runtime.RouteEpoch() == epoch_before);

    // And it reached the surviving context's live graph.
    Enable(second.context);
    PumpAudio(second.context);
    assert(QueryInt(second.context, kParamGetConvolutionKernelId) == 2626);
    assert(QueryInt(second.context, kParamGetEnabled) == 1);

    std::filesystem::remove_all(config.state_root);
}

// A torn or truncated current.snapshot must fall back to previous.snapshot rather
// than bypassing to driver defaults. The store keeps both files precisely so a
// failed write cannot cost the user their settings; bypassing here would throw that
// away on the next boot and look like the daemon had simply forgotten them.
void TestCorruptCurrentSnapshotFallsBackToPrevious() {
    const DaemonConfig config = MakeConfig("corrupt", std::chrono::milliseconds(0));
    // Two commits, so previous.snapshot holds the older valid kernel id.
    StoreSnapshot(config.state_root, SnapshotFor(SpeakerIdentity(), 3131, 4, 1));
    StoreSnapshot(config.state_root, SnapshotFor(SpeakerIdentity(), 3232, 5, 2));

    // Corrupt only the current file, exactly as an interrupted write would.
    const auto current_path = config.state_root / "routes"
        / HashOf(SpeakerIdentity()) / "current.snapshot";
    {
        std::ofstream corrupt(current_path, std::ios::binary | std::ios::trunc);
        corrupt << "corrupt";
    }

    auto adapter = std::make_unique<FakeRouteAdapter>();
    adapter->SetRoute(SpeakerIdentity());
    DaemonRuntime runtime(config, std::move(adapter));
    std::string error;
    assert(runtime.Start(&error));

    DriverHarness driver(config.driver_socket_name, SpeakerIdentity());
    assert(PumpUntil(runtime, [&] { return runtime.Status().restores_accepted == 1; }));
    assert(runtime.Status().restores_rejected == 0);
    // Not a bypass: a recoverable store must not be reported as "nothing stored".
    assert(runtime.Status().restores_bypassed == 0);

    // The previous generation's state reached the graph, not the driver defaults.
    Enable(driver.context);
    PumpAudio(driver.context);
    assert(QueryInt(driver.context, kParamGetConvolutionKernelId) == 3131);
    assert(QueryInt(driver.context, kParamGetEnabled) == 1);

    std::filesystem::remove_all(config.state_root);
}

// The fallback must not become a second way to inherit foreign state: a previous
// snapshot belonging to another device is still refused.
void TestCorruptCurrentDoesNotInheritForeignPrevious() {
    const DaemonConfig config = MakeConfig("corrupt-foreign", std::chrono::milliseconds(0));
    StoreSnapshot(config.state_root, SnapshotFor(SpeakerIdentity(), 4141, 4, 1));
    StoreSnapshot(config.state_root, SnapshotFor(SpeakerIdentity(), 4242, 5, 2));

    const auto route_directory = config.state_root / "routes" / HashOf(SpeakerIdentity());
    {
        std::ofstream corrupt(route_directory / "current.snapshot",
                              std::ios::binary | std::ios::trunc);
        corrupt << "corrupt";
    }
    // A valid Bluetooth snapshot forged into the speaker route's previous slot.
    PlantForeignSnapshot(
        config.state_root, HashOf(SpeakerIdentity()), SnapshotFor(BluetoothIdentity(), 9191));
    std::filesystem::rename(
        route_directory / "current.snapshot", route_directory / "previous.snapshot");
    {
        std::ofstream corrupt(route_directory / "current.snapshot",
                              std::ios::binary | std::ios::trunc);
        corrupt << "corrupt";
    }

    auto adapter = std::make_unique<FakeRouteAdapter>();
    adapter->SetRoute(SpeakerIdentity());
    DaemonRuntime runtime(config, std::move(adapter));
    std::string error;
    assert(runtime.Start(&error));

    DriverHarness driver(config.driver_socket_name, SpeakerIdentity());
    assert(PumpUntil(runtime, [&] { return runtime.Status().restores_bypassed >= 1; }));
    // Both files unusable for this route: bypass, and nothing was sent to the driver.
    assert(runtime.Status().restores_accepted == 0);
    assert(runtime.Status().restores_attempted == 0);
    assert(runtime.Server().Statistics().snapshot_commands_sent == 0);

    Enable(driver.context);
    PumpAudio(driver.context);
    assert(QueryInt(driver.context, kParamGetConvolutionKernelId) == 0);

    std::filesystem::remove_all(config.state_root);
}

} // namespace

int main() {
    TestStoredSnapshotIsAppliedOnRouteDetection();
    TestMissingSnapshotBypassesSafely();
    TestOtherDeviceSnapshotIsNotInherited();
    TestRouteChangeSelectsThatRoutesSnapshot();
    TestTransientRouteIsDebounced();
    TestDaemonGenerationAdvancesPerRestore();
    TestDriverRouteIsCorrectedByTheDaemonAnnounce();
    TestRefusedAnnounceStopsRetryingAfterTheBudget();
    TestPlantedForeignSnapshotIsNotApplied();
    TestRestoreWaitsForTheDriverToConnect();
    TestNewDriverContextTriggersRestore();
    TestReconnectBeforeReleaseRestoresExactlyOnce();
    TestCorruptCurrentSnapshotFallsBackToPrevious();
    TestCorruptCurrentDoesNotInheritForeignPrevious();
    TestObserveOnlyRuntimeNeverApplies();
    std::puts("route restore integration tests passed");
    return 0;
}
