#pragma once

#include "AppCommand.h"
#include "SnapshotCommand.h"
#include "ViperDaemonProtocol.h"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace viper::daemon {

/**
 * Accepts the App on the private abstract SOCK_SEQPACKET socket
 * @viper4android.app.v1 and hands decoded messages to a Delegate.
 *
 * Separate from DriverEventServer on purpose: the driver socket only admits
 * root/audioserver because a driver event is trusted lifecycle data, while this
 * socket must be reachable by an ordinary app uid.
 *
 * Single-threaded and non-blocking: the daemon control loop calls Poll(). Every
 * request produces exactly one reply frame carrying the request's request_id.
 */
class AppEventServer final {
public:
    struct Stats {
        uint64_t accepted_connections = 0;
        uint64_t rejected_frames = 0;
        uint64_t rejected_peers = 0;
        uint64_t hellos = 0;
        uint64_t route_reports = 0;
        uint64_t snapshot_commands = 0;
        uint64_t replies_sent = 0;
    };

    // SO_PEERCRED of a connecting peer, kept free of <sys/socket.h> so callers
    // can supply an admission rule without pulling in socket headers.
    struct PeerCredentials {
        uint32_t uid = 0;
        uint32_t gid = 0;
        int32_t pid = 0;
    };

    using AllowedPeer = std::function<bool(const PeerCredentials &)>;

    // Implemented by the daemon runtime, which owns the route cache, the
    // generation arbiter and the driver connection.
    struct Delegate {
        Delegate() = default;
        virtual ~Delegate() = default;

        Delegate(const Delegate &) = delete;
        Delegate &operator=(const Delegate &) = delete;

        // Reports the daemon's current generation, route and capability flags.
        virtual AppHelloAck OnHello(const AppHello &hello) = 0;

        // Validates and caches the reported route. Returning false makes the
        // server answer with a rejecting ack; `error` is for daemon logging.
        virtual bool OnRouteReport(
            const AppRouteReport &report,
            AppRouteAck *ack,
            std::string *error
        ) = 0;

        // Forwards one snapshot command to the driver. `payload` is the already
        // decoded-and-validated wire payload of the matching SnapshotCommand
        // codec, so the delegate can relay it verbatim.
        virtual bool OnSnapshotCommand(
            SnapshotCommandType type,
            std::span<const uint8_t> payload,
            AppApplyResult *result
        ) = 0;
    };

    explicit AppEventServer(std::string socket_name = kAppSocketName);
    ~AppEventServer();

    AppEventServer(const AppEventServer &) = delete;
    AppEventServer &operator=(const AppEventServer &) = delete;

    bool Listen(std::string *error);
    void Close();

    void SetDelegate(Delegate *delegate) noexcept { delegate_ = delegate; }

    // Replaces the admission rule. An empty predicate restores the default.
    void SetAllowedPeer(AllowedPeer predicate);

    // Accepts root, this process's own uid, and any Android app uid: appid
    // 10000-19999 in any user profile (uid = user * 100000 + appid).
    //
    // WHY: the threat is a third-party app forging route reports or streaming a
    // snapshot into the root daemon, so system uids (1000 system, 1041
    // audioserver, 2000 shell) and isolated processes (appid 99000+) are refused
    // outright. Pinning a single uid would be tighter, but the App's uid is
    // assigned at install time and differs per device and per work profile, so a
    // compile-time value would lock the real App out; the daemon narrows this
    // further via SetAllowedPeer once it has resolved the App's package uid.
    static bool DefaultAllowedPeer(const PeerCredentials &credentials) noexcept;

    // Processes at most `max_messages` pending frames. Returns the number of
    // messages dispatched to the delegate.
    std::size_t Poll(std::size_t max_messages = 64);

    bool Listening() const noexcept { return listen_fd_ >= 0; }
    bool Connected() const noexcept { return client_fd_ >= 0; }
    const Stats &Statistics() const noexcept { return stats_; }
    const std::string &SocketName() const noexcept { return socket_name_; }

private:
    bool AcceptPending();
    void DropClient();
    bool HandleFrame(const FrameHeader &header, std::span<const uint8_t> payload);
    bool HandleAppMessage(const FrameHeader &header, std::span<const uint8_t> payload);
    bool HandleSnapshotCommand(const FrameHeader &header, std::span<const uint8_t> payload);
    bool SendReply(AppMessageType type, uint64_t request_id);

    std::string socket_name_;
    AllowedPeer allowed_peer_;
    Delegate *delegate_ = nullptr;
    int listen_fd_ = -1;
    int client_fd_ = -1;
    Stats stats_{};
    uint64_t reply_sequence_ = 0;
    std::vector<uint8_t> receive_buffer_;
    std::vector<uint8_t> payload_buffer_;
    std::vector<uint8_t> reply_payload_;
    std::vector<uint8_t> frame_buffer_;
};

} // namespace viper::daemon
