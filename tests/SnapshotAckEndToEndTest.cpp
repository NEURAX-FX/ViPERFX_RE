// Proves a daemon snapshot apply command travels over the real
// @viper4android.driver.v1 socket into the real ViperContext, and that the
// resulting ACK/NACK frame travels back. Every other test in this area stops at
// an in-process call boundary.
#include "ViperContext.h"

#include "AudioFormat.h"
#include "DeviceKey.h"
#include "DriverEventPublisher.h"
#include "FakeDaemonServer.h"
#include "ParameterStream.h"
#include "SnapshotCommand.h"
#include "SnapshotSchema.h"
#include "ViPERParams.h"
#include "ViperDaemonProtocol.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using viper::audio::ApplyError;
using viper::daemon::DeviceIdentity;
using viper::daemon::DriverEvent;
using viper::daemon::DriverEventType;
using viper::daemon::RawParamRecord;
using viper::daemon::Snapshot;
using viper::daemon::SnapshotCommandType;

constexpr auto kTimeout = std::chrono::milliseconds(3000);
constexpr uint32_t kSampleRate = 48000;

DeviceIdentity SpeakerIdentity() {
    DeviceIdentity identity{};
    identity.route_type = "speaker";
    identity.stable_address_or_port = "builtin";
    identity.product_name = "internal";
    identity.sample_rate = kSampleRate;
    identity.channel_mask = 3;
    identity.encoding = "pcm_16";
    return identity;
}

std::string RouteHash() {
    return viper::daemon::HashDeviceKey(
        viper::daemon::NormalizeDeviceKey(SpeakerIdentity()));
}

void Configure(ViperContext &context) {
    effect_config_t config{};
    config.input_cfg.buffer.frame_count = viper::audio::kMaxBlockFrames;
    config.input_cfg.sampling_rate = kSampleRate;
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
    assert(reply == 0);
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

std::vector<uint8_t> ParameterBlob(int32_t kernel_id) {
    using namespace viper::params;
    const std::size_t samples = 64;
    std::vector<float> kernel(samples, 0.0F);
    kernel[0] = 1.0F;

    RawParamRecord fill{};
    fill.param = kParamConvolverSetBuffer;
    fill.arr_size = static_cast<uint32_t>(samples);
    fill.payload.assign(samples * sizeof(float), 0U);
    std::memcpy(fill.payload.data(), kernel.data(), fill.payload.size());
    const uint32_t crc = viper::daemon::Crc32(fill.payload);

    RawParamRecord enable{};
    enable.param = kParamConvolverEnable;
    enable.val1 = 1;

    RawParamRecord prepare{};
    prepare.param = kParamConvolverPrepareBuffer;
    prepare.val1 = static_cast<int32_t>(samples);
    prepare.val2 = 2;

    RawParamRecord commit{};
    commit.param = kParamConvolverCommitBuffer;
    commit.val1 = static_cast<int32_t>(samples);
    commit.val2 = static_cast<int32_t>(crc);
    commit.val3 = kernel_id;

    std::vector<uint8_t> encoded;
    std::string error;
    const bool ok = viper::daemon::EncodeParameterStream(
        {enable, prepare, fill, commit}, &encoded, &error);
    assert(ok);
    return encoded;
}

std::vector<uint8_t> EncodeSnapshotBytes(
    uint64_t app_generation,
    uint64_t daemon_generation,
    int32_t kernel_id
) {
    Snapshot snapshot{};
    snapshot.device_key = viper::daemon::NormalizeDeviceKey(SpeakerIdentity());
    snapshot.device_key_hash = viper::daemon::HashDeviceKey(snapshot.device_key);
    snapshot.boot_id = 0xB007ULL;
    snapshot.daemon_generation = daemon_generation;
    snapshot.app_generation = app_generation;
    snapshot.created_at_millis = 1700000000000ULL;
    snapshot.master_enabled = true;
    snapshot.parameters = ParameterBlob(kernel_id);

    std::vector<uint8_t> bytes;
    std::string error;
    const bool ok = viper::daemon::EncodeSnapshot(snapshot, &bytes, &error);
    assert(ok);
    return bytes;
}

// Reads frames until an apply result arrives, skipping lifecycle/replay events.
bool AwaitApplyResult(viper::test::FakeDaemonServer &server, DriverEvent *event) {
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (!server.ReceiveEvent(event, kTimeout)) return false;
        if (event->type == DriverEventType::SNAPSHOT_APPLIED_ACK
            || event->type == DriverEventType::SNAPSHOT_APPLIED_NACK) {
            return true;
        }
    }
    return false;
}

