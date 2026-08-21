#include "DriverDaemonBridge.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <utility>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace viper::daemon {
namespace {

constexpr std::chrono::milliseconds kIdlePoll{20};
constexpr std::chrono::milliseconds kReconnectBaseDelay{25};
constexpr std::chrono::milliseconds kReconnectMaxDelay{400};

// Boot identity keeps the daemon from trusting snapshots across reboots.
uint64_t ResolveBootId() {
    std::ifstream input("/proc/sys/kernel/random/boot_id");
    std::string text;
    if (input && std::getline(input, text) && !text.empty()) {
        uint64_t hash = 1469598103934665603ULL;
        for (const unsigned char byte : text) {
            hash ^= byte;
            hash *= 1099511628211ULL;
        }
        if (hash != 0U) return hash;
    }
    const auto fallback = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return fallback == 0U ? 1U : fallback;
}

} // namespace

DriverDaemonBridge::DriverDaemonBridge(std::string socket_name)
    : socket_name_(std::move(socket_name)), boot_id_(ResolveBootId()) {}

DriverDaemonBridge::~DriverDaemonBridge() { Stop(); }

void DriverDaemonBridge::SetRescanProvider(RescanProvider provider) {
    std::lock_guard<std::mutex> lock(mutex_);
    rescan_provider_ = std::move(provider);
}

void DriverDaemonBridge::SetSnapshotHandler(SnapshotHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_handler_ = std::move(handler);
}

void DriverDaemonBridge::Start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    receive_buffer_.assign(kMaxFrameSize, 0U);
    frame_buffer_.reserve(kFrameHeaderSize + kDriverEventWireSize);
    payload_buffer_.reserve(kDriverEventWireSize);
    thread_ = std::thread(&DriverDaemonBridge::RunBridgeThread, this);
}

void DriverDaemonBridge::Stop() {
    if (!running_.exchange(false)) return;
    signal_.notify_all();
    if (thread_.joinable()) thread_.join();
    CloseConnection();
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
}

bool DriverDaemonBridge::Connected() const noexcept {
    return connected_.load(std::memory_order_acquire);
}

uint64_t DriverDaemonBridge::DroppedEvents() const noexcept {
    return dropped_events_.load(std::memory_order_relaxed);
}

bool DriverDaemonBridge::Publish(const DriverEvent &event) {
    DriverEvent stamped = event;
    stamped.boot_id = boot_id_;
    stamped.event_sequence = next_sequence_.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= kQueueCapacity) {
            dropped_events_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        queue_.push_back(stamped);
    }
    signal_.notify_one();
    return true;
}

void DriverDaemonBridge::RequestRescan() {
    rescan_requested_.store(true, std::memory_order_release);
    signal_.notify_one();
}

void DriverDaemonBridge::RunBridgeThread() {
    while (running_.load(std::memory_order_acquire)) {
        if (!EnsureConnected()) {
            const auto delay = std::min(
                kReconnectMaxDelay,
                kReconnectBaseDelay * (reconnect_attempts_ == 0U ? 1U : reconnect_attempts_)
            );
            std::unique_lock<std::mutex> lock(mutex_);
            signal_.wait_for(lock, delay, [this] {
                return !running_.load(std::memory_order_acquire);
            });
            continue;
        }

        HandleRescan();
        DrainQueue();

        // Detect daemon restarts: a closed peer reports POLLHUP or EOF.
        pollfd descriptor{};
        descriptor.fd = socket_fd_;
        descriptor.events = POLLIN;
        const int ready = ::poll(&descriptor, 1, 0);
        if (ready > 0 && (descriptor.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
            CloseConnection();
            continue;
        }
        if (ready > 0 && (descriptor.revents & POLLIN) != 0) {
            const ssize_t received = ::recv(
                socket_fd_, receive_buffer_.data(), receive_buffer_.size(), MSG_DONTWAIT);
            if (received == 0) {
                CloseConnection();
                continue;
            }
            if (received > 0) {
                FrameHeader header{};
                std::string error;
                const std::span<const uint8_t> bytes(
                    receive_buffer_.data(), static_cast<std::size_t>(received));
                if (DecodeFrame(bytes, &header, &payload_buffer_, &error)) {
                    if (header.message_type
                        == static_cast<uint16_t>(DriverEventType::RESCAN_RESPONSE)) {
                        rescan_requested_.store(true, std::memory_order_release);
                    } else if (IsSnapshotCommandType(header.message_type)) {
                        HandleSnapshotCommand(header, payload_buffer_);
                    }
                }
            }
        }

        std::unique_lock<std::mutex> lock(mutex_);
        signal_.wait_for(lock, kIdlePoll, [this] {
            return !running_.load(std::memory_order_acquire)
                || !queue_.empty()
                || rescan_requested_.load(std::memory_order_acquire);
        });
    }
}

bool DriverDaemonBridge::EnsureConnected() {
    if (socket_fd_ >= 0) return true;

    const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        ++reconnect_attempts_;
        return false;
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    address.sun_path[0] = '\0';
    const std::size_t name_length = std::min(
        socket_name_.size(), sizeof(address.sun_path) - 1U);
    std::memcpy(address.sun_path + 1, socket_name_.data(), name_length);
    const socklen_t length = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + 1U + name_length);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&address), length) != 0) {
        ::close(fd);
        ++reconnect_attempts_;
        return false;
    }

    socket_fd_ = fd;
    reconnect_attempts_ = 0;
    connected_.store(true, std::memory_order_release);

    DriverEvent hello{};
    hello.type = DriverEventType::DRIVER_HELLO;
    hello.boot_id = boot_id_;
    hello.event_sequence = next_sequence_.fetch_add(1, std::memory_order_relaxed);
    if (!SendEvent(hello)) return false;

    // A fresh daemon connection has no session state, so replay live contexts.
    rescan_requested_.store(true, std::memory_order_release);
    return true;
}

