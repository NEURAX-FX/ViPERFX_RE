#include "SnapshotCommand.h"

#include "SnapshotSchema.h"

namespace viper::daemon {
namespace {

constexpr std::size_t kDeviceHashSize = 64;

void SetError(std::string *error, const char *message) {
    if (error != nullptr) error->assign(message);
}

void PutU16(std::vector<uint8_t> &out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8U));
}

void PutU32(std::vector<uint8_t> &out, uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<uint8_t>(value >> shift));
    }
}

void PutU64(std::vector<uint8_t> &out, uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<uint8_t>(value >> shift));
    }
}

uint16_t ReadU16(std::span<const uint8_t> bytes, std::size_t &offset) noexcept {
    const uint16_t value = static_cast<uint16_t>(bytes[offset])
        | static_cast<uint16_t>(bytes[offset + 1U]) << 8U;
    offset += 2U;
    return value;
}

uint32_t ReadU32(std::span<const uint8_t> bytes, std::size_t &offset) noexcept {
    uint32_t value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        value |= static_cast<uint32_t>(bytes[offset + shift / 8U]) << shift;
    }
    offset += 4U;
    return value;
}

uint64_t ReadU64(std::span<const uint8_t> bytes, std::size_t &offset) noexcept {
    uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        value |= static_cast<uint64_t>(bytes[offset + shift / 8U]) << shift;
    }
    offset += 8U;
    return value;
}

bool IsLowerHex(std::string_view value) noexcept {
    if (value.size() != kDeviceHashSize) return false;
    for (const unsigned char byte : value) {
        if ((byte < '0' || byte > '9') && (byte < 'a' || byte > 'f')) return false;
    }
    return true;
}

} // namespace

bool IsSnapshotCommandType(uint16_t value) noexcept {
    return value >= static_cast<uint16_t>(SnapshotCommandType::SNAPSHOT_BEGIN)
        && value <= static_cast<uint16_t>(SnapshotCommandType::ROUTE_ANNOUNCE);
}

bool EncodeSnapshotBegin(
    const SnapshotBegin &begin,
    std::vector<uint8_t> *out,
    std::string *error
) {
    if (out == nullptr) {
        SetError(error, "null output");
        return false;
    }
    if (begin.version != kSnapshotCommandVersion) {
        SetError(error, "unsupported snapshot command version");
        return false;
    }
    if (!IsLowerHex(begin.device_key_hash)) {
        SetError(error, "device_key_hash must be 64 lowercase hex characters");
        return false;
    }
    if (begin.total_size == 0U || begin.total_size > kMaxSnapshotSize) {
        SetError(error, "snapshot size is out of range");
        return false;
    }
    if (begin.app_generation == 0U || begin.daemon_generation == 0U) {
        SetError(error, "snapshot generations must be non-zero");
        return false;
    }

    out->clear();
    out->reserve(kSnapshotBeginWireSize);
    PutU16(*out, begin.version);
    PutU16(*out, 0U); // reserved
    PutU32(*out, begin.total_size);
    PutU64(*out, begin.app_generation);
    PutU64(*out, begin.daemon_generation);
    PutU32(*out, begin.crc32);
    PutU32(*out, 0U); // reserved
    out->insert(out->end(), begin.device_key_hash.begin(), begin.device_key_hash.end());
    return out->size() == kSnapshotBeginWireSize;
}

