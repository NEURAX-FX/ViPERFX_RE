#include "SnapshotSchema.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <set>
#include <utility>

namespace viper::daemon {
namespace {

constexpr std::array<uint8_t, 4> kMagic{'V', '4', 'A', 'S'};
constexpr std::size_t kResourceFixedSize = 32;

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

uint16_t ReadU16(std::span<const uint8_t> bytes, std::size_t &offset) {
    const uint16_t value = static_cast<uint16_t>(bytes[offset])
        | static_cast<uint16_t>(bytes[offset + 1U]) << 8U;
    offset += 2U;
    return value;
}

uint32_t ReadU32(std::span<const uint8_t> bytes, std::size_t &offset) {
    uint32_t value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        value |= static_cast<uint32_t>(bytes[offset + shift / 8U]) << shift;
    }
    offset += 4U;
    return value;
}

uint64_t ReadU64(std::span<const uint8_t> bytes, std::size_t &offset) {
    uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        value |= static_cast<uint64_t>(bytes[offset + shift / 8U]) << shift;
    }
    offset += 8U;
    return value;
}

bool ReadBytes(
    std::span<const uint8_t> bytes,
    std::size_t &offset,
    std::size_t length,
    std::vector<uint8_t> *out
) {
    if (length > bytes.size() - offset) return false;
    out->assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
        bytes.begin() + static_cast<std::ptrdiff_t>(offset + length));
    offset += length;
    return true;
}

bool ReadString(
    std::span<const uint8_t> bytes,
    std::size_t &offset,
    std::string *out
) {
    if (bytes.size() - offset < 4U) return false;
    const uint32_t length = ReadU32(bytes, offset);
    if (length > kMaxSnapshotString || length > bytes.size() - offset) return false;
    out->assign(reinterpret_cast<const char *>(bytes.data() + offset), length);
    offset += length;
    return true;
}

bool IsHex64(std::string_view value) noexcept {
    if (value.size() != 64U) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return std::isxdigit(byte) != 0;
    });
}

bool IsLowerHex64(std::string_view value) noexcept {
    if (!IsHex64(value)) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return std::isdigit(byte) != 0 || std::islower(byte) != 0;
    });
}

} // namespace

bool ValidateSnapshot(const Snapshot &snapshot, std::string *error) {
    if (snapshot.schema_version != kSnapshotSchemaVersion) {
        SetError(error, "unsupported snapshot schema");
        return false;
    }
    if (snapshot.driver_protocol_version != kSnapshotDriverProtocolVersion) {
        SetError(error, "unsupported driver protocol version");
        return false;
    }
    if (snapshot.device_key.empty() || snapshot.device_key.size() > kMaxSnapshotString) {
        SetError(error, "device_key is invalid");
        return false;
    }
    if (snapshot.device_key.find('|') == std::string::npos) {
        SetError(error, "device_key is not canonical");
        return false;
    }
    if (!IsLowerHex64(snapshot.device_key_hash)) {
        SetError(error, "device_key_hash must be 64 lowercase hex characters");
        return false;
    }
    if (HashDeviceKey(snapshot.device_key) != snapshot.device_key_hash) {
        SetError(error, "device_key_hash mismatch");
        return false;
    }
    if (snapshot.boot_id == 0U) {
        SetError(error, "boot_id must be non-zero");
        return false;
    }
    if (snapshot.daemon_generation == 0U) {
        SetError(error, "daemon_generation must be non-zero");
        return false;
    }
    if (snapshot.app_generation == 0U) {
        SetError(error, "app_generation must be non-zero");
        return false;
    }
    if (snapshot.created_at_millis == 0U) {
        SetError(error, "created_at_millis must be non-zero");
        return false;
    }
    if (snapshot.parameters.size() > kMaxParameterBytes
        || snapshot.iem_parameters.size() > kMaxParameterBytes) {
        SetError(error, "parameter payload is too large");
        return false;
    }
    if (snapshot.resources.size() > kMaxSnapshotResources) {
        SetError(error, "too many resources");
        return false;
    }

    std::set<std::string> resource_ids;
    std::set<std::string> resource_hashes;
    for (const auto &resource : snapshot.resources) {
        if (resource.resource_id.empty() || resource.resource_id.size() > kMaxSnapshotString) {
            SetError(error, "resource_id is invalid");
            return false;
        }
        if (!IsLowerHex64(resource.content_sha256)) {
            SetError(error, "resource content_sha256 must be 64 hex characters");
            return false;
        }
        if (!resource_ids.insert(resource.resource_id).second) {
            SetError(error, "duplicate resource_id");
            return false;
        }
        if (!resource_hashes.insert(resource.content_sha256).second) {
            SetError(error, "duplicate resource hash");
            return false;
        }
    }
    if (error != nullptr) error->clear();
    return true;
}

