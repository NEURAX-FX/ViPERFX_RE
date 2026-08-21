// Full App -> daemon -> driver path, with no fake on either end of the wire.
//
// An App-side client speaks the real @viper4android.app.v1 protocol into a real
// DaemonRuntime, which relays into a real ViperContext over the real
// @viper4android.driver.v1 socket. Only the App's socket client is written here,
// because a host test cannot run the Kotlin App; every other component is
// production code.
//
// This is the path the device could not exercise: the App used to connect to the
// driver endpoint and be refused by its peer check, so nothing it sent ever
// reached the driver.
#include "DaemonRuntime.h"

#include "AppCommand.h"
#include "AudioFormat.h"
#include "DeviceKey.h"
#include "DriverEventPublisher.h"
#include "ParameterStream.h"
#include "SnapshotCommand.h"
#include "SnapshotSchema.h"
#include "ViPERParams.h"
#include "ViperContext.h"
#include "ViperDaemonProtocol.h"
#include "viper/constants.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

using viper::daemon::AppApplyResult;
using viper::daemon::AppHello;
using viper::daemon::AppHelloAck;
using viper::daemon::AppMessageType;
using viper::daemon::AppRouteAck;
using viper::daemon::AppRouteReport;
using viper::daemon::DaemonConfig;
using viper::daemon::DaemonRuntime;
using viper::daemon::DeviceIdentity;
using viper::daemon::FrameHeader;
using viper::daemon::Snapshot;
using viper::daemon::SnapshotCommandType;
using viper::daemon::SnapshotStore;

std::string UniqueName(const char *suffix) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::string("viper4android.e2eapp.") + suffix + "." + std::to_string(stamp);
}

std::filesystem::path UniqueStateRoot(const char *suffix) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path()
        / ("viper-e2eapp-" + std::string(suffix) + "-" + std::to_string(stamp));
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

AppRouteReport ReportFor(const DeviceIdentity &identity) {
    AppRouteReport report{};
    report.route_type = identity.route_type;
    report.stable_address_or_port = identity.stable_address_or_port;
    report.product_name = identity.product_name;
    report.encoding = identity.encoding;
    report.sample_rate = identity.sample_rate;
    report.channel_mask = identity.channel_mask;
    report.output_flags = identity.output_flags;
    return report;
}

