#include "DriverEvent.h"

namespace viper::daemon {
namespace {

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

} // namespace

bool IsKnownDriverEventType(uint16_t value) noexcept {
    return value >= static_cast<uint16_t>(DriverEventType::DRIVER_HELLO)
        && value <= static_cast<uint16_t>(DriverEventType::SNAPSHOT_APPLIED_NACK);
}

bool EncodeDriverEvent(
    const DriverEvent &event,
    std::vector<uint8_t> *out,
    std::string *error
) {
    if (out == nullptr) {
        SetError(error, "null output");
        return false;
    }
    if (!IsKnownDriverEventType(static_cast<uint16_t>(event.type))) {
        SetError(error, "unknown driver event type");
        return false;
    }

    out->clear();
    out->reserve(kDriverEventWireSize);
    PutU16(*out, static_cast<uint16_t>(event.type));
    PutU16(*out, event.enabled ? 1U : 0U);
    PutU64(*out, event.boot_id);
    PutU64(*out, event.event_sequence);
    PutU64(*out, event.context_instance_id);
    PutU32(*out, event.audio_session_id);
    PutU32(*out, event.io_id);
    PutU32(*out, event.sample_rate);
    PutU32(*out, event.channel_mask);
    PutU64(*out, event.session_generation);
    PutU64(*out, event.resource_generation);
    PutU64(*out, event.graph_generation);
    PutU32(*out, event.bypass_reason);
    return out->size() == kDriverEventWireSize;
}

bool DecodeDriverEvent(
    std::span<const uint8_t> bytes,
    DriverEvent *event,
    std::string *error
) {
    if (event == nullptr) {
        SetError(error, "null output");
        return false;
    }
    if (bytes.size() != kDriverEventWireSize) {
        SetError(error, "driver event size mismatch");
        return false;
    }

    std::size_t offset = 0;
    const uint16_t raw_type = ReadU16(bytes, offset);
    if (!IsKnownDriverEventType(raw_type)) {
        SetError(error, "unknown driver event type");
        return false;
    }

    DriverEvent decoded{};
    decoded.type = static_cast<DriverEventType>(raw_type);
    decoded.enabled = ReadU16(bytes, offset) != 0U;
    decoded.boot_id = ReadU64(bytes, offset);
    decoded.event_sequence = ReadU64(bytes, offset);
    decoded.context_instance_id = ReadU64(bytes, offset);
    decoded.audio_session_id = ReadU32(bytes, offset);
    decoded.io_id = ReadU32(bytes, offset);
    decoded.sample_rate = ReadU32(bytes, offset);
    decoded.channel_mask = ReadU32(bytes, offset);
    decoded.session_generation = ReadU64(bytes, offset);
    decoded.resource_generation = ReadU64(bytes, offset);
    decoded.graph_generation = ReadU64(bytes, offset);
    decoded.bypass_reason = ReadU32(bytes, offset);
    *event = decoded;
    if (error != nullptr) error->clear();
    return true;
}

} // namespace viper::daemon