bool SendBegin(
    viper::test::FakeDaemonServer &server,
    const std::vector<uint8_t> &bytes,
    uint64_t app_generation,
    uint64_t daemon_generation,
    const std::string &device_hash,
    uint64_t request_id
) {
    viper::daemon::SnapshotBegin begin{};
    begin.app_generation = app_generation;
    begin.daemon_generation = daemon_generation;
    begin.total_size = static_cast<uint32_t>(bytes.size());
    begin.crc32 = viper::daemon::Crc32(bytes);
    begin.device_key_hash = device_hash;

    std::vector<uint8_t> payload;
    std::string error;
    if (!viper::daemon::EncodeSnapshotBegin(begin, &payload, &error)) return false;
    return server.SendCommand(
        static_cast<uint16_t>(SnapshotCommandType::SNAPSHOT_BEGIN), payload, request_id);
}

bool SendChunks(
    viper::test::FakeDaemonServer &server,
    const std::vector<uint8_t> &bytes,
    uint64_t request_id
) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const std::size_t size = std::min<std::size_t>(256, bytes.size() - offset);
        viper::daemon::SnapshotChunk chunk{};
        chunk.offset = static_cast<uint32_t>(offset);
        chunk.data.assign(
            bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            bytes.begin() + static_cast<std::ptrdiff_t>(offset + size)
        );
        std::vector<uint8_t> payload;
        std::string error;
        if (!viper::daemon::EncodeSnapshotChunk(chunk, &payload, &error)) return false;
        if (!server.SendCommand(
                static_cast<uint16_t>(SnapshotCommandType::SNAPSHOT_CHUNK),
                payload,
                request_id
            )) {
            return false;
        }
        offset += size;
    }
    return true;
}

bool SendCommit(
    viper::test::FakeDaemonServer &server,
    uint64_t app_generation,
    uint64_t daemon_generation,
    uint64_t request_id
) {
    viper::daemon::SnapshotCommit commit{};
    commit.app_generation = app_generation;
    commit.daemon_generation = daemon_generation;
    std::vector<uint8_t> payload;
    std::string error;
    if (!viper::daemon::EncodeSnapshotCommit(commit, &payload, &error)) return false;
    return server.SendCommand(
        static_cast<uint16_t>(SnapshotCommandType::SNAPSHOT_COMMIT), payload, request_id);
}

void TestSnapshotAppliesOverTheSocketAndAcks() {
    const auto name = viper::test::UniqueSocketName("ack");
    viper::test::FakeDaemonServer server(name);
    assert(server.Listen());

    // Publisher owns the bridge; a context registers itself as the applier.
    viper::audio::DriverEventPublisher publisher(name);
    ViperContext context;
    Configure(context);
    context.SetDaemonRoute(RouteHash());
    const uint64_t instance = publisher.RegisterContext(
        11,
        3,
        [&context](SnapshotCommandType type, std::span<const uint8_t> payload) {
            return context.HandleSnapshotCommand(type, payload);
        }
    );
    assert(instance != 0);
    assert(server.Accept(kTimeout));

    const std::vector<uint8_t> bytes = EncodeSnapshotBytes(4, 6, 9001);
    assert(SendBegin(server, bytes, 4, 6, RouteHash(), 77));

    DriverEvent begin_ack{};
    assert(AwaitApplyResult(server, &begin_ack));
    assert(begin_ack.type == DriverEventType::SNAPSHOT_APPLIED_ACK);
    // request_id correlates the ACK with the daemon's command.
    assert(begin_ack.context_instance_id == 77);
    assert(begin_ack.bypass_reason == static_cast<uint32_t>(ApplyError::NONE));

    // Chunks are not individually acknowledged while they succeed.
    assert(SendChunks(server, bytes, 77));
    assert(SendCommit(server, 4, 6, 77));

    DriverEvent commit_ack{};
    assert(AwaitApplyResult(server, &commit_ack));
    assert(commit_ack.type == DriverEventType::SNAPSHOT_APPLIED_ACK);
    assert(commit_ack.bypass_reason == static_cast<uint32_t>(ApplyError::NONE));
    assert(commit_ack.session_generation == 4);
    assert(commit_ack.graph_generation == 2);

    // The snapshot really reached the DSP graph, not just the staging buffer.
    PumpAudio(context);
    assert(QueryInt(context, 5 /* kParamGetConvolutionKernelId */) == 9001);
}

