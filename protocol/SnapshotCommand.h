#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace viper::daemon {

// Daemon-to-driver snapshot apply commands, carried in the payload of a frame
// whose message_type is the matching SnapshotCommandType.
//
// A snapshot is streamed rather than sent whole so the driver never has to hold
// a frame larger than kMaxFrameSize, and so a partial transfer can be abandoned
// without disturbing the running graph.
constexpr uint16_t kSnapshotCommandVersion = 1;

// Frame message types. These live above the DriverEventType range so a decoder
// can tell a driver event from a daemon command by header alone.
enum class SnapshotCommandType : uint16_t {
    SNAPSHOT_BEGIN = 100,
    SNAPSHOT_CHUNK = 101,
    SNAPSHOT_COMMIT = 102,
    SNAPSHOT_ABORT = 103,
    // Tells the driver which route the daemon believes is live.
    //
    // The driver cannot determine this itself: it sees an AudioFlinger effect
    // instance, not a mixer route. Without this the driver's route hash stays
    // empty and it refuses every snapshot as DEVICE_MISMATCH, which is exactly
    // what happened on device. The daemon is the only authority for the route,
    // so it announces it rather than the driver trusting BEGIN's own hash --
    // that would make the mismatch check vacuous against a corrupt store.
    ROUTE_ANNOUNCE = 104,
};

bool IsSnapshotCommandType(uint16_t value) noexcept;

constexpr std::size_t kSnapshotBeginWireSize = 96;
constexpr std::size_t kSnapshotChunkHeaderSize = 16;
constexpr std::size_t kSnapshotCommitWireSize = 24;
constexpr std::size_t kSnapshotAbortWireSize = 16;
// version + reserved + hash_length + 64-byte hash field, hash zero-padded when
// the announce clears the route.
constexpr std::size_t kRouteAnnounceWireSize = 72;
// Bounded so one chunk always fits in a single SOCK_SEQPACKET frame.
constexpr std::size_t kMaxSnapshotChunkBytes = 64U * 1024U;

struct SnapshotBegin {
    uint16_t version = kSnapshotCommandVersion;
    uint64_t app_generation = 0;
    uint64_t daemon_generation = 0;
    uint32_t total_size = 0;
    uint32_t crc32 = 0;
    // 64 lowercase hex characters.
    std::string device_key_hash;
};

struct SnapshotChunk {
    uint32_t offset = 0;
    std::vector<uint8_t> data;
};

struct SnapshotCommit {
    uint64_t app_generation = 0;
    uint64_t daemon_generation = 0;
};

struct SnapshotAbort {
    uint32_t reason = 0;
};

struct RouteAnnounce {
    uint16_t version = kSnapshotCommandVersion;
    // 64 lowercase hex characters, or empty to clear the route.
    std::string device_key_hash;
};

bool EncodeSnapshotBegin(
    const SnapshotBegin &begin,
    std::vector<uint8_t> *out,
    std::string *error
);
bool DecodeSnapshotBegin(
    std::span<const uint8_t> bytes,
    SnapshotBegin *begin,
    std::string *error
);

bool EncodeSnapshotChunk(
    const SnapshotChunk &chunk,
    std::vector<uint8_t> *out,
    std::string *error
);
bool DecodeSnapshotChunk(
    std::span<const uint8_t> bytes,
    SnapshotChunk *chunk,
    std::string *error
);

bool EncodeSnapshotCommit(
    const SnapshotCommit &commit,
    std::vector<uint8_t> *out,
    std::string *error
);
bool DecodeSnapshotCommit(
    std::span<const uint8_t> bytes,
    SnapshotCommit *commit,
    std::string *error
);

bool EncodeSnapshotAbort(
    const SnapshotAbort &abort,
    std::vector<uint8_t> *out,
    std::string *error
);
bool DecodeSnapshotAbort(
    std::span<const uint8_t> bytes,
    SnapshotAbort *abort,
    std::string *error
);

bool EncodeRouteAnnounce(
    const RouteAnnounce &announce,
    std::vector<uint8_t> *out,
    std::string *error
);
bool DecodeRouteAnnounce(
    std::span<const uint8_t> bytes,
    RouteAnnounce *announce,
    std::string *error
);

} // namespace viper::daemon
