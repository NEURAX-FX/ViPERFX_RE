#include "SnapshotStore.h"

#include <cerrno>
#include <cctype>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>

namespace viper::daemon {
namespace {

constexpr std::string_view kCurrentName = "current.snapshot";
constexpr std::string_view kPreviousName = "previous.snapshot";
constexpr std::string_view kTemporaryName = "current.snapshot.tmp";
constexpr std::string_view kPreviousTemporaryName = "previous.snapshot.tmp";

void SetError(std::string *error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}

bool IsSafeDeviceHash(std::string_view value) noexcept {
    if (value.size() != 64U) return false;
    for (const unsigned char byte : value) {
        if ((byte < '0' || byte > '9') && (byte < 'a' || byte > 'f')) return false;
    }
    return true;
}

bool SetPrivateMode(const std::filesystem::path &path, mode_t mode) noexcept {
    return ::chmod(path.c_str(), mode) == 0;
}

bool FsyncPath(const std::filesystem::path &path, std::string *error) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        SetError(error, "failed to open file for fsync");
        return false;
    }
    const bool synced = ::fsync(fd) == 0;
    const int close_result = ::close(fd);
    if (!synced || close_result != 0) {
        SetError(error, "failed to fsync snapshot file");
        return false;
    }
    return true;
}

bool FsyncDirectory(const std::filesystem::path &path, std::string *error) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        SetError(error, "failed to open snapshot directory for fsync");
        return false;
    }
    const bool synced = ::fsync(fd) == 0;
    const int close_result = ::close(fd);
    if (!synced || close_result != 0) {
        SetError(error, "failed to fsync snapshot directory");
        return false;
    }
    return true;
}

bool ReadFile(
    const std::filesystem::path &path,
    std::vector<uint8_t> *bytes,
    std::string *error
) {
    std::error_code filesystem_error;
    if (!std::filesystem::exists(path, filesystem_error)) {
        SetError(error, "snapshot missing");
        return false;
    }
    const auto file_size = std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error || file_size > kMaxSnapshotSize) {
        SetError(error, "snapshot file is too large");
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        SetError(error, "failed to open snapshot");
        return false;
    }
    bytes->resize(static_cast<std::size_t>(file_size));
    if (!bytes->empty()) {
        input.read(reinterpret_cast<char *>(bytes->data()),
            static_cast<std::streamsize>(bytes->size()));
    }
    if (!input && !input.eof()) {
        SetError(error, "failed to read snapshot");
        return false;
    }
    return true;
}

bool WriteFile(
    const std::filesystem::path &path,
    std::span<const uint8_t> bytes,
    std::string *error
) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        SetError(error, "failed to create snapshot temporary file");
        return false;
    }
    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char *>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }
    output.flush();
    if (!output) {
        SetError(error, "failed to write snapshot temporary file");
        return false;
    }
    output.close();
    if (!SetPrivateMode(path, 0600)) {
        SetError(error, "failed to set snapshot permissions");
        return false;
    }
    return FsyncPath(path, error);
}

bool RemoveIfExists(const std::filesystem::path &path, std::string *error) {
    std::error_code filesystem_error;
    if (std::filesystem::remove(path, filesystem_error) || !filesystem_error) return true;
    SetError(error, "failed to remove snapshot temporary file");
    return false;
}

} // namespace

SnapshotStore::SnapshotStore(std::filesystem::path root) : root_(std::move(root)) {}

std::filesystem::path SnapshotStore::RouteDirectory(std::string_view device_hash) const {
    return root_ / "routes" / std::string(device_hash);
}