void TestForeignRouteBeginNacks() {
    const auto name = viper::test::UniqueSocketName("nack-route");
    viper::test::FakeDaemonServer server(name);
    assert(server.Listen());

    viper::audio::DriverEventPublisher publisher(name);
    ViperContext context;
    Configure(context);
    // Driver is on a different route than the snapshot claims.
    DeviceIdentity other = SpeakerIdentity();
    other.route_type = "bluetooth_a2dp";
    other.stable_address_or_port = "AA:BB:CC:DD:EE:FF";
    context.SetDaemonRoute(
        viper::daemon::HashDeviceKey(viper::daemon::NormalizeDeviceKey(other)));

    publisher.RegisterContext(
        12,
        4,
        [&context](SnapshotCommandType type, std::span<const uint8_t> payload) {
            return context.HandleSnapshotCommand(type, payload);
        }
    );
    assert(server.Accept(kTimeout));

    const std::vector<uint8_t> bytes = EncodeSnapshotBytes(2, 2, 4242);
    assert(SendBegin(server, bytes, 2, 2, RouteHash(), 12));

    DriverEvent nack{};
    assert(AwaitApplyResult(server, &nack));
    assert(nack.type == DriverEventType::SNAPSHOT_APPLIED_NACK);
    // Deterministic error code, so the daemon can distinguish causes.
    assert(nack.bypass_reason == static_cast<uint32_t>(ApplyError::DEVICE_MISMATCH));
}

void TestCorruptSnapshotNacksAtCommit() {
    const auto name = viper::test::UniqueSocketName("nack-crc");
    viper::test::FakeDaemonServer server(name);
    assert(server.Listen());

    viper::audio::DriverEventPublisher publisher(name);
    ViperContext context;
    Configure(context);
    context.SetDaemonRoute(RouteHash());
    publisher.RegisterContext(
        13,
        5,
        [&context](SnapshotCommandType type, std::span<const uint8_t> payload) {
            return context.HandleSnapshotCommand(type, payload);
        }
    );
    assert(server.Accept(kTimeout));

    std::vector<uint8_t> bytes = EncodeSnapshotBytes(3, 3, 555);
    // BEGIN carries the CRC of the clean bytes; the streamed bytes are corrupted.
    assert(SendBegin(server, bytes, 3, 3, RouteHash(), 21));
    DriverEvent begin_ack{};
    assert(AwaitApplyResult(server, &begin_ack));
    assert(begin_ack.type == DriverEventType::SNAPSHOT_APPLIED_ACK);

    bytes[bytes.size() - 1] ^= 0xFFU;
    assert(SendChunks(server, bytes, 21));
    assert(SendCommit(server, 3, 3, 21));

    DriverEvent nack{};
    assert(AwaitApplyResult(server, &nack));
    assert(nack.type == DriverEventType::SNAPSHOT_APPLIED_NACK);
    assert(nack.bypass_reason == static_cast<uint32_t>(ApplyError::CRC_MISMATCH));
    // The echoed generations let the daemon correlate the failure.
    assert(nack.session_generation == 3);

    // Nothing was applied, so the graph never took the snapshot's kernel.
    PumpAudio(context);
    assert(QueryInt(context, 5) == 0);
}

void TestAbortIsAcknowledgedAndDiscardsStaging() {
    const auto name = viper::test::UniqueSocketName("abort");
    viper::test::FakeDaemonServer server(name);
    assert(server.Listen());

    viper::audio::DriverEventPublisher publisher(name);
    ViperContext context;
    Configure(context);
    context.SetDaemonRoute(RouteHash());
    publisher.RegisterContext(
        14,
        6,
        [&context](SnapshotCommandType type, std::span<const uint8_t> payload) {
            return context.HandleSnapshotCommand(type, payload);
        }
    );
    assert(server.Accept(kTimeout));

    const std::vector<uint8_t> bytes = EncodeSnapshotBytes(5, 5, 777);
    assert(SendBegin(server, bytes, 5, 5, RouteHash(), 31));
    DriverEvent begin_ack{};
    assert(AwaitApplyResult(server, &begin_ack));
    assert(begin_ack.type == DriverEventType::SNAPSHOT_APPLIED_ACK);

    viper::daemon::SnapshotAbort abort{};
    abort.reason = 9;
    std::vector<uint8_t> payload;
    std::string error;
    assert(viper::daemon::EncodeSnapshotAbort(abort, &payload, &error));
    assert(server.SendCommand(
        static_cast<uint16_t>(SnapshotCommandType::SNAPSHOT_ABORT), payload, 31));

    DriverEvent abort_ack{};
    assert(AwaitApplyResult(server, &abort_ack));
    // The driver did what was asked, so an abort is acknowledged, not refused.
    assert(abort_ack.type == DriverEventType::SNAPSHOT_APPLIED_ACK);
    assert(abort_ack.bypass_reason == static_cast<uint32_t>(ApplyError::ABORTED));

    // Staging is gone: a commit without a fresh BEGIN must be refused.
    assert(SendCommit(server, 5, 5, 31));
    DriverEvent commit_nack{};
    assert(AwaitApplyResult(server, &commit_nack));
    assert(commit_nack.type == DriverEventType::SNAPSHOT_APPLIED_NACK);
    assert(commit_nack.bypass_reason == static_cast<uint32_t>(ApplyError::NOT_STAGING));
}

