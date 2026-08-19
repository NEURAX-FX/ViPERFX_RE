#include "ViperDaemonProtocol.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace viper::daemon {
namespace {

constexpr std::array<uint8_t, 4> kMagic{'V', '4', 'A', 'D'};

void SetError(std::string *error, FrameError code) {
    if (error != nullptr) error->assign(FrameErrorMessage(code));
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

} // namespace

const char *FrameErrorMessage(FrameError error) noexcept {
    switch (error) {
        case FrameError::NONE: return "";
        case FrameError::NULL_OUTPUT: return "null output";
        case FrameError::PAYLOAD_TOO_LARGE: return "payload too large";
        case FrameError::FRAME_TOO_SMALL: return "frame too small";
        case FrameError::BAD_MAGIC: return "bad magic";
        case FrameError::UNSUPPORTED_VERSION: return "unsupported protocol version";
        case FrameError::UNKNOWN_FLAGS: return "unknown frame flags";
        case FrameError::LENGTH_MISMATCH: return "frame length mismatch";
        case FrameError::TRAILING_BYTES: return "trailing bytes";
        case FrameError::CRC_MISMATCH: return "payload crc mismatch";
    }
    return "unknown frame error";
}

uint32_t Crc32(std::span<const uint8_t> bytes) noexcept {
    uint32_t crc = 0xFFFFFFFFU;
    for (const uint8_t byte : bytes) {
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit) {
            const uint32_t mask = -(crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

bool EncodeFrame(
    const FrameHeader &header,
    std::string_view payload,
    std::vector<uint8_t> *out,
    std::string *error
) {
    if (out == nullptr) {
        SetError(error, FrameError::NULL_OUTPUT);
        return false;
    }
    if (payload.size() > kMaxPayloadSize) {
        SetError(error, FrameError::PAYLOAD_TOO_LARGE);
        return false;
    }
    if (header.protocol_version != kProtocolVersion) {
        SetError(error, FrameError::UNSUPPORTED_VERSION);
        return false;
    }
    if ((header.flags & ~kKnownFrameFlags) != 0U) {
        SetError(error, FrameError::UNKNOWN_FLAGS);
        return false;
    }

    const auto payload_bytes = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t *>(payload.data()), payload.size());
    out->clear();
    out->reserve(kFrameHeaderSize + payload.size());
    out->insert(out->end(), kMagic.begin(), kMagic.end());
    PutU16(*out, header.protocol_version);
    PutU16(*out, header.message_type);
    PutU32(*out, header.flags);
    PutU64(*out, header.request_id);
    PutU64(*out, header.sequence);
    PutU32(*out, static_cast<uint32_t>(payload.size()));
    PutU32(*out, Crc32(payload_bytes));
    out->insert(out->end(), payload_bytes.begin(), payload_bytes.end());
    return true;
}

bool DecodeFrame(
    std::span<const uint8_t> bytes,
    FrameHeader *header,
    std::vector<uint8_t> *payload,
    std::string *error
) {
    if (header == nullptr || payload == nullptr) {
        SetError(error, FrameError::NULL_OUTPUT);
        return false;
    }
    if (bytes.size() < kFrameHeaderSize) {
        SetError(error, FrameError::FRAME_TOO_SMALL);
        return false;
    }
    if (!std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
        SetError(error, FrameError::BAD_MAGIC);
        return false;
    }

    FrameHeader decoded{};
    decoded.protocol_version = ReadU16(bytes, 4);
    decoded.message_type = ReadU16(bytes, 6);
    decoded.flags = ReadU32(bytes, 8);
    decoded.request_id = ReadU64(bytes, 12);
    decoded.sequence = ReadU64(bytes, 20);
    decoded.payload_length = ReadU32(bytes, 28);
    decoded.payload_crc32 = ReadU32(bytes, 32);

    if (decoded.protocol_version != kProtocolVersion) {
        SetError(error, FrameError::UNSUPPORTED_VERSION);
        return false;
    }
    if ((decoded.flags & ~kKnownFrameFlags) != 0U) {
        SetError(error, FrameError::UNKNOWN_FLAGS);
        return false;
    }
    if (decoded.payload_length > kMaxPayloadSize) {
        SetError(error, FrameError::PAYLOAD_TOO_LARGE);
        return false;
    }

    const std::size_t expected_size = kFrameHeaderSize + decoded.payload_length;
    if (bytes.size() < expected_size) {
        SetError(error, FrameError::LENGTH_MISMATCH);
        return false;
    }
    if (bytes.size() > expected_size) {
        SetError(error, FrameError::TRAILING_BYTES);
        return false;
    }

    const auto payload_bytes = bytes.subspan(kFrameHeaderSize, decoded.payload_length);
    if (Crc32(payload_bytes) != decoded.payload_crc32) {
        SetError(error, FrameError::CRC_MISMATCH);
        return false;
    }

    *header = decoded;
    payload->assign(payload_bytes.begin(), payload_bytes.end());
    if (error != nullptr) error->clear();
    return true;
}

} // namespace viper::daemon
