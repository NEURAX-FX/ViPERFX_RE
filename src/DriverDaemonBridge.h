#pragma once

#include "DriverEvent.h"
#include "SnapshotCommand.h"
#include "ViperDaemonProtocol.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace viper::daemon {

/**
 * Publishes driver lifecycle events to the root daemon over a private,
 * non-blocking abstract SOCK_SEQPACKET socket.
 *
 * Publish() only enqueues into a bounded, preallocated ring. All socket
 * syscalls happen on the bridge thread, so the audio thread never blocks and
 * never touches the socket. Dropping events under pressure is intentional: the
 * daemon recovers lost state via RequestRescan().
 */
class DriverDaemonBridge final {
public:
    static constexpr std::size_t kQueueCapacity = 256;
    using RescanProvider = std::function<void(std::vector<DriverEvent> *)>;

    // Outcome of one snapshot apply command, reported back to the daemon as a
    // SNAPSHOT_APPLIED_ACK or SNAPSHOT_APPLIED_NACK event.
    struct SnapshotAck {
        bool accepted = false;
        uint32_t error_code = 0;
        uint64_t app_generation = 0;
        uint64_t daemon_generation = 0;
        uint64_t resource_generation = 0;
        uint64_t graph_generation = 0;
    };

    // Applies daemon snapshot commands. Invoked on the bridge thread, never on
    // the audio thread. A handler must not block: it holds up event delivery.
    using SnapshotHandler = std::function<SnapshotAck(
        SnapshotCommandType type,
        std::span<const uint8_t> payload
    )>;

    explicit DriverDaemonBridge(std::string socket_name = kDriverSocketName);
    ~DriverDaemonBridge();

    DriverDaemonBridge(const DriverDaemonBridge &) = delete;
    DriverDaemonBridge &operator=(const DriverDaemonBridge &) = delete;

    void SetRescanProvider(RescanProvider provider);
    void SetSnapshotHandler(SnapshotHandler handler);

    void Start();
    void Stop();

    // Non-blocking. Returns false when the bounded queue is full.
    bool Publish(const DriverEvent &event);
    void RequestRescan();

    bool Connected() const noexcept;
    uint64_t DroppedEvents() const noexcept;
    uint64_t BootId() const noexcept { return boot_id_; }

private:
    void RunBridgeThread();
    bool EnsureConnected();
    void CloseConnection();
    bool SendEvent(const DriverEvent &event);
    void DrainQueue();
    void HandleRescan();
    void HandleSnapshotCommand(const FrameHeader &header, std::span<const uint8_t> payload);

    std::string socket_name_;
    uint64_t boot_id_;

    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> rescan_requested_{false};
    std::atomic<uint64_t> dropped_events_{0};
    std::atomic<uint64_t> next_sequence_{1};

    mutable std::mutex mutex_;
    std::condition_variable signal_;
    std::deque<DriverEvent> queue_;

    RescanProvider rescan_provider_;
    SnapshotHandler snapshot_handler_;
    std::thread thread_;
    int socket_fd_ = -1;
    unsigned reconnect_attempts_ = 0;

    // Owned by the bridge thread; heap-allocated to keep the thread stack small.
    std::vector<uint8_t> receive_buffer_;
    std::vector<uint8_t> frame_buffer_;
    std::vector<uint8_t> payload_buffer_;
};

} // namespace viper::daemon
