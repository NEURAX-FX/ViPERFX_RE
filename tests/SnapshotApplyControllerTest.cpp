#include "SnapshotApplyController.h"

#include "DeviceKey.h"
#include "ParameterStream.h"
#include "SnapshotSchema.h"
#include "ViPERParams.h"
#include "ViperDaemonProtocol.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using viper::audio::ApplyError;
using viper::audio::ApplyOutcome;
using viper::audio::ApplyResult;
using viper::audio::SnapshotApplyController;
using viper::audio::SnapshotMetadata;
using viper::daemon::DeviceIdentity;
using viper::daemon::RawParamRecord;
using viper::daemon::Snapshot;

DeviceIdentity SpeakerIdentity() {
    DeviceIdentity identity{};
    identity.route_type = "speaker";
    identity.stable_address_or_port = "builtin";
    identity.product_name = "internal";
    identity.sample_rate = 48000;
    identity.channel_mask = 3;
    identity.encoding = "pcm_16";
    return identity;
}

DeviceIdentity BluetoothIdentity() {
    DeviceIdentity identity{};
    identity.route_type = "bluetooth_a2dp";
    identity.stable_address_or_port = "AA:BB:CC:DD:EE:FF";
    identity.product_name = "Buds";
    identity.sample_rate = 44100;
    identity.channel_mask = 3;
    identity.encoding = "pcm_16";
    return identity;
}

std::vector<uint8_t> FloatPayload(std::size_t count) {
    std::vector<uint8_t> payload(count * sizeof(float), 0U);
    for (std::size_t index = 0; index < count; ++index) {
        const float value = 0.25F * static_cast<float>(index + 1U);
        std::memcpy(payload.data() + index * sizeof(float), &value, sizeof(float));
    }
    return payload;
}

std::vector<uint8_t> ParameterBlob() {
    using namespace viper::params;
    RawParamRecord enable{};
    enable.param = kParamEqualizerEnable;
    enable.val1 = 1;

    RawParamRecord bands{};
    bands.param = kParamEqualizerBandLevels;
    bands.arr_size = 10;
    bands.payload = FloatPayload(10);

    std::vector<uint8_t> encoded;
    std::string error;
    const bool ok = viper::daemon::EncodeParameterStream({enable, bands}, &encoded, &error);
    assert(ok);
    return encoded;
}

Snapshot MakeSnapshot(
    const DeviceIdentity &identity,
    uint64_t app_generation,
    uint64_t daemon_generation
) {
    Snapshot snapshot{};
    snapshot.device_key = viper::daemon::NormalizeDeviceKey(identity);
    snapshot.device_key_hash = viper::daemon::HashDeviceKey(snapshot.device_key);
    snapshot.boot_id = 0xB007ULL;
    snapshot.daemon_generation = daemon_generation;
    snapshot.app_generation = app_generation;
    snapshot.created_at_millis = 1700000000000ULL;
    snapshot.master_enabled = true;
    snapshot.parameters = ParameterBlob();
    snapshot.resource_generation = 3;
    snapshot.graph_generation = 4;
    return snapshot;
}

std::vector<uint8_t> Encode(const Snapshot &snapshot) {
    std::vector<uint8_t> bytes;
    std::string error;
    const bool ok = viper::daemon::EncodeSnapshot(snapshot, &bytes, &error);
    assert(ok);
    return bytes;
}

SnapshotMetadata MakeMetadata(const Snapshot &snapshot, const std::vector<uint8_t> &bytes) {
    SnapshotMetadata metadata{};
    metadata.app_generation = snapshot.app_generation;
    metadata.daemon_generation = snapshot.daemon_generation;
    metadata.device_key_hash = snapshot.device_key_hash;
    metadata.total_size = static_cast<uint32_t>(bytes.size());
    metadata.crc32 = viper::daemon::Crc32(bytes);
    return metadata;
}

// Records what the driver side was asked to do.
struct CommitLog {
    unsigned calls = 0;
    bool succeed = true;
    uint64_t resource_generation = 11;
    uint64_t graph_generation = 12;
    std::string failure = "prepare failed";
    std::size_t parameter_records = 0;
    std::size_t iem_records = 0;
    uint64_t app_generation = 0;
    bool master_enabled = false;
};

