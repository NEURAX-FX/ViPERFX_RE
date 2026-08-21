#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace viper::daemon {

constexpr std::size_t kDriverEventWireSize = 72;
constexpr const char *kDriverSocketName = "viper4android.driver.v1";

enum class DriverEventType : uint16_t {
    DRIVER_HELLO = 1,
    CONTEXT_CREATED = 2,
    CONTEXT_CONFIGURED = 3,
    CONTEXT_ENABLED = 4,
    CONTEXT_DISABLED = 5,
    CONTEXT_RELEASED = 6,
    RESOURCE_GENERATION_CHANGED = 7,
    TELEMETRY = 8,
    RESCAN_RESPONSE = 9,
    SNAPSHOT_APPLIED_ACK = 10,
    SNAPSHOT_APPLIED_NACK = 11,
};

bool IsKnownDriverEventType(uint16_t value) noexcept;

struct DriverEvent {
    DriverEventType type = DriverEventType::DRIVER_HELLO;
    uint64_t boot_id = 0;
    uint64_t event_sequence = 0;
    uint64_t context_instance_id = 0;
    uint32_t audio_session_id = 0;
    uint32_t io_id = 0;
    uint32_t sample_rate = 0;
    uint32_t channel_mask = 0;
    bool enabled = false;
    uint64_t session_generation = 0;
    uint64_t resource_generation = 0;
    uint64_t graph_generation = 0;
    uint32_t bypass_reason = 0;
};

bool EncodeDriverEvent(
    const DriverEvent &event,
    std::vector<uint8_t> *out,
    std::string *error
);

bool DecodeDriverEvent(
    std::span<const uint8_t> bytes,
    DriverEvent *event,
    std::string *error
);

} // namespace viper::daemon
