#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace viper::daemon {

constexpr uint16_t kProtocolVersion = 1;
constexpr std::size_t kFrameHeaderSize = 36;
constexpr std::size_t kMaxFrameSize = 1024U * 1024U;
constexpr std::size_t kMaxPayloadSize = kMaxFrameSize - kFrameHeaderSize;
constexpr uint32_t kKnownFrameFlags = 0x0000FFFFU;

enum class FrameError {
    NONE,
    NULL_OUTPUT,
    PAYLOAD_TOO_LARGE,
    FRAME_TOO_SMALL,
    BAD_MAGIC,
    UNSUPPORTED_VERSION,
    UNKNOWN_FLAGS,
    LENGTH_MISMATCH,
    TRAILING_BYTES,
    CRC_MISMATCH,
};

const char *FrameErrorMessage(FrameError error) noexcept;

struct FrameHeader {
    uint16_t protocol_version = kProtocolVersion;
    uint16_t message_type = 0;
    uint32_t flags = 0;
    uint64_t request_id = 0;
    uint64_t sequence = 0;
    uint32_t payload_length = 0;
    uint32_t payload_crc32 = 0;
};

uint32_t Crc32(std::span<const uint8_t> bytes) noexcept;

bool EncodeFrame(
    const FrameHeader &header,
    std::string_view payload,
    std::vector<uint8_t> *out,
    std::string *error
);

bool DecodeFrame(
    std::span<const uint8_t> bytes,
    FrameHeader *header,
    std::vector<uint8_t> *payload,
    std::string *error
);

} // namespace viper::daemon
