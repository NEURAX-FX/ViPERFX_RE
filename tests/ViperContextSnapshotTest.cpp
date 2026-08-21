// Drives the real ViperContext through the daemon snapshot apply path, so the
// committer that touches DspGraphSlots/DspResources/ParameterMailbox is exercised
// rather than a test double.
#include "ViperContext.h"

#include "AudioFormat.h"
#include "DeviceKey.h"
#include "ParameterStream.h"
#include "SnapshotSchema.h"
#include "ViPERParams.h"
#include "ViperDaemonProtocol.h"
#include "viper/constants.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using viper::audio::ApplyError;
using viper::audio::ApplyOutcome;
using viper::audio::ApplyResult;
using viper::audio::SnapshotMetadata;
using viper::daemon::DeviceIdentity;
using viper::daemon::RawParamRecord;
using viper::daemon::Snapshot;

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

effect_config_t StereoConfig() {
    effect_config_t config{};
    config.input_cfg.buffer.frame_count = viper::audio::kMaxBlockFrames;
    config.input_cfg.sampling_rate = kSampleRate;
    config.input_cfg.channels = AUDIO_CHANNEL_OUT_STEREO;
    config.input_cfg.format = AUDIO_FORMAT_PCM_16_BIT;
    config.input_cfg.access_mode = EFFECT_BUFFER_ACCESS_WRITE;
    config.input_cfg.mask = EFFECT_CONFIG_ALL & ~EFFECT_CONFIG_PROVIDER;
    config.output_cfg = config.input_cfg;
    return config;
}

// Runs EFFECT_CMD_SET_CONFIG so the context has graph geometry, exactly like
// AudioFlinger does before the first process call.
void Configure(ViperContext &context) {
    effect_config_t config = StereoConfig();
    int32_t reply = -1;
    uint32_t reply_size = sizeof(reply);
    const int32_t status = context.HandleCommand(
        EFFECT_CMD_SET_CONFIG, sizeof(config), &config, &reply_size, &reply);
    assert(status == 0);
    assert(reply == 0);
}

// EFFECT_CMD_ENABLE, as AudioFlinger issues before the first process() call.
void Enable(ViperContext &context) {
    int32_t reply = -1;
    uint32_t reply_size = sizeof(reply);
    const int32_t status =
        context.HandleCommand(EFFECT_CMD_ENABLE, 0, nullptr, &reply_size, &reply);
    assert(status == 0);
    assert(reply == 0);
}

// Pushes one block of silence through the effect. DspGraphSlots publishes a
// prepared graph only when Process() consumes the pending slot, so a snapshot's
// graph does not become Active() until audio flows.
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

