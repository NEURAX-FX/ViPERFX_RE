#include "RouteCache.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>

namespace viper::daemon {
namespace {

constexpr std::string_view kFileName = "route.cache";
constexpr std::string_view kTemporaryName = "route.cache.tmp";
constexpr std::string_view kVersionValue = "1";

// A cache file holds seven identity fields plus version and timestamp; anything
// larger is not a file this daemon wrote.
constexpr std::uintmax_t kMaxCacheSize = 8192U;

void SetError(std::string *error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}

bool SetPrivateMode(const std::filesystem::path &path, mode_t mode) noexcept {
    return ::chmod(path.c_str(), mode) == 0;
}

bool FsyncPath(const std::filesystem::path &path, std::string *error) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        SetError(error, "failed to open route cache for fsync");
        return false;
    }
    const bool synced = ::fsync(fd) == 0;
    const int close_result = ::close(fd);
    if (!synced || close_result != 0) {
        SetError(error, "failed to fsync route cache file");
        return false;
    }
    return true;
}

bool FsyncDirectory(const std::filesystem::path &path, std::string *error) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        SetError(error, "failed to open route cache directory for fsync");
        return false;
    }
    const bool synced = ::fsync(fd) == 0;
    const int close_result = ::close(fd);
    if (!synced || close_result != 0) {
        SetError(error, "failed to fsync route cache directory");
        return false;
    }
    return true;
}

// Rejects anything that could smuggle a second field into a normalized device
// key or corrupt the line-oriented file format.
bool IsSafeFieldValue(std::string_view value) noexcept {
    return std::none_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte == '|' || byte < 0x20U || byte == 0x7FU;
    });
}

bool ParseUnsigned(std::string_view text, uint32_t *out) noexcept {
    if (text.empty() || text.size() > 10U) return false;
    uint64_t value = 0;
    for (const unsigned char byte : text) {
        if (byte < '0' || byte > '9') return false;
        value = value * 10U + static_cast<uint64_t>(byte - '0');
        if (value > std::numeric_limits<uint32_t>::max()) return false;
    }
    *out = static_cast<uint32_t>(value);
    return true;
}

int64_t NowMillis() {
    using clock = std::chrono::system_clock;
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        clock::now().time_since_epoch()
    ).count();
}

} // namespace

RouteCache::RouteCache(std::filesystem::path state_root)
    : state_root_(std::move(state_root)),
      path_(state_root_ / std::string(kFileName)),
      temporary_path_(state_root_ / std::string(kTemporaryName)) {}

bool RouteCache::Store(const DeviceIdentity &identity, std::string *error) {
    if (!IsValidDeviceIdentity(identity)) {
        SetError(error, "route cache refuses an invalid identity");
        return false;
    }
    if (!IsSafeFieldValue(identity.route_type)
        || !IsSafeFieldValue(identity.stable_address_or_port)
        || !IsSafeFieldValue(identity.product_name)
        || !IsSafeFieldValue(identity.encoding)) {
        SetError(error, "route cache refuses an unserializable identity");
        return false;
    }

    std::error_code filesystem_error;
    std::filesystem::create_directories(state_root_, filesystem_error);
    if (filesystem_error && !std::filesystem::is_directory(state_root_)) {
        SetError(error, "failed to create route cache directory");
        return false;
    }
    // The cache names the user's audio hardware; keep the whole tree root-only.
    SetPrivateMode(state_root_, 0700);

    std::ostringstream body;
    body << "version=" << kVersionValue << "\n"
         << "route_type=" << identity.route_type << "\n"
         << "stable_address_or_port=" << identity.stable_address_or_port << "\n"
         << "product_name=" << identity.product_name << "\n"
         << "encoding=" << identity.encoding << "\n"
         << "sample_rate=" << identity.sample_rate << "\n"
         << "channel_mask=" << identity.channel_mask << "\n"
         << "output_flags=" << identity.output_flags << "\n"
         << "updated_at_millis=" << NowMillis() << "\n";
    const std::string encoded = body.str();

    std::filesystem::remove(temporary_path_, filesystem_error);
    {
        std::ofstream output(temporary_path_, std::ios::binary | std::ios::trunc);
        if (!output) {
            SetError(error, "failed to create route cache temporary file");
            return false;
        }
        output.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
        output.flush();
        if (!output) {
            SetError(error, "failed to write route cache temporary file");
            std::filesystem::remove(temporary_path_, filesystem_error);
            return false;
        }
    }
    if (!SetPrivateMode(temporary_path_, 0600) || !FsyncPath(temporary_path_, error)) {
        if (error == nullptr || error->empty()) {
            SetError(error, "failed to secure route cache temporary file");
        }
        std::filesystem::remove(temporary_path_, filesystem_error);
        return false;
    }
    if (::rename(temporary_path_.c_str(), path_.c_str()) != 0) {
        SetError(error, "failed to publish route cache");
        std::filesystem::remove(temporary_path_, filesystem_error);
        return false;
    }
    if (!FsyncDirectory(state_root_, error)) return false;
    if (error != nullptr) error->clear();
    return true;
}

