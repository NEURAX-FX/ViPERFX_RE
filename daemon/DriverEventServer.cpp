#include "DriverEventServer.h"

#include "ViperDaemonProtocol.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <span>
#include <utility>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace viper::daemon {
namespace {

void SetError(std::string *error, const char *message) {
    if (error != nullptr) error->assign(message);
}

bool BuildAddress(
    const std::string &name,
    sockaddr_un *address,
    socklen_t *length
) noexcept {
    if (name.empty() || name.size() >= sizeof(address->sun_path) - 1U) return false;
    std::memset(address, 0, sizeof(*address));
    address->sun_family = AF_UNIX;
    address->sun_path[0] = '\0';
    std::memcpy(address->sun_path + 1, name.data(), name.size());
    *length = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + 1U + name.size());
    return true;
}

// The driver runs as the audioserver UID, never as an unprivileged app. Refusing
// unexpected peers keeps a sandboxed app from injecting fake lifecycle events.
bool PeerIsAcceptable(int fd) noexcept {
    ucred credentials{};
    socklen_t length = sizeof(credentials);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0) {
        // Without peer credentials there is nothing to validate against.
        return true;
    }
    return credentials.uid == 0U || credentials.uid == 1041U /* AID_AUDIOSERVER */
        || credentials.uid == ::getuid();
}

} // namespace

DriverEventServer::DriverEventServer(std::string socket_name)
    : socket_name_(std::move(socket_name)) {}

DriverEventServer::~DriverEventServer() { Close(); }

bool DriverEventServer::Listen(std::string *error) {
    if (listen_fd_ >= 0) return true;

    sockaddr_un address{};
    socklen_t length = 0;
    if (!BuildAddress(socket_name_, &address, &length)) {
        SetError(error, "invalid driver socket name");
        return false;
    }

    const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        SetError(error, "failed to create driver socket");
        return false;
    }
    if (::bind(fd, reinterpret_cast<sockaddr *>(&address), length) != 0) {
        ::close(fd);
        SetError(error, "failed to bind driver socket");
        return false;
    }
    if (::listen(fd, 4) != 0) {
        ::close(fd);
        SetError(error, "failed to listen on driver socket");
        return false;
    }

    listen_fd_ = fd;
    receive_buffer_.assign(kMaxFrameSize, 0U);
    payload_buffer_.reserve(kDriverEventWireSize);
    frame_buffer_.reserve(kFrameHeaderSize + kDriverEventWireSize);
    if (error != nullptr) error->clear();
    return true;
}

void DriverEventServer::Close() {
    DropClient();
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

void DriverEventServer::DropClient() {
    if (client_fd_ >= 0) {
        ::close(client_fd_);
        client_fd_ = -1;
    }
}

bool DriverEventServer::AcceptPending() {
    if (listen_fd_ < 0) return false;

    const int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd < 0) return false;
    if (!PeerIsAcceptable(fd)) {
        ::close(fd);
        ++stats_.rejected_frames;
        return false;
    }

    // Only one driver instance publishes at a time; a new connection means the
    // previous one is gone.
    DropClient();
    client_fd_ = fd;
    ++stats_.accepted_connections;
    return true;
}