SnapshotApplyController::Committer MakeCommitter(CommitLog *log) {
    return [log](
               const SnapshotApplyController::CommitRequest &request,
               uint64_t *resource_generation,
               uint64_t *graph_generation,
               std::string *error
           ) {
        ++log->calls;
        log->parameter_records = request.parameters.size();
        log->iem_records = request.iem_parameters.size();
        log->app_generation = request.snapshot.app_generation;
        log->master_enabled = request.snapshot.master_enabled;
        if (!log->succeed) {
            if (error != nullptr) *error = log->failure;
            return false;
        }
        *resource_generation = log->resource_generation;
        *graph_generation = log->graph_generation;
        return true;
    };
}

// Streams a snapshot in fixed-size chunks, like the daemon does.
bool StreamSnapshot(
    SnapshotApplyController &controller,
    const std::vector<uint8_t> &bytes,
    std::size_t chunk_size,
    ApplyError *error
) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const std::size_t size = std::min(chunk_size, bytes.size() - offset);
        const std::span<const uint8_t> chunk(bytes.data() + offset, size);
        if (!controller.Append(static_cast<uint32_t>(offset), chunk, error)) return false;
        offset += size;
    }
    return true;
}

void TestCompleteSnapshotApplies() {
    CommitLog log{};
    SnapshotApplyController controller(MakeCommitter(&log));
    const Snapshot snapshot = MakeSnapshot(SpeakerIdentity(), 5, 7);
    const std::vector<uint8_t> bytes = Encode(snapshot);
    controller.SetDeviceKeyHash(snapshot.device_key_hash);

    ApplyError error = ApplyError::NONE;
    assert(controller.Begin(MakeMetadata(snapshot, bytes), &error));
    assert(controller.Staging());
    assert(StreamSnapshot(controller, bytes, 64, &error));
    assert(controller.StagedBytes() == bytes.size());

    ApplyResult result{};
    std::string message;
    assert(controller.Commit(&result, &error, &message));
    assert(result.outcome == ApplyOutcome::APPLIED);
    assert(result.app_generation == 5);
    assert(result.daemon_generation == 7);
    assert(result.resource_generation == 11);
    assert(result.graph_generation == 12);
    assert(log.calls == 1);
    // The parameter blob must reach the driver decoded, not as raw bytes.
    assert(log.parameter_records == 2);
    assert(log.iem_records == 0);
    assert(log.master_enabled);
    // Staging buffers are released after commit.
    assert(!controller.Staging());
    assert(controller.StagedBytes() == 0);
    assert(controller.LastAppliedGeneration() == 5);
    assert(controller.LastAppliedDaemonGeneration() == 7);
}

void TestOutOfOrderChunkAborts() {
    CommitLog log{};
    SnapshotApplyController controller(MakeCommitter(&log));
    const Snapshot snapshot = MakeSnapshot(SpeakerIdentity(), 1, 1);
    const std::vector<uint8_t> bytes = Encode(snapshot);
    controller.SetDeviceKeyHash(snapshot.device_key_hash);

    ApplyError error = ApplyError::NONE;
    assert(controller.Begin(MakeMetadata(snapshot, bytes), &error));
    const std::span<const uint8_t> head(bytes.data(), 32);
    assert(controller.Append(0, head, &error));

    // Gap: offset 64 while only 32 bytes are staged.
    const std::span<const uint8_t> gapped(bytes.data() + 64, 16);
    assert(!controller.Append(64, gapped, &error));
    assert(error == ApplyError::CHUNK_OUT_OF_ORDER);
    // Staging is torn down, so a resumed stream cannot smuggle bytes in.
    assert(!controller.Staging());
    assert(log.calls == 0);

    assert(!controller.Commit(nullptr, &error, nullptr));
    assert(error == ApplyError::NOT_STAGING);
}

void TestRewindChunkIsRejected() {
    CommitLog log{};
    SnapshotApplyController controller(MakeCommitter(&log));
    const Snapshot snapshot = MakeSnapshot(SpeakerIdentity(), 1, 1);
    const std::vector<uint8_t> bytes = Encode(snapshot);
    controller.SetDeviceKeyHash(snapshot.device_key_hash);

    ApplyError error = ApplyError::NONE;
    assert(controller.Begin(MakeMetadata(snapshot, bytes), &error));
    assert(controller.Append(0, std::span<const uint8_t>(bytes.data(), 48), &error));
    // Rewriting an already-staged prefix must not be accepted.
    assert(!controller.Append(16, std::span<const uint8_t>(bytes.data() + 16, 8), &error));
    assert(error == ApplyError::CHUNK_OUT_OF_ORDER);
}

