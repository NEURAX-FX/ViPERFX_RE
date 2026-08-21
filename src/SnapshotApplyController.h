#pragma once

#include "ParameterStream.h"
#include "SnapshotSchema.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace viper::audio {

enum class ApplyError : uint32_t {
    NONE = 0,
    NOT_STAGING = 1,
    ALREADY_STAGING = 2,
    BAD_METADATA = 3,
    CHUNK_OUT_OF_ORDER = 4,
    CHUNK_RANGE = 5,
    SIZE_MISMATCH = 6,
    CRC_MISMATCH = 7,
    DECODE_FAILED = 8,
    DEVICE_MISMATCH = 9,
    STALE_GENERATION = 10,
    GRAPH_PREPARE_FAILED = 11,
    ABORTED = 12,
};

const char *ApplyErrorMessage(ApplyError error) noexcept;

// Announced before the first chunk so staging buffers are sized exactly once.
struct SnapshotMetadata {
    uint64_t app_generation = 0;
    uint64_t daemon_generation = 0;
    std::string device_key_hash;
    uint32_t total_size = 0;
    uint32_t crc32 = 0;
};

enum class ApplyOutcome {
    APPLIED,
    // Same generation pair already applied; the graph is left untouched.
    IDEMPOTENT,
};

struct ApplyResult {
    ApplyOutcome outcome = ApplyOutcome::APPLIED;
    uint64_t app_generation = 0;
    uint64_t daemon_generation = 0;
    uint64_t resource_generation = 0;
    uint64_t graph_generation = 0;
};

/**
 * Stages a daemon snapshot on the control thread and commits it atomically.
 *
 * Every method here runs on AudioFlinger command threads. Nothing in this class
 * is reachable from ViperContext::Process(): staging allocates, validates, and
 * prepares graphs, all of which are forbidden on the audio thread.
 *
 * The owner supplies a Committer that performs the driver-side apply. A failing
 * Committer must leave the previously active graph running.
 */
class SnapshotApplyController final {
public:
    struct CommitRequest {
        const daemon::Snapshot &snapshot;
        const std::vector<daemon::RawParamRecord> &parameters;
        const std::vector<daemon::RawParamRecord> &iem_parameters;
    };

    // Returns false when the driver could not adopt the snapshot; `error`
    // receives a bounded reason. `resource_generation`/`graph_generation` report
    // the generations observable after a successful commit.
    using Committer = std::function<bool(
        const CommitRequest &request,
        uint64_t *resource_generation,
        uint64_t *graph_generation,
        std::string *error
    )>;

    explicit SnapshotApplyController(Committer committer);

    void SetDeviceKeyHash(std::string device_key_hash);

    bool Begin(const SnapshotMetadata &metadata, ApplyError *error);
    bool Append(uint32_t offset, std::span<const uint8_t> chunk, ApplyError *error);
    bool Commit(ApplyResult *result, ApplyError *error, std::string *message);
    void Abort(ApplyError reason);

    bool Staging() const noexcept { return staging_; }
    uint32_t StagedBytes() const noexcept {
        return static_cast<uint32_t>(staged_.size());
    }
    uint64_t LastAppliedGeneration() const noexcept { return last_app_generation_; }
    uint64_t LastAppliedDaemonGeneration() const noexcept {
        return last_daemon_generation_;
    }
    ApplyError LastError() const noexcept { return last_error_; }
    const std::string &LastMessage() const noexcept { return last_message_; }

private:
    bool Fail(ApplyError reason, ApplyError *error);
    void Reset();

    Committer committer_;
    std::string device_key_hash_;

    bool staging_ = false;
    SnapshotMetadata metadata_{};
    std::vector<uint8_t> staged_;

    uint64_t last_app_generation_ = 0;
    uint64_t last_daemon_generation_ = 0;
    uint64_t last_resource_generation_ = 0;
    uint64_t last_graph_generation_ = 0;
    ApplyError last_error_ = ApplyError::NONE;
    std::string last_message_;
};

} // namespace viper::audio
