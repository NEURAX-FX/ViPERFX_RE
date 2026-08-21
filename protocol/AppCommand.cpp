#include "AppCommand.h"

#include <string_view>

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

bool IsLowerHexHash(std::string_view value) noexcept {
    if (value.size() != kAppDeviceHashSize) return false;
    for (const unsigned char byte : value) {
        if ((byte < '0' || byte > '9') && (byte < 'a' || byte > 'f')) return false;
    }
    return true;
}

// A hash field is fixed width on the wire, so an absent hash is all zero bytes
// rather than a length prefix. Distinguishing "no route yet" from a real hash
// matters: the App must not cache an empty key as if it were valid.
void PutHash(std::vector<uint8_t> &out, const std::string &hash) {
    for (std::size_t index = 0; index < kAppDeviceHashSize; ++index) {
        out.push_back(index < hash.size() ? static_cast<uint8_t>(hash[index]) : 0U);
    }
}

bool ReadHash(std::span<const uint8_t> bytes, std::size_t &offset, std::string *hash) {
    const auto begin = bytes.begin() + static_cast<std::ptrdiff_t>(offset);
    std::string decoded(reinterpret_cast<const char *>(&*begin), kAppDeviceHashSize);
    offset += kAppDeviceHashSize;
    const std::size_t terminator = decoded.find('\0');
    if (terminator != std::string::npos) decoded.resize(terminator);
    if (!decoded.empty() && !IsLowerHexHash(decoded)) return false;
    *hash = std::move(decoded);
    return true;
}

bool ValidateRouteField(const std::string &value, std::string *error) {
    if (value.empty()) {
        SetError(error, "route field must not be empty");
        return false;
    }
    if (value.size() > kMaxAppRouteFieldBytes) {
        SetError(error, "route field is too large");
        return false;
    }
    // '|' is the device-key delimiter; a field containing it could forge a key.
    if (value.find('|') != std::string::npos) {
        SetError(error, "route field contains the key delimiter");
        return false;
    }
    for (const unsigned char byte : value) {
        if (byte < 0x20U || byte == 0x7FU) {
            SetError(error, "route field contains a control character");
            return false;
        }
    }
    return true;
}

} // namespace

bool IsAppMessageType(uint16_t value) noexcept {
    return value >= static_cast<uint16_t>(AppMessageType::APP_HELLO)
        && value <= static_cast<uint16_t>(AppMessageType::APP_APPLY_RESULT);
}

bool EncodeAppHello(const AppHello &hello, std::vector<uint8_t> *out, std::string *error) {
    if (out == nullptr) {
        SetError(error, "null output");
        return false;
    }
    if (hello.version != kAppProtocolVersion) {
        SetError(error, "unsupported app protocol version");
        return false;
    }
    out->clear();
    out->reserve(kAppHelloWireSize);
    PutU16(*out, hello.version);
    PutU16(*out, 0U);
    PutU64(*out, hello.app_generation);
    PutU64(*out, 0U);
    PutU32(*out, 0U);
    return out->size() == kAppHelloWireSize;
}

bool DecodeAppHello(std::span<const uint8_t> bytes, AppHello *hello, std::string *error) {
    if (hello == nullptr) {
        SetError(error, "null output");
        return false;
    }
    if (bytes.size() != kAppHelloWireSize) {
        SetError(error, "app hello size mismatch");
        return false;
    }
    std::size_t offset = 0;
    AppHello decoded{};
    decoded.version = ReadU16(bytes, offset);
    if (decoded.version != kAppProtocolVersion) {
        SetError(error, "unsupported app protocol version");
        return false;
    }
    if (ReadU16(bytes, offset) != 0U) {
        SetError(error, "reserved field must be zero");
        return false;
    }
    decoded.app_generation = ReadU64(bytes, offset);
    if (ReadU64(bytes, offset) != 0U || ReadU32(bytes, offset) != 0U) {
        SetError(error, "reserved field must be zero");
        return false;
    }
    *hello = decoded;
    if (error != nullptr) error->clear();
    return true;
}

bool EncodeAppHelloAck(const AppHelloAck &ack, std::vector<uint8_t> *out, std::string *error) {
    if (out == nullptr) {
        SetError(error, "null output");
        return false;
    }
    if (!ack.route_key_hash.empty() && !IsLowerHexHash(ack.route_key_hash)) {
        SetError(error, "route_key_hash must be 64 lowercase hex characters");
        return false;
    }
    out->clear();
    out->reserve(kAppHelloAckWireSize);
    PutU16(*out, ack.version);
    PutU16(*out, ack.flags);
    PutU64(*out, ack.daemon_generation);
    PutU64(*out, ack.route_epoch);
    PutU64(*out, 0U);
    PutU32(*out, 0U);
    PutHash(*out, ack.route_key_hash);
    return out->size() == kAppHelloAckWireSize;
}

