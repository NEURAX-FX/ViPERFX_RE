#pragma once

#include "DriverEvent.h"
#include "SessionRegistry.h"
#include "SnapshotCommand.h"

#include <cstdint>
#include <span>
#include <functional>
#include <string>
#include <vector>

namespace viper::daemon {

/**
 * Accepts the driver bridge on the private abstract SOCK_SEQPACKET socket and
 * feeds validated events into a SessionRegistry.
 *
 * Single-threaded and non-blocking: the daemon control loop calls Poll(). The
 * server never creates or releases an AudioEffect; it only observes.
 */
class DriverEventServer final {
public:
    struct Stats {
        uint64_t accepted_connections = 0;
        uint64_t applied_events = 0;
        uint64_t rejected_frames = 0;
        uint64_t rescan_requests_sent = 0;
        uint64_t snapshot_commands_sent = 0;
        // Route announces carry no snapshot bytes, so they are counted apart.
        uint64_t route_announces_sent = 0;
        uint64_t apply_acks = 0;
        uint64_t apply_nacks = 0;
    };

    // Last apply result reported by the driver, matched by request id.
    struct ApplyOutcome {
        bool valid = false;
        bool accepted = false;
        uint64_t request_id = 0;
        uint32_t error_code = 0;
        uint64_t app_generation = 0;
        uint64_t resource_generation = 0;
        uint64_t graph_generation = 0;
    };

    explicit DriverEventServer(std::string socket_name = kDriverSocketName);
    ~DriverEventServer();

    DriverEventServer(const DriverEventServer &) = delete;
    DriverEventServer &operator=(const DriverEventServer &) = delete;

    bool Listen(std::string *error);
    void Close();

    // Processes at most `max_events` pending frames. Returns the number applied.
    std::size_t Poll(SessionRegistry *registry, std::size_t max_events = 64);

    // Asks the driver to replay its live contexts.
    bool RequestRescan();

    // Sends one snapshot apply command. `payload` must already be encoded by the
    // matching SnapshotCommand encoder.
    bool SendSnapshotCommand(
        SnapshotCommandType type,
        std::span<const uint8_t> payload,
        uint64_t request_id
    );

    // Consumes the last apply result, if one arrived since the previous call.
    ApplyOutcome TakeApplyOutcome() noexcept;

    bool Listening() const noexcept { return listen_fd_ >= 0; }
    bool Connected() const noexcept { return client_fd_ >= 0; }
    const Stats &Statistics() const noexcept { return stats_; }
    const std::string &SocketName() const noexcept { return socket_name_; }

private:
    bool AcceptPending();
    void DropClient();

    std::string socket_name_;
    int listen_fd_ = -1;
    int client_fd_ = -1;
    Stats stats_{};
    std::vector<uint8_t> receive_buffer_;
    std::vector<uint8_t> payload_buffer_;
    std::vector<uint8_t> frame_buffer_;
    ApplyOutcome pending_outcome_{};
};

} // namespace viper::daemon
