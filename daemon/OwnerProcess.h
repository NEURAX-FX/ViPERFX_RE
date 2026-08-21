#pragma once

#include "OwnerSupervisor.h"

#include <filesystem>
#include <memory>
#include <string>

namespace viper::daemon {

/**
 * Real owner spawner: execs `app_process64` with the owner dex on CLASSPATH.
 *
 * `socket_name` is passed to the owner as argv, because the daemon's endpoint is
 * configurable and an owner hardcoding the default could never reach a daemon
 * running on any other socket.
 *
 * An empty `dex_path` produces an adapter that never spawns, so a daemon on an
 * install without the owner dex adopts an externally started owner at most and
 * otherwise leaves the App's legacy backend in charge.
 */
std::unique_ptr<OwnerProcessAdapter> MakeOwnerProcessAdapter(
    std::filesystem::path dex_path,
    std::string socket_name
);

} // namespace viper::daemon
