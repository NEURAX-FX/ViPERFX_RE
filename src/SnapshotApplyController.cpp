#include "SnapshotApplyController.h"

#include "ViperDaemonProtocol.h"

#include <utility>

namespace viper::audio {
namespace {

constexpr std::size_t kMaxStagedBytes = daemon::kMaxSnapshotSize;

} // namespace

const char *ApplyErrorMessage(ApplyError error) noexcept {
    switch (error) {
        case ApplyError::NONE: return "ok";
        case ApplyError::NOT_STAGING: return "no snapshot is being staged";
        case ApplyError::ALREADY_STAGING: return "a snapshot is already being staged";
        case ApplyError::BAD_METADATA: return "snapshot metadata is invalid";
        case ApplyError::CHUNK_OUT_OF_ORDER: return "snapshot chunk is out of order";
        case ApplyError::CHUNK_RANGE: return "snapshot chunk exceeds the declared size";
        case ApplyError::SIZE_MISMATCH: return "staged size does not match metadata";
        case ApplyError::CRC_MISMATCH: return "staged snapshot failed CRC validation";
        case ApplyError::DECODE_FAILED: return "snapshot payload could not be decoded";
        case ApplyError::DEVICE_MISMATCH: return "snapshot belongs to another device";
        case ApplyError::STALE_GENERATION: return "snapshot generation is stale";
        case ApplyError::GRAPH_PREPARE_FAILED: return "driver failed to prepare the snapshot";
        case ApplyError::ABORTED: return "snapshot staging was aborted";
    }
    return "unknown apply error";
}

SnapshotApplyController::SnapshotApplyController(Committer committer)
    : committer_(std::move(committer)) {}

void SnapshotApplyController::SetDeviceKeyHash(std::string device_key_hash) {
    device_key_hash_ = std::move(device_key_hash);
}

bool SnapshotApplyController::Fail(ApplyError reason, ApplyError *error) {
    last_error_ = reason;
    last_message_ = ApplyErrorMessage(reason);
    if (error != nullptr) *error = reason;
    return false;
}

void SnapshotApplyController::Reset() {
    staging_ = false;
    metadata_ = SnapshotMetadata{};
    // Release the staging buffer: a 4 MiB snapshot must not stay resident inside
    // the audio effect for the lifetime of the session.
    staged_.clear();
    staged_.shrink_to_fit();
}

bool SnapshotApplyController::Begin(
    const SnapshotMetadata &metadata,
    ApplyError *error
) {
    if (staging_) return Fail(ApplyError::ALREADY_STAGING, error);
    if (metadata.total_size == 0U || metadata.total_size > kMaxStagedBytes) {
        return Fail(ApplyError::BAD_METADATA, error);
    }
    if (metadata.app_generation == 0U || metadata.daemon_generation == 0U) {
        return Fail(ApplyError::BAD_METADATA, error);
    }
    if (metadata.device_key_hash.size() != 64U) {
        return Fail(ApplyError::BAD_METADATA, error);
    }
    // Reject a foreign snapshot before allocating for it. An empty local hash
    // means the driver has not been told its route yet, so nothing can be
    // matched and a snapshot must not be guessed onto this device.
    if (device_key_hash_.empty() || metadata.device_key_hash != device_key_hash_) {
        return Fail(ApplyError::DEVICE_MISMATCH, error);
    }

    metadata_ = metadata;
    staged_.clear();
    staged_.reserve(metadata.total_size);
    staging_ = true;
    last_error_ = ApplyError::NONE;
    last_message_.clear();
    if (error != nullptr) *error = ApplyError::NONE;
    return true;
}

bool SnapshotApplyController::Append(
    uint32_t offset,
    std::span<const uint8_t> chunk,
    ApplyError *error
) {
    if (!staging_) return Fail(ApplyError::NOT_STAGING, error);
    // Chunks are strictly sequential: a gap would leave uninitialized bytes and
    // a rewind would silently accept a rewritten prefix.
    if (offset != static_cast<uint32_t>(staged_.size())) {
        Abort(ApplyError::CHUNK_OUT_OF_ORDER);
        return Fail(ApplyError::CHUNK_OUT_OF_ORDER, error);
    }
    if (chunk.empty()) {
        Abort(ApplyError::CHUNK_RANGE);
        return Fail(ApplyError::CHUNK_RANGE, error);
    }
    if (chunk.size() > metadata_.total_size - staged_.size()) {
        Abort(ApplyError::CHUNK_RANGE);
        return Fail(ApplyError::CHUNK_RANGE, error);
    }

    staged_.insert(staged_.end(), chunk.begin(), chunk.end());
    if (error != nullptr) *error = ApplyError::NONE;
    return true;
}

bool SnapshotApplyController::Commit(
    ApplyResult *result,
    ApplyError *error,
    std::string *message
) {
    if (!staging_) return Fail(ApplyError::NOT_STAGING, error);

    if (staged_.size() != metadata_.total_size) {
        Abort(ApplyError::SIZE_MISMATCH);
        return Fail(ApplyError::SIZE_MISMATCH, error);
    }
    if (daemon::Crc32(staged_) != metadata_.crc32) {
        Abort(ApplyError::CRC_MISMATCH);
        return Fail(ApplyError::CRC_MISMATCH, error);
    }

    daemon::Snapshot snapshot{};
    std::string decode_error;
    if (!daemon::DecodeSnapshot(staged_, &snapshot, &decode_error)) {
        Abort(ApplyError::DECODE_FAILED);
        if (message != nullptr) *message = decode_error;
        return Fail(ApplyError::DECODE_FAILED, error);
    }
    // The header is authoritative for identity; metadata must agree with it.
    if (snapshot.device_key_hash != device_key_hash_) {
        Abort(ApplyError::DEVICE_MISMATCH);
        return Fail(ApplyError::DEVICE_MISMATCH, error);
    }
    if (snapshot.app_generation != metadata_.app_generation
        || snapshot.daemon_generation != metadata_.daemon_generation) {
        Abort(ApplyError::BAD_METADATA);
        return Fail(ApplyError::BAD_METADATA, error);
    }

    // Older or equal generations must never roll the driver backwards. Equal
    // generations are reported as idempotent instead of re-preparing the graph.
    if (snapshot.app_generation < last_app_generation_
        || (snapshot.app_generation == last_app_generation_
            && snapshot.daemon_generation < last_daemon_generation_)) {
        Abort(ApplyError::STALE_GENERATION);
        return Fail(ApplyError::STALE_GENERATION, error);
    }
    if (snapshot.app_generation == last_app_generation_
        && snapshot.daemon_generation == last_daemon_generation_) {
        Reset();
        if (result != nullptr) {
            result->outcome = ApplyOutcome::IDEMPOTENT;
            result->app_generation = last_app_generation_;
            result->daemon_generation = last_daemon_generation_;
            result->resource_generation = last_resource_generation_;
            result->graph_generation = last_graph_generation_;
        }
        last_error_ = ApplyError::NONE;
        last_message_.clear();
        if (error != nullptr) *error = ApplyError::NONE;
        return true;
    }

    std::vector<daemon::RawParamRecord> parameters;
    std::vector<daemon::RawParamRecord> iem_parameters;
    if (!daemon::DecodeParameterStream(snapshot.parameters, &parameters, &decode_error)
        || !daemon::DecodeParameterStream(
            snapshot.iem_parameters, &iem_parameters, &decode_error
        )) {
        Abort(ApplyError::DECODE_FAILED);
        if (message != nullptr) *message = decode_error;
        return Fail(ApplyError::DECODE_FAILED, error);
    }

    if (!committer_) {
        Abort(ApplyError::GRAPH_PREPARE_FAILED);
        return Fail(ApplyError::GRAPH_PREPARE_FAILED, error);
    }

    uint64_t resource_generation = 0;
    uint64_t graph_generation = 0;
    std::string commit_error;
    const CommitRequest request{snapshot, parameters, iem_parameters};
    if (!committer_(request, &resource_generation, &graph_generation, &commit_error)) {
        // The committer contract keeps the previously active graph running, so a
        // failed apply is observable but not audible.
        Abort(ApplyError::GRAPH_PREPARE_FAILED);
        if (message != nullptr) *message = commit_error;
        return Fail(ApplyError::GRAPH_PREPARE_FAILED, error);
    }

    last_app_generation_ = snapshot.app_generation;
    last_daemon_generation_ = snapshot.daemon_generation;
    last_resource_generation_ = resource_generation;
    last_graph_generation_ = graph_generation;
    Reset();

    if (result != nullptr) {
        result->outcome = ApplyOutcome::APPLIED;
        result->app_generation = last_app_generation_;
        result->daemon_generation = last_daemon_generation_;
        result->resource_generation = resource_generation;
        result->graph_generation = graph_generation;
    }
    last_error_ = ApplyError::NONE;
    last_message_.clear();
    if (error != nullptr) *error = ApplyError::NONE;
    if (message != nullptr) message->clear();
    return true;
}

void SnapshotApplyController::Abort(ApplyError reason) {
    Reset();
    last_error_ = reason;
    last_message_ = ApplyErrorMessage(reason);
}

} // namespace viper::audio
