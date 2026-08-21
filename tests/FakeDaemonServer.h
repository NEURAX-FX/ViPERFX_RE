#pragma once

#include "DriverDaemonBridge.h"
#include "ViperDaemonProtocol.h"

#include <chrono>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace viper::test {

inline std::string UniqueSocketName(const char *suffix) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::string("viper4android.test.") + suffix + "." + std::to_string(stamp);
}

// Minimal abstract-namespace SOCK_SEQPACKET server used to observe bridge output.
class FakeDaemonServer final {
public:
    explicit FakeDaemonServer(std::string socket_name)
        : socket_name_(std::move(socket_name)) {}

    ~FakeDaemonServer() { Close(); }

    FakeDaemonServer(const FakeDaemonServer &) = delete;
    FakeDaemonServer &operator=(const FakeDaemonServer &) = delete;

    bool Listen() {
        listen_fd_ = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (listen_fd_ < 0) return false;

        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        address.sun_path[0] = '\0';
        std::memcpy(address.sun_path + 1, socket_name_.data(), socket_name_.size());
        const socklen_t length = static_cast<socklen_t>(
            offsetof(sockaddr_un, sun_path) + 1U + socket_name_.size());
        if (::bind(listen_fd_, reinterpret_cast<sockaddr *>(&address), length) != 0) {
            Close();
            return false;
        }
        return ::listen(listen_fd_, 4) == 0;
    }

    bool Accept(std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            const int fd =
                ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (fd >= 0) {
                client_fd_ = fd;
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }

    // Reads one framed driver event, skipping the handshake, or times out.
    bool ReceiveEvent(
        viper::daemon::DriverEvent *event,
        std::chrono::milliseconds timeout
    ) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::vector<uint8_t> buffer(viper::daemon::kMaxFrameSize);
        while (std::chrono::steady_clock::now() < deadline) {
            const ssize_t received =
                ::recv(client_fd_, buffer.data(), buffer.size(), MSG_DONTWAIT);
            if (received > 0) {
                viper::daemon::FrameHeader header{};
                std::vector<uint8_t> payload;
                std::string error;
                const std::span<const uint8_t> bytes(
                    buffer.data(), static_cast<std::size_t>(received));
                if (!viper::daemon::DecodeFrame(bytes, &header, &payload, &error)) {
                    return false;
                }
                if (!viper::daemon::DecodeDriverEvent(payload, event, &error)) return false;
                if (event->type == viper::daemon::DriverEventType::DRIVER_HELLO) continue;
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }

    // Sends a framed daemon-to-driver command payload.
    bool SendCommand(uint16_t message_type, std::span<const uint8_t> payload,
                     uint64_t request_id = 0) {
        viper::daemon::FrameHeader header{};
        header.message_type = message_type;
        header.request_id = request_id;
        const std::string_view view(
            reinterpret_cast<const char *>(payload.data()), payload.size());
        std::vector<uint8_t> frame;
        std::string error;
        if (!viper::daemon::EncodeFrame(header, view, &frame, &error)) return false;
        return ::send(client_fd_, frame.data(), frame.size(), 0)
            == static_cast<ssize_t>(frame.size());
    }

    void DropClient() {
        if (client_fd_ >= 0) {
            ::close(client_fd_);
            client_fd_ = -1;
        }
    }

    void Close() {
        DropClient();
        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
    }

private:
    std::string socket_name_;
    int listen_fd_ = -1;
    int client_fd_ = -1;
};

} // namespace viper::test
