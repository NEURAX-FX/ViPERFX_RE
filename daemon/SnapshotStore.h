#pragma once

#include "SnapshotSchema.h"

#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

namespace viper::daemon {

class SnapshotStore final {
public:
    explicit SnapshotStore(std::filesystem::path root);

    bool LoadCurrent(
        std::string_view device_hash,
        Snapshot *snapshot,
        std::string *error
    ) const;

    bool LoadPrevious(
        std::string_view device_hash,
        Snapshot *snapshot,
        std::string *error
    ) const;

    bool Commit(
        std::string_view device_hash,
        const Snapshot &snapshot,
        std::string *error
    );

private:
    bool LoadNamed(
        std::string_view device_hash,
        std::string_view filename,
        Snapshot *snapshot,
        std::string *error
    ) const;

    std::filesystem::path RouteDirectory(std::string_view device_hash) const;
    bool EnsureRouteDirectory(
        std::string_view device_hash,
        std::filesystem::path *route_directory,
        std::string *error
    ) const;

    std::filesystem::path root_;
    mutable std::mutex mutex_;
};

} // namespace viper::daemon