bool EncodeSnapshot(const Snapshot &snapshot, std::vector<uint8_t> *out, std::string *error) {
    if (out == nullptr) {
        SetError(error, "null output");
        return false;
    }
    if (!ValidateSnapshot(snapshot, error)) return false;

    const std::size_t estimated_size = kSnapshotHeaderSize
        + snapshot.device_key.size() + snapshot.device_key_hash.size()
        + snapshot.parameters.size() + snapshot.iem_parameters.size();
    out->clear();
    out->reserve(estimated_size);
    out->insert(out->end(), kMagic.begin(), kMagic.end());
    PutU16(*out, snapshot.schema_version);
    PutU16(*out, snapshot.driver_protocol_version);
    PutU64(*out, snapshot.boot_id);
    PutU64(*out, snapshot.daemon_generation);
    PutU64(*out, snapshot.app_generation);
    PutU64(*out, snapshot.created_at_millis);
    out->push_back(snapshot.master_enabled ? 1U : 0U);
    out->push_back(snapshot.global_mode ? 1U : 0U);
    PutU16(*out, 0U);
    PutU32(*out, static_cast<uint32_t>(snapshot.device_key.size()));
    PutU32(*out, static_cast<uint32_t>(snapshot.device_key_hash.size()));
    PutU32(*out, static_cast<uint32_t>(snapshot.parameters.size()));
    PutU32(*out, static_cast<uint32_t>(snapshot.iem_parameters.size()));
    PutU32(*out, static_cast<uint32_t>(snapshot.resources.size()));
    PutU64(*out, snapshot.resource_generation);
    PutU64(*out, snapshot.graph_generation);
    out->insert(out->end(), snapshot.device_key.begin(), snapshot.device_key.end());
    out->insert(out->end(), snapshot.device_key_hash.begin(), snapshot.device_key_hash.end());
    out->insert(out->end(), snapshot.parameters.begin(), snapshot.parameters.end());
    out->insert(out->end(), snapshot.iem_parameters.begin(), snapshot.iem_parameters.end());

    for (const auto &resource : snapshot.resources) {
        PutU32(*out, static_cast<uint32_t>(resource.resource_id.size()));
        PutU32(*out, static_cast<uint32_t>(resource.content_sha256.size()));
        PutU64(*out, resource.size);
        PutU32(*out, resource.kind);
        PutU32(*out, resource.format);
        PutU32(*out, resource.channels);
        PutU32(*out, resource.order);
        out->insert(out->end(), resource.resource_id.begin(), resource.resource_id.end());
        out->insert(out->end(), resource.content_sha256.begin(), resource.content_sha256.end());
    }
    if (out->size() > kMaxSnapshotSize) {
        SetError(error, "snapshot is too large");
        out->clear();
        return false;
    }
    return true;
}

