#include "OwnerServer.h"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <utility>

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

} // namespace

bool OwnerServer::DefaultAllowedPeer(const PeerCredentials &credentials) noexcept {
    // The owner is spawned by this daemon and always runs as root. Host tests run
    // as an ordinary uid, so the process's own uid is accepted too; anything else
    // on this socket is not the owner.
    return credentials.uid == 0U || credentials.uid == static_cast<uint32_t>(::getuid());
}

OwnerServer::OwnerServer(std::string socket_name)
    : socket_name_(std::move(socket_name)), allowed_peer_(&DefaultAllowedPeer) {}

OwnerServer::~OwnerServer() { Close(); }

void OwnerServer::SetAllowedPeer(AllowedPeer predicate) {
    allowed_peer_ = predicate ? std::move(predicate) : AllowedPeer(&DefaultAllowedPeer);
}

bool OwnerServer::Listen(std::string *error) {
    if (listen_fd_ >= 0) return true;

    sockaddr_un address{};
    socklen_t length = 0;
    if (!BuildAddress(socket_name_, &address, &length)) {
        SetError(error, "invalid owner socket name");
        return false;
    }

    const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        SetError(error, "failed to create owner socket");
        return false;
    }
    if (::bind(fd, reinterpret_cast<sockaddr *>(&address), length) != 0) {
        ::close(fd);
        SetError(error, "failed to bind owner socket");
        return false;
    }
    if (::listen(fd, 2) != 0) {
        ::close(fd);
        SetError(error, "failed to listen on owner socket");
        return false;
    }

    listen_fd_ = fd;
    receive_buffer_.assign(kMaxFrameSize, 0U);
    payload_buffer_.reserve(owner::kOwnerMaxPayloadBytes);
    frame_buffer_.reserve(kFrameHeaderSize + owner::kOwnerMaxPayloadBytes);
    if (error != nullptr) error->clear();
    return true;
}

void OwnerServer::Close() {
    DropClient(false);
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

void OwnerServer::DropClient(bool notify_delegate) {
    if (client_fd_ < 0) return;
    ::close(client_fd_);
    client_fd_ = -1;
    if (notify_delegate && delegate_ != nullptr) delegate_->OnOwnerDisconnected();
}

bool OwnerServer::AcceptPending() {
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
        // Without credentials there is no way to tell the owner from anything
        // else, and this socket hands out effect ownership, so refuse.
        ::close(fd);
        ++stats_.rejected_peers;
        return false;
    }
    if (!allowed_peer_(peer)) {
        ::close(fd);
        ++stats_.rejected_peers;
        return false;
    }

    // Only one owner at a time: a new connection means the previous owner is
    // gone, so its handle is gone with it.
    DropClient(true);
    client_fd_ = fd;
    ++stats_.accepted_connections;
    return true;
}

