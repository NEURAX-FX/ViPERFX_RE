#include "SnapshotStore.h"

#include "DeviceKey.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

namespace {

viper::daemon::Snapshot MakeSnapshot(
    std::string device_key,
    uint64_t daemon_generation,
    uint64_t app_generation
) {
    viper::daemon::Snapshot snapshot{};
    snapshot.device_key = std::move(device_key);
    snapshot.device_key_hash = viper::daemon::HashDeviceKey(snapshot.device_key);
    snapshot.boot_id = 7;
    snapshot.daemon_generation = daemon_generation;
    snapshot.app_generation = app_generation;
    snapshot.created_at_millis = 1700000000000ULL + daemon_generation;
    snapshot.parameters = {1, 2, 3};
    return snapshot;
}

class TempDirectory final {
public:
    TempDirectory() {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path()
            / ("viper-daemon-store-test-" + std::to_string(suffix));
        std::filesystem::create_directories(path_);
    }

    ~TempDirectory() { std::error_code error; std::filesystem::remove_all(path_, error); }
    const std::filesystem::path &path() const { return path_; }

private:
    std::filesystem::path path_;
};

void TestCommitRotatesPreviousAndKeepsDevicesSeparate() {
    TempDirectory directory;
    viper::daemon::SnapshotStore store(directory.path());
    const std::string speaker = "speaker|builtin|internal speaker|48000|3|pcm16|0";
    const std::string usb = "usb|card=2;device=0;port=usb-1|usb dac|48000|3|pcm16|0";
    const auto speaker_hash = viper::daemon::HashDeviceKey(speaker);
    const auto usb_hash = viper::daemon::HashDeviceKey(usb);
    std::string error;

    auto first = MakeSnapshot(speaker, 1, 1);
    auto second = MakeSnapshot(speaker, 2, 2);
    auto other = MakeSnapshot(usb, 3, 1);
    assert(store.Commit(speaker_hash, first, &error));
    assert(store.Commit(speaker_hash, second, &error));
    assert(store.Commit(usb_hash, other, &error));

    viper::daemon::Snapshot current{};
    viper::daemon::Snapshot previous{};
    assert(store.LoadCurrent(speaker_hash, &current, &error));
    assert(store.LoadPrevious(speaker_hash, &previous, &error));
    assert(current.daemon_generation == 2);
    assert(previous.daemon_generation == 1);

    assert(store.LoadCurrent(usb_hash, &current, &error));
    assert(current.device_key == usb);
    assert(!store.LoadPrevious(usb_hash, &previous, &error));
}

void TestCorruptCurrentCanFallBackToPrevious() {
    TempDirectory directory;
    viper::daemon::SnapshotStore store(directory.path());
    const std::string key = "speaker|builtin|internal speaker|48000|3|pcm16|0";
    const auto hash = viper::daemon::HashDeviceKey(key);
    std::string error;
    assert(store.Commit(hash, MakeSnapshot(key, 1, 1), &error));
    assert(store.Commit(hash, MakeSnapshot(key, 2, 2), &error));

    const auto current_path = directory.path() / "routes" / hash / "current.snapshot";
    std::ofstream corrupt(current_path, std::ios::binary | std::ios::trunc);
    corrupt << "corrupt";
    corrupt.close();

    viper::daemon::Snapshot decoded{};
    assert(!store.LoadCurrent(hash, &decoded, &error));
    assert(store.LoadPrevious(hash, &decoded, &error));
    assert(decoded.daemon_generation == 1);
}

void TestCommitRejectsWrongDeviceAndUnsafeHash() {
    TempDirectory directory;
    viper::daemon::SnapshotStore store(directory.path());
    const std::string key = "speaker|builtin|internal speaker|48000|3|pcm16|0";
    const auto valid_hash = viper::daemon::HashDeviceKey(key);
    std::string error;
    const auto snapshot = MakeSnapshot(key, 1, 1);
    assert(!store.Commit(std::string(64, 'b'), snapshot, &error));
    assert(error == "device hash does not match snapshot");
    assert(!store.Commit("../escape", snapshot, &error));
    assert(error == "device hash is invalid");
    assert(store.Commit(valid_hash, snapshot, &error));
}

} // namespace

int main() {
    TestCommitRotatesPreviousAndKeepsDevicesSeparate();
    TestCorruptCurrentCanFallBackToPrevious();
    TestCommitRejectsWrongDeviceAndUnsafeHash();
    return 0;
}