void TestChunkBeyondDeclaredSizeIsRejected() {
    CommitLog log{};
    SnapshotApplyController controller(MakeCommitter(&log));
    const Snapshot snapshot = MakeSnapshot(SpeakerIdentity(), 1, 1);
    const std::vector<uint8_t> bytes = Encode(snapshot);
    controller.SetDeviceKeyHash(snapshot.device_key_hash);

    SnapshotMetadata metadata = MakeMetadata(snapshot, bytes);
    // Declare a smaller snapshot than we then try to push.
    metadata.total_size = 32;
    ApplyError error = ApplyError::NONE;
    assert(controller.Begin(metadata, &error));
    assert(!controller.Append(0, std::span<const uint8_t>(bytes.data(), 64), &error));
    assert(error == ApplyError::CHUNK_RANGE);
    assert(!controller.Staging());

    // Empty chunks carry no information and would mask a stalled stream.
    assert(controller.Begin(metadata, &error));
    assert(!controller.Append(0, std::span<const uint8_t>(), &error));
    assert(error == ApplyError::CHUNK_RANGE);
}

void TestSizeMismatchAtCommit() {
    CommitLog log{};
    SnapshotApplyController controller(MakeCommitter(&log));
    const Snapshot snapshot = MakeSnapshot(SpeakerIdentity(), 1, 1);
    const std::vector<uint8_t> bytes = Encode(snapshot);
    controller.SetDeviceKeyHash(snapshot.device_key_hash);

    ApplyError error = ApplyError::NONE;
    assert(controller.Begin(MakeMetadata(snapshot, bytes), &error));
    assert(controller.Append(0, std::span<const uint8_t>(bytes.data(), 32), &error));
    assert(!controller.Commit(nullptr, &error, nullptr));
    assert(error == ApplyError::SIZE_MISMATCH);
    assert(log.calls == 0);
}

void TestCrcMismatchIsRejected() {
    CommitLog log{};
    SnapshotApplyController controller(MakeCommitter(&log));
    const Snapshot snapshot = MakeSnapshot(SpeakerIdentity(), 1, 1);
    std::vector<uint8_t> bytes = Encode(snapshot);
    controller.SetDeviceKeyHash(snapshot.device_key_hash);

    SnapshotMetadata metadata = MakeMetadata(snapshot, bytes);
    // Flip a payload byte after the CRC was computed.
    bytes[bytes.size() - 1] ^= 0xFFU;

    ApplyError error = ApplyError::NONE;
    assert(controller.Begin(metadata, &error));
    assert(StreamSnapshot(controller, bytes, 128, &error));
    assert(!controller.Commit(nullptr, &error, nullptr));
    assert(error == ApplyError::CRC_MISMATCH);
    assert(log.calls == 0);
}

void TestUndecodableSnapshotIsRejected() {
    CommitLog log{};
    SnapshotApplyController controller(MakeCommitter(&log));
    const Snapshot snapshot = MakeSnapshot(SpeakerIdentity(), 1, 1);
    controller.SetDeviceKeyHash(snapshot.device_key_hash);

    // Well-formed metadata over bytes that are not a snapshot.
    std::vector<uint8_t> garbage(96, 0xA5U);
    SnapshotMetadata metadata{};
    metadata.app_generation = 1;
    metadata.daemon_generation = 1;
    metadata.device_key_hash = snapshot.device_key_hash;
    metadata.total_size = static_cast<uint32_t>(garbage.size());
    metadata.crc32 = viper::daemon::Crc32(garbage);

    ApplyError error = ApplyError::NONE;
    std::string message;
    assert(controller.Begin(metadata, &error));
    assert(StreamSnapshot(controller, garbage, 32, &error));
    assert(!controller.Commit(nullptr, &error, &message));
    assert(error == ApplyError::DECODE_FAILED);
    assert(!message.empty());
    assert(log.calls == 0);
}

void TestForeignDeviceSnapshotIsRejected() {
    CommitLog log{};
    SnapshotApplyController controller(MakeCommitter(&log));
    const Snapshot local = MakeSnapshot(SpeakerIdentity(), 1, 1);
    const Snapshot foreign = MakeSnapshot(BluetoothIdentity(), 1, 1);
    const std::vector<uint8_t> bytes = Encode(foreign);
    controller.SetDeviceKeyHash(local.device_key_hash);

    ApplyError error = ApplyError::NONE;
    // Rejected up front: no buffer is ever allocated for a foreign snapshot.
    assert(!controller.Begin(MakeMetadata(foreign, bytes), &error));
    assert(error == ApplyError::DEVICE_MISMATCH);
    assert(!controller.Staging());
    assert(log.calls == 0);
}

