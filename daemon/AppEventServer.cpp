#include "AppEventServer.h"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <utility>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace viper::daemon {
namespace {

// Android uid layout: uid = user_id * kUsersOffset + appid.
constexpr uint32_t kUserIdOffset = 100000U;
constexpr uint32_t kFirstApplicationAppId = 10000U;
constexpr uint32_t kLastApplicationAppId = 19999U;

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

} // namespace

bool AppEventServer::DefaultAllowedPeer(const PeerCredentials &credentials) noexcept {
    if (credentials.uid == 0U) return true;
    if (credentials.uid == static_cast<uint32_t>(::getuid())) return true;
    const uint32_t app_id = credentials.uid % kUserIdOffset;
    return app_id >= kFirstApplicationAppId && app_id <= kLastApplicationAppId;
}

AppEventServer::AppEventServer(std::string socket_name)
    : socket_name_(std::move(socket_name)), allowed_peer_(&DefaultAllowedPeer) {}

AppEventServer::~AppEventServer() { Close(); }

void AppEventServer::SetAllowedPeer(AllowedPeer predicate) {
    allowed_peer_ = predicate ? std::move(predicate) : AllowedPeer(&DefaultAllowedPeer);
}

bool AppEventServer::Listen(std::string *error) {
    if (listen_fd_ >= 0) return true;

    sockaddr_un address{};
    socklen_t length = 0;
    if (!BuildAddress(socket_name_, &address, &length)) {
        SetError(error, "invalid app socket name");
        return false;
    }

    const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        SetError(error, "failed to create app socket");
        return false;
    }
    if (::bind(fd, reinterpret_cast<sockaddr *>(&address), length) != 0) {
        ::close(fd);
        SetError(error, "failed to bind app socket");
        return false;
    }
    if (::listen(fd, 4) != 0) {
        ::close(fd);
        SetError(error, "failed to listen on app socket");
        return false;
    }

    listen_fd_ = fd;
    receive_buffer_.assign(kMaxFrameSize, 0U);
    payload_buffer_.reserve(kAppRouteReportHeaderSize);
    reply_payload_.reserve(kAppHelloAckWireSize);
    frame_buffer_.reserve(kFrameHeaderSize + kAppHelloAckWireSize);
    if (error != nullptr) error->clear();
    return true;
}

void AppEventServer::Close() {
    DropClient();
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

void AppEventServer::DropClient() {
    if (client_fd_ >= 0) {
        ::close(client_fd_);
        client_fd_ = -1;
    }
}

bool AppEventServer::AcceptPending() {
    if (listen_fd_ < 0) return false;

    const int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd < 0) return false;

    ucred credentials{};
    socklen_t length = sizeof(credentials);
    PeerCredentials peer{};
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) == 0) {
        peer.uid = credentials.uid;
        peer.gid = credentials.gid;
        peer.pid = credentials.pid;
    } else {
        // No credentials means no way to tell the App from a hostile app, and on
        // this socket that is exactly the decision being made, so refuse.
        ::close(fd);
        ++stats_.rejected_peers;
        return false;
    }
    if (!allowed_peer_(peer)) {
        ::close(fd);
        ++stats_.rejected_peers;
        return false;
    }

    // One App connection at a time; a new connection means the previous App
    // process is gone or restarted.
    DropClient();
    client_fd_ = fd;
    ++stats_.accepted_connections;
    return true;
}

std::size_t AppEventServer::Poll(std::size_t max_messages) {
    if (listen_fd_ < 0) return 0;

    AcceptPending();
    if (client_fd_ < 0) return 0;

    std::size_t dispatched = 0;
    for (std::size_t attempt = 0; attempt < max_messages; ++attempt) {
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
        if (HandleFrame(header, payload_buffer_)) ++dispatched;
        if (client_fd_ < 0) break;
    }

    return dispatched;
}

bool AppEventServer::HandleFrame(
    const FrameHeader &header,
    std::span<const uint8_t> payload
) {
    if (delegate_ == nullptr) {
        ++stats_.rejected_frames;
        return false;
    }
    if (IsAppMessageType(header.message_type)) {
        return HandleAppMessage(header, payload);
    }
    if (IsSnapshotCommandType(header.message_type)) {
        return HandleSnapshotCommand(header, payload);
    }
    // Anything else is not part of this endpoint's contract. Silently ignoring it
    // would look like success to the App, so it is counted as a rejection.
    ++stats_.rejected_frames;
    return false;
}