bool DecodeAppHelloAck(std::span<const uint8_t> bytes, AppHelloAck *ack, std::string *error) {
    if (ack == nullptr) {
        SetError(error, "null output");
        return false;
    }
    if (bytes.size() != kAppHelloAckWireSize) {
        SetError(error, "app hello ack size mismatch");
        return false;
    }
    std::size_t offset = 0;
    AppHelloAck decoded{};
    decoded.version = ReadU16(bytes, offset);
    if (decoded.version != kAppProtocolVersion) {
        SetError(error, "unsupported app protocol version");
        return false;
    }
    decoded.flags = ReadU16(bytes, offset);
    decoded.daemon_generation = ReadU64(bytes, offset);
    decoded.route_epoch = ReadU64(bytes, offset);
    if (ReadU64(bytes, offset) != 0U || ReadU32(bytes, offset) != 0U) {
        SetError(error, "reserved field must be zero");
        return false;
    }
    if (!ReadHash(bytes, offset, &decoded.route_key_hash)) {
        SetError(error, "route_key_hash must be 64 lowercase hex characters");
        return false;
    }
    *ack = decoded;
    if (error != nullptr) error->clear();
    return true;
}

bool EncodeAppRouteReport(
    const AppRouteReport &report,
    std::vector<uint8_t> *out,
    std::string *error
) {
    if (out == nullptr) {
        SetError(error, "null output");
        return false;
    }
    if (report.version != kAppProtocolVersion) {
        SetError(error, "unsupported app protocol version");
        return false;
    }
    if (!ValidateRouteField(report.route_type, error)
        || !ValidateRouteField(report.stable_address_or_port, error)
        || !ValidateRouteField(report.product_name, error)
        || !ValidateRouteField(report.encoding, error)) {
        return false;
    }
    if (report.sample_rate == 0U || report.channel_mask == 0U) {
        SetError(error, "route format fields must be non-zero");
        return false;
    }

    out->clear();
    out->reserve(
        kAppRouteReportHeaderSize + report.route_type.size()
        + report.stable_address_or_port.size() + report.product_name.size()
        + report.encoding.size()
    );
    PutU16(*out, report.version);
    PutU16(*out, 0U);
    PutU32(*out, report.sample_rate);
    PutU32(*out, report.channel_mask);
    PutU32(*out, report.output_flags);
    PutU32(*out, static_cast<uint32_t>(report.route_type.size()));
    PutU32(*out, static_cast<uint32_t>(report.stable_address_or_port.size()));
    PutU32(*out, static_cast<uint32_t>(report.product_name.size()));
    PutU32(*out, static_cast<uint32_t>(report.encoding.size()));
    out->insert(out->end(), report.route_type.begin(), report.route_type.end());
    out->insert(
        out->end(),
        report.stable_address_or_port.begin(),
        report.stable_address_or_port.end()
    );
    out->insert(out->end(), report.product_name.begin(), report.product_name.end());
    out->insert(out->end(), report.encoding.begin(), report.encoding.end());
    return true;
}

bool DecodeAppRouteReport(
    std::span<const uint8_t> bytes,
    AppRouteReport *report,
    std::string *error
) {
    if (report == nullptr) {
        SetError(error, "null output");
        return false;
    }
    if (bytes.size() < kAppRouteReportHeaderSize) {
        SetError(error, "app route report is truncated");
        return false;
    }

    std::size_t offset = 0;
    AppRouteReport decoded{};
    decoded.version = ReadU16(bytes, offset);
    if (decoded.version != kAppProtocolVersion) {
        SetError(error, "unsupported app protocol version");
        return false;
    }
    if (ReadU16(bytes, offset) != 0U) {
        SetError(error, "reserved field must be zero");
        return false;
    }
    decoded.sample_rate = ReadU32(bytes, offset);
    decoded.channel_mask = ReadU32(bytes, offset);
    decoded.output_flags = ReadU32(bytes, offset);
    const uint32_t route_type_size = ReadU32(bytes, offset);
    const uint32_t address_size = ReadU32(bytes, offset);
    const uint32_t product_size = ReadU32(bytes, offset);
    const uint32_t encoding_size = ReadU32(bytes, offset);

    // Every length is bounded before it is summed, so the total cannot overflow.
    if (route_type_size > kMaxAppRouteFieldBytes || address_size > kMaxAppRouteFieldBytes
        || product_size > kMaxAppRouteFieldBytes || encoding_size > kMaxAppRouteFieldBytes) {
        SetError(error, "route field is too large");
        return false;
    }
    const std::size_t total = static_cast<std::size_t>(route_type_size)
        + address_size + product_size + encoding_size;
    if (bytes.size() != kAppRouteReportHeaderSize + total) {
        SetError(error, "app route report length mismatch");
        return false;
    }

    const auto read_field = [&](uint32_t size) {
        const auto begin = bytes.begin() + static_cast<std::ptrdiff_t>(offset);
        std::string value(reinterpret_cast<const char *>(&*begin), size);
        offset += size;
        return value;
    };
    decoded.route_type = read_field(route_type_size);
    decoded.stable_address_or_port = read_field(address_size);
    decoded.product_name = read_field(product_size);
    decoded.encoding = read_field(encoding_size);

    if (!ValidateRouteField(decoded.route_type, error)
        || !ValidateRouteField(decoded.stable_address_or_port, error)
        || !ValidateRouteField(decoded.product_name, error)
        || !ValidateRouteField(decoded.encoding, error)) {
        return false;
    }
    if (decoded.sample_rate == 0U || decoded.channel_mask == 0U) {
        SetError(error, "route format fields must be non-zero");
        return false;
    }

    *report = std::move(decoded);
    if (error != nullptr) error->clear();
    return true;
}