void TestUnknownLocalRouteRejectsEverything() {
    CommitLog log{};
    SnapshotApplyController controller(MakeCommitter(&log));
    const Snapshot snapshot = MakeSnapshot(SpeakerIdentity(), 1, 1);
    const std::vector<uint8_t> bytes = Encode(snapshot);
    // No SetDeviceKeyHash: the driver does not know its route yet.

    ApplyError error = ApplyError::NONE;
    assert(!controller.Begin(MakeMetadata(snapshot, bytes), &error));
    assert(error == ApplyError::DEVICE_MISMATCH);
}

void TestMetadataMustMatchSnapshotHeader() {
    CommitLog log{};
    SnapshotApplyController controller(MakeCommitter(&log));
    const Snapshot snapshot = MakeSnapshot(SpeakerIdentity(), 4, 9);
    const std::vector<uint8_t> bytes = Encode(snapshot);
    controller.SetDeviceKeyHash(snapshot.device_key_hash);

    SnapshotMetadata metadata = MakeMetadata(snapshot, bytes);
    // Metadata claims a newer generation than the payload actually carries.
    metadata.app_generation = 40;

    ApplyError error = ApplyError::NONE;
    assert(controller.Begin(metadata, &error));
    assert(StreamSnapshot(controller, bytes, 256, &error));
    assert(!controller.Commit(nullptr, &error, nullptr));
    assert(error == ApplyError::BAD_METADATA);
    assert(log.calls == 0);
}

void TestBadMetadataIsRejectedAtBegin() {
    CommitLog log{};
    SnapshotApplyController controller(MakeCommitter(&log));
    const Snapshot snapshot = MakeSnapshot(SpeakerIdentity(), 1, 1);
    const std::vector<uint8_t> bytes = Encode(snapshot);
    controller.SetDeviceKeyHash(snapshot.device_key_hash);
    const SnapshotMetadata good = MakeMetadata(snapshot, bytes);

    ApplyError error = ApplyError::NONE;

    SnapshotMetadata empty = good;
    empty.total_size = 0;
    assert(!controller.Begin(empty, &error));
    assert(error == ApplyError::BAD_METADATA);

    SnapshotMetadata huge = good;
    huge.total_size = static_cast<uint32_t>(viper::daemon::kMaxSnapshotSize) + 1U;
    assert(!controller.Begin(huge, &error));
    assert(error == ApplyError::BAD_METADATA);

    SnapshotMetadata no_app = good;
    no_app.app_generation = 0;
    assert(!controller.Begin(no_app, &error));
    assert(error == ApplyError::BAD_METADATA);

    SnapshotMetadata no_daemon = good;
    no_daemon.daemon_generation = 0;
    assert(!controller.Begin(no_daemon, &error));
    assert(error == ApplyError::BAD_METADATA);

    SnapshotMetadata short_hash = good;
    short_hash.device_key_hash = "abcd";
    assert(!controller.Begin(short_hash, &error));
    assert(error == ApplyError::BAD_METADATA);

    // None of the rejections left staging armed.
    assert(!controller.Staging());
    assert(controller.Begin(good, &error));
    // A second Begin while staging is a protocol error.
    assert(!controller.Begin(good, &error));
    assert(error == ApplyError::ALREADY_STAGING);
}