bool RouteCache::Load(DeviceIdentity *identity, std::string *error) const {
    if (identity == nullptr) {
        SetError(error, "null identity output");
        return false;
    }

    std::error_code filesystem_error;
    if (!std::filesystem::is_regular_file(path_, filesystem_error)) {
        SetError(error, "route cache missing at " + path_.string());
        return false;
    }
    const auto file_size = std::filesystem::file_size(path_, filesystem_error);
    if (filesystem_error || file_size > kMaxCacheSize) {
        SetError(error, "route cache file is not a cache this daemon wrote");
        return false;
    }

    std::ifstream input(path_, std::ios::binary);
    if (!input) {
        SetError(error, "failed to open route cache");
        return false;
    }

    std::map<std::string, std::string> fields;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        const auto separator = line.find('=');
        if (separator == std::string::npos) {
            SetError(error, "route cache has a malformed line");
            return false;
        }
        std::string key = line.substr(0, separator);
        std::string value = line.substr(separator + 1);
        // A trailing CR would end up inside a device-key field.
        if (!value.empty() && value.back() == '\r') value.pop_back();
        if (key.empty() || !IsSafeFieldValue(value)) {
            SetError(error, "route cache field " + key + " is not trustworthy");
            return false;
        }
        if (!fields.emplace(std::move(key), std::move(value)).second) {
            SetError(error, "route cache has a duplicated key");
            return false;
        }
    }

    const auto find_field = [&fields](const char *key) -> const std::string * {
        const auto it = fields.find(key);
        return it == fields.end() ? nullptr : &it->second;
    };

    const std::string *version = find_field("version");
    if (version == nullptr || *version != kVersionValue) {
        SetError(error, "route cache version is missing or unknown");
        return false;
    }

    const std::string *route_type = find_field("route_type");
    const std::string *address = find_field("stable_address_or_port");
    const std::string *product = find_field("product_name");
    const std::string *encoding = find_field("encoding");
    const std::string *sample_rate = find_field("sample_rate");
    const std::string *channel_mask = find_field("channel_mask");
    const std::string *output_flags = find_field("output_flags");
    const std::string *updated_at = find_field("updated_at_millis");
    if (route_type == nullptr || address == nullptr || product == nullptr
        || encoding == nullptr || sample_rate == nullptr || channel_mask == nullptr
        || output_flags == nullptr || updated_at == nullptr) {
        SetError(error, "route cache is incomplete");
        return false;
    }

    DeviceIdentity restored{};
    restored.route_type = *route_type;
    restored.stable_address_or_port = *address;
    restored.product_name = *product;
    restored.encoding = *encoding;
    if (!ParseUnsigned(*sample_rate, &restored.sample_rate)
        || !ParseUnsigned(*channel_mask, &restored.channel_mask)
        || !ParseUnsigned(*output_flags, &restored.output_flags)) {
        SetError(error, "route cache has a non-numeric field");
        return false;
    }
    if (updated_at->empty()
        || std::any_of(updated_at->begin(), updated_at->end(), [](unsigned char byte) {
               return byte < '0' || byte > '9';
           })) {
        SetError(error, "route cache timestamp is not numeric");
        return false;
    }

    if (!IsValidDeviceIdentity(restored)) {
        SetError(error, "route cache holds an invalid identity");
        return false;
    }

    *identity = std::move(restored);
    if (error != nullptr) error->clear();
    return true;
}

} // namespace viper::daemon