bool EncodeAppRouteAck(const AppRouteAck &ack, std::vector<uint8_t> *out, std::string *error) {
    if (out == nullptr) {
        SetError(error, "null output");
        return false;
    }
    if (!ack.route_key_hash.empty() && !IsLowerHexHash(ack.route_key_hash)) {
        SetError(error, "route_key_hash must be 64 lowercase hex characters");
        return false;
    }
    out->clear();
    out->reserve(kAppRouteAckWireSize);
    PutU16(*out, ack.accepted ? 1U : 0U);
    PutU16(*out, 0U);
    PutU64(*out, ack.daemon_generation);
    PutU64(*out, ack.route_epoch);
    PutU64(*out, 0U);
    PutU32(*out, 0U);
    PutHash(*out, ack.route_key_hash);
    return out->size() == kAppRouteAckWireSize;
}

bool DecodeAppRouteAck(std::span<const uint8_t> bytes, AppRouteAck *ack, std::string *error) {
    if (ack == nullptr) {
        SetError(error, "null output");
        return false;
    }
    if (bytes.size() != kAppRouteAckWireSize) {
        SetError(error, "app route ack size mismatch");
        return false;
    }
    std::size_t offset = 0;
    AppRouteAck decoded{};
    decoded.accepted = ReadU16(bytes, offset) != 0U;
    if (ReadU16(bytes, offset) != 0U) {
        SetError(error, "reserved field must be zero");
        return false;
    }
    decoded.daemon_generation = ReadU64(bytes, offset);
    decoded.route_epoch = ReadU64(bytes, offset);
    if (ReadU64(bytes, offset) != 0U || ReadU32(bytes, offset) != 0U) {
        SetError(error, "reserved field must be zero");
        return false;
    }
    if (!ReadHash(bytes, offset, &decoded.route_key_hash)) {
        SetError(error, "route_key_hash must be 64 lowercase hex characters");
        return false;
    }
    *ack = decoded;
    if (error != nullptr) error->clear();
    return true;
}

bool EncodeAppApplyResult(
    const AppApplyResult &result,
    std::vector<uint8_t> *out,
    std::string *error
) {
    if (out == nullptr) {
        SetError(error, "null output");
        return false;
    }
    out->clear();
    out->reserve(kAppApplyResultWireSize);
    PutU16(*out, result.accepted ? 1U : 0U);
    PutU16(*out, 0U);
    PutU32(*out, result.error_code);
    PutU64(*out, result.app_generation);
    PutU64(*out, result.daemon_generation);
    PutU64(*out, result.resource_generation);
    PutU64(*out, result.graph_generation);
    PutU64(*out, 0U);
    return out->size() == kAppApplyResultWireSize;
}

bool DecodeAppApplyResult(
    std::span<const uint8_t> bytes,
    AppApplyResult *result,
    std::string *error
) {
    if (result == nullptr) {
        SetError(error, "null output");
        return false;
    }
    if (bytes.size() != kAppApplyResultWireSize) {
        SetError(error, "app apply result size mismatch");
        return false;
    }
    std::size_t offset = 0;
    AppApplyResult decoded{};
    decoded.accepted = ReadU16(bytes, offset) != 0U;
    if (ReadU16(bytes, offset) != 0U) {
        SetError(error, "reserved field must be zero");
        return false;
    }
    decoded.error_code = ReadU32(bytes, offset);
    decoded.app_generation = ReadU64(bytes, offset);
    decoded.daemon_generation = ReadU64(bytes, offset);
    decoded.resource_generation = ReadU64(bytes, offset);
    decoded.graph_generation = ReadU64(bytes, offset);
    if (ReadU64(bytes, offset) != 0U) {
        SetError(error, "reserved field must be zero");
        return false;
    }
    *result = decoded;
    if (error != nullptr) error->clear();
    return true;
}

} // namespace viper::daemon