void TestGraphPrepareFailurePreservesLastApplied() {
    CommitLog log{};
    SnapshotApplyController controller(MakeCommitter(&log));
    const Snapshot first = MakeSnapshot(SpeakerIdentity(), 2, 2);
    const std::vector<uint8_t> first_bytes = Encode(first);
    controller.SetDeviceKeyHash(first.device_key_hash);

    ApplyError error = ApplyError::NONE;
    ApplyResult result{};
    assert(controller.Begin(MakeMetadata(first, first_bytes), &error));
    assert(StreamSnapshot(controller, first_bytes, 96, &error));
    assert(controller.Commit(&result, &error, nullptr));
    assert(result.outcome == ApplyOutcome::APPLIED);

    // The driver now refuses the next snapshot.
    log.succeed = false;
    const Snapshot second = MakeSnapshot(SpeakerIdentity(), 3, 3);
    const std::vector<uint8_t> second_bytes = Encode(second);
    std::string message;
    assert(controller.Begin(MakeMetadata(second, second_bytes), &error));
    assert(StreamSnapshot(controller, second_bytes, 96, &error));
    assert(!controller.Commit(nullptr, &error, &message));
    assert(error == ApplyError::GRAPH_PREPARE_FAILED);
    assert(message == log.failure);
    // The previously applied generation stands: the running graph is unchanged.
    assert(controller.LastAppliedGeneration() == 2);
    assert(controller.LastAppliedDaemonGeneration() == 2);
    assert(!controller.Staging());

    // Recovery: the same snapshot applies once the driver accepts it again.
    log.succeed = true;
    assert(controller.Begin(MakeMetadata(second, second_bytes), &error));
    assert(StreamSnapshot(controller, second_bytes, 96, &error));
    assert(controller.Commit(&result, &error, nullptr));
    assert(result.outcome == ApplyOutcome::APPLIED);
    assert(controller.LastAppliedGeneration() == 3);
}

void TestIdempotentGenerationSkipsCommit() {
    CommitLog log{};
    SnapshotApplyController controller(MakeCommitter(&log));
    const Snapshot snapshot = MakeSnapshot(SpeakerIdentity(), 6, 8);
    const std::vector<uint8_t> bytes = Encode(snapshot);
    controller.SetDeviceKeyHash(snapshot.device_key_hash);

    ApplyError error = ApplyError::NONE;
    ApplyResult result{};
    assert(controller.Begin(MakeMetadata(snapshot, bytes), &error));
    assert(StreamSnapshot(controller, bytes, 128, &error));
    assert(controller.Commit(&result, &error, nullptr));
    assert(log.calls == 1);

    // Replaying the same generation pair must not re-prepare the graph.
    assert(controller.Begin(MakeMetadata(snapshot, bytes), &error));
    assert(StreamSnapshot(controller, bytes, 128, &error));
    assert(controller.Commit(&result, &error, nullptr));
    assert(result.outcome == ApplyOutcome::IDEMPOTENT);
    assert(log.calls == 1);
    // The reported generations still describe the live graph.
    assert(result.resource_generation == 11);
    assert(result.graph_generation == 12);
}

void TestStaleGenerationIsRejected() {
    CommitLog log{};
    SnapshotApplyController controller(MakeCommitter(&log));
    const Snapshot current = MakeSnapshot(SpeakerIdentity(), 10, 10);
    const std::vector<uint8_t> current_bytes = Encode(current);
    controller.SetDeviceKeyHash(current.device_key_hash);

    ApplyError error = ApplyError::NONE;
    ApplyResult result{};
    assert(controller.Begin(MakeMetadata(current, current_bytes), &error));
    assert(StreamSnapshot(controller, current_bytes, 128, &error));
    assert(controller.Commit(&result, &error, nullptr));

    const Snapshot older = MakeSnapshot(SpeakerIdentity(), 9, 12);
    const std::vector<uint8_t> older_bytes = Encode(older);
    assert(controller.Begin(MakeMetadata(older, older_bytes), &error));
    assert(StreamSnapshot(controller, older_bytes, 128, &error));
    assert(!controller.Commit(nullptr, &error, nullptr));
    assert(error == ApplyError::STALE_GENERATION);
    assert(log.calls == 1);
    assert(controller.LastAppliedGeneration() == 10);

    // Same app generation but an older daemon generation is also stale.
    const Snapshot same_app = MakeSnapshot(SpeakerIdentity(), 10, 9);
    const std::vector<uint8_t> same_app_bytes = Encode(same_app);
    assert(controller.Begin(MakeMetadata(same_app, same_app_bytes), &error));
    assert(StreamSnapshot(controller, same_app_bytes, 128, &error));
    assert(!controller.Commit(nullptr, &error, nullptr));
    assert(error == ApplyError::STALE_GENERATION);

    // A newer daemon generation under the same app generation is accepted: the
    // daemon restored a route after the App last wrote.
    const Snapshot newer_daemon = MakeSnapshot(SpeakerIdentity(), 10, 11);
    const std::vector<uint8_t> newer_bytes = Encode(newer_daemon);
    assert(controller.Begin(MakeMetadata(newer_daemon, newer_bytes), &error));
    assert(StreamSnapshot(controller, newer_bytes, 128, &error));
    assert(controller.Commit(&result, &error, nullptr));
    assert(result.outcome == ApplyOutcome::APPLIED);
    assert(log.calls == 2);
}