// A snapshot whose only observable effect is the convolver kernel id, so a test
// can prove exactly which state reached the live graph.
Snapshot SnapshotFor(
    const DeviceIdentity &identity,
    int32_t kernel_id,
    uint64_t app_generation,
    uint64_t daemon_generation,
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

// Live driver instance: publisher owning the bridge plus a context registered as
// the snapshot applier, exactly as ViPER4Android.cpp wires it.
struct DriverHarness {
    DriverHarness(const std::string &socket_name, const DeviceIdentity &route)
        : publisher(socket_name) {
        Configure(context);
        context.SetDaemonRoute(HashOf(route));
        instance = publisher.RegisterContext(
            77,
            5,
            [this](SnapshotCommandType type, std::span<const uint8_t> payload) {
                return context.HandleSnapshotCommand(type, payload);
            }
        );
    }

    viper::audio::DriverEventPublisher publisher;
    ViperContext context;
    uint64_t instance = 0;
};

// The App's side of @viper4android.app.v1. Blocking reads are fine because the
// daemon is pumped from this same thread between sends.
class AppClient final {
public:
    ~AppClient() { Close(); }

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

    bool Send(uint16_t message_type, const std::vector<uint8_t> &payload, uint64_t request_id) {
        FrameHeader header{};
        header.message_type = message_type;
        header.request_id = request_id;
        const std::string_view view(
            reinterpret_cast<const char *>(payload.data()), payload.size());
        std::vector<uint8_t> frame;
        std::string error;
        if (!viper::daemon::EncodeFrame(header, view, &frame, &error)) return false;
        return ::send(fd_, frame.data(), frame.size(), 0)
            == static_cast<ssize_t>(frame.size());
    }

    bool Receive(FrameHeader *header, std::vector<uint8_t> *payload) {
        std::vector<uint8_t> buffer(viper::daemon::kMaxFrameSize, 0U);
        const ssize_t received = ::recv(fd_, buffer.data(), buffer.size(), MSG_DONTWAIT);
        if (received <= 0) return false;
        std::string error;
        return viper::daemon::DecodeFrame(
            std::span<const uint8_t>(buffer.data(), static_cast<std::size_t>(received)),
            header,
            payload,
            &error
        );
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

template <typename Predicate>
bool PumpUntil(DaemonRuntime &runtime, Predicate predicate, int attempts = 400) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        runtime.RunOnce();
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

// Pumps the daemon until the client has a reply of `message_type`.
bool PumpForReply(
    DaemonRuntime &runtime,
    AppClient &client,
    uint16_t message_type,
    FrameHeader *header,
    std::vector<uint8_t> *payload,
    int attempts = 400
) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        runtime.RunOnce();
        if (client.Receive(header, payload) && header->message_type == message_type) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

DaemonConfig MakeConfig(const char *suffix) {
    DaemonConfig config{};
    config.state_root = UniqueStateRoot(suffix);
    config.driver_socket_name = UniqueName(suffix);
    config.app_socket_name = UniqueName(std::string(std::string(suffix) + ".app").c_str());
    // Zero debounce: the settle window is covered by the route-restore test, and
    // waiting for it here would only slow every case down.
    config.route_debounce = std::chrono::milliseconds(0);
    return config;
}

// Streams a snapshot the way DaemonClient does: BEGIN, chunks, COMMIT.
bool StreamSnapshot(
    DaemonRuntime &runtime,
    AppClient &client,
    const Snapshot &snapshot,
    uint64_t request_id,
    AppApplyResult *result
) {
    std::vector<uint8_t> bytes;
    std::string error;
    if (!viper::daemon::EncodeSnapshot(snapshot, &bytes, &error)) return false;

    viper::daemon::SnapshotBegin begin{};
    begin.app_generation = snapshot.app_generation;
    begin.daemon_generation = snapshot.daemon_generation;
    begin.total_size = static_cast<uint32_t>(bytes.size());
    begin.crc32 = viper::daemon::Crc32(bytes);
    begin.device_key_hash = snapshot.device_key_hash;

    std::vector<uint8_t> payload;
    if (!viper::daemon::EncodeSnapshotBegin(begin, &payload, &error)) return false;
    if (!client.Send(
            static_cast<uint16_t>(SnapshotCommandType::SNAPSHOT_BEGIN), payload, request_id)) {
        return false;
    }

    FrameHeader header{};
    std::vector<uint8_t> reply;
    if (!PumpForReply(
            runtime,
            client,
            static_cast<uint16_t>(AppMessageType::APP_APPLY_RESULT),
            &header,
            &reply
        )) {
        return false;
    }
    AppApplyResult begin_result{};
    if (!viper::daemon::DecodeAppApplyResult(reply, &begin_result, &error)) return false;
    if (!begin_result.accepted) {
        if (result != nullptr) *result = begin_result;
        return false;
    }

    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const std::size_t size =
            std::min<std::size_t>(viper::daemon::kMaxSnapshotChunkBytes, bytes.size() - offset);
        viper::daemon::SnapshotChunk chunk{};
        chunk.offset = static_cast<uint32_t>(offset);
        chunk.data.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                          bytes.begin() + static_cast<std::ptrdiff_t>(offset + size));
        if (!viper::daemon::EncodeSnapshotChunk(chunk, &payload, &error)) return false;
        if (!client.Send(
                static_cast<uint16_t>(SnapshotCommandType::SNAPSHOT_CHUNK), payload, request_id)) {
            return false;
        }
        // Chunks are not individually acknowledged, so just let the daemon drain.
        runtime.RunOnce();
        offset += size;
    }

    viper::daemon::SnapshotCommit commit{};
    commit.app_generation = snapshot.app_generation;
    commit.daemon_generation = snapshot.daemon_generation;
    if (!viper::daemon::EncodeSnapshotCommit(commit, &payload, &error)) return false;
    if (!client.Send(
            static_cast<uint16_t>(SnapshotCommandType::SNAPSHOT_COMMIT), payload, request_id)) {
        return false;
    }
    if (!PumpForReply(
            runtime,
            client,
            static_cast<uint16_t>(AppMessageType::APP_APPLY_RESULT),
            &header,
            &reply
        )) {
        return false;
    }
    AppApplyResult commit_result{};
    if (!viper::daemon::DecodeAppApplyResult(reply, &commit_result, &error)) return false;
    if (result != nullptr) *result = commit_result;
    return commit_result.accepted;
}

// Connects, handshakes and reports a route. Returns the route ack.
bool Attach(
    DaemonRuntime &runtime,
    AppClient &client,
    const DaemonConfig &config,
    const DeviceIdentity &route,
    AppHelloAck *hello_ack,
    AppRouteAck *route_ack
) {
    if (!client.Connect(config.app_socket_name)) return false;

    std::vector<uint8_t> payload;
    std::string error;
    AppHello hello{};
    hello.app_generation = 1;
    if (!viper::daemon::EncodeAppHello(hello, &payload, &error)) return false;
    if (!client.Send(static_cast<uint16_t>(AppMessageType::APP_HELLO), payload, 1)) return false;

    FrameHeader header{};
    std::vector<uint8_t> reply;
    if (!PumpForReply(
            runtime, client, static_cast<uint16_t>(AppMessageType::APP_HELLO_ACK), &header, &reply
        )) {
        return false;
    }
    if (!viper::daemon::DecodeAppHelloAck(reply, hello_ack, &error)) return false;

    if (!viper::daemon::EncodeAppRouteReport(ReportFor(route), &payload, &error)) return false;
    if (!client.Send(static_cast<uint16_t>(AppMessageType::APP_ROUTE_REPORT), payload, 2)) {
        return false;
    }
    if (!PumpForReply(
            runtime, client, static_cast<uint16_t>(AppMessageType::APP_ROUTE_ACK), &header, &reply
        )) {
        return false;
    }
    return viper::daemon::DecodeAppRouteAck(reply, route_ack, &error);
}

void TestAppStateReachesTheDriverThroughTheDaemon() {
    const DaemonConfig config = MakeConfig("apply");
    auto runtime = DaemonRuntime(config, nullptr);
    std::string error;
    assert(runtime.Start(&error));

    DriverHarness driver(config.driver_socket_name, SpeakerIdentity());
    assert(PumpUntil(runtime, [&] { return runtime.Status().driver_connected; }));

    AppClient client;
    AppHelloAck hello_ack{};
    AppRouteAck route_ack{};
    assert(Attach(runtime, client, config, SpeakerIdentity(), &hello_ack, &route_ack));

    // The daemon had no route until the App named one: that is the whole reason
    // this endpoint exists on a device with no headset switch node.
    assert(route_ack.accepted);
    assert(route_ack.route_key_hash == HashOf(SpeakerIdentity()));
    assert(runtime.Status().route_known);
    assert(runtime.Status().route_from_app);

    AppApplyResult result{};
    const Snapshot snapshot =
        SnapshotFor(SpeakerIdentity(), 4242, /*app_generation=*/5, route_ack.daemon_generation);
    assert(StreamSnapshot(runtime, client, snapshot, 3, &result));
    assert(result.accepted);

    // The kernel id proves the App's parameters were replayed into the live graph,
    // not merely accepted at the socket.
    Enable(driver.context);
    PumpAudio(driver.context);
    assert(QueryInt(driver.context, kParamGetConvolutionKernelId) == 4242);
    assert(QueryInt(driver.context, kParamGetEnabled) == 1);

    assert(runtime.Status().app_route_reports == 1);
    assert(runtime.Status().app_snapshot_commands >= 2);
    assert(runtime.Status().app_rejected_peers == 0);

    std::filesystem::remove_all(config.state_root);
}

void TestReportedRouteIsCachedForTheNextBoot() {
    const DaemonConfig config = MakeConfig("cache");
    {
        auto runtime = DaemonRuntime(config, nullptr);
        std::string error;
        assert(runtime.Start(&error));

        DriverHarness driver(config.driver_socket_name, BluetoothIdentity());
        assert(PumpUntil(runtime, [&] { return runtime.Status().driver_connected; }));

        AppClient client;
        AppHelloAck hello_ack{};
        AppRouteAck route_ack{};
        assert(Attach(runtime, client, config, BluetoothIdentity(), &hello_ack, &route_ack));
        assert(route_ack.accepted);

        AppApplyResult result{};
        const Snapshot snapshot = SnapshotFor(
            BluetoothIdentity(), 777, /*app_generation=*/9, route_ack.daemon_generation);
        assert(StreamSnapshot(runtime, client, snapshot, 3, &result));
        assert(result.accepted);
    }

    // A fresh runtime on the same state root stands in for the next boot: it must
    // recover the route from the cache and restore before any App connects. Without
    // this the daemon would sit with route_known=0 forever on this device.
    auto restarted = DaemonRuntime(config, nullptr);
    std::string error;
    assert(restarted.Start(&error));
    assert(restarted.Status().route_known);
    assert(restarted.Status().route_key_hash == HashOf(BluetoothIdentity()));

    DriverHarness driver(config.driver_socket_name, BluetoothIdentity());
    assert(PumpUntil(restarted, [&] { return restarted.Status().restores_accepted == 1; }));

    Enable(driver.context);
    PumpAudio(driver.context);
    assert(QueryInt(driver.context, kParamGetConvolutionKernelId) == 777);

    std::filesystem::remove_all(config.state_root);
}

void TestForeignRouteSnapshotIsRefused() {
    const DaemonConfig config = MakeConfig("foreign");
    auto runtime = DaemonRuntime(config, nullptr);
    std::string error;
    assert(runtime.Start(&error));

    // The driver is on speaker; the App reports speaker but then streams a snapshot
    // keyed to Bluetooth. Accepting it would apply another device's settings.
    DriverHarness driver(config.driver_socket_name, SpeakerIdentity());
    assert(PumpUntil(runtime, [&] { return runtime.Status().driver_connected; }));

    AppClient client;
    AppHelloAck hello_ack{};
    AppRouteAck route_ack{};
    assert(Attach(runtime, client, config, SpeakerIdentity(), &hello_ack, &route_ack));

    AppApplyResult result{};
    const Snapshot foreign =
        SnapshotFor(BluetoothIdentity(), 999, /*app_generation=*/5, route_ack.daemon_generation);
    assert(!StreamSnapshot(runtime, client, foreign, 3, &result));
    assert(!result.accepted);

    Enable(driver.context);
    PumpAudio(driver.context);
    assert(QueryInt(driver.context, kParamGetConvolutionKernelId) == 0);

    std::filesystem::remove_all(config.state_root);
}

void TestSnapshotWithoutADriverIsRefusedNotLost() {
    const DaemonConfig config = MakeConfig("nodriver");
    auto runtime = DaemonRuntime(config, nullptr);
    std::string error;
    assert(runtime.Start(&error));

    AppClient client;
    AppHelloAck hello_ack{};
    AppRouteAck route_ack{};
    assert(Attach(runtime, client, config, SpeakerIdentity(), &hello_ack, &route_ack));
    // No driver has loaded yet, so the handshake must say so rather than implying
    // an apply would land.
    assert((hello_ack.flags & viper::daemon::kAppFlagDriverConnected) == 0U);

    AppApplyResult result{};
    const Snapshot snapshot =
        SnapshotFor(SpeakerIdentity(), 55, /*app_generation=*/5, route_ack.daemon_generation);
    assert(!StreamSnapshot(runtime, client, snapshot, 3, &result));
    assert(!result.accepted);
    // Refused, not silently dropped: the App must be able to fall back.
    assert(result.error_code != 0);

    std::filesystem::remove_all(config.state_root);
}

void TestRouteChangeFromTheAppRekeysTheDaemon() {
    const DaemonConfig config = MakeConfig("rekey");
    auto runtime = DaemonRuntime(config, nullptr);
    std::string error;
    assert(runtime.Start(&error));

    DriverHarness driver(config.driver_socket_name, SpeakerIdentity());
    assert(PumpUntil(runtime, [&] { return runtime.Status().driver_connected; }));

    AppClient client;
    AppHelloAck hello_ack{};
    AppRouteAck first{};
    assert(Attach(runtime, client, config, SpeakerIdentity(), &hello_ack, &first));
    assert(first.route_key_hash == HashOf(SpeakerIdentity()));
    const uint64_t first_epoch = first.route_epoch;

    // The user unplugs and the App reports the new route. The daemon must re-key,
    // otherwise the next snapshot would be filed under the old device.
    std::vector<uint8_t> payload;
    assert(viper::daemon::EncodeAppRouteReport(
        ReportFor(BluetoothIdentity()), &payload, &error));
    assert(client.Send(static_cast<uint16_t>(AppMessageType::APP_ROUTE_REPORT), payload, 9));

    FrameHeader header{};
    std::vector<uint8_t> reply;
    assert(PumpForReply(
        runtime, client, static_cast<uint16_t>(AppMessageType::APP_ROUTE_ACK), &header, &reply));
    AppRouteAck second{};
    assert(viper::daemon::DecodeAppRouteAck(reply, &second, &error));
    assert(second.accepted);
    assert(second.route_key_hash == HashOf(BluetoothIdentity()));
    assert(second.route_epoch > first_epoch);

    // Reporting the same route again is not a change, so the epoch must hold: a
    // moving epoch would make the App think it had missed a restore.
    assert(client.Send(static_cast<uint16_t>(AppMessageType::APP_ROUTE_REPORT), payload, 10));
    assert(PumpForReply(
        runtime, client, static_cast<uint16_t>(AppMessageType::APP_ROUTE_ACK), &header, &reply));
    AppRouteAck third{};
    assert(viper::daemon::DecodeAppRouteAck(reply, &third, &error));
    assert(third.accepted);
    assert(third.route_epoch == second.route_epoch);

    std::filesystem::remove_all(config.state_root);
}

void TestMalformedRouteReportIsRefusedWithoutBreakingTheSession() {
    const DaemonConfig config = MakeConfig("malformed");
    auto runtime = DaemonRuntime(config, nullptr);
    std::string error;
    assert(runtime.Start(&error));

    DriverHarness driver(config.driver_socket_name, SpeakerIdentity());
    assert(PumpUntil(runtime, [&] { return runtime.Status().driver_connected; }));

    AppClient client;
    assert(client.Connect(config.app_socket_name));

    // Garbage that is not even a frame, then a valid hello. The session must
    // survive: dropping it would leave a working App unable to reconnect.
    assert(client.Send(static_cast<uint16_t>(AppMessageType::APP_ROUTE_REPORT), {1, 2, 3}, 1));
    runtime.RunOnce();

    std::vector<uint8_t> payload;
    AppHello hello{};
    hello.app_generation = 1;
    assert(viper::daemon::EncodeAppHello(hello, &payload, &error));
    assert(client.Send(static_cast<uint16_t>(AppMessageType::APP_HELLO), payload, 2));

    FrameHeader header{};
    std::vector<uint8_t> reply;
    assert(PumpForReply(
        runtime, client, static_cast<uint16_t>(AppMessageType::APP_HELLO_ACK), &header, &reply));
    assert(header.request_id == 2);
    assert(runtime.Status().app_rejected_frames >= 1);
    assert(!runtime.Status().route_known);

    std::filesystem::remove_all(config.state_root);
}

} // namespace

int main() {
    TestAppStateReachesTheDriverThroughTheDaemon();
    TestReportedRouteIsCachedForTheNextBoot();
    TestForeignRouteSnapshotIsRefused();
    TestSnapshotWithoutADriverIsRefusedNotLost();
    TestRouteChangeFromTheAppRekeysTheDaemon();
    TestMalformedRouteReportIsRefusedWithoutBreakingTheSession();
    std::puts("app/daemon/driver end-to-end tests passed");
    return 0;
}