std::size_t DriverEventServer::Poll(SessionRegistry *registry, std::size_t max_events) {
    if (registry == nullptr || listen_fd_ < 0) return 0;

    AcceptPending();
    if (client_fd_ < 0) return 0;

    std::size_t applied = 0;
    for (std::size_t attempt = 0; attempt < max_events; ++attempt) {
        const ssize_t received = ::recv(
            client_fd_, receive_buffer_.data(), receive_buffer_.size(), MSG_DONTWAIT);
        if (received == 0) {
            DropClient();
            break;
        }
        if (received < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            DropClient();
            break;
        }

        FrameHeader header{};
        std::string error;
        const std::span<const uint8_t> bytes(
            receive_buffer_.data(), static_cast<std::size_t>(received));
        if (!DecodeFrame(bytes, &header, &payload_buffer_, &error)) {
            ++stats_.rejected_frames;
            continue;
        }

        DriverEvent event{};
        if (!DecodeDriverEvent(payload_buffer_, &event, &error)) {
            ++stats_.rejected_frames;
            continue;
        }
        // The frame header must agree with its payload, otherwise the stream is
        // not trustworthy.
        if (header.message_type != static_cast<uint16_t>(event.type)
            || header.sequence != event.event_sequence) {
            ++stats_.rejected_frames;
            continue;
        }

        // Apply results answer our own snapshot commands rather than describing a
        // context, so they are captured here instead of entering the registry.
        if (event.type == DriverEventType::SNAPSHOT_APPLIED_ACK
            || event.type == DriverEventType::SNAPSHOT_APPLIED_NACK) {
            const bool accepted = event.type == DriverEventType::SNAPSHOT_APPLIED_ACK;
            pending_outcome_.valid = true;
            pending_outcome_.accepted = accepted;
            pending_outcome_.request_id = header.request_id;
            pending_outcome_.error_code = event.bypass_reason;
            pending_outcome_.app_generation = event.session_generation;
            pending_outcome_.resource_generation = event.resource_generation;
            pending_outcome_.graph_generation = event.graph_generation;
            if (accepted) {
                ++stats_.apply_acks;
            } else {
                ++stats_.apply_nacks;
            }
            continue;
        }

        const ApplyResult result = registry->Apply(event);
        if (result == ApplyResult::APPLIED || result == ApplyResult::APPLIED_WITH_GAP) {
            ++applied;
            ++stats_.applied_events;
        }
        if (result == ApplyResult::APPLIED_WITH_GAP) {
            registry->MarkStaleAfter(registry->HighestSequence());
            RequestRescan();
        }
    }

    return applied;
}

bool DriverEventServer::RequestRescan() {
    if (client_fd_ < 0) return false;

    DriverEvent request{};
    request.type = DriverEventType::RESCAN_RESPONSE;
    std::string error;
    if (!EncodeDriverEvent(request, &payload_buffer_, &error)) return false;

    FrameHeader header{};
    header.message_type = static_cast<uint16_t>(DriverEventType::RESCAN_RESPONSE);
    const std::string_view payload(
        reinterpret_cast<const char *>(payload_buffer_.data()), payload_buffer_.size());
    if (!EncodeFrame(header, payload, &frame_buffer_, &error)) return false;

    const ssize_t sent = ::send(
        client_fd_, frame_buffer_.data(), frame_buffer_.size(), MSG_DONTWAIT);
    if (sent != static_cast<ssize_t>(frame_buffer_.size())) return false;
    ++stats_.rescan_requests_sent;
    return true;
}

bool DriverEventServer::SendSnapshotCommand(
    SnapshotCommandType type,
    std::span<const uint8_t> payload,
    uint64_t request_id
) {
    if (client_fd_ < 0) return false;

    FrameHeader header{};
    header.message_type = static_cast<uint16_t>(type);
    // The driver echoes this back on the ACK/NACK, which is how a result is
    // matched to the transfer that produced it.
    header.request_id = request_id;
    const std::string_view view(
        reinterpret_cast<const char *>(payload.data()), payload.size());
    std::string error;
    if (!EncodeFrame(header, view, &frame_buffer_, &error)) return false;

    const ssize_t sent = ::send(
        client_fd_, frame_buffer_.data(), frame_buffer_.size(), MSG_DONTWAIT);
    if (sent != static_cast<ssize_t>(frame_buffer_.size())) return false;
    // Counted apart from snapshot transfers: a route announce carries no snapshot
    // bytes, so folding it in here would make "snapshot_commands" report transfers
    // that never happened.
    if (type == SnapshotCommandType::ROUTE_ANNOUNCE) {
        ++stats_.route_announces_sent;
    } else {
        ++stats_.snapshot_commands_sent;
    }
    return true;
}

DriverEventServer::ApplyOutcome DriverEventServer::TakeApplyOutcome() noexcept {
    const ApplyOutcome outcome = pending_outcome_;
    pending_outcome_ = ApplyOutcome{};
    return outcome;
}

} // namespace viper::daemon
