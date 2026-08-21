#pragma once

#include "DeviceKey.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace viper::daemon {

/**
 * Source of the current output route.
 *
 * Implementations must read only information available to a root daemon without
 * hidden AudioFlinger Binder APIs. Host tests use FakeRouteAdapter.
 */
class RouteAdapter {
public:
    virtual ~RouteAdapter() = default;

    // Returns false when the route cannot be determined right now.
    virtual bool Read(DeviceIdentity *identity, std::string *error) = 0;
};

// Deterministic adapter for host tests.
class FakeRouteAdapter final : public RouteAdapter {
public:
    void SetRoute(DeviceIdentity identity);
    void SetUnavailable(std::string error);

    bool Read(DeviceIdentity *identity, std::string *error) override;

private:
    DeviceIdentity identity_{};
    std::string error_;
    bool available_ = false;
};

/**
 * Device adapter reading Android system properties and ALSA/sysfs state.
 *
 * Missing sources are reported honestly rather than guessed: a wrong route key
 * would apply another device's snapshot.
 */
class AndroidRouteAdapter final : public RouteAdapter {
public:
    explicit AndroidRouteAdapter(std::string sysfs_root = "/sys/class/switch");

    bool Read(DeviceIdentity *identity, std::string *error) override;

private:
    std::string sysfs_root_;
};

/**
 * Route source fed by the App over the app endpoint.
 *
 * The App holds the only reliable view of the output route on devices whose
 * sysfs exposes nothing usable, so the daemon trusts what the App reports and
 * caches it for the next boot.
 *
 * The seed is an already-loaded optional identity rather than a RouteCache
 * reference: the adapter then has no filesystem dependency, the caller decides
 * how to react to a corrupt cache, and tests exercise both states directly.
 */
class AppReportedRouteAdapter final : public RouteAdapter {
public:
    AppReportedRouteAdapter() = default;
    explicit AppReportedRouteAdapter(std::optional<DeviceIdentity> cached_identity);

    // Ignores an invalid identity: a bad key applies another device's snapshot.
    void SetReportedRoute(DeviceIdentity identity);

    bool HasReportedRoute() const noexcept { return identity_.has_value(); }

    bool Read(DeviceIdentity *identity, std::string *error) override;

private:
    std::optional<DeviceIdentity> identity_;
};

/**
 * Tracks route changes and exposes the normalized key/hash pair the snapshot
 * store is addressed by.
 */
class RouteWatcher final {
public:
    explicit RouteWatcher(std::unique_ptr<RouteAdapter> adapter);

    // Returns true when the route key changed since the previous poll.
    bool Poll(std::string *error);

    const DeviceIdentity &CurrentRoute() const noexcept { return identity_; }
    const std::string &CurrentKey() const noexcept { return key_; }
    const std::string &CurrentKeyHash() const noexcept { return key_hash_; }
    bool HasRoute() const noexcept { return !key_.empty(); }

private:
    std::unique_ptr<RouteAdapter> adapter_;
    DeviceIdentity identity_{};
    std::string key_;
    std::string key_hash_;
};

} // namespace viper::daemon