void DriverDaemonBridge::CloseConnection() {
    if (socket_fd_ >= 0) {
        ::close(socket_fd_);
        socket_fd_ = -1;
    }
    connected_.store(false, std::memory_order_release);
}

bool DriverDaemonBridge::SendEvent(const DriverEvent &event) {
    if (socket_fd_ < 0) return false;

    std::string error;
    if (!EncodeDriverEvent(event, &payload_buffer_, &error)) return true; // Drop malformed event.

    FrameHeader header{};
    header.message_type = static_cast<uint16_t>(event.type);
    header.request_id = event.context_instance_id;
    header.sequence = event.event_sequence;
    const std::string_view payload_view(
        reinterpret_cast<const char *>(payload_buffer_.data()), payload_buffer_.size());
    if (!EncodeFrame(header, payload_view, &frame_buffer_, &error)) return true;

    const std::vector<uint8_t> &frame = frame_buffer_;
    while (true) {
        const ssize_t sent = ::send(socket_fd_, frame.data(), frame.size(), MSG_DONTWAIT);
        if (sent == static_cast<ssize_t>(frame.size())) return true;
        if (sent < 0 && errno == EINTR) continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            dropped_events_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        CloseConnection();
        return false;
    }
}

void DriverDaemonBridge::DrainQueue() {
    while (running_.load(std::memory_order_acquire) && socket_fd_ >= 0) {
        DriverEvent event{};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.empty()) return;
            event = queue_.front();
            queue_.pop_front();
        }
        if (!SendEvent(event)) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.size() < kQueueCapacity) {
                queue_.push_front(event);
            } else {
                dropped_events_.fetch_add(1, std::memory_order_relaxed);
            }
            return;
        }
    }
}

void DriverDaemonBridge::HandleRescan() {
    if (!rescan_requested_.exchange(false, std::memory_order_acq_rel)) return;

    RescanProvider provider;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        provider = rescan_provider_;
    }
    if (!provider) return;

    std::vector<DriverEvent> events;
    provider(&events);
    for (auto &event : events) {
        event.boot_id = boot_id_;
        event.event_sequence = next_sequence_.fetch_add(1, std::memory_order_relaxed);
        if (!SendEvent(event)) return;
    }
}

void DriverDaemonBridge::HandleSnapshotCommand(
    const FrameHeader &header,
    std::span<const uint8_t> payload
) {
    SnapshotHandler handler;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        handler = snapshot_handler_;
    }

    const auto type = static_cast<SnapshotCommandType>(header.message_type);
    SnapshotAck ack{};
    if (handler) {
        ack = handler(type, payload);
    }
    // No handler means the driver cannot apply snapshots; NACK rather than let the
    // daemon believe its state landed.

    // Chunks are acknowledged only when they fail: ACKing every chunk would cost
    // one frame per 64 KiB with nothing actionable in it.
    if (type == SnapshotCommandType::SNAPSHOT_CHUNK && ack.accepted) return;

    DriverEvent response{};
    response.type = ack.accepted
        ? DriverEventType::SNAPSHOT_APPLIED_ACK
        : DriverEventType::SNAPSHOT_APPLIED_NACK;
    // Correlate with the daemon's request rather than a driver context.
    response.context_instance_id = header.request_id;
    response.session_generation = ack.app_generation;
    response.resource_generation = ack.resource_generation;
    response.graph_generation = ack.graph_generation;
    response.bypass_reason = ack.error_code;
    response.boot_id = boot_id_;
    response.event_sequence = next_sequence_.fetch_add(1, std::memory_order_relaxed);
    // Sent directly instead of queued: an apply result is only useful in-order
    // with the command that produced it.
    SendEvent(response);
}

} // namespace viper::daemon