void TestObserveOnlyContextNacksInsteadOfSilence() {
    const auto name = viper::test::UniqueSocketName("observe-only");
    viper::test::FakeDaemonServer server(name);
    assert(server.Listen());

    // Registered without an applier: an older driver build, or a context that
    // cannot apply snapshots. The daemon must still get an answer.
    viper::audio::DriverEventPublisher publisher(name);
    publisher.RegisterContext(15, 7);
    assert(server.Accept(kTimeout));

    const std::vector<uint8_t> bytes = EncodeSnapshotBytes(1, 1, 1);
    assert(SendBegin(server, bytes, 1, 1, RouteHash(), 41));

    DriverEvent nack{};
    assert(AwaitApplyResult(server, &nack));
    assert(nack.type == DriverEventType::SNAPSHOT_APPLIED_NACK);
    assert(nack.bypass_reason == static_cast<uint32_t>(ApplyError::NOT_STAGING));
}

void TestMalformedCommandPayloadNacks() {
    const auto name = viper::test::UniqueSocketName("malformed");
    viper::test::FakeDaemonServer server(name);
    assert(server.Listen());

    viper::audio::DriverEventPublisher publisher(name);
    ViperContext context;
    Configure(context);
    context.SetDaemonRoute(RouteHash());
    publisher.RegisterContext(
        16,
        8,
        [&context](SnapshotCommandType type, std::span<const uint8_t> payload) {
            return context.HandleSnapshotCommand(type, payload);
        }
    );
    assert(server.Accept(kTimeout));

    // Garbage where a SNAPSHOT_BEGIN payload should be.
    const std::vector<uint8_t> garbage(16, 0xA5U);
    assert(server.SendCommand(
        static_cast<uint16_t>(SnapshotCommandType::SNAPSHOT_BEGIN), garbage, 51));

    DriverEvent nack{};
    assert(AwaitApplyResult(server, &nack));
    assert(nack.type == DriverEventType::SNAPSHOT_APPLIED_NACK);
    assert(nack.bypass_reason == static_cast<uint32_t>(ApplyError::BAD_METADATA));

    // The bridge is still usable: a valid transfer afterwards succeeds.
    const std::vector<uint8_t> bytes = EncodeSnapshotBytes(2, 2, 314);
    assert(SendBegin(server, bytes, 2, 2, RouteHash(), 52));
    DriverEvent begin_ack{};
    assert(AwaitApplyResult(server, &begin_ack));
    assert(begin_ack.type == DriverEventType::SNAPSHOT_APPLIED_ACK);
    assert(SendChunks(server, bytes, 52));
    assert(SendCommit(server, 2, 2, 52));
    DriverEvent commit_ack{};
    assert(AwaitApplyResult(server, &commit_ack));
    assert(commit_ack.type == DriverEventType::SNAPSHOT_APPLIED_ACK);
    PumpAudio(context);
    assert(QueryInt(context, 5) == 314);
}

} // namespace

int main() {
    TestSnapshotAppliesOverTheSocketAndAcks();
    TestForeignRouteBeginNacks();
    TestCorruptSnapshotNacksAtCommit();
    TestAbortIsAcknowledgedAndDiscardsStaging();
    TestObserveOnlyContextNacksInsteadOfSilence();
    TestMalformedCommandPayloadNacks();
    std::puts("snapshot ACK end-to-end tests passed");
    return 0;
}
