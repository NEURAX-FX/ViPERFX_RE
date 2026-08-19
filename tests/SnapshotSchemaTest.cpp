#include "SnapshotSchema.h"

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

namespace {

viper::daemon::Snapshot MakeSnapshot() {
    viper::daemon::Snapshot snapshot{};
    snapshot.device_key = "speaker|builtin|internal speaker|48000|3|pcm16|0";
    snapshot.device_key_hash = viper::daemon::HashDeviceKey(snapshot.device_key);
    snapshot.boot_id = 11;
    snapshot.daemon_generation = 12;
    snapshot.app_generation = 8;
    snapshot.created_at_millis = 1700000000000ULL;
    snapshot.master_enabled = true;
    snapshot.global_mode = false;
    snapshot.parameters = {1, 2, 3, 4};
    snapshot.iem_parameters = {9, 8, 7};
    snapshot.resource_generation = 13;
    snapshot.graph_generation = 14;

    viper::daemon::ResourceReference resource{};
    resource.resource_id = "convolver/main";
    resource.content_sha256 = std::string(64, 'a');
    resource.size = 128;
    resource.kind = 2;
    resource.format = 1;
    resource.channels = 2;
    resource.order = 0;
    snapshot.resources.push_back(resource);
    return snapshot;
}

void TestRoundTripAndDeterminism() {
    const auto snapshot = MakeSnapshot();
    std::string error;
    assert(viper::daemon::ValidateSnapshot(snapshot, &error));

    std::vector<uint8_t> first;
    std::vector<uint8_t> second;
    assert(viper::daemon::EncodeSnapshot(snapshot, &first, &error));
    assert(viper::daemon::EncodeSnapshot(snapshot, &second, &error));
    assert(first == second);

    viper::daemon::Snapshot decoded{};
    assert(viper::daemon::DecodeSnapshot(first, &decoded, &error));
    assert(decoded.device_key == snapshot.device_key);
    assert(decoded.device_key_hash == snapshot.device_key_hash);
    assert(decoded.parameters == snapshot.parameters);
    assert(decoded.iem_parameters == snapshot.iem_parameters);
    assert(decoded.resources.size() == 1);
    assert(decoded.resources[0].content_sha256 == snapshot.resources[0].content_sha256);
    assert(decoded.daemon_generation == snapshot.daemon_generation);
}

void TestRejectsInvalidSnapshots() {
    auto snapshot = MakeSnapshot();
    std::string error;

    snapshot.device_key_hash[0] = snapshot.device_key_hash[0] == 'a' ? 'b' : 'a';
    assert(!viper::daemon::ValidateSnapshot(snapshot, &error));
    assert(error == "device_key_hash mismatch");

    snapshot = MakeSnapshot();
    snapshot.daemon_generation = 0;
    assert(!viper::daemon::ValidateSnapshot(snapshot, &error));
    assert(error == "daemon_generation must be non-zero");

    snapshot = MakeSnapshot();
    snapshot.resources[0].content_sha256 = "bad";
    assert(!viper::daemon::ValidateSnapshot(snapshot, &error));
    assert(error == "resource content_sha256 must be 64 hex characters");
}

void TestRejectsTruncatedAndTrailingData() {
    const auto snapshot = MakeSnapshot();
    std::vector<uint8_t> encoded;
    std::string error;
    assert(viper::daemon::EncodeSnapshot(snapshot, &encoded, &error));

    viper::daemon::Snapshot decoded{};
    encoded.pop_back();
    assert(!viper::daemon::DecodeSnapshot(encoded, &decoded, &error));

    assert(viper::daemon::EncodeSnapshot(snapshot, &encoded, &error));
    encoded.push_back(0);
    assert(!viper::daemon::DecodeSnapshot(encoded, &decoded, &error));
}

} // namespace

int main() {
    TestRoundTripAndDeterminism();
    TestRejectsInvalidSnapshots();
    TestRejectsTruncatedAndTrailingData();
    return 0;
}