bool ContextIsConfigured(ViperContext &context) {
    struct {
        effect_param_t header;
        int32_t param;
        int32_t value;
    } request{}, response{};
    request.header.psize = sizeof(int32_t);
    request.header.vsize = sizeof(int32_t);
    request.param = 2; // kParamGetConfigure
    uint32_t reply_size = sizeof(response);
    const int32_t status = context.HandleCommand(
        EFFECT_CMD_GET_PARAM, sizeof(request), &request, &reply_size, &response);
    assert(status == 0);
    return response.value != 0;
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

std::vector<uint8_t> FloatPayload(const std::vector<float> &values) {
    std::vector<uint8_t> payload(values.size() * sizeof(float));
    std::memcpy(payload.data(), values.data(), payload.size());
    return payload;
}

RawParamRecord Scalar(int32_t param, int32_t val1, int32_t val2 = 0, int32_t val3 = 0) {
    RawParamRecord record{};
    record.param = param;
    record.val1 = val1;
    record.val2 = val2;
    record.val3 = val3;
    return record;
}

// Convolver kernel records: prepare + fill + commit, mirroring what the App sends.
std::vector<RawParamRecord> ConvolverRecords(int32_t kernel_id, bool valid_crc) {
    using namespace viper::params;
    const std::size_t samples = 64;
    std::vector<float> kernel(samples, 0.0F);
    kernel[0] = 1.0F;

    RawParamRecord fill{};
    fill.param = kParamConvolverSetBuffer;
    fill.arr_size = static_cast<uint32_t>(samples);
    fill.payload = FloatPayload(kernel);

    const uint32_t crc = viper::daemon::Crc32(fill.payload);

    return {
        Scalar(kParamConvolverPrepareBuffer, static_cast<int32_t>(samples), 2, 0),
        fill,
        Scalar(
            kParamConvolverCommitBuffer,
            static_cast<int32_t>(samples),
            static_cast<int32_t>(valid_crc ? crc : crc ^ 0xFFFFFFFFU),
            kernel_id
        ),
    };
}

std::vector<uint8_t> EncodeParams(const std::vector<RawParamRecord> &records) {
    std::vector<uint8_t> bytes;
    std::string error;
    const bool ok = viper::daemon::EncodeParameterStream(records, &bytes, &error);
    assert(ok);
    return bytes;
}

Snapshot MakeSnapshot(
    uint64_t app_generation,
    uint64_t daemon_generation,
    const std::vector<RawParamRecord> &records,
    bool master_enabled = true
) {
    Snapshot snapshot{};
    snapshot.device_key = viper::daemon::NormalizeDeviceKey(SpeakerIdentity());
    snapshot.device_key_hash = viper::daemon::HashDeviceKey(snapshot.device_key);
    snapshot.boot_id = 0xB007ULL;
    snapshot.daemon_generation = daemon_generation;
    snapshot.app_generation = app_generation;
    snapshot.created_at_millis = 1700000000000ULL;
    snapshot.master_enabled = master_enabled;
    snapshot.parameters = EncodeParams(records);
    return snapshot;
}

std::string LocalRouteHash() {
    return viper::daemon::HashDeviceKey(
        viper::daemon::NormalizeDeviceKey(SpeakerIdentity()));
}

// Streams a snapshot into the context the way the daemon would.
bool Apply(
    ViperContext &context,
    const Snapshot &snapshot,
    ApplyResult *result,
    ApplyError *error,
    std::string *message
) {
    std::vector<uint8_t> bytes;
    std::string encode_error;
    if (!viper::daemon::EncodeSnapshot(snapshot, &bytes, &encode_error)) return false;

    SnapshotMetadata metadata{};
    metadata.app_generation = snapshot.app_generation;
    metadata.daemon_generation = snapshot.daemon_generation;
    metadata.device_key_hash = snapshot.device_key_hash;
    metadata.total_size = static_cast<uint32_t>(bytes.size());
    metadata.crc32 = viper::daemon::Crc32(bytes);

    if (!context.BeginSnapshot(metadata, error)) return false;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const std::size_t size = std::min<std::size_t>(128, bytes.size() - offset);
        if (!context.AppendSnapshot(
                static_cast<uint32_t>(offset),
                std::span<const uint8_t>(bytes.data() + offset, size),
                error
            )) {
            return false;
        }
        offset += size;
    }
    return context.CommitSnapshot(result, error, message);
}

void TestSnapshotAppliesThroughRealGraphSlots() {
    using namespace viper::params;
    ViperContext context;
    Configure(context);
    assert(ContextIsConfigured(context));
    context.SetDaemonRoute(LocalRouteHash());

    std::vector<RawParamRecord> records{
        Scalar(kParamConvolverEnable, 1),
        Scalar(kParamBassEnable, 1),
        Scalar(kParamBassFrequency, 80),
    };
    const auto kernel = ConvolverRecords(4242, true);
    records.insert(records.end(), kernel.begin(), kernel.end());

    ApplyResult result{};
    ApplyError error = ApplyError::NONE;
    std::string message;
    assert(Apply(context, MakeSnapshot(1, 1, records), &result, &error, &message));
    assert(result.outcome == ApplyOutcome::APPLIED);
    // The graph generation must have advanced: a new graph was prepared.
    assert(result.graph_generation == 2);
    assert(error == ApplyError::NONE);

    // The prepared graph becomes Active() only once Process() consumes the
    // pending slot, so pump a block before reading live graph state.
    Enable(context);
    PumpAudio(context);

    // The convolver kernel from the snapshot reached the live graph, which only
    // happens if DspResources replayed and the pending graph was swapped in.
    assert(QueryInt(context, 5 /* kParamGetConvolutionKernelId */) == 4242);
    // master_enabled=true must leave the effect enabled.
    assert(QueryInt(context, 1 /* kParamGetEnabled */) == 1);
    assert(QueryInt(context, 4 /* kParamGetSamplingRate */) == static_cast<int32_t>(kSampleRate));
}

