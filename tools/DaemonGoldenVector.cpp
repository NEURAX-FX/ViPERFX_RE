// Prints hex golden vectors for the daemon wire formats.
//
// The App has an independent Kotlin implementation of these formats, so both
// sides assert against the same fixtures. Run with no arguments to print them.
#include "AppCommand.h"
#include "ParameterStream.h"
#include "SnapshotCommand.h"
#include "SnapshotSchema.h"
#include "ViPERParams.h"
#include "ViperDaemonProtocol.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

void PrintHex(const char *label, const std::vector<uint8_t> &bytes) {
    std::printf("%s ", label);
    for (const uint8_t byte : bytes) std::printf("%02x", byte);
    std::printf("\n");
}

// Fixed inputs; any change here invalidates the fixtures on both sides.
constexpr uint64_t kBootId = 0x1122334455667788ULL;
constexpr uint64_t kDaemonGeneration = 7;
constexpr uint64_t kAppGeneration = 5;
constexpr uint64_t kCreatedAtMillis = 1700000000000ULL;
const std::string kDeviceKey = "speaker|builtin|internal|48000|3|pcm_16|0";

std::vector<viper::daemon::RawParamRecord> GoldenRecords() {
    using namespace viper::params;

    viper::daemon::RawParamRecord enable{};
    enable.param = kParamEqualizerEnable;
    enable.val1 = 1;

    viper::daemon::RawParamRecord triple{};
    triple.param = kParamMultibandCompressorBandThreshold;
    triple.val1 = 2;
    triple.val2 = -18;
    triple.val3 = 3;

    // Array parameter: 4 float band levels with deterministic values.
    viper::daemon::RawParamRecord bands{};
    bands.param = kParamEqualizerBandLevels;
    bands.arr_size = 4;
    bands.payload.assign(4U * sizeof(float), 0U);
    for (std::size_t index = 0; index < 4; ++index) {
        const float value = 0.25F * static_cast<float>(index + 1U);
        std::memcpy(bands.payload.data() + index * sizeof(float), &value, sizeof(float));
    }

    return {enable, triple, bands};
}

} // namespace