bool DecodeSnapshotBegin(
    std::span<const uint8_t> bytes,
    SnapshotBegin *begin,
    std::string *error
) {
    if (begin == nullptr) {
        SetError(error, "null output");
        return false;
    }
    if (bytes.size() != kSnapshotBeginWireSize) {
        SetError(error, "snapshot begin size mismatch");
        return false;
    }

    std::size_t offset = 0;
    SnapshotBegin decoded{};
    decoded.version = ReadU16(bytes, offset);
    if (decoded.version != kSnapshotCommandVersion) {
        SetError(error, "unsupported snapshot command version");
        return false;
    }
    if (ReadU16(bytes, offset) != 0U) {
        SetError(error, "reserved field must be zero");
        return false;
    }
    decoded.total_size = ReadU32(bytes, offset);
    decoded.app_generation = ReadU64(bytes, offset);
    decoded.daemon_generation = ReadU64(bytes, offset);
    decoded.crc32 = ReadU32(bytes, offset);
    if (ReadU32(bytes, offset) != 0U) {
        SetError(error, "reserved field must be zero");
        return false;
    }
    decoded.device_key_hash.assign(
        reinterpret_cast<const char *>(bytes.data() + offset), kDeviceHashSize);

    if (!IsLowerHex(decoded.device_key_hash)) {
        SetError(error, "device_key_hash must be 64 lowercase hex characters");
        return false;
    }
    if (decoded.total_size == 0U || decoded.total_size > kMaxSnapshotSize) {
        SetError(error, "snapshot size is out of range");
        return false;
    }
    if (decoded.app_generation == 0U || decoded.daemon_generation == 0U) {
        SetError(error, "snapshot generations must be non-zero");
        return false;
    }

    *begin = std::move(decoded);
    if (error != nullptr) error->clear();
    return true;
}

bool EncodeSnapshotChunk(
    const SnapshotChunk &chunk,
    std::vector<uint8_t> *out,
    std::string *error
) {
    if (out == nullptr) {
        SetError(error, "null output");
        return false;
    }
    if (chunk.data.empty() || chunk.data.size() > kMaxSnapshotChunkBytes) {
        SetError(error, "snapshot chunk size is out of range");
        return false;
    }

    out->clear();
    out->reserve(kSnapshotChunkHeaderSize + chunk.data.size());
    PutU32(*out, chunk.offset);
    PutU32(*out, static_cast<uint32_t>(chunk.data.size()));
    PutU64(*out, 0U); // reserved
    out->insert(out->end(), chunk.data.begin(), chunk.data.end());
    return true;
}

bool DecodeSnapshotChunk(
    std::span<const uint8_t> bytes,
    SnapshotChunk *chunk,
    std::string *error
) {
    if (chunk == nullptr) {
        SetError(error, "null output");
        return false;
    }
    if (bytes.size() < kSnapshotChunkHeaderSize) {
        SetError(error, "snapshot chunk header is truncated");
        return false;
    }

    std::size_t offset = 0;
    SnapshotChunk decoded{};
    decoded.offset = ReadU32(bytes, offset);
    const uint32_t length = ReadU32(bytes, offset);
    if (ReadU64(bytes, offset) != 0U) {
        SetError(error, "reserved field must be zero");
        return false;
    }
    if (length == 0U || length > kMaxSnapshotChunkBytes) {
        SetError(error, "snapshot chunk size is out of range");
        return false;
    }
    // Declared length must match the frame exactly: a short frame would silently
    // stage fewer bytes than the daemon believes it sent.
    if (bytes.size() - offset != length) {
        SetError(error, "snapshot chunk length does not match the frame");
        return false;
    }
    decoded.data.assign(
        bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.end());

    *chunk = std::move(decoded);
    if (error != nullptr) error->clear();
    return true;
}

bool EncodeSnapshotCommit(
    const SnapshotCommit &commit,
    std::vector<uint8_t> *out,
    std::string *error
) {
    if (out == nullptr) {
        SetError(error, "null output");
        return false;
    }
    if (commit.app_generation == 0U || commit.daemon_generation == 0U) {
        SetError(error, "snapshot generations must be non-zero");
        return false;
    }

    out->clear();
    out->reserve(kSnapshotCommitWireSize);
    PutU64(*out, commit.app_generation);
    PutU64(*out, commit.daemon_generation);
    PutU64(*out, 0U); // reserved
    return out->size() == kSnapshotCommitWireSize;
}

bool DecodeSnapshotCommit(
    std::span<const uint8_t> bytes,
    SnapshotCommit *commit,
    std::string *error
) {
    if (commit == nullptr) {
        SetError(error, "null output");
        return false;
    }
    if (bytes.size() != kSnapshotCommitWireSize) {
        SetError(error, "snapshot commit size mismatch");
        return false;
    }

    std::size_t offset = 0;
    SnapshotCommit decoded{};
    decoded.app_generation = ReadU64(bytes, offset);
    decoded.daemon_generation = ReadU64(bytes, offset);
    if (ReadU64(bytes, offset) != 0U) {
        SetError(error, "reserved field must be zero");
        return false;
    }
    if (decoded.app_generation == 0U || decoded.daemon_generation == 0U) {
        SetError(error, "snapshot generations must be non-zero");
        return false;
    }

    *commit = decoded;
    if (error != nullptr) error->clear();
    return true;
}