void TestMasterDisabledSnapshotDisablesProcessing() {
    using namespace viper::params;
    ViperContext context;
    Configure(context);
    context.SetDaemonRoute(LocalRouteHash());

    ApplyResult result{};
    ApplyError error = ApplyError::NONE;
    assert(Apply(
        context,
        MakeSnapshot(1, 1, {Scalar(kParamBassEnable, 1)}, /*master_enabled=*/false),
        &result,
        &error,
        nullptr
    ));
    assert(result.outcome == ApplyOutcome::APPLIED);
    assert(QueryInt(context, 1 /* kParamGetEnabled */) == 0);
}

void TestRejectedResourcePreservesLiveGraph() {
    using namespace viper::params;
    ViperContext context;
    Configure(context);
    context.SetDaemonRoute(LocalRouteHash());

    // First apply installs a good kernel.
    std::vector<RawParamRecord> good{Scalar(kParamConvolverEnable, 1)};
    const auto good_kernel = ConvolverRecords(111, true);
    good.insert(good.end(), good_kernel.begin(), good_kernel.end());

    ApplyResult result{};
    ApplyError error = ApplyError::NONE;
    assert(Apply(context, MakeSnapshot(1, 1, good), &result, &error, nullptr));
    Enable(context);
    PumpAudio(context);
    assert(QueryInt(context, 5) == 111);
    const int32_t rate_before = QueryInt(context, 4);

    // Second snapshot carries a CRC-broken kernel: DspResources rejects it, so the
    // commit must fail and the previously applied kernel must still be live.
    std::vector<RawParamRecord> bad{Scalar(kParamConvolverEnable, 1)};
    const auto bad_kernel = ConvolverRecords(222, false);
    bad.insert(bad.end(), bad_kernel.begin(), bad_kernel.end());

    std::string message;
    assert(!Apply(context, MakeSnapshot(2, 2, bad), &result, &error, &message));
    assert(error == ApplyError::GRAPH_PREPARE_FAILED);
    assert(!message.empty());
    // Live graph untouched: same kernel, same rate, still configured. Pumping
    // again proves no half-prepared graph was left waiting in the pending slot.
    PumpAudio(context);
    assert(QueryInt(context, 5) == 111);
    assert(QueryInt(context, 4) == rate_before);
    assert(ContextIsConfigured(context));

    // A later valid snapshot still applies: the failure was not sticky.
    std::vector<RawParamRecord> recovery{Scalar(kParamConvolverEnable, 1)};
    const auto recovery_kernel = ConvolverRecords(333, true);
    recovery.insert(recovery.end(), recovery_kernel.begin(), recovery_kernel.end());
    assert(Apply(context, MakeSnapshot(2, 2, recovery), &result, &error, nullptr));
    assert(result.outcome == ApplyOutcome::APPLIED);
    PumpAudio(context);
    assert(QueryInt(context, 5) == 333);
}

void TestInvalidParameterRejectsWholeSnapshot() {
    using namespace viper::params;
    ViperContext context;
    Configure(context);
    context.SetDaemonRoute(LocalRouteHash());

    ApplyResult result{};
    ApplyError error = ApplyError::NONE;
    assert(Apply(
        context, MakeSnapshot(1, 1, {Scalar(kParamBassFrequency, 80)}), &result, &error, nullptr));
    const uint64_t applied_graph = result.graph_generation;

    // Band count beyond the array bounds: UpdateParameterSnapshot returns INVALID.
    std::vector<RawParamRecord> bad{
        Scalar(kParamBassFrequency, 120),
        Scalar(kParamEqualizerBandCount, 9999),
    };
    std::string message;
    assert(!Apply(context, MakeSnapshot(2, 2, bad), &result, &error, &message));
    assert(error == ApplyError::GRAPH_PREPARE_FAILED);
    assert(message.find("parameter rejected") != std::string::npos);

    // The earlier snapshot's state is intact; no partial parameter application.
    ApplyResult after{};
    assert(Apply(
        context, MakeSnapshot(2, 2, {Scalar(kParamBassFrequency, 120)}), &after, &error, nullptr));
    assert(after.graph_generation > applied_graph);
}