bool DecodeSnapshot(std::span<const uint8_t> bytes, Snapshot *snapshot, std::string *error) {
    if (snapshot == nullptr) {
        SetError(error, "null output");
        return false;
    }
    if (bytes.size() < kSnapshotHeaderSize) {
        SetError(error, "snapshot truncated");
        return false;
    }
    if (bytes.size() > kMaxSnapshotSize
        || !std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
        SetError(error, bytes.size() > kMaxSnapshotSize ? "snapshot is too large" : "bad snapshot magic");
        return false;
    }

    Snapshot decoded{};
    std::size_t offset = 4U;
    decoded.schema_version = ReadU16(bytes, offset);
    decoded.driver_protocol_version = ReadU16(bytes, offset);
    decoded.boot_id = ReadU64(bytes, offset);
    decoded.daemon_generation = ReadU64(bytes, offset);
    decoded.app_generation = ReadU64(bytes, offset);
    decoded.created_at_millis = ReadU64(bytes, offset);
    decoded.master_enabled = bytes[offset++] != 0U;
    decoded.global_mode = bytes[offset++] != 0U;
    static_cast<void>(ReadU16(bytes, offset));
    const uint32_t device_key_length = ReadU32(bytes, offset);
    const uint32_t device_hash_length = ReadU32(bytes, offset);
    const uint32_t parameter_length = ReadU32(bytes, offset);
    const uint32_t iem_parameter_length = ReadU32(bytes, offset);
    const uint32_t resource_count = ReadU32(bytes, offset);
    decoded.resource_generation = ReadU64(bytes, offset);
    decoded.graph_generation = ReadU64(bytes, offset);

    if (device_key_length > kMaxSnapshotString || device_hash_length > kMaxSnapshotString
        || parameter_length > kMaxParameterBytes || iem_parameter_length > kMaxParameterBytes
        || resource_count > kMaxSnapshotResources) {
        SetError(error, "snapshot field exceeds limit");
        return false;
    }
    if (bytes.size() - offset < device_key_length + device_hash_length
        + parameter_length + iem_parameter_length) {
        SetError(error, "snapshot truncated");
        return false;
    }
    decoded.device_key.assign(reinterpret_cast<const char *>(bytes.data() + offset), device_key_length);
    offset += device_key_length;
    decoded.device_key_hash.assign(reinterpret_cast<const char *>(bytes.data() + offset), device_hash_length);
    offset += device_hash_length;
    if (!ReadBytes(bytes, offset, parameter_length, &decoded.parameters)
        || !ReadBytes(bytes, offset, iem_parameter_length, &decoded.iem_parameters)) {
        SetError(error, "snapshot truncated");
        return false;
    }

    decoded.resources.reserve(resource_count);
    for (uint32_t index = 0; index < resource_count; ++index) {
        if (bytes.size() - offset < kResourceFixedSize) {
            SetError(error, "snapshot resource truncated");
            return false;
        }
        const uint32_t resource_id_length = ReadU32(bytes, offset);
        const uint32_t hash_length = ReadU32(bytes, offset);
        ResourceReference resource{};
        resource.size = ReadU64(bytes, offset);
        resource.kind = ReadU32(bytes, offset);
        resource.format = ReadU32(bytes, offset);
        resource.channels = ReadU32(bytes, offset);
        resource.order = ReadU32(bytes, offset);
        if (resource_id_length > kMaxSnapshotString || hash_length > kMaxSnapshotString
            || bytes.size() - offset < resource_id_length + hash_length) {
            SetError(error, "snapshot resource truncated");
            return false;
        }
        resource.resource_id.assign(reinterpret_cast<const char *>(bytes.data() + offset), resource_id_length);
        offset += resource_id_length;
        resource.content_sha256.assign(reinterpret_cast<const char *>(bytes.data() + offset), hash_length);
        offset += hash_length;
        decoded.resources.push_back(std::move(resource));
    }

    if (offset != bytes.size()) {
        SetError(error, "snapshot trailing bytes");
        return false;
    }
    if (!ValidateSnapshot(decoded, error)) return false;
    *snapshot = std::move(decoded);
    return true;
}

} // namespace viper::daemon