int main() {
    std::string error;

    // 1. Frame with a fixed payload.
    const std::vector<uint8_t> payload{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    viper::daemon::FrameHeader header{};
    header.message_type = 0x1234;
    header.request_id = 0x0102030405060708ULL;
    header.sequence = 42;
    std::vector<uint8_t> frame;
    const std::string_view payload_view(
        reinterpret_cast<const char *>(payload.data()), payload.size());
    if (!viper::daemon::EncodeFrame(header, payload_view, &frame, &error)) {
        std::fprintf(stderr, "frame: %s\n", error.c_str());
        return 1;
    }
    PrintHex("frame", frame);

    // 2. Parameter stream.
    const auto records = GoldenRecords();
    std::vector<uint8_t> parameters;
    if (!viper::daemon::EncodeParameterStream(records, &parameters, &error)) {
        std::fprintf(stderr, "parameters: %s\n", error.c_str());
        return 1;
    }
    PrintHex("parameters", parameters);

    // 3. Snapshot carrying that parameter stream.
    viper::daemon::Snapshot snapshot{};
    snapshot.device_key = kDeviceKey;
    snapshot.device_key_hash = viper::daemon::HashDeviceKey(kDeviceKey);
    snapshot.boot_id = kBootId;
    snapshot.daemon_generation = kDaemonGeneration;
    snapshot.app_generation = kAppGeneration;
    snapshot.created_at_millis = kCreatedAtMillis;
    snapshot.master_enabled = true;
    snapshot.global_mode = false;
    snapshot.parameters = parameters;
    snapshot.resource_generation = 11;
    snapshot.graph_generation = 12;

    viper::daemon::ResourceReference resource{};
    resource.resource_id = "kernel-a";
    resource.content_sha256 = std::string(64, 'b');
    resource.size = 4096;
    resource.kind = 1;
    resource.format = 2;
    resource.channels = 2;
    resource.order = 0;
    snapshot.resources.push_back(resource);

    std::vector<uint8_t> encoded_snapshot;
    if (!viper::daemon::EncodeSnapshot(snapshot, &encoded_snapshot, &error)) {
        std::fprintf(stderr, "snapshot: %s\n", error.c_str());
        return 1;
    }
    PrintHex("snapshot", encoded_snapshot);
    std::printf("device_key_hash %s\n", snapshot.device_key_hash.c_str());
    std::printf("snapshot_crc32 %08x\n", viper::daemon::Crc32(encoded_snapshot));

    // 4. Snapshot commands.
    viper::daemon::SnapshotBegin begin{};
    begin.app_generation = kAppGeneration;
    begin.daemon_generation = kDaemonGeneration;
    begin.total_size = static_cast<uint32_t>(encoded_snapshot.size());
    begin.crc32 = viper::daemon::Crc32(encoded_snapshot);
    begin.device_key_hash = snapshot.device_key_hash;
    std::vector<uint8_t> begin_bytes;
    if (!viper::daemon::EncodeSnapshotBegin(begin, &begin_bytes, &error)) {
        std::fprintf(stderr, "begin: %s\n", error.c_str());
        return 1;
    }
    PrintHex("begin", begin_bytes);

    viper::daemon::SnapshotChunk chunk{};
    chunk.offset = 256;
    chunk.data.assign(8, 0xA5U);
    std::vector<uint8_t> chunk_bytes;
    if (!viper::daemon::EncodeSnapshotChunk(chunk, &chunk_bytes, &error)) {
        std::fprintf(stderr, "chunk: %s\n", error.c_str());
        return 1;
    }
    PrintHex("chunk", chunk_bytes);

    viper::daemon::SnapshotCommit commit{};
    commit.app_generation = kAppGeneration;
    commit.daemon_generation = kDaemonGeneration;
    std::vector<uint8_t> commit_bytes;
    if (!viper::daemon::EncodeSnapshotCommit(commit, &commit_bytes, &error)) {
        std::fprintf(stderr, "commit: %s\n", error.c_str());
        return 1;
    }
    PrintHex("commit", commit_bytes);

    viper::daemon::SnapshotAbort abort{};
    abort.reason = 12;
    std::vector<uint8_t> abort_bytes;
    if (!viper::daemon::EncodeSnapshotAbort(abort, &abort_bytes, &error)) {
        std::fprintf(stderr, "abort: %s\n", error.c_str());
        return 1;
    }
    PrintHex("abort", abort_bytes);

    // 5. App control messages on @viper4android.app.v1.
    viper::daemon::AppHello app_hello{};
    app_hello.app_generation = kAppGeneration;
    std::vector<uint8_t> app_hello_bytes;
    if (!viper::daemon::EncodeAppHello(app_hello, &app_hello_bytes, &error)) {
        std::fprintf(stderr, "app_hello: %s\n", error.c_str());
        return 1;
    }
    PrintHex("app_hello", app_hello_bytes);

    viper::daemon::AppHelloAck app_hello_ack{};
    app_hello_ack.flags = viper::daemon::kAppFlagRestoreEnabled
        | viper::daemon::kAppFlagDriverConnected | viper::daemon::kAppFlagRouteKnown;
    app_hello_ack.daemon_generation = kDaemonGeneration;
    app_hello_ack.route_epoch = 9;
    app_hello_ack.route_key_hash = viper::daemon::HashDeviceKey(kDeviceKey);
    std::vector<uint8_t> app_hello_ack_bytes;
    if (!viper::daemon::EncodeAppHelloAck(app_hello_ack, &app_hello_ack_bytes, &error)) {
        std::fprintf(stderr, "app_hello_ack: %s\n", error.c_str());
        return 1;
    }
    PrintHex("app_hello_ack", app_hello_ack_bytes);

    viper::daemon::AppRouteReport app_route_report{};
    app_route_report.route_type = "bluetooth_a2dp";
    app_route_report.stable_address_or_port = "ac:12:2f:00:11:22";
    app_route_report.product_name = "WH-1000XM4";
    app_route_report.encoding = "pcm_16";
    app_route_report.sample_rate = 48000;
    app_route_report.channel_mask = 3;
    app_route_report.output_flags = 6;
    std::vector<uint8_t> app_route_report_bytes;
    if (!viper::daemon::EncodeAppRouteReport(app_route_report, &app_route_report_bytes, &error)) {
        std::fprintf(stderr, "app_route_report: %s\n", error.c_str());
        return 1;
    }
    PrintHex("app_route_report", app_route_report_bytes);

    viper::daemon::AppRouteAck app_route_ack{};
    app_route_ack.accepted = true;
    app_route_ack.daemon_generation = kDaemonGeneration;
    app_route_ack.route_epoch = 10;
    app_route_ack.route_key_hash = viper::daemon::HashDeviceKey(kDeviceKey);
    std::vector<uint8_t> app_route_ack_bytes;
    if (!viper::daemon::EncodeAppRouteAck(app_route_ack, &app_route_ack_bytes, &error)) {
        std::fprintf(stderr, "app_route_ack: %s\n", error.c_str());
        return 1;
    }
    PrintHex("app_route_ack", app_route_ack_bytes);

    viper::daemon::AppApplyResult app_apply_result{};
    app_apply_result.accepted = false;
    app_apply_result.error_code = 10; // STALE_GENERATION
    app_apply_result.app_generation = kAppGeneration;
    app_apply_result.daemon_generation = kDaemonGeneration;
    app_apply_result.resource_generation = 11;
    app_apply_result.graph_generation = 12;
    std::vector<uint8_t> app_apply_result_bytes;
    if (!viper::daemon::EncodeAppApplyResult(app_apply_result, &app_apply_result_bytes, &error)) {
        std::fprintf(stderr, "app_apply_result: %s\n", error.c_str());
        return 1;
    }
    PrintHex("app_apply_result", app_apply_result_bytes);

    return 0;
}
