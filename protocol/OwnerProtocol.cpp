#include "OwnerProtocol.h"

#include <algorithm>

namespace viper::owner {
namespace {

void SetError(std::string *error, const char *message) {
    if (error != nullptr) error->assign(message);
}

void ClearError(std::string *error) {
    if (error != nullptr) error->clear();
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

uint16_t ReadU16(std::span<const uint8_t> bytes, std::size_t offset) noexcept {
    return static_cast<uint16_t>(bytes[offset])
        | static_cast<uint16_t>(bytes[offset + 1U]) << 8U;
}

uint32_t ReadU32(std::span<const uint8_t> bytes, std::size_t offset) noexcept {
    uint32_t value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        value |= static_cast<uint32_t>(bytes[offset + shift / 8U]) << shift;
    }
    return value;
}

uint64_t ReadU64(std::span<const uint8_t> bytes, std::size_t offset) noexcept {
    uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        value |= static_cast<uint64_t>(bytes[offset + shift / 8U]) << shift;
    }
    return value;
}

bool BeginEncode(
    std::vector<uint8_t> *out,
    std::size_t size,
    std::string *error
) {
    if (out == nullptr) {
        SetError(error, "null output");
        return false;
    }
    out->clear();
    out->reserve(size);
    return true;
}

template <typename T>
bool CheckDecode(
    std::span<const uint8_t> bytes,
    std::size_t expected,
    T *out,
    std::string *error
) {
    if (out == nullptr) {
        SetError(error, "null output");
        return false;
    }
    if (bytes.size() != expected) {
        SetError(error, "owner payload size mismatch");
        return false;
    }
    return true;
}

bool CheckReserved(std::span<const uint8_t> bytes, std::size_t begin, std::size_t end) {
    return std::all_of(bytes.begin() + static_cast<std::ptrdiff_t>(begin),
        bytes.begin() + static_cast<std::ptrdiff_t>(end), [](uint8_t value) { return value == 0; });
}

} // namespace

bool IsOwnerMessage(uint16_t value) noexcept {
    return value >= static_cast<uint16_t>(OwnerMessage::OWNER_HELLO)
        && value <= static_cast<uint16_t>(OwnerMessage::SESSION_DELTA);
}

bool IsAllowedEffectSelector(uint16_t value) noexcept {
    return value == static_cast<uint16_t>(EffectTypeSelector::HIDL)
        || value == static_cast<uint16_t>(EffectTypeSelector::AIDL);
}

bool EncodeOwnerHello(const OwnerHello &value, std::vector<uint8_t> *out, std::string *error) {
    if (value.owner_pid == 0 || value.boot_id == 0) {
        SetError(error, "owner hello identity must be non-zero");
        return false;
    }
    if (!BeginEncode(out, kOwnerHelloWireSize, error)) return false;
    PutU16(*out, kOwnerProtocolVersion);
    PutU16(*out, 0);
    PutU64(*out, value.owner_pid);
    PutU64(*out, value.boot_id);
    ClearError(error);
    return out->size() == kOwnerHelloWireSize;
}

bool DecodeOwnerHello(std::span<const uint8_t> bytes, OwnerHello *out, std::string *error) {
    if (!CheckDecode(bytes, kOwnerHelloWireSize, out, error)) return false;
    if (ReadU16(bytes, 0) != kOwnerProtocolVersion || ReadU16(bytes, 2) != 0
        || !CheckReserved(bytes, 4, 4)) {
        SetError(error, "invalid owner hello header");
        return false;
    }
    out->owner_pid = ReadU64(bytes, 4);
    out->boot_id = ReadU64(bytes, 12);
    if (out->owner_pid == 0 || out->boot_id == 0) {
        SetError(error, "owner hello identity must be non-zero");
        return false;
    }
    ClearError(error);
    return true;
}

bool EncodeOwnerHelloAck(const OwnerHelloAck &value, std::vector<uint8_t> *out, std::string *error) {
    if (value.daemon_generation == 0) {
        SetError(error, "daemon generation must be non-zero");
        return false;
    }
    if (!BeginEncode(out, kOwnerHelloAckWireSize, error)) return false;
    PutU16(*out, kOwnerProtocolVersion);
    PutU16(*out, value.accepted ? 1 : 0);
    PutU64(*out, value.daemon_generation);
    PutU32(*out, 0);
    ClearError(error);
    return out->size() == kOwnerHelloAckWireSize;
}

