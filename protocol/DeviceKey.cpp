#include "DeviceKey.h"

#include "Sha256.h"

#include <algorithm>
#include <cctype>

namespace viper::daemon {
namespace {

std::string NormalizeField(std::string_view value) {
    std::string normalized;
    normalized.reserve(value.size());
    bool pending_space = false;
    for (const unsigned char byte : value) {
        if (std::isspace(byte) != 0) {
            pending_space = !normalized.empty();
            continue;
        }
        if (pending_space) normalized.push_back(' ');
        pending_space = false;
        normalized.push_back(static_cast<char>(std::tolower(byte)));
    }
    while (!normalized.empty() && normalized.back() == ' ') normalized.pop_back();
    return normalized;
}

bool ContainsVolatileToken(std::string_view value) noexcept {
    std::string lower;
    lower.reserve(value.size());
    for (const unsigned char byte : value) {
        lower.push_back(static_cast<char>(std::tolower(byte)));
    }
    return lower.find("session") != std::string::npos
        || lower.find("process") != std::string::npos
        || lower.find("pid") != std::string::npos
        || lower.find("track") != std::string::npos;
}

bool HasInvalidDelimiter(std::string_view value) noexcept {
    return value.find('|') != std::string_view::npos
        || std::any_of(value.begin(), value.end(), [](unsigned char byte) {
            return byte < 0x20U || byte == 0x7FU;
        });
}

} // namespace

bool IsValidDeviceIdentity(const DeviceIdentity &identity) noexcept {
    if (identity.audio_session_id != 0U || identity.process_id != 0U) return false;
    if (identity.sample_rate == 0U || identity.channel_mask == 0U) return false;
    if (NormalizeField(identity.route_type).empty()
        || NormalizeField(identity.stable_address_or_port).empty()
        || NormalizeField(identity.product_name).empty()
        || NormalizeField(identity.encoding).empty()) {
        return false;
    }
    return !HasInvalidDelimiter(identity.route_type)
        && !HasInvalidDelimiter(identity.stable_address_or_port)
        && !HasInvalidDelimiter(identity.product_name)
        && !HasInvalidDelimiter(identity.encoding)
        && !ContainsVolatileToken(identity.stable_address_or_port);
}

std::string NormalizeDeviceKey(const DeviceIdentity &identity) {
    if (!IsValidDeviceIdentity(identity)) return {};
    return NormalizeField(identity.route_type) + "|"
        + NormalizeField(identity.stable_address_or_port) + "|"
        + NormalizeField(identity.product_name) + "|"
        + std::to_string(identity.sample_rate) + "|"
        + std::to_string(identity.channel_mask) + "|"
        + NormalizeField(identity.encoding) + "|"
        + std::to_string(identity.output_flags);
}

std::string HashDeviceKey(std::string_view normalized_key) {
    if (normalized_key.empty()) return {};
    return iem::tools::Sha256Hex(
        reinterpret_cast<const uint8_t *>(normalized_key.data()),
        normalized_key.size()
    );
}

} // namespace viper::daemon