std::size_t OwnerServer::Poll(std::size_t max_frames) {
    if (listen_fd_ < 0) return 0;

    AcceptPending();
    if (client_fd_ < 0) return 0;

    std::size_t handled = 0;
    for (std::size_t attempt = 0; attempt < max_frames; ++attempt) {
        const ssize_t received = ::recv(
            client_fd_, receive_buffer_.data(), receive_buffer_.size(), MSG_DONTWAIT);
        if (received == 0) {
            DropClient(true);
            break;
        }
        if (received < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            DropClient(true);
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
        if (!owner::IsOwnerMessage(header.message_type)) {
            ++stats_.rejected_frames;
            continue;
        }
        if (HandleFrame(header, payload_buffer_)) ++handled;
    }
    return handled;
}

bool OwnerServer::HandleFrame(
    const FrameHeader &header,
    const std::vector<uint8_t> &payload
) {
    std::string error;
    switch (static_cast<owner::OwnerMessage>(header.message_type)) {
        case owner::OwnerMessage::OWNER_HELLO: {
            owner::OwnerHello hello{};
            if (!owner::DecodeOwnerHello(payload, &hello, &error)) {
                ++stats_.rejected_frames;
                return false;
            }
            ++stats_.hellos;
            owner::OwnerHelloAck ack{};
            if (delegate_ != nullptr) ack = delegate_->OnOwnerHello(hello);
            std::vector<uint8_t> reply;
            if (!owner::EncodeOwnerHelloAck(ack, &reply, &error)) {
                ++stats_.rejected_frames;
                return false;
            }
            SendFrame(owner::OwnerMessage::OWNER_HELLO_ACK, reply, header.request_id);
            // A refused owner must not keep the socket: it would otherwise sit
            // connected without ever being allowed to own a handle.
            if (!ack.accepted) DropClient(true);
            return true;
        }
        case owner::OwnerMessage::OWNED: {
            owner::Owned owned{};
            if (!owner::DecodeOwned(payload, &owned, &error)) {
                ++stats_.rejected_frames;
                return false;
            }
            ++stats_.owned_reports;
            if (delegate_ != nullptr) delegate_->OnOwned(owned);
            return true;
        }
        case owner::OwnerMessage::OWN_FAILED: {
            owner::OwnerFailed failed{};
            if (!owner::DecodeOwnerFailed(payload, &failed, &error)) {
                ++stats_.rejected_frames;
                return false;
            }
            ++stats_.failure_reports;
            if (delegate_ != nullptr) delegate_->OnOwnerFailed(failed);
            return true;
        }
        case owner::OwnerMessage::RELEASED: {
            owner::Released released{};
            if (!owner::DecodeReleased(payload, &released, &error)) {
                ++stats_.rejected_frames;
                return false;
            }
            ++stats_.released_reports;
            if (delegate_ != nullptr) delegate_->OnReleased(released);
            return true;
        }
        case owner::OwnerMessage::SESSION_DELTA: {
            owner::SessionDelta delta{};
            if (!owner::DecodeSessionDelta(payload, &delta, &error)) {
                ++stats_.rejected_frames;
                return false;
            }
            ++stats_.session_deltas;
            if (delegate_ != nullptr) delegate_->OnSessionDelta(delta);
            return true;
        }
        case owner::OwnerMessage::OWNER_HELLO_ACK:
        case owner::OwnerMessage::OWN_SESSION:
        case owner::OwnerMessage::RELEASE_SESSION:
            // Daemon-to-owner directions. An owner sending these is either buggy
            // or hostile; either way the frame is not usable.
            ++stats_.rejected_frames;
            return false;
    }
    ++stats_.rejected_frames;
    return false;
}

bool OwnerServer::SendFrame(
    owner::OwnerMessage type,
    const std::vector<uint8_t> &payload,
    uint64_t request_id
) {
    if (client_fd_ < 0) return false;

    FrameHeader header{};
    header.message_type = static_cast<uint16_t>(type);
    header.request_id = request_id;
    header.sequence = sequence_++;
    const std::string_view view(
        reinterpret_cast<const char *>(payload.data()), payload.size());
    std::string error;
    if (!EncodeFrame(header, view, &frame_buffer_, &error)) return false;

    const ssize_t sent = ::send(
        client_fd_, frame_buffer_.data(), frame_buffer_.size(), MSG_DONTWAIT);
    return sent == static_cast<ssize_t>(frame_buffer_.size());
}

bool OwnerServer::RequestOwnSession(
    uint32_t audio_session_id,
    owner::EffectTypeSelector selector,
    uint64_t request_id
) {
    if (client_fd_ < 0) return false;

    owner::OwnSession request{};
    request.audio_session_id = audio_session_id;
    request.selector = selector;
    std::vector<uint8_t> payload;
    std::string error;
    if (!owner::EncodeOwnSession(request, &payload, &error)) return false;
    if (!SendFrame(owner::OwnerMessage::OWN_SESSION, payload, request_id)) return false;
    ++stats_.commands_sent;
    return true;
}

bool OwnerServer::RequestRelease(uint32_t audio_session_id, uint64_t request_id) {
    if (client_fd_ < 0) return false;

    owner::ReleaseSession request{};
    request.audio_session_id = audio_session_id;
    std::vector<uint8_t> payload;
    std::string error;
    if (!owner::EncodeReleaseSession(request, &payload, &error)) return false;
    if (!SendFrame(owner::OwnerMessage::RELEASE_SESSION, payload, request_id)) return false;
    ++stats_.commands_sent;
    return true;
}

} // namespace viper::daemon
