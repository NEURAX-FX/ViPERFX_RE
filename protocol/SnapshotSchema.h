#pragma once

#include "DeviceKey.h"
#include "ViperDaemonProtocol.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace viper::daemon {

constexpr uint16_t kSnapshotSchemaVersion = 1;
constexpr uint16_t kSnapshotDriverProtocolVersion = kProtocolVersion;
constexpr std::size_t kSnapshotHeaderSize = 80;
constexpr std::size_t kMaxSnapshotSize = 4U * 1024U * 1024U;
constexpr std::size_t kMaxParameterBytes = 1024U * 1024U;
constexpr std::size_t kMaxSnapshotString = 4096U;
constexpr std::size_t kMaxSnapshotResources = 128U;

struct ResourceReference {
    std::string resource_id;
    std::string content_sha256;
    uint64_t size = 0;
    uint32_t kind = 0;
    uint32_t format = 0;
    uint32_t channels = 0;
    uint32_t order = 0;
};

struct Snapshot {
    uint16_t schema_version = kSnapshotSchemaVersion;
    uint16_t driver_protocol_version = kSnapshotDriverProtocolVersion;
    std::string device_key;
    std::string device_key_hash;
    uint64_t boot_id = 0;
    uint64_t daemon_generation = 0;
    uint64_t app_generation = 0;
    uint64_t created_at_millis = 0;
    bool master_enabled = false;
    bool global_mode = false;
    std::vector<uint8_t> parameters;
    std::vector<uint8_t> iem_parameters;
    std::vector<ResourceReference> resources;
    uint64_t resource_generation = 0;
    uint64_t graph_generation = 0;
};

bool ValidateSnapshot(const Snapshot &snapshot, std::string *error);
bool EncodeSnapshot(const Snapshot &snapshot, std::vector<uint8_t> *out, std::string *error);
bool DecodeSnapshot(std::span<const uint8_t> bytes, Snapshot *snapshot, std::string *error);

} // namespace viper::daemon
