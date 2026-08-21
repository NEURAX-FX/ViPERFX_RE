#include "RouteWatcher.h"

#include <filesystem>
#include <fstream>
#include <utility>

namespace viper::daemon {
namespace {

void SetError(std::string *error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}

bool ReadFirstLine(const std::filesystem::path &path, std::string *value) {
    std::ifstream input(path);
    if (!input) return false;
    std::string line;
    if (!std::getline(input, line)) return false;
    *value = line;
    return true;
}

} // namespace

void FakeRouteAdapter::SetRoute(DeviceIdentity identity) {
    identity_ = std::move(identity);
    error_.clear();
    available_ = true;
}

void FakeRouteAdapter::SetUnavailable(std::string error) {
    error_ = std::move(error);
    available_ = false;
}

bool FakeRouteAdapter::Read(DeviceIdentity *identity, std::string *error) {
    if (identity == nullptr) {
        SetError(error, "null identity");
        return false;
    }
    if (!available_) {
        SetError(error, error_.empty() ? "route unavailable" : error_);
        return false;
    }
    *identity = identity_;
    if (error != nullptr) error->clear();
    return true;
}

AndroidRouteAdapter::AndroidRouteAdapter(std::string sysfs_root)
    : sysfs_root_(std::move(sysfs_root)) {}

bool AndroidRouteAdapter::Read(DeviceIdentity *identity, std::string *error) {
    if (identity == nullptr) {
        SetError(error, "null identity");
        return false;
    }

    // Wired accessories expose a switch node; its state is the only route signal
    // available without hidden AudioFlinger APIs.
    const std::filesystem::path headset_state =
        std::filesystem::path(sysfs_root_) / "h2w" / "state";
    std::string state;
    if (!ReadFirstLine(headset_state, &state)) {
        SetError(error, "no route source available at " + headset_state.string());
        return false;
    }

    DeviceIdentity resolved{};
    resolved.route_type = state == "0" ? "speaker" : "wired_headset";
    resolved.stable_address_or_port = state == "0" ? "builtin" : "h2w";
    resolved.product_name = resolved.route_type;
    // Fixed canonical format: the live mixer format is not readable here, and the
    // App fills the same constants, so a derived value would never match.
    resolved.sample_rate = kRouteIdentitySampleRate;
    resolved.channel_mask = kRouteIdentityChannelMask;
    resolved.encoding = kRouteIdentityEncoding;
    if (!IsValidDeviceIdentity(resolved)) {
        SetError(error, "resolved route identity is not valid");
        return false;
    }

    *identity = std::move(resolved);
    if (error != nullptr) error->clear();
    return true;
}

AppReportedRouteAdapter::AppReportedRouteAdapter(
    std::optional<DeviceIdentity> cached_identity
) {
    if (cached_identity.has_value() && IsValidDeviceIdentity(*cached_identity)) {
        identity_ = std::move(cached_identity);
    }
}

void AppReportedRouteAdapter::SetReportedRoute(DeviceIdentity identity) {
    if (!IsValidDeviceIdentity(identity)) return;
    identity_ = std::move(identity);
}

bool AppReportedRouteAdapter::Read(DeviceIdentity *identity, std::string *error) {
    if (identity == nullptr) {
        SetError(error, "null identity");
        return false;
    }
    if (!identity_.has_value()) {
        SetError(error, "no route reported by the app yet");
        return false;
    }
    *identity = *identity_;
    if (error != nullptr) error->clear();
    return true;
}

RouteWatcher::RouteWatcher(std::unique_ptr<RouteAdapter> adapter)
    : adapter_(std::move(adapter)) {}

bool RouteWatcher::Poll(std::string *error) {
    if (!adapter_) {
        SetError(error, "no route adapter");
        return false;
    }

    DeviceIdentity identity{};
    if (!adapter_->Read(&identity, error)) return false;
    if (!IsValidDeviceIdentity(identity)) {
        SetError(error, "route adapter produced an invalid identity");
        return false;
    }

    const std::string key = NormalizeDeviceKey(identity);
    if (key.empty()) {
        SetError(error, "route key normalization failed");
        return false;
    }
    if (key == key_) return false;

    identity_ = std::move(identity);
    key_ = key;
    key_hash_ = HashDeviceKey(key_);
    if (error != nullptr) error->clear();
    return true;
}

} // namespace viper::daemon
