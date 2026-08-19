#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace viper::daemon {

struct DeviceIdentity {
    std::string route_type;
    std::string stable_address_or_port;
    std::string product_name;
    uint32_t sample_rate = 0;
    uint32_t channel_mask = 0;
    std::string encoding;
    uint32_t output_flags = 0;

    // These fields are deliberately not part of a device key. Non-zero values
    // indicate that a caller accidentally supplied a volatile identity.
    uint32_t audio_session_id = 0;
    uint32_t process_id = 0;
};

bool IsValidDeviceIdentity(const DeviceIdentity &identity) noexcept;
std::string NormalizeDeviceKey(const DeviceIdentity &identity);
std::string HashDeviceKey(std::string_view normalized_key);

} // namespace viper::daemon