void TestUnconfiguredContextRejectsSnapshot() {
    using namespace viper::params;
    ViperContext context;
    // No EFFECT_CMD_SET_CONFIG: there is no graph geometry to prepare against.
    context.SetDaemonRoute(LocalRouteHash());

    ApplyResult result{};
    ApplyError error = ApplyError::NONE;
    std::string message;
    assert(!Apply(
        context, MakeSnapshot(1, 1, {Scalar(kParamBassEnable, 1)}), &result, &error, &message));
    assert(error == ApplyError::GRAPH_PREPARE_FAILED);
    assert(!message.empty());
}

void TestForeignRouteSnapshotNeverReachesTheGraph() {
    using namespace viper::params;
    ViperContext context;
    Configure(context);
    // The driver believes it is on a different route.
    DeviceIdentity other = SpeakerIdentity();
    other.route_type = "bluetooth_a2dp";
    other.stable_address_or_port = "AA:BB:CC:DD:EE:FF";
    context.SetDaemonRoute(
        viper::daemon::HashDeviceKey(viper::daemon::NormalizeDeviceKey(other)));

    ApplyResult result{};
    ApplyError error = ApplyError::NONE;
    assert(!Apply(
        context, MakeSnapshot(1, 1, {Scalar(kParamBassEnable, 1)}), &result, &error, nullptr));
    assert(error == ApplyError::DEVICE_MISMATCH);
}

void TestIdempotentReapplyDoesNotRebuildGraph() {
    using namespace viper::params;
    ViperContext context;
    Configure(context);
    context.SetDaemonRoute(LocalRouteHash());

    const Snapshot snapshot = MakeSnapshot(7, 7, {Scalar(kParamBassEnable, 1)});
    ApplyResult first{};
    ApplyError error = ApplyError::NONE;
    assert(Apply(context, snapshot, &first, &error, nullptr));
    assert(first.outcome == ApplyOutcome::APPLIED);

    ApplyResult second{};
    assert(Apply(context, snapshot, &second, &error, nullptr));
    assert(second.outcome == ApplyOutcome::IDEMPOTENT);
    // No new graph was prepared, so the generation is unchanged.
    assert(second.graph_generation == first.graph_generation);
}

void TestReconfigureAfterSnapshotKeepsSnapshotState() {
    using namespace viper::params;
    ViperContext context;
    Configure(context);
    context.SetDaemonRoute(LocalRouteHash());

    std::vector<RawParamRecord> records{Scalar(kParamConvolverEnable, 1)};
    const auto kernel = ConvolverRecords(555, true);
    records.insert(records.end(), kernel.begin(), kernel.end());

    ApplyResult result{};
    ApplyError error = ApplyError::NONE;
    assert(Apply(context, MakeSnapshot(3, 3, records), &result, &error, nullptr));
    Enable(context);
    PumpAudio(context);
    assert(QueryInt(context, 5) == 555);

    // A route/format change re-runs SET_CONFIG; the snapshot's resources must be
    // replayed into the rebuilt graph rather than lost.
    Configure(context);
    assert(ContextIsConfigured(context));
    PumpAudio(context);
    assert(QueryInt(context, 5) == 555);
}

} // namespace

int main() {
    TestSnapshotAppliesThroughRealGraphSlots();
    TestMasterDisabledSnapshotDisablesProcessing();
    TestRejectedResourcePreservesLiveGraph();
    TestInvalidParameterRejectsWholeSnapshot();
    TestUnconfiguredContextRejectsSnapshot();
    TestForeignRouteSnapshotNeverReachesTheGraph();
    TestIdempotentReapplyDoesNotRebuildGraph();
    TestReconfigureAfterSnapshotKeepsSnapshotState();
    std::puts("viper context snapshot tests passed");
    return 0;
}
