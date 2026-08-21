#pragma once

#include "DeviceKey.h"

#include <filesystem>
#include <string>

namespace viper::daemon {

/**
 * Persists the last route the App reported so a reboot can restore a snapshot
 * before the App runs.
 *
 * This device exposes no usable sysfs route source, so the App is the only
 * component that can name the live output route. Without an on-disk memory the
 * daemon would start every boot with no route and never restore anything.
 *
 * A wrong route key silently applies another device's audio settings, so Load
 * refuses anything it cannot fully trust instead of returning a partially
 * populated identity.
 */
class RouteCache final {
public:
    explicit RouteCache(std::filesystem::path state_root);

    const std::filesystem::path &Path() const noexcept { return path_; }

    // Atomically replaces the cached identity. Rejects invalid identities.
    bool Store(const DeviceIdentity &identity, std::string *error);

    // Fills *identity only when the file is complete, well-formed and valid.
    bool Load(DeviceIdentity *identity, std::string *error) const;

private:
    std::filesystem::path state_root_;
    std::filesystem::path path_;
    std::filesystem::path temporary_path_;
};

} // namespace viper::daemon
