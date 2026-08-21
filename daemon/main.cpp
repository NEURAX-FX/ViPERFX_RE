#include "DaemonRuntime.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace {

void PrintUsage() {
    std::fputs(
        "usage: viper-daemon [options]\n"
        "  --state-root <path>   snapshot/state directory "
        "(default /data/adb/viper4android)\n"
        "  --socket <name>       abstract driver socket name\n"
        "  --app-socket <name>   abstract app socket name\n"
        "  --owner-socket <name> abstract owner socket name\n"
        "  --owner-dex <path>    owner dex; enables the owner when readable\n"
        "  --no-owner            never spawn or accept an effect owner\n"
        "  --poll-ms <ms>        control loop interval\n"
        "  --iterations <count>  stop after N loop passes (0 = run forever)\n"
        "  --status              print one status line and exit\n"
        "  --help                show this message\n",
        stderr
    );
}

bool ParseUnsigned(const char *text, uint64_t *value) {
    if (text == nullptr || *text == '\0') return false;
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (end == nullptr || *end != '\0') return false;
    *value = static_cast<uint64_t>(parsed);
    return true;
}

} // namespace

int main(int argc, char **argv) {
    viper::daemon::DaemonConfig config{};
    bool status_only = false;
    bool owner_disabled = false;

    for (int index = 1; index < argc; ++index) {
        const char *argument = argv[index];
        const bool has_value = index + 1 < argc;
        if (std::strcmp(argument, "--help") == 0) {
            PrintUsage();
            return 0;
        }
        if (std::strcmp(argument, "--status") == 0) {
            status_only = true;
            continue;
        }
        if (std::strcmp(argument, "--state-root") == 0 && has_value) {
            config.state_root = argv[++index];
            continue;
        }
        if (std::strcmp(argument, "--socket") == 0 && has_value) {
            config.driver_socket_name = argv[++index];
            continue;
        }
        if (std::strcmp(argument, "--app-socket") == 0 && has_value) {
            config.app_socket_name = argv[++index];
            continue;
        }
        if (std::strcmp(argument, "--owner-socket") == 0 && has_value) {
            config.owner_socket_name = argv[++index];
            continue;
        }
        if (std::strcmp(argument, "--owner-dex") == 0 && has_value) {
            // Supplying a dex is the opt-in: an install without the owner keeps the
            // App's legacy backend, so ownership is never enabled implicitly.
            config.owner_dex_path = argv[++index];
            config.owner_enabled = !config.owner_dex_path.empty();
            continue;
        }
        if (std::strcmp(argument, "--no-owner") == 0) {
            // Explicit override, evaluated after parsing so ordering with
            // --owner-dex does not change the outcome.
            owner_disabled = true;
            continue;
        }
        if (std::strcmp(argument, "--poll-ms") == 0 && has_value) {
            uint64_t milliseconds = 0;
            if (!ParseUnsigned(argv[++index], &milliseconds) || milliseconds == 0U) {
                std::fprintf(stderr, "viper-daemon: invalid --poll-ms\n");
                return 2;
            }
            config.poll_interval = std::chrono::milliseconds(milliseconds);
            continue;
        }
        if (std::strcmp(argument, "--iterations") == 0 && has_value) {
            if (!ParseUnsigned(argv[++index], &config.max_iterations)) {
                std::fprintf(stderr, "viper-daemon: invalid --iterations\n");
                return 2;
            }
            continue;
        }
        std::fprintf(stderr, "viper-daemon: unknown argument '%s'\n", argument);
        PrintUsage();
        return 2;
    }

    // Applied after parsing so --no-owner wins regardless of argument order.
    if (owner_disabled) {
        config.owner_enabled = false;
        config.owner_dex_path.clear();
    }

    // Null adapter selects the App-reported route source seeded from RouteCache.
    // AndroidRouteAdapter is not used: it reads /sys/class/switch/h2w/state, which
    // many devices (MTK mt6989 among them) do not expose, leaving the daemon with no
    // route forever. The App can see the real route and reports it over app.v1.
    viper::daemon::DaemonRuntime runtime(config, nullptr);

    std::string error;
    if (!runtime.Start(&error)) {
        std::fprintf(stderr, "viper-daemon: start failed: %s\n", error.c_str());
        return 1;
    }
    // Start() reports a bound driver socket even when the app socket failed, because
    // cache-based restore still works. Surface it: without app.v1 the App can never
    // reach the daemon, which is worth a log line rather than silence.
    if (!error.empty()) {
        std::fprintf(stderr, "viper-daemon: %s\n", error.c_str());
    }

    if (status_only) {
        runtime.RunOnce();
        const viper::daemon::DaemonStatus status = runtime.Status();
        std::printf(
            "driver_connected=%d route_known=%d route_key_hash=%s live_contexts=%zu\n",
            status.driver_connected ? 1 : 0,
            status.route_known ? 1 : 0,
            status.route_key_hash.c_str(),
            status.live_contexts
        );
        return 0;
    }

    viper::daemon::InstallSignalHandlers(&runtime);
    if (!runtime.Run(&error)) {
        std::fprintf(stderr, "viper-daemon: run failed: %s\n", error.c_str());
        return 1;
    }
    return 0;
}
