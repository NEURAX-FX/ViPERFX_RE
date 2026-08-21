#include "OwnerServer.h"

#include "OwnerProtocol.h"
#include "ViperDaemonProtocol.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

using viper::daemon::FrameHeader;
using viper::daemon::OwnerServer;
using namespace viper::owner;

std::string UniqueSocketName(const char *suffix) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::string("viper4android.ownertest.") + suffix + "." + std::to_string(stamp);
}

// Minimal owner-side client: the real owner is an ART process, so the host test
// drives the same wire format from C++.
class FakeOwnerClient final {
public:
    ~FakeOwnerClient() { Close(); }

    bool Connect(const std::string &socket_name) {
        fd_ = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (fd_ < 0) return false;

        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        address.sun_path[0] = '\0';
        std::memcpy(address.sun_path + 1, socket_name.data(), socket_name.size());
        const socklen_t length = static_cast<socklen_t>(
            offsetof(sockaddr_un, sun_path) + 1U + socket_name.size());
        if (::connect(fd_, reinterpret_cast<sockaddr *>(&address), length) != 0) {
            Close();
            return false;
        }
        return true;
    }

    bool SendFrame(
        OwnerMessage type,
        const std::vector<uint8_t> &payload,
        uint64_t request_id = 0
    ) {
        FrameHeader header{};
        header.message_type = static_cast<uint16_t>(type);
        header.request_id = request_id;
        const std::string_view view(
            reinterpret_cast<const char *>(payload.data()), payload.size());
        std::vector<uint8_t> frame;
        std::string error;
        if (!viper::daemon::EncodeFrame(header, view, &frame, &error)) return false;
        return SendRaw(frame);
    }

    bool SendRaw(const std::vector<uint8_t> &bytes) {
        return ::send(fd_, bytes.data(), bytes.size(), 0)
            == static_cast<ssize_t>(bytes.size());
    }

    bool Receive(FrameHeader *header, std::vector<uint8_t> *payload) {
        std::vector<uint8_t> buffer(viper::daemon::kMaxFrameSize, 0U);
        const ssize_t received = ::recv(fd_, buffer.data(), buffer.size(), MSG_DONTWAIT);
        if (received <= 0) return false;
        std::string error;
        return viper::daemon::DecodeFrame(
            std::span<const uint8_t>(buffer.data(), static_cast<std::size_t>(received)),
            header,
            payload,
            &error
        );
    }

    void Close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
};

class RecordingDelegate final : public OwnerServer::Delegate {
public:
    OwnerHelloAck OnOwnerHello(const OwnerHello &hello) override {
        ++hello_calls;
        last_hello = hello;
        return ack;
    }

    void OnOwned(const Owned &owned) override {
        ++owned_calls;
        last_owned = owned;
    }

    void OnOwnerFailed(const OwnerFailed &failed) override {
        ++failed_calls;
        last_failed = failed;
    }

    void OnReleased(const Released &released) override {
        ++released_calls;
        last_released = released;
    }

    void OnSessionDelta(const SessionDelta &delta) override {
        ++delta_calls;
        last_delta = delta;
    }

    void OnOwnerDisconnected() override { ++disconnect_calls; }

    OwnerHelloAck ack{true, 3};
    unsigned hello_calls = 0;
    unsigned owned_calls = 0;
    unsigned failed_calls = 0;
    unsigned released_calls = 0;
    unsigned delta_calls = 0;
    unsigned disconnect_calls = 0;
    std::optional<OwnerHello> last_hello;
    std::optional<Owned> last_owned;
    std::optional<OwnerFailed> last_failed;
    std::optional<Released> last_released;
    std::optional<SessionDelta> last_delta;
};

std::vector<uint8_t> HelloPayload() {
    OwnerHello hello{};
    hello.owner_pid = 4242;
    hello.boot_id = 0xB007ULL;
    std::vector<uint8_t> bytes;
    std::string error;
    assert(EncodeOwnerHello(hello, &bytes, &error));
    return bytes;
}

void TestHelloIsAckedAndReported() {
    const std::string name = UniqueSocketName("hello");
    OwnerServer server(name);
    RecordingDelegate delegate;
    server.SetDelegate(&delegate);

    std::string error;
    assert(server.Listen(&error));

    FakeOwnerClient client;
    assert(client.Connect(name));
    assert(client.SendFrame(OwnerMessage::OWNER_HELLO, HelloPayload(), 7));

    assert(server.Poll() >= 1);
    assert(server.Connected());
    assert(delegate.hello_calls == 1);
    assert(delegate.last_hello->owner_pid == 4242);

    FrameHeader header{};
    std::vector<uint8_t> payload;
    assert(client.Receive(&header, &payload));
    assert(header.message_type == static_cast<uint16_t>(OwnerMessage::OWNER_HELLO_ACK));
    assert(header.request_id == 7);

    OwnerHelloAck ack{};
    assert(DecodeOwnerHelloAck(payload, &ack, &error));
    assert(ack.accepted);
    assert(ack.daemon_generation == 3);
}