bool EncodeSnapshotAbort(
    const SnapshotAbort &abort,
    std::vector<uint8_t> *out,
    std::string *error
) {
    if (out == nullptr) {
        SetError(error, "null output");
        return false;
    }

    out->clear();
    out->reserve(kSnapshotAbortWireSize);
    PutU32(*out, abort.reason);
    PutU32(*out, 0U); // reserved
    PutU64(*out, 0U); // reserved
    return out->size() == kSnapshotAbortWireSize;
}

bool DecodeSnapshotAbort(
    std::span<const uint8_t> bytes,
    SnapshotAbort *abort,
    std::string *error
) {
    if (abort == nullptr) {
        SetError(error, "null output");
        return false;
    }
    if (bytes.size() != kSnapshotAbortWireSize) {
        SetError(error, "snapshot abort size mismatch");
        return false;
    }

    std::size_t offset = 0;
    SnapshotAbort decoded{};
    decoded.reason = ReadU32(bytes, offset);
    if (ReadU32(bytes, offset) != 0U || ReadU64(bytes, offset) != 0U) {
        SetError(error, "reserved field must be zero");
        return false;
    }

    *abort = decoded;
    if (error != nullptr) error->clear();
    return true;
}

bool EncodeRouteAnnounce(
    const RouteAnnounce &announce,
    std::vector<uint8_t> *out,
    std::string *error
) {
    if (out == nullptr) {
        SetError(error, "null output");
        return false;
    }
    if (announce.version != kSnapshotCommandVersion) {
        SetError(error, "unsupported snapshot command version");
        return false;
    }
    // Empty is legal and means "no route": the daemon uses it to revoke a route it
    // can no longer vouch for, which must stop the driver applying snapshots.
    if (!announce.device_key_hash.empty() && !IsLowerHex(announce.device_key_hash)) {
        SetError(error, "device_key_hash must be 64 lowercase hex characters or empty");
        return false;
    }

    out->clear();
    out->reserve(kRouteAnnounceWireSize);
    PutU16(*out, announce.version);
    PutU16(*out, 0U); // reserved
    PutU32(*out, static_cast<uint32_t>(announce.device_key_hash.size()));
    out->insert(out->end(), announce.device_key_hash.begin(), announce.device_key_hash.end());
    // Zero-pad so the record is fixed size regardless of hash presence.
    out->resize(kRouteAnnounceWireSize, 0U);
    return out->size() == kRouteAnnounceWireSize;
}

bool DecodeRouteAnnounce(
    std::span<const uint8_t> bytes,
    RouteAnnounce *announce,
    std::string *error
) {
    if (announce == nullptr) {
        SetError(error, "null output");
        return false;
    }
    if (bytes.size() != kRouteAnnounceWireSize) {
        SetError(error, "route announce size mismatch");
        return false;
    }

    std::size_t offset = 0;
    RouteAnnounce decoded{};
    decoded.version = ReadU16(bytes, offset);
    if (decoded.version != kSnapshotCommandVersion) {
        SetError(error, "unsupported snapshot command version");
        return false;
    }
    if (ReadU16(bytes, offset) != 0U) {
        SetError(error, "reserved field must be zero");
        return false;
    }
    const uint32_t length = ReadU32(bytes, offset);
    if (length != 0U && length != 64U) {
        SetError(error, "device_key_hash length must be 0 or 64");
        return false;
    }
    if (length != 0U) {
        decoded.device_key_hash.assign(
            reinterpret_cast<const char *>(bytes.data() + offset), length);
        if (!IsLowerHex(decoded.device_key_hash)) {
            SetError(error, "device_key_hash must be 64 lowercase hex characters");
            return false;
        }
    }

    *announce = std::move(decoded);
    if (error != nullptr) error->clear();
    return true;
}

} // namespace viper::daemon