bool DecodeOwnerHelloAck(std::span<const uint8_t> bytes, OwnerHelloAck *out, std::string *error) {
    if (!CheckDecode(bytes, kOwnerHelloAckWireSize, out, error)) return false;
    const uint16_t accepted = ReadU16(bytes, 2);
    if (ReadU16(bytes, 0) != kOwnerProtocolVersion || accepted > 1
        || ReadU32(bytes, 12) != 0) {
        SetError(error, "invalid owner hello ack");
        return false;
    }
    out->accepted = accepted != 0;
    out->daemon_generation = ReadU64(bytes, 4);
    if (out->daemon_generation == 0) {
        SetError(error, "daemon generation must be non-zero");
        return false;
    }
    ClearError(error);
    return true;
}

bool EncodeOwnSession(const OwnSession &value, std::vector<uint8_t> *out, std::string *error) {
    if (!IsAllowedEffectSelector(static_cast<uint16_t>(value.selector))) {
        SetError(error, "unsupported effect type selector");
        return false;
    }
    if (!BeginEncode(out, kOwnSessionWireSize, error)) return false;
    PutU16(*out, kOwnerProtocolVersion);
    PutU16(*out, static_cast<uint16_t>(value.selector));
    PutU32(*out, value.audio_session_id);
    PutU32(*out, 0);
    PutU32(*out, 0);
    ClearError(error);
    return out->size() == kOwnSessionWireSize;
}

bool DecodeOwnSession(std::span<const uint8_t> bytes, OwnSession *out, std::string *error) {
    if (!CheckDecode(bytes, kOwnSessionWireSize, out, error)) return false;
    const uint16_t selector = ReadU16(bytes, 2);
    if (ReadU16(bytes, 0) != kOwnerProtocolVersion
        || !IsAllowedEffectSelector(selector)
        || ReadU32(bytes, 8) != 0 || ReadU32(bytes, 12) != 0) {
        SetError(error, "invalid own session payload");
        return false;
    }
    out->selector = static_cast<EffectTypeSelector>(selector);
    out->audio_session_id = ReadU32(bytes, 4);
    if (out->audio_session_id != 0) {
        SetError(error, "only session zero ownership is supported");
        return false;
    }
    ClearError(error);
    return true;
}

bool EncodeOwned(const Owned &value, std::vector<uint8_t> *out, std::string *error) {
    if (value.effect_id == 0) {
        SetError(error, "effect id must be non-zero");
        return false;
    }
    if (!BeginEncode(out, kOwnedWireSize, error)) return false;
    PutU16(*out, kOwnerProtocolVersion);
    PutU16(*out, value.has_control ? 1 : 0);
    PutU32(*out, value.audio_session_id);
    PutU32(*out, value.effect_id);
    PutU32(*out, 0);
    ClearError(error);
    return out->size() == kOwnedWireSize;
}

bool DecodeOwned(std::span<const uint8_t> bytes, Owned *out, std::string *error) {
    if (!CheckDecode(bytes, kOwnedWireSize, out, error)) return false;
    const uint16_t control = ReadU16(bytes, 2);
    if (ReadU16(bytes, 0) != kOwnerProtocolVersion || control > 1
        || ReadU32(bytes, 12) != 0) {
        SetError(error, "invalid owned payload");
        return false;
    }
    out->has_control = control != 0;
    out->audio_session_id = ReadU32(bytes, 4);
    out->effect_id = ReadU32(bytes, 8);
    if (out->audio_session_id != 0 || out->effect_id == 0) {
        SetError(error, "invalid owned session or effect id");
        return false;
    }
    ClearError(error);
    return true;
}

