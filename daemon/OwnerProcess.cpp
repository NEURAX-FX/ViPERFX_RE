#include "OwnerProcess.h"

#include <cerrno>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
namespace viper::daemon {
namespace {

constexpr const char *kAppProcess64 = "/system/bin/app_process64";
constexpr const char *kOwnerClass = "com.llsl.viper4android.owner.OwnerMain";

void SetError(std::string *error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}

/**
 * Spawns the owner with fork+exec.
 *
 * posix_spawn would be tidier but is only available from API 28, and this module
 * targets API 21. Socket leakage into the child is prevented at creation instead:
 * every daemon socket is opened with SOCK_CLOEXEC, so exec closes them.
 *
 * The child inherits Android's ART environment (`BOOTCLASSPATH`, `ANDROID_ROOT`,
 * etc.). Only CLASSPATH is replaced with the owner dex. Passing a one-entry
 * environment caused app_process to exit before Java started, because those ART
 * variables are not optional on the device.
 */
class AppProcessOwnerAdapter final : public OwnerProcessAdapter {
public:
    AppProcessOwnerAdapter(std::filesystem::path dex_path, std::string socket_name)
        : dex_path_(std::move(dex_path)), socket_name_(std::move(socket_name)) {}

    int Spawn(std::string *error) override {
        if (dex_path_.empty()) {
            SetError(error, "owner dex path is not configured");
            return -1;
        }
        std::error_code code;
        if (!std::filesystem::exists(dex_path_, code) || code) {
            SetError(error, "owner dex is missing: " + dex_path_.string());
            return -1;
        }

        // app_process takes a parent directory for its own bookkeeping; the class is
        // resolved from CLASSPATH, which is why the dex goes there rather than argv.
        std::string dex_dir = dex_path_.parent_path().string();
        std::string binary = kAppProcess64;
        std::string owner_class = kOwnerClass;
        std::string socket_argument = socket_name_;

        char *arguments[] = {
            binary.data(),
            dex_dir.data(),
            owner_class.data(),
            socket_argument.data(),
            nullptr};

        // Build a child environment from environ, replacing any inherited
        // CLASSPATH entry. Keep BOOTCLASSPATH and the Android runtime variables.
        std::vector<std::string> environment_storage;
        for (char **entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
            std::string value(*entry);
            if (value.rfind("CLASSPATH=", 0) == 0) continue;
            environment_storage.push_back(std::move(value));
        }
        environment_storage.push_back("CLASSPATH=" + dex_path_.string());
        std::vector<char *> environment;
        environment.reserve(environment_storage.size() + 1U);
        for (std::string &entry : environment_storage) environment.push_back(entry.data());
        environment.push_back(nullptr);

        const pid_t pid = ::fork();
        if (pid < 0) {
            SetError(error, "failed to fork owner process");
            return -1;
        }
        if (pid == 0) {
            // Child. Only async-signal-safe work here: the parent may hold locks
            // this process can never see released.
            ::execve(kAppProcess64, arguments, environment.data());
            // exec only returns on failure, and the child must not run any of the
            // daemon's cleanup paths.
            ::_exit(127);
        }
        if (error != nullptr) error->clear();
        return static_cast<int>(pid);
    }

    bool IsAlive(int pid) const override {
        if (pid <= 0) return false;
        // Reap first: a zombie answers signal 0 as if it were alive, which would
        // make a crashed owner look healthy forever.
        int status = 0;
        if (::waitpid(static_cast<pid_t>(pid), &status, WNOHANG)
            == static_cast<pid_t>(pid)) {
            return false;
        }
        return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
    }

    void Kill(int pid) override {
        if (pid <= 0) return;
        ::kill(static_cast<pid_t>(pid), SIGTERM);
        // Reap so the pid is not left a zombie that IsAlive() would misread.
        int status = 0;
        ::waitpid(static_cast<pid_t>(pid), &status, WNOHANG);
    }

private:
    std::filesystem::path dex_path_;
    std::string socket_name_;
};

} // namespace

std::unique_ptr<OwnerProcessAdapter> MakeOwnerProcessAdapter(
    std::filesystem::path dex_path,
    std::string socket_name
) {
    return std::make_unique<AppProcessOwnerAdapter>(
        std::move(dex_path), std::move(socket_name));
}

} // namespace viper::daemon
