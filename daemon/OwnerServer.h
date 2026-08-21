#pragma once

#include "OwnerProtocol.h"
#include "ViperDaemonProtocol.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace viper::daemon {

/**
 * Accepts the ART effect owner on the private abstract SOCK_SEQPACKET socket
 * @viper4android.owner.v1.
 *
 * Separate from DriverEventServer and AppEventServer because the peers have
 * different trust: the driver endpoint admits root/audioserver, the App endpoint
 * admits an ordinary app uid, and this one admits root only. Reusing either
 * would widen its accepted peer set.
 *
 * Single-threaded and non-blocking: the daemon control loop calls Poll(). The
 * server never creates an AudioEffect itself; it only relays ownership commands
 * to the owner process and reports what the owner observed.
 */
class OwnerServer final {
public:
    struct Stats {
        uint64_t accepted_connections = 0;
        uint64_t rejected_frames = 0;
        uint64_t rejected_peers = 0;
        uint64_t hellos = 0;
        uint64_t owned_reports = 0;
        uint64_t failure_reports = 0;
        uint64_t released_reports = 0;
        uint64_t session_deltas = 0;
        uint64_t commands_sent = 0;
    };

    // SO_PEERCRED of a connecting peer, kept free of <sys/socket.h> so callers can
    // supply an admission rule without pulling in socket headers.
    struct PeerCredentials {
        uint32_t uid = 0;
        uint32_t gid = 0;
        int32_t pid = 0;
    };

    using AllowedPeer = std::function<bool(const PeerCredentials &)>;

    struct Delegate {
        Delegate() = default;
        virtual ~Delegate() = default;

        Delegate(const Delegate &) = delete;
        Delegate &operator=(const Delegate &) = delete;

        // Returns the ack the server sends back. Refusing here keeps an owner
        // from believing it may hold a handle.
        virtual owner::OwnerHelloAck OnOwnerHello(const owner::OwnerHello &hello) = 0;

        virtual void OnOwned(const owner::Owned &owned) = 0;
        virtual void OnOwnerFailed(const owner::OwnerFailed &failed) = 0;
        virtual void OnReleased(const owner::Released &released) = 0;
        virtual void OnSessionDelta(const owner::SessionDelta &delta) = 0;

        // The owner's socket closed. The effect module is gone with it, so the
        // supervisor must decide whether to respawn.
        virtual void OnOwnerDisconnected() = 0;
    };

    explicit OwnerServer(std::string socket_name = owner::kOwnerSocketName);
    ~OwnerServer();

    OwnerServer(const OwnerServer &) = delete;
    OwnerServer &operator=(const OwnerServer &) = delete;

    bool Listen(std::string *error);
    void Close();

    void SetDelegate(Delegate *delegate) noexcept { delegate_ = delegate; }

    // Replaces the admission rule. An empty predicate restores the default.
    void SetAllowedPeer(AllowedPeer predicate);

    // Production rule: root only. The owner is spawned by this daemon, so any
    // other uid on this socket is not the owner.
    static bool DefaultAllowedPeer(const PeerCredentials &credentials) noexcept;

    // Processes at most `max_frames` pending frames. Returns the number handled.
    std::size_t Poll(std::size_t max_frames = 32);

    // Asks the owner to create and enable the session's effect handle. Returns
    // false when no owner is connected: a command must not be silently queued,
    // because the supervisor's state depends on knowing it was not delivered.
    bool RequestOwnSession(
        uint32_t audio_session_id,
        owner::EffectTypeSelector selector,
        uint64_t request_id
    );

    bool RequestRelease(uint32_t audio_session_id, uint64_t request_id);

    bool Listening() const noexcept { return listen_fd_ >= 0; }
    bool Connected() const noexcept { return client_fd_ >= 0; }
    const Stats &Statistics() const noexcept { return stats_; }
    const std::string &SocketName() const noexcept { return socket_name_; }

private:
    bool AcceptPending();
    void DropClient(bool notify_delegate);
    bool SendFrame(
        owner::OwnerMessage type,
        const std::vector<uint8_t> &payload,
        uint64_t request_id
    );
    bool HandleFrame(const FrameHeader &header, const std::vector<uint8_t> &payload);

    std::string socket_name_;
    Delegate *delegate_ = nullptr;
    AllowedPeer allowed_peer_;
    int listen_fd_ = -1;
    int client_fd_ = -1;
    Stats stats_{};
    std::vector<uint8_t> receive_buffer_;
    std::vector<uint8_t> payload_buffer_;
    std::vector<uint8_t> frame_buffer_;
    uint64_t sequence_ = 1;
};

} // namespace viper::daemon