bool AppEventServer::HandleAppMessage(
    const FrameHeader &header,
    std::span<const uint8_t> payload
) {
    std::string error;
    switch (static_cast<AppMessageType>(header.message_type)) {
        case AppMessageType::APP_HELLO: {
            AppHello hello{};
            if (!DecodeAppHello(payload, &hello, &error)) {
                ++stats_.rejected_frames;
                return false;
            }
            const AppHelloAck ack = delegate_->OnHello(hello);
            if (!EncodeAppHelloAck(ack, &reply_payload_, &error)) {
                ++stats_.rejected_frames;
                return false;
            }
            ++stats_.hellos;
            SendReply(AppMessageType::APP_HELLO_ACK, header.request_id);
            return true;
        }
        case AppMessageType::APP_ROUTE_REPORT: {
            AppRouteReport report{};
            if (!DecodeAppRouteReport(payload, &report, &error)) {
                ++stats_.rejected_frames;
                return false;
            }
            AppRouteAck ack{};
            if (!delegate_->OnRouteReport(report, &ack, &error)) ack.accepted = false;
            if (!EncodeAppRouteAck(ack, &reply_payload_, &error)) {
                ++stats_.rejected_frames;
                return false;
            }
            ++stats_.route_reports;
            SendReply(AppMessageType::APP_ROUTE_ACK, header.request_id);
            return true;
        }
        case AppMessageType::APP_HELLO_ACK:
        case AppMessageType::APP_ROUTE_ACK:
        case AppMessageType::APP_APPLY_RESULT:
            // Daemon-to-App directions. An App sending one is either confused or
            // probing, and must not be treated as a request.
            ++stats_.rejected_frames;
            return false;
    }
    ++stats_.rejected_frames;
    return false;
}

bool AppEventServer::HandleSnapshotCommand(
    const FrameHeader &header,
    std::span<const uint8_t> payload
) {
    const auto type = static_cast<SnapshotCommandType>(header.message_type);
    std::string error;

    // The payload is decoded here only to reject malformed input before the
    // driver sees it; the delegate relays the original bytes so the frame the
    // driver receives is byte-identical to the one the App produced.
    bool decoded = false;
    switch (type) {
        case SnapshotCommandType::SNAPSHOT_BEGIN: {
            SnapshotBegin begin{};
            decoded = DecodeSnapshotBegin(payload, &begin, &error);
            break;
        }
        case SnapshotCommandType::SNAPSHOT_CHUNK: {
            SnapshotChunk chunk{};
            decoded = DecodeSnapshotChunk(payload, &chunk, &error);
            break;
        }
        case SnapshotCommandType::SNAPSHOT_COMMIT: {
            SnapshotCommit commit{};
            decoded = DecodeSnapshotCommit(payload, &commit, &error);
            break;
        }
        case SnapshotCommandType::SNAPSHOT_ABORT: {
            SnapshotAbort abort{};
            decoded = DecodeSnapshotAbort(payload, &abort, &error);
            break;
        }
    }
    if (!decoded) {
        ++stats_.rejected_frames;
        return false;
    }

    AppApplyResult result{};
    if (!delegate_->OnSnapshotCommand(type, payload, &result)) result.accepted = false;
    if (!EncodeAppApplyResult(result, &reply_payload_, &error)) {
        ++stats_.rejected_frames;
        return false;
    }
    ++stats_.snapshot_commands;
    SendReply(AppMessageType::APP_APPLY_RESULT, header.request_id);
    return true;
}

bool AppEventServer::SendReply(AppMessageType type, uint64_t request_id) {
    if (client_fd_ < 0) return false;

    FrameHeader header{};
    header.message_type = static_cast<uint16_t>(type);
    // The App matches a reply to its request by this id, so it must be echoed
    // unchanged even when the request was refused.
    header.request_id = request_id;
    header.sequence = ++reply_sequence_;
    const std::string_view view(
        reinterpret_cast<const char *>(reply_payload_.data()), reply_payload_.size());
    std::string error;
    if (!EncodeFrame(header, view, &frame_buffer_, &error)) return false;

    const ssize_t sent = ::send(
        client_fd_, frame_buffer_.data(), frame_buffer_.size(), MSG_DONTWAIT);
    if (sent != static_cast<ssize_t>(frame_buffer_.size())) return false;
    ++stats_.replies_sent;
    return true;
}

} // namespace viper::daemon
