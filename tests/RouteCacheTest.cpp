#include "RouteCache.h"

#include "DeviceKey.h"
#include "RouteWatcher.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <utility>

namespace {

using viper::daemon::AppReportedRouteAdapter;
using viper::daemon::DeviceIdentity;
using viper::daemon::RouteCache;
using viper::daemon::RouteWatcher;

std::filesystem::path UniqueRoot(const char *suffix) {
    static int counter = 0;
    return std::filesystem::temp_directory_path()
        / ("viper-route-cache-" + std::string(suffix) + "-" + std::to_string(++counter));
}

DeviceIdentity BluetoothIdentity() {
    DeviceIdentity identity{};
    identity.route_type = "bluetooth_a2dp";
    identity.stable_address_or_port = "ac:12:2f:00:9b:41";
    identity.product_name = "WF-1000XM5";
    identity.sample_rate = viper::daemon::kRouteIdentitySampleRate;
    identity.channel_mask = viper::daemon::kRouteIdentityChannelMask;
    identity.encoding = viper::daemon::kRouteIdentityEncoding;
    identity.output_flags = 6;
    return identity;
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

void WriteRaw(const std::filesystem::path &path, const std::string &body) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << body;
}

void TestRoundTripPreservesEveryField() {
    const auto root = UniqueRoot("roundtrip");
    RouteCache cache(root);
    const DeviceIdentity original = BluetoothIdentity();

    std::string error = "unset";
    assert(cache.Store(original, &error));
    assert(error.empty());

    DeviceIdentity restored{};
    assert(cache.Load(&restored, &error));
    assert(restored.route_type == original.route_type);
    assert(restored.stable_address_or_port == original.stable_address_or_port);
    assert(restored.product_name == original.product_name);
    assert(restored.encoding == original.encoding);
    assert(restored.sample_rate == original.sample_rate);
    assert(restored.channel_mask == original.channel_mask);
    assert(restored.output_flags == original.output_flags);
    // The key is what addresses the snapshot store; equality here is the contract
    // that a restored route reaches the same snapshot directory.
    assert(viper::daemon::NormalizeDeviceKey(restored)
        == viper::daemon::NormalizeDeviceKey(original));

    std::filesystem::remove_all(root);
}

void TestPublishIsPrivateAndLeavesNoTemporary() {
    const auto root = UniqueRoot("mode");
    RouteCache cache(root);

    std::string error;
    assert(cache.Store(BluetoothIdentity(), &error));

    struct ::stat status{};
    assert(::stat(cache.Path().c_str(), &status) == 0);
    assert((status.st_mode & 07777) == 0600);
    assert(!std::filesystem::exists(root / "route.cache.tmp"));

    std::filesystem::remove_all(root);
}

void TestMissingFileFailsCleanly() {
    const auto root = UniqueRoot("missing");
    RouteCache cache(root);

    DeviceIdentity restored = BluetoothIdentity();
    std::string error;
    assert(!cache.Load(&restored, &error));
    assert(!error.empty());

    std::filesystem::remove_all(root);
}

void TestSecondStoreReplacesTheFirst() {
    const auto root = UniqueRoot("replace");
    RouteCache cache(root);

    std::string error;
    assert(cache.Store(BluetoothIdentity(), &error));
    assert(cache.Store(SpeakerIdentity(), &error));

    DeviceIdentity restored{};
    assert(cache.Load(&restored, &error));
    assert(viper::daemon::NormalizeDeviceKey(restored)
        == viper::daemon::NormalizeDeviceKey(SpeakerIdentity()));
    assert(!std::filesystem::exists(root / "route.cache.tmp"));

    std::filesystem::remove_all(root);
}

void TestStoreRefusesInvalidIdentity() {
    const auto root = UniqueRoot("store-invalid");
    RouteCache cache(root);

    DeviceIdentity broken = BluetoothIdentity();
    broken.sample_rate = 0;
    std::string error;
    assert(!cache.Store(broken, &error));
    assert(!error.empty());
    assert(!std::filesystem::exists(cache.Path()));

    std::filesystem::remove_all(root);
}

// Every rejection case must leave the caller with nothing, not a half-built key.
void TestRefusesUntrustworthyFiles() {
    const std::string valid =
        "version=1\n"
        "route_type=bluetooth_a2dp\n"
        "stable_address_or_port=ac:12:2f:00:9b:41\n"
        "product_name=WF-1000XM5\n"
        "encoding=pcm_16\n"
        "sample_rate=48000\n"
        "channel_mask=3\n"
        "output_flags=6\n"
        "updated_at_millis=1755600000000\n";

    const std::pair<const char *, std::string> cases[] = {
        {"empty", ""},
        {"truncated", valid.substr(0, 60)},
        {"no-version",
            "route_type=speaker\nstable_address_or_port=builtin\nproduct_name=internal\n"
            "encoding=pcm_16\nsample_rate=48000\nchannel_mask=3\noutput_flags=0\n"
            "updated_at_millis=1\n"},
        {"unknown-version",
            "version=99\nroute_type=speaker\nstable_address_or_port=builtin\n"
            "product_name=internal\nencoding=pcm_16\nsample_rate=48000\nchannel_mask=3\n"
            "output_flags=0\nupdated_at_millis=1\n"},
        {"missing-key",
            "version=1\nroute_type=speaker\nstable_address_or_port=builtin\n"
            "product_name=internal\nencoding=pcm_16\nsample_rate=48000\nchannel_mask=3\n"
            "updated_at_millis=1\n"},
        {"non-numeric-rate",
            "version=1\nroute_type=speaker\nstable_address_or_port=builtin\n"
            "product_name=internal\nencoding=pcm_16\nsample_rate=48k\nchannel_mask=3\n"
            "output_flags=0\nupdated_at_millis=1\n"},
        {"non-numeric-timestamp",
            "version=1\nroute_type=speaker\nstable_address_or_port=builtin\n"
            "product_name=internal\nencoding=pcm_16\nsample_rate=48000\nchannel_mask=3\n"
            "output_flags=0\nupdated_at_millis=never\n"},
        // A delimiter in a field would forge extra device-key fields.
        {"delimiter-injection",
            "version=1\nroute_type=speaker|builtin|internal|48000|3|pcm_16|0\n"
            "stable_address_or_port=builtin\nproduct_name=internal\nencoding=pcm_16\n"
            "sample_rate=48000\nchannel_mask=3\noutput_flags=0\nupdated_at_millis=1\n"},
        {"control-character",
            "version=1\nroute_type=spea\tker\nstable_address_or_port=builtin\n"
            "product_name=internal\nencoding=pcm_16\nsample_rate=48000\nchannel_mask=3\n"
            "output_flags=0\nupdated_at_millis=1\n"},
        {"invalid-identity",
            "version=1\nroute_type=speaker\nstable_address_or_port=builtin\n"
            "product_name=internal\nencoding=pcm_16\nsample_rate=0\nchannel_mask=3\n"
            "output_flags=0\nupdated_at_millis=1\n"},
        {"empty-field",
            "version=1\nroute_type=\nstable_address_or_port=builtin\n"
            "product_name=internal\nencoding=pcm_16\nsample_rate=48000\nchannel_mask=3\n"
            "output_flags=0\nupdated_at_millis=1\n"},
        {"malformed-line",
            "version=1\nthis line has no separator\nroute_type=speaker\n"
            "stable_address_or_port=builtin\nproduct_name=internal\nencoding=pcm_16\n"
            "sample_rate=48000\nchannel_mask=3\noutput_flags=0\nupdated_at_millis=1\n"},
    };

    for (const auto &[name, body] : cases) {
        const auto root = UniqueRoot(name);
        RouteCache cache(root);
        WriteRaw(cache.Path(), body);

        DeviceIdentity restored{};
        std::string error;
        const bool loaded = cache.Load(&restored, &error);
        assert(!loaded);
        assert(!error.empty());
        assert(restored.route_type.empty());
        assert(restored.sample_rate == 0U);

        std::filesystem::remove_all(root);
    }

    // The same content written through Store must still be accepted, otherwise
    // the rejection cases above prove nothing.
    const auto root = UniqueRoot("control");
    RouteCache cache(root);
    WriteRaw(cache.Path(), valid);
    DeviceIdentity restored{};
    std::string error;
    assert(cache.Load(&restored, &error));
    assert(viper::daemon::NormalizeDeviceKey(restored)
        == viper::daemon::NormalizeDeviceKey(BluetoothIdentity()));
    std::filesystem::remove_all(root);
}

void TestAdapterFailsWhileEmpty() {
    AppReportedRouteAdapter adapter;
    DeviceIdentity identity{};
    std::string error;
    assert(!adapter.HasReportedRoute());
    assert(!adapter.Read(&identity, &error));
    assert(!error.empty());

    // Through a watcher: no route rather than an invented one.
    RouteWatcher watcher(std::make_unique<AppReportedRouteAdapter>());
    assert(!watcher.Poll(&error));
    assert(!watcher.HasRoute());
}

void TestAdapterSeedsFromCache() {
    const auto root = UniqueRoot("seed");
    RouteCache cache(root);
    std::string error;
    assert(cache.Store(BluetoothIdentity(), &error));

    DeviceIdentity cached{};
    assert(cache.Load(&cached, &error));

    // Boot-time restore: the watcher resolves a route before the App connects.
    RouteWatcher watcher(std::make_unique<AppReportedRouteAdapter>(cached));
    assert(watcher.Poll(&error));
    assert(watcher.HasRoute());
    assert(watcher.CurrentKey() == viper::daemon::NormalizeDeviceKey(BluetoothIdentity()));

    std::filesystem::remove_all(root);
}

void TestAdapterIgnoresUnusableCacheSeed() {
    DeviceIdentity broken = BluetoothIdentity();
    broken.channel_mask = 0;
    AppReportedRouteAdapter adapter(broken);
    assert(!adapter.HasReportedRoute());

    DeviceIdentity identity{};
    std::string error;
    assert(!adapter.Read(&identity, &error));
}

void TestReportedRouteTakesEffectThroughWatcher() {
    auto adapter = std::make_unique<AppReportedRouteAdapter>();
    AppReportedRouteAdapter *raw = adapter.get();
    RouteWatcher watcher(std::move(adapter));

    std::string error;
    assert(!watcher.Poll(&error));

    raw->SetReportedRoute(SpeakerIdentity());
    assert(watcher.Poll(&error));
    assert(watcher.CurrentKey() == viper::daemon::NormalizeDeviceKey(SpeakerIdentity()));
    const std::string speaker_hash = watcher.CurrentKeyHash();
    assert(speaker_hash.size() == 64U);
    // Reporting the same route again is not a change.
    raw->SetReportedRoute(SpeakerIdentity());
    assert(!watcher.Poll(&error));
    assert(watcher.CurrentKeyHash() == speaker_hash);

    raw->SetReportedRoute(BluetoothIdentity());
    assert(watcher.Poll(&error));
    assert(watcher.CurrentKey() == viper::daemon::NormalizeDeviceKey(BluetoothIdentity()));
    assert(!watcher.Poll(&error));
}

void TestAdapterRefusesInvalidReportedRoute() {
    auto adapter = std::make_unique<AppReportedRouteAdapter>();
    AppReportedRouteAdapter *raw = adapter.get();
    raw->SetReportedRoute(SpeakerIdentity());
    RouteWatcher watcher(std::move(adapter));

    std::string error;
    assert(watcher.Poll(&error));
    const std::string known = watcher.CurrentKeyHash();

    DeviceIdentity forged = BluetoothIdentity();
    forged.route_type = "bluetooth|a2dp";
    raw->SetReportedRoute(forged);

    DeviceIdentity current{};
    assert(raw->Read(&current, &error));
    // The rejected report must not have displaced the good route.
    assert(viper::daemon::NormalizeDeviceKey(current)
        == viper::daemon::NormalizeDeviceKey(SpeakerIdentity()));
    assert(!watcher.Poll(&error));
    assert(watcher.CurrentKeyHash() == known);
}

} // namespace

int main() {
    TestRoundTripPreservesEveryField();
    TestPublishIsPrivateAndLeavesNoTemporary();
    TestMissingFileFailsCleanly();
    TestSecondStoreReplacesTheFirst();
    TestStoreRefusesInvalidIdentity();
    TestRefusesUntrustworthyFiles();
    TestAdapterFailsWhileEmpty();
    TestAdapterSeedsFromCache();
    TestAdapterIgnoresUnusableCacheSeed();
    TestReportedRouteTakesEffectThroughWatcher();
    TestAdapterRefusesInvalidReportedRoute();
    std::puts("route cache tests passed");
    return 0;
}
