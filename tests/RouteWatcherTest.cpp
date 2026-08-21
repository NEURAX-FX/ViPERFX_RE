#include "RouteWatcher.h"

#include "DeviceKey.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace {

using viper::daemon::AndroidRouteAdapter;
using viper::daemon::DeviceIdentity;
using viper::daemon::FakeRouteAdapter;
using viper::daemon::RouteWatcher;

std::filesystem::path UniqueRoot(const char *suffix) {
    static int counter = 0;
    return std::filesystem::temp_directory_path()
        / ("viper-route-" + std::string(suffix) + "-" + std::to_string(++counter));
}

// Writes a sysfs-like h2w switch node.
void WriteHeadsetState(const std::filesystem::path &root, const char *state) {
    std::filesystem::create_directories(root / "h2w");
    std::ofstream output(root / "h2w" / "state");
    output << state << "\n";
}

DeviceIdentity SpeakerIdentity() {
    DeviceIdentity identity{};
    identity.route_type = "speaker";
    identity.stable_address_or_port = "builtin";
    identity.product_name = "internal";
    identity.sample_rate = viper::daemon::kRouteIdentitySampleRate;
    identity.channel_mask = viper::daemon::kRouteIdentityChannelMask;
    identity.encoding = viper::daemon::kRouteIdentityEncoding;
    return identity;
}

void TestAndroidAdapterProducesAUsableKey() {
    const auto root = UniqueRoot("android");
    WriteHeadsetState(root, "0");

    AndroidRouteAdapter adapter(root.string());
    DeviceIdentity identity{};
    std::string error;
    // The adapter must fill every field the key requires. Leaving sample_rate or
    // channel_mask at zero makes IsValidDeviceIdentity fail and the daemon would
    // never know its route.
    assert(adapter.Read(&identity, &error));
    assert(error.empty());
    assert(identity.route_type == "speaker");
    assert(identity.stable_address_or_port == "builtin");
    assert(identity.sample_rate != 0U);
    assert(identity.channel_mask != 0U);
    assert(!identity.encoding.empty());
    assert(viper::daemon::IsValidDeviceIdentity(identity));
    assert(!viper::daemon::NormalizeDeviceKey(identity).empty());

    std::filesystem::remove_all(root);
}

void TestAndroidAdapterDistinguishesWiredFromSpeaker() {
    const auto root = UniqueRoot("wired");
    WriteHeadsetState(root, "0");

    AndroidRouteAdapter adapter(root.string());
    DeviceIdentity speaker{};
    std::string error;
    assert(adapter.Read(&speaker, &error));

    WriteHeadsetState(root, "1");
    DeviceIdentity wired{};
    assert(adapter.Read(&wired, &error));

    assert(wired.route_type == "wired_headset");
    // Different routes must yield different keys, or a headset would inherit the
    // speaker's snapshot.
    assert(viper::daemon::NormalizeDeviceKey(speaker)
        != viper::daemon::NormalizeDeviceKey(wired));

    std::filesystem::remove_all(root);
}

void TestAndroidAdapterReportsMissingSource() {
    const auto root = UniqueRoot("missing");
    // No h2w node at all.
    AndroidRouteAdapter adapter(root.string());
    DeviceIdentity identity{};
    std::string error;
    assert(!adapter.Read(&identity, &error));
    // Honest failure instead of guessing a route.
    assert(!error.empty());
}

void TestWatcherReportsChangeOnlyOnce() {
    auto adapter = std::make_unique<FakeRouteAdapter>();
    adapter->SetRoute(SpeakerIdentity());
    RouteWatcher watcher(std::move(adapter));

    std::string error;
    assert(watcher.Poll(&error));
    assert(watcher.HasRoute());
    const std::string first_hash = watcher.CurrentKeyHash();
    assert(first_hash.size() == 64U);
    // An unchanged route is not a change.
    assert(!watcher.Poll(&error));
    assert(watcher.CurrentKeyHash() == first_hash);
}

void TestWatcherKeepsLastKnownRouteWhenUnavailable() {
    auto adapter = std::make_unique<FakeRouteAdapter>();
    adapter->SetRoute(SpeakerIdentity());
    FakeRouteAdapter *raw = adapter.get();
    RouteWatcher watcher(std::move(adapter));

    std::string error;
    assert(watcher.Poll(&error));
    const std::string known = watcher.CurrentKeyHash();

    raw->SetUnavailable("no route source");
    assert(!watcher.Poll(&error));
    assert(!error.empty());
    // Losing the signal must not erase the route: bypassing is better than
    // applying another device's snapshot.
    assert(watcher.HasRoute());
    assert(watcher.CurrentKeyHash() == known);
}

void TestWatcherRejectsInvalidIdentity() {
    auto adapter = std::make_unique<FakeRouteAdapter>();
    DeviceIdentity broken = SpeakerIdentity();
    broken.sample_rate = 0;
    adapter->SetRoute(broken);
    RouteWatcher watcher(std::move(adapter));

    std::string error;
    assert(!watcher.Poll(&error));
    assert(!error.empty());
    assert(!watcher.HasRoute());
    assert(watcher.CurrentKeyHash().empty());
}

void TestWatcherWithoutAdapterFails() {
    RouteWatcher watcher(nullptr);
    std::string error;
    assert(!watcher.Poll(&error));
    assert(!error.empty());
    assert(!watcher.HasRoute());
}

} // namespace

int main() {
    TestAndroidAdapterProducesAUsableKey();
    TestAndroidAdapterDistinguishesWiredFromSpeaker();
    TestAndroidAdapterReportsMissingSource();
    TestWatcherReportsChangeOnlyOnce();
    TestWatcherKeepsLastKnownRouteWhenUnavailable();
    TestWatcherRejectsInvalidIdentity();
    TestWatcherWithoutAdapterFails();
    std::puts("route watcher tests passed");
    return 0;
}