void TestOwnerReportsReachTheDelegate() {
    const std::string name = UniqueSocketName("reports");
    OwnerServer server(name);
    RecordingDelegate delegate;
    server.SetDelegate(&delegate);

    std::string error;
    assert(server.Listen(&error));

    FakeOwnerClient client;
    assert(client.Connect(name));
    assert(client.SendFrame(OwnerMessage::OWNER_HELLO, HelloPayload()));
    assert(server.Poll() >= 1);

    Owned owned{};
    owned.effect_id = 99;
    owned.has_control = true;
    std::vector<uint8_t> bytes;
    assert(EncodeOwned(owned, &bytes, &error));
    assert(client.SendFrame(OwnerMessage::OWNED, bytes, 11));
    assert(server.Poll() >= 1);
    assert(delegate.owned_calls == 1);
    assert(delegate.last_owned->effect_id == 99);
    assert(delegate.last_owned->has_control);

    OwnerFailed failed{};
    failed.reason_code = 2;
    assert(EncodeOwnerFailed(failed, &bytes, &error));
    assert(client.SendFrame(OwnerMessage::OWN_FAILED, bytes));
    assert(server.Poll() >= 1);
    assert(delegate.failed_calls == 1);
    assert(delegate.last_failed->reason_code == 2);

    SessionDelta delta{};
    delta.audio_session_id = 4242;
    delta.client_uid = 10438;
    delta.appeared = true;
    assert(EncodeSessionDelta(delta, &bytes, &error));
    assert(client.SendFrame(OwnerMessage::SESSION_DELTA, bytes));
    assert(server.Poll() >= 1);
    assert(delegate.delta_calls == 1);
    assert(delegate.last_delta->audio_session_id == 4242);
    assert(delegate.last_delta->client_uid == 10438);

    Released released{};
    assert(EncodeReleased(released, &bytes, &error));
    assert(client.SendFrame(OwnerMessage::RELEASED, bytes));
    assert(server.Poll() >= 1);
    assert(delegate.released_calls == 1);
}

void TestCommandsReachTheOwner() {
    const std::string name = UniqueSocketName("commands");
    OwnerServer server(name);
    RecordingDelegate delegate;
    server.SetDelegate(&delegate);

    std::string error;
    assert(server.Listen(&error));
    // No owner yet: a command must fail rather than be queued forever.
    assert(!server.RequestOwnSession(0, EffectTypeSelector::HIDL, 1));

    FakeOwnerClient client;
    assert(client.Connect(name));
    assert(client.SendFrame(OwnerMessage::OWNER_HELLO, HelloPayload()));
    assert(server.Poll() >= 1);

    assert(server.RequestOwnSession(0, EffectTypeSelector::AIDL, 21));
    FrameHeader header{};
    std::vector<uint8_t> payload;
    // Skip the hello ack the server already queued.
    assert(client.Receive(&header, &payload));
    assert(client.Receive(&header, &payload));
    assert(header.message_type == static_cast<uint16_t>(OwnerMessage::OWN_SESSION));
    assert(header.request_id == 21);
    OwnSession request{};
    assert(DecodeOwnSession(payload, &request, &error));
    assert(request.audio_session_id == 0);
    assert(request.selector == EffectTypeSelector::AIDL);

    assert(server.RequestRelease(0, 22));
    assert(client.Receive(&header, &payload));
    assert(header.message_type == static_cast<uint16_t>(OwnerMessage::RELEASE_SESSION));
    ReleaseSession release{};
    assert(DecodeReleaseSession(payload, &release, &error));
    assert(release.audio_session_id == 0);
    assert(server.Statistics().commands_sent == 2);
}

void TestRejectedPeerIsDropped() {
    const std::string name = UniqueSocketName("peer");
    OwnerServer server(name);
    RecordingDelegate delegate;
    server.SetDelegate(&delegate);
    // Only uid 0 is admitted in production; the predicate is the test seam.
    server.SetAllowedPeer([](const OwnerServer::PeerCredentials &) { return false; });

    std::string error;
    assert(server.Listen(&error));

    FakeOwnerClient client;
    assert(client.Connect(name));
    assert(client.SendFrame(OwnerMessage::OWNER_HELLO, HelloPayload()));

    for (int attempt = 0; attempt < 4; ++attempt) server.Poll();
    assert(!server.Connected());
    assert(delegate.hello_calls == 0);
    assert(server.Statistics().rejected_peers == 1);
}

void TestMalformedFramesDoNotStallLaterFrames() {
    const std::string name = UniqueSocketName("malformed");
    OwnerServer server(name);
    RecordingDelegate delegate;
    server.SetDelegate(&delegate);

    std::string error;
    assert(server.Listen(&error));

    FakeOwnerClient client;
    assert(client.Connect(name));

    // Garbage, then an unknown message type, then a valid hello.
    assert(client.SendRaw(std::vector<uint8_t>(8, 0xEE)));
    assert(client.SendFrame(static_cast<OwnerMessage>(999), std::vector<uint8_t>(4, 0)));
    assert(client.SendFrame(OwnerMessage::OWNER_HELLO, HelloPayload()));

    for (int attempt = 0; attempt < 8; ++attempt) server.Poll();
    assert(delegate.hello_calls == 1);
    assert(server.Statistics().rejected_frames >= 2);
}

void TestDisconnectIsReported() {
    const std::string name = UniqueSocketName("disconnect");
    OwnerServer server(name);
    RecordingDelegate delegate;
    server.SetDelegate(&delegate);

    std::string error;
    assert(server.Listen(&error));

    {
        FakeOwnerClient client;
        assert(client.Connect(name));
        assert(client.SendFrame(OwnerMessage::OWNER_HELLO, HelloPayload()));
        assert(server.Poll() >= 1);
        assert(server.Connected());
    }

    for (int attempt = 0; attempt < 8 && server.Connected(); ++attempt) server.Poll();
    assert(!server.Connected());
    assert(delegate.disconnect_calls == 1);
}

} // namespace

int main() {
    TestHelloIsAckedAndReported();
    TestOwnerReportsReachTheDelegate();
    TestCommandsReachTheOwner();
    TestRejectedPeerIsDropped();
    TestMalformedFramesDoNotStallLaterFrames();
    TestDisconnectIsReported();
    std::puts("owner server tests passed");
    return 0;
}