void TestAbortDiscardsStagedBytes() {
    CommitLog log{};
    SnapshotApplyController controller(MakeCommitter(&log));
    const Snapshot snapshot = MakeSnapshot(SpeakerIdentity(), 1, 1);
    const std::vector<uint8_t> bytes = Encode(snapshot);
    controller.SetDeviceKeyHash(snapshot.device_key_hash);

    ApplyError error = ApplyError::NONE;
    assert(controller.Begin(MakeMetadata(snapshot, bytes), &error));
    assert(controller.Append(0, std::span<const uint8_t>(bytes.data(), 64), &error));
    controller.Abort(ApplyError::ABORTED);
    assert(!controller.Staging());
    assert(controller.StagedBytes() == 0);
    assert(controller.LastError() == ApplyError::ABORTED);

    // Append/Commit after abort are protocol errors, not silent no-ops.
    assert(!controller.Append(64, std::span<const uint8_t>(bytes.data() + 64, 8), &error));
    assert(error == ApplyError::NOT_STAGING);
    assert(!controller.Commit(nullptr, &error, nullptr));
    assert(error == ApplyError::NOT_STAGING);

    // A fresh stream still works after an abort.
    assert(controller.Begin(MakeMetadata(snapshot, bytes), &error));
    assert(StreamSnapshot(controller, bytes, 32, &error));
    ApplyResult result{};
    assert(controller.Commit(&result, &error, nullptr));
    assert(result.outcome == ApplyOutcome::APPLIED);
}

void TestMissingCommitterFailsClosed() {
    SnapshotApplyController controller(nullptr);
    const Snapshot snapshot = MakeSnapshot(SpeakerIdentity(), 1, 1);
    const std::vector<uint8_t> bytes = Encode(snapshot);
    controller.SetDeviceKeyHash(snapshot.device_key_hash);

    ApplyError error = ApplyError::NONE;
    assert(controller.Begin(MakeMetadata(snapshot, bytes), &error));
    assert(StreamSnapshot(controller, bytes, 64, &error));
    assert(!controller.Commit(nullptr, &error, nullptr));
    assert(error == ApplyError::GRAPH_PREPARE_FAILED);
    assert(controller.LastAppliedGeneration() == 0);
}

void TestErrorMessagesAreDistinct() {
    // Deterministic error codes need distinct, non-empty reasons for the ACK.
    const ApplyError errors[] = {
        ApplyError::NONE,
        ApplyError::NOT_STAGING,
        ApplyError::ALREADY_STAGING,
        ApplyError::BAD_METADATA,
        ApplyError::CHUNK_OUT_OF_ORDER,
        ApplyError::CHUNK_RANGE,
        ApplyError::SIZE_MISMATCH,
        ApplyError::CRC_MISMATCH,
        ApplyError::DECODE_FAILED,
        ApplyError::DEVICE_MISMATCH,
        ApplyError::STALE_GENERATION,
        ApplyError::GRAPH_PREPARE_FAILED,
        ApplyError::ABORTED,
    };
    for (std::size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        const char *left = viper::audio::ApplyErrorMessage(errors[i]);
        assert(left != nullptr && left[0] != '\0');
        for (std::size_t j = i + 1; j < sizeof(errors) / sizeof(errors[0]); ++j) {
            assert(std::strcmp(left, viper::audio::ApplyErrorMessage(errors[j])) != 0);
        }
    }
}

} // namespace

int main() {
    TestCompleteSnapshotApplies();
    TestOutOfOrderChunkAborts();
    TestRewindChunkIsRejected();
    TestChunkBeyondDeclaredSizeIsRejected();
    TestSizeMismatchAtCommit();
    TestCrcMismatchIsRejected();
    TestUndecodableSnapshotIsRejected();
    TestForeignDeviceSnapshotIsRejected();
    TestUnknownLocalRouteRejectsEverything();
    TestMetadataMustMatchSnapshotHeader();
    TestBadMetadataIsRejectedAtBegin();
    TestGraphPrepareFailurePreservesLastApplied();
    TestIdempotentGenerationSkipsCommit();
    TestStaleGenerationIsRejected();
    TestAbortDiscardsStagedBytes();
    TestMissingCommitterFailsClosed();
    TestErrorMessagesAreDistinct();
    std::puts("snapshot apply controller tests passed");
    return 0;
}