bool SnapshotStore::EnsureRouteDirectory(
    std::string_view device_hash,
    std::filesystem::path *route_directory,
    std::string *error
) const {
    if (route_directory == nullptr) {
        SetError(error, "null route directory");
        return false;
    }
    if (!IsSafeDeviceHash(device_hash)) {
        SetError(error, "device hash is invalid");
        return false;
    }

    std::error_code filesystem_error;
    *route_directory = RouteDirectory(device_hash);
    std::filesystem::create_directories(*route_directory, filesystem_error);
    if (filesystem_error) {
        SetError(error, "failed to create snapshot directory");
        return false;
    }
    SetPrivateMode(root_, 0700);
    SetPrivateMode(root_ / "routes", 0700);
    SetPrivateMode(*route_directory, 0700);
    return true;
}

bool SnapshotStore::LoadNamed(
    std::string_view device_hash,
    std::string_view filename,
    Snapshot *snapshot,
    std::string *error
) const {
    if (snapshot == nullptr) {
        SetError(error, "null snapshot output");
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!IsSafeDeviceHash(device_hash)) {
        SetError(error, "device hash is invalid");
        return false;
    }
    std::vector<uint8_t> bytes;
    if (!ReadFile(RouteDirectory(device_hash) / std::string(filename), &bytes, error)) {
        return false;
    }
    return DecodeSnapshot(bytes, snapshot, error);
}

bool SnapshotStore::LoadCurrent(
    std::string_view device_hash,
    Snapshot *snapshot,
    std::string *error
) const {
    return LoadNamed(device_hash, kCurrentName, snapshot, error);
}

bool SnapshotStore::LoadPrevious(
    std::string_view device_hash,
    Snapshot *snapshot,
    std::string *error
) const {
    return LoadNamed(device_hash, kPreviousName, snapshot, error);
}

bool SnapshotStore::Commit(
    std::string_view device_hash,
    const Snapshot &snapshot,
    std::string *error
) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!IsSafeDeviceHash(device_hash)) {
        SetError(error, "device hash is invalid");
        return false;
    }
    if (snapshot.device_key_hash != device_hash) {
        SetError(error, "device hash does not match snapshot");
        return false;
    }
    if (!ValidateSnapshot(snapshot, error)) return false;

    std::vector<uint8_t> encoded;
    if (!EncodeSnapshot(snapshot, &encoded, error)) return false;

    std::filesystem::path route_directory;
    if (!EnsureRouteDirectory(device_hash, &route_directory, error)) return false;
    const auto current = route_directory / std::string(kCurrentName);
    const auto previous = route_directory / std::string(kPreviousName);
    const auto temporary = route_directory / std::string(kTemporaryName);
    const auto previous_temporary = route_directory / std::string(kPreviousTemporaryName);

    if (!RemoveIfExists(temporary, error) || !RemoveIfExists(previous_temporary, error)) {
        return false;
    }
    if (!WriteFile(temporary, encoded, error)) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }

    Snapshot old_snapshot{};
    std::string old_error;
    std::vector<uint8_t> old_bytes;
    const bool has_valid_current =
        ReadFile(current, &old_bytes, &old_error)
        && DecodeSnapshot(old_bytes, &old_snapshot, &old_error);
    if (has_valid_current) {
        std::error_code copy_error;
        std::filesystem::copy_file(
            current,
            previous_temporary,
            std::filesystem::copy_options::overwrite_existing,
            copy_error
        );
        if (copy_error || !SetPrivateMode(previous_temporary, 0600)
            || !FsyncPath(previous_temporary, error)) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            std::filesystem::remove(previous_temporary, ignored);
            if (copy_error) SetError(error, "failed to stage previous snapshot");
            return false;
        }
        if (::rename(previous_temporary.c_str(), previous.c_str()) != 0) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            std::filesystem::remove(previous_temporary, ignored);
            SetError(error, "failed to rotate previous snapshot");
            return false;
        }
    }

    if (::rename(temporary.c_str(), current.c_str()) != 0) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        SetError(error, "failed to publish current snapshot");
        return false;
    }
    if (!FsyncDirectory(route_directory, error)) return false;
    if (error != nullptr) error->clear();
    return true;
}

} // namespace viper::daemon