bool EncodeOwnerFailed(const OwnerFailed &value, std::vector<uint8_t> *out, std::string *error) {
    if (value.reason_code == 0) {
        SetError(error, "owner failure reason must be non-zero");
        return false;
    }
    if (!BeginEncode(out, kOwnerFailedWireSize, error)) return false;
    PutU16(*out, kOwnerProtocolVersion);
    PutU16(*out, 0);
    PutU32(*out, value.audio_session_id);
    PutU32(*out, value.reason_code);
    PutU32(*out, 0);
    ClearError(error);
    return out->size() == kOwnerFailedWireSize;
}

bool DecodeOwnerFailed(std::span<const uint8_t> bytes, OwnerFailed *out, std::string *error) {
    if (!CheckDecode(bytes, kOwnerFailedWireSize, out, error)) return false;
    if (ReadU16(bytes, 0) != kOwnerProtocolVersion || ReadU16(bytes, 2) != 0
        || ReadU32(bytes, 12) != 0) {
        SetError(error, "invalid owner failure payload");
        return false;
    }
    out->audio_session_id = ReadU32(bytes, 4);
    out->reason_code = ReadU32(bytes, 8);
    if (out->reason_code == 0) {
        SetError(error, "owner failure reason must be non-zero");
        return false;
    }
    ClearError(error);
    return true;
}

bool EncodeReleaseSession(const ReleaseSession &value, std::vector<uint8_t> *out, std::string *error) {
    if (value.audio_session_id != 0) {
        SetError(error, "only session zero release is supported");
        return false;
    }
    if (!BeginEncode(out, kReleaseSessionWireSize, error)) return false;
    PutU16(*out, kOwnerProtocolVersion);
    PutU16(*out, 0);
    PutU32(*out, value.audio_session_id);
    PutU32(*out, 0);
    ClearError(error);
    return out->size() == kReleaseSessionWireSize;
}

bool DecodeReleaseSession(std::span<const uint8_t> bytes, ReleaseSession *out, std::string *error) {
    if (!CheckDecode(bytes, kReleaseSessionWireSize, out, error)) return false;
    if (ReadU16(bytes, 0) != kOwnerProtocolVersion || ReadU16(bytes, 2) != 0
        || ReadU32(bytes, 8) != 0 || ReadU32(bytes, 4) != 0) {
        SetError(error, "invalid release session payload");
        return false;
    }
    out->audio_session_id = 0;
    ClearError(error);
    return true;
}

bool EncodeReleased(const Released &value, std::vector<uint8_t> *out, std::string *error) {
    ReleaseSession release{value.audio_session_id};
    return EncodeReleaseSession(release, out, error);
}

bool DecodeReleased(std::span<const uint8_t> bytes, Released *out, std::string *error) {
    if (out == nullptr) {
        SetError(error, "null output");
        return false;
    }
    ReleaseSession decoded{};
    if (!DecodeReleaseSession(bytes, &decoded, error)) return false;
    out->audio_session_id = decoded.audio_session_id;
    return true;
}

bool EncodeSessionDelta(const SessionDelta &value, std::vector<uint8_t> *out, std::string *error) {
    if (value.audio_session_id == 0) {
        SetError(error, "session delta requires a non-zero session id");
        return false;
    }
    if (!BeginEncode(out, kSessionDeltaWireSize, error)) return false;
    PutU16(*out, kOwnerProtocolVersion);
    PutU16(*out, value.appeared ? 1 : 0);
    PutU32(*out, value.audio_session_id);
    PutU32(*out, value.client_uid);
    PutU32(*out, 0);
    ClearError(error);
    return out->size() == kSessionDeltaWireSize;
}

bool DecodeSessionDelta(std::span<const uint8_t> bytes, SessionDelta *out, std::string *error) {
    if (!CheckDecode(bytes, kSessionDeltaWireSize, out, error)) return false;
    const uint16_t appeared = ReadU16(bytes, 2);
    if (ReadU16(bytes, 0) != kOwnerProtocolVersion || appeared > 1
        || ReadU32(bytes, 12) != 0) {
        SetError(error, "invalid session delta payload");
        return false;
    }
    out->appeared = appeared != 0;
    out->audio_session_id = ReadU32(bytes, 4);
    out->client_uid = ReadU32(bytes, 8);
    if (out->audio_session_id == 0) {
        SetError(error, "session delta requires a non-zero session id");
        return false;
    }
    ClearError(error);
    return true;
}

} // namespace viper::owner
