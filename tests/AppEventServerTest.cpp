#include "AppEventServer.h"

#include "AppCommand.h"
#include "SnapshotCommand.h"
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

using viper::daemon::AppApplyResult;
using viper::daemon::AppEventServer;
using viper::daemon::AppHello;
using viper::daemon::AppHelloAck;
using viper::daemon::AppMessageType;
using viper::daemon::AppRouteAck;
using viper::daemon::AppRouteReport;
using viper::daemon::FrameHeader;
using viper::daemon::SnapshotCommandType;

std::string UniqueSocketName(const char *suffix) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::string("viper4android.apptest.") + suffix + "." + std::to_string(stamp);
}

// Minimal App-side client: connects to the daemon endpoint and exchanges frames.
class FakeAppClient final {
public:
    ~FakeAppClient() { Close(); }

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
        uint16_t message_type,
        const std::vector<uint8_t> &payload,
        uint64_t request_id
    ) {
        FrameHeader header{};
        header.message_type = message_type;
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

    // Reads one reply frame. Returns false when nothing arrived.
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

class RecordingDelegate final : public AppEventServer::Delegate {
public:
    AppHelloAck OnHello(const AppHello &hello) override {
        ++hello_calls;
        last_hello = hello;
        return hello_ack;
    }

    bool OnRouteReport(
        const AppRouteReport &report,
        AppRouteAck *ack,
        std::string *error
    ) override {
        ++route_calls;
        last_report = report;
        *ack = route_ack;
        if (!route_accepts && error != nullptr) error->assign("rejected by test");
        return route_accepts;
    }

    bool OnSnapshotCommand(
        SnapshotCommandType type,
        std::span<const uint8_t> payload,
        AppApplyResult *result
    ) override {
        ++snapshot_calls;
        last_command = type;
        last_payload.assign(payload.begin(), payload.end());
        *result = apply_result;
        return apply_result.accepted;
    }

    int hello_calls = 0;
    int route_calls = 0;
    int snapshot_calls = 0;
    AppHello last_hello{};
    std::optional<AppRouteReport> last_report;
    std::optional<SnapshotCommandType> last_command;
    std::vector<uint8_t> last_payload;
    AppHelloAck hello_ack{};
    AppRouteAck route_ack{};
    AppApplyResult apply_result{};
    bool route_accepts = true;
};

std::string SampleHash(char fill) { return std::string(64, fill); }

AppRouteReport SampleReport() {
    AppRouteReport report{};
    report.route_type = "wired_headphones";
    report.stable_address_or_port = "port-3";
    report.product_name = "usb dac";
    report.encoding = "pcm_16";
    report.sample_rate = 48000;
    report.channel_mask = 3;
    report.output_flags = 5;
    return report;
}

// Pumps the server until `predicate` holds, so the test never depends on a
// single Poll() seeing a message that is still in flight.
template <typename Predicate>
bool PumpUntil(AppEventServer &server, Predicate predicate) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        server.Poll();
        if (predicate()) return true;
        ::usleep(1000);
    }
    return false;
}

// Waits for one reply frame, pumping the server between attempts.
bool AwaitReply(
    AppEventServer &server,
    FakeAppClient &client,
    FrameHeader *header,
    std::vector<uint8_t> *payload
) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        server.Poll();
        if (client.Receive(header, payload)) return true;
        ::usleep(1000);
    }
    return false;
}

void TestHelloRoundTripCarriesDelegateState() {
    AppEventServer server(UniqueSocketName("hello"));
    std::string error;
    assert(server.Listen(&error));
    assert(server.Listening());

    RecordingDelegate delegate;
    delegate.hello_ack.flags = viper::daemon::kAppFlagRestoreEnabled
        | viper::daemon::kAppFlagDriverConnected;
    delegate.hello_ack.daemon_generation = 77;
    delegate.hello_ack.route_epoch = 5;
    delegate.hello_ack.route_key_hash = SampleHash('a');
    server.SetDelegate(&delegate);

    FakeAppClient client;
    assert(client.Connect(server.SocketName()));

    AppHello hello{};
    hello.app_generation = 9;
    std::vector<uint8_t> payload;
    assert(viper::daemon::EncodeAppHello(hello, &payload, &error));
    assert(client.SendFrame(
        static_cast<uint16_t>(AppMessageType::APP_HELLO), payload, 4242U));

    FrameHeader reply{};
    std::vector<uint8_t> reply_payload;
    assert(AwaitReply(server, client, &reply, &reply_payload));
    assert(reply.message_type == static_cast<uint16_t>(AppMessageType::APP_HELLO_ACK));
    assert(reply.request_id == 4242U);

    AppHelloAck ack{};
    assert(viper::daemon::DecodeAppHelloAck(reply_payload, &ack, &error));
    // The App decides whether to stream a snapshot from these, so they must be
    // the delegate's values and not defaults.
    assert(ack.daemon_generation == 77U);
    assert(ack.route_epoch == 5U);
    assert(ack.flags == delegate.hello_ack.flags);
    assert(ack.route_key_hash == SampleHash('a'));
    assert(delegate.hello_calls == 1);
    assert(delegate.last_hello.app_generation == 9U);
    assert(server.Statistics().hellos == 1U);
    assert(server.Statistics().replies_sent == 1U);
    assert(server.Statistics().accepted_connections == 1U);
}

void TestRouteReportReachesDelegateAndAcks() {
    AppEventServer server(UniqueSocketName("route"));
    std::string error;
    assert(server.Listen(&error));

    RecordingDelegate delegate;
    delegate.route_ack.accepted = true;
    delegate.route_ack.daemon_generation = 12;
    delegate.route_ack.route_epoch = 3;
    delegate.route_ack.route_key_hash = SampleHash('b');
    server.SetDelegate(&delegate);

    FakeAppClient client;
    assert(client.Connect(server.SocketName()));

    const AppRouteReport report = SampleReport();
    std::vector<uint8_t> payload;
    assert(viper::daemon::EncodeAppRouteReport(report, &payload, &error));
    assert(client.SendFrame(
        static_cast<uint16_t>(AppMessageType::APP_ROUTE_REPORT), payload, 777U));

    FrameHeader reply{};
    std::vector<uint8_t> reply_payload;
    assert(AwaitReply(server, client, &reply, &reply_payload));
    assert(reply.message_type == static_cast<uint16_t>(AppMessageType::APP_ROUTE_ACK));
    assert(reply.request_id == 777U);

    AppRouteAck ack{};
    assert(viper::daemon::DecodeAppRouteAck(reply_payload, &ack, &error));
    assert(ack.accepted);
    assert(ack.daemon_generation == 12U);
    assert(ack.route_epoch == 3U);
    assert(ack.route_key_hash == SampleHash('b'));

    assert(delegate.route_calls == 1);
    assert(delegate.last_report.has_value());
    // Every field must survive the round trip: a dropped one silently changes the
    // device key and the daemon would restore the wrong snapshot.
    assert(delegate.last_report->route_type == report.route_type);
    assert(delegate.last_report->stable_address_or_port == report.stable_address_or_port);
    assert(delegate.last_report->product_name == report.product_name);
    assert(delegate.last_report->encoding == report.encoding);
    assert(delegate.last_report->sample_rate == report.sample_rate);
    assert(delegate.last_report->channel_mask == report.channel_mask);
    assert(delegate.last_report->output_flags == report.output_flags);
    assert(server.Statistics().route_reports == 1U);
}

void TestRejectedRouteReportStillAcks() {
    AppEventServer server(UniqueSocketName("route-reject"));
    std::string error;
    assert(server.Listen(&error));

    RecordingDelegate delegate;
    delegate.route_accepts = false;
    delegate.route_ack.accepted = true; // Overridden because the delegate refused.
    server.SetDelegate(&delegate);

    FakeAppClient client;
    assert(client.Connect(server.SocketName()));

    std::vector<uint8_t> payload;
    assert(viper::daemon::EncodeAppRouteReport(SampleReport(), &payload, &error));
    assert(client.SendFrame(
        static_cast<uint16_t>(AppMessageType::APP_ROUTE_REPORT), payload, 5U));

    FrameHeader reply{};
    std::vector<uint8_t> reply_payload;
    assert(AwaitReply(server, client, &reply, &reply_payload));
    AppRouteAck ack{};
    assert(viper::daemon::DecodeAppRouteAck(reply_payload, &ack, &error));
    // A refusal must be visible to the App; silence would look like acceptance.
    assert(!ack.accepted);
    assert(reply.request_id == 5U);
}

void TestSnapshotCommandsAreForwarded() {
    AppEventServer server(UniqueSocketName("snapshot"));
    std::string error;
    assert(server.Listen(&error));

    RecordingDelegate delegate;
    delegate.apply_result.accepted = true;
    delegate.apply_result.app_generation = 4;
    delegate.apply_result.daemon_generation = 6;
    delegate.apply_result.resource_generation = 8;
    delegate.apply_result.graph_generation = 10;
    server.SetDelegate(&delegate);

    FakeAppClient client;
    assert(client.Connect(server.SocketName()));

    viper::daemon::SnapshotBegin begin{};
    begin.app_generation = 4;
    begin.daemon_generation = 6;
    begin.total_size = 32;
    begin.crc32 = 0x1234U;
    begin.device_key_hash = SampleHash('c');
    std::vector<uint8_t> payload;
    assert(viper::daemon::EncodeSnapshotBegin(begin, &payload, &error));
    assert(client.SendFrame(
        static_cast<uint16_t>(SnapshotCommandType::SNAPSHOT_BEGIN), payload, 31U));

    FrameHeader reply{};
    std::vector<uint8_t> reply_payload;
    assert(AwaitReply(server, client, &reply, &reply_payload));
    assert(reply.message_type == static_cast<uint16_t>(AppMessageType::APP_APPLY_RESULT));
    assert(reply.request_id == 31U);

    AppApplyResult result{};
    assert(viper::daemon::DecodeAppApplyResult(reply_payload, &result, &error));
    assert(result.accepted);
    assert(result.app_generation == 4U);
    assert(result.daemon_generation == 6U);
    assert(result.resource_generation == 8U);
    assert(result.graph_generation == 10U);

    assert(delegate.snapshot_calls == 1);
    assert(delegate.last_command == SnapshotCommandType::SNAPSHOT_BEGIN);
    // The delegate relays the payload to the driver verbatim, so it must be the
    // exact bytes the App encoded.
    assert(delegate.last_payload == payload);

    // A commit on the same connection must also reach the delegate: the App
    // streams begin/chunk/commit without reconnecting.
    viper::daemon::SnapshotCommit commit{};
    commit.app_generation = 4;
    commit.daemon_generation = 6;
    std::vector<uint8_t> commit_payload;
    assert(viper::daemon::EncodeSnapshotCommit(commit, &commit_payload, &error));
    delegate.apply_result.error_code = 0;
    assert(client.SendFrame(
        static_cast<uint16_t>(SnapshotCommandType::SNAPSHOT_COMMIT), commit_payload, 32U));
    assert(AwaitReply(server, client, &reply, &reply_payload));
    assert(reply.request_id == 32U);
    assert(delegate.last_command == SnapshotCommandType::SNAPSHOT_COMMIT);
    assert(server.Statistics().snapshot_commands == 2U);
}

void TestRejectedSnapshotCommandReportsError() {
    AppEventServer server(UniqueSocketName("snapshot-nack"));
    std::string error;
    assert(server.Listen(&error));

    RecordingDelegate delegate;
    delegate.apply_result.accepted = false;
    delegate.apply_result.error_code = 10; // STALE_GENERATION
    server.SetDelegate(&delegate);

    FakeAppClient client;
    assert(client.Connect(server.SocketName()));

    viper::daemon::SnapshotAbort abort{};
    abort.reason = 12;
    std::vector<uint8_t> payload;
    assert(viper::daemon::EncodeSnapshotAbort(abort, &payload, &error));
    assert(client.SendFrame(
        static_cast<uint16_t>(SnapshotCommandType::SNAPSHOT_ABORT), payload, 99U));

    FrameHeader reply{};
    std::vector<uint8_t> reply_payload;
    assert(AwaitReply(server, client, &reply, &reply_payload));
    AppApplyResult result{};
    assert(viper::daemon::DecodeAppApplyResult(reply_payload, &result, &error));
    assert(!result.accepted);
    // The App maps this code to a user-visible reason, so it must survive.
    assert(result.error_code == 10U);
    assert(reply.request_id == 99U);
}

void TestMalformedAndUnknownFramesAreCountedNotFatal() {
    AppEventServer server(UniqueSocketName("malformed"));
    std::string error;
    assert(server.Listen(&error));

    RecordingDelegate delegate;
    delegate.hello_ack.daemon_generation = 3;
    server.SetDelegate(&delegate);

    FakeAppClient client;
    assert(client.Connect(server.SocketName()));

    // Garbage bytes: no valid frame header at all.
    assert(client.SendRaw(std::vector<uint8_t>{0x01U, 0x02U, 0x03U, 0x04U}));

    // A well-formed frame whose message_type belongs to neither range.
    std::vector<uint8_t> hello_payload;
    AppHello hello{};
    assert(viper::daemon::EncodeAppHello(hello, &hello_payload, &error));
    assert(client.SendFrame(9000U, hello_payload, 1U));

    // A daemon-to-App type sent in the App-to-daemon direction.
    assert(client.SendFrame(
        static_cast<uint16_t>(AppMessageType::APP_HELLO_ACK), hello_payload, 2U));

    // A valid header whose payload fails the codec for that type.
    std::vector<uint8_t> truncated(hello_payload.begin(), hello_payload.end() - 4);
    assert(client.SendFrame(
        static_cast<uint16_t>(AppMessageType::APP_HELLO), truncated, 3U));

    assert(PumpUntil(server, [&] { return server.Statistics().rejected_frames >= 4U; }));
    assert(delegate.hello_calls == 0);
    assert(server.Statistics().replies_sent == 0U);

    // The connection must survive: a later valid hello is still answered.
    hello.app_generation = 21;
    assert(viper::daemon::EncodeAppHello(hello, &hello_payload, &error));
    assert(client.SendFrame(
        static_cast<uint16_t>(AppMessageType::APP_HELLO), hello_payload, 500U));

    FrameHeader reply{};
    std::vector<uint8_t> reply_payload;
    assert(AwaitReply(server, client, &reply, &reply_payload));
    assert(reply.request_id == 500U);
    AppHelloAck ack{};
    assert(viper::daemon::DecodeAppHelloAck(reply_payload, &ack, &error));
    assert(ack.daemon_generation == 3U);
    assert(delegate.hello_calls == 1);
    assert(delegate.last_hello.app_generation == 21U);
}

void TestReconnectReplacesClient() {
    AppEventServer server(UniqueSocketName("reconnect"));
    std::string error;
    assert(server.Listen(&error));

    RecordingDelegate delegate;
    server.SetDelegate(&delegate);

    std::vector<uint8_t> hello_payload;
    AppHello hello{};
    hello.app_generation = 1;
    assert(viper::daemon::EncodeAppHello(hello, &hello_payload, &error));

    {
        FakeAppClient first;
        assert(first.Connect(server.SocketName()));
        assert(first.SendFrame(
            static_cast<uint16_t>(AppMessageType::APP_HELLO), hello_payload, 1U));
        assert(PumpUntil(server, [&] { return delegate.hello_calls == 1; }));
        assert(server.Connected());
    }

    FakeAppClient second;
    assert(second.Connect(server.SocketName()));
    hello.app_generation = 2;
    assert(viper::daemon::EncodeAppHello(hello, &hello_payload, &error));
    assert(second.SendFrame(
        static_cast<uint16_t>(AppMessageType::APP_HELLO), hello_payload, 2U));

    FrameHeader reply{};
    std::vector<uint8_t> reply_payload;
    assert(AwaitReply(server, second, &reply, &reply_payload));
    assert(reply.request_id == 2U);
    assert(delegate.last_hello.app_generation == 2U);
    assert(server.Statistics().accepted_connections == 2U);
    assert(server.Connected());
}

void TestPollWithoutClientIsANoOp() {
    AppEventServer server(UniqueSocketName("idle"));
    RecordingDelegate delegate;
    server.SetDelegate(&delegate);

    // Not listening yet: Poll must not touch anything.
    assert(server.Poll() == 0U);

    std::string error;
    assert(server.Listen(&error));
    assert(!server.Connected());
    for (int attempt = 0; attempt < 5; ++attempt) {
        assert(server.Poll() == 0U);
    }
    const auto &stats = server.Statistics();
    assert(stats.accepted_connections == 0U);
    assert(stats.rejected_frames == 0U);
    assert(stats.rejected_peers == 0U);
    assert(stats.replies_sent == 0U);

    // Closing an idle server and polling again must stay harmless.
    server.Close();
    assert(!server.Listening());
    assert(server.Poll() == 0U);
}

void TestRefusedPeerIsCountedAndDropped() {
    AppEventServer server(UniqueSocketName("peer"));
    std::string error;
    assert(server.Listen(&error));

    RecordingDelegate delegate;
    server.SetDelegate(&delegate);
    server.SetAllowedPeer([](const AppEventServer::PeerCredentials &) { return false; });

    FakeAppClient client;
    assert(client.Connect(server.SocketName()));

    std::vector<uint8_t> hello_payload;
    AppHello hello{};
    assert(viper::daemon::EncodeAppHello(hello, &hello_payload, &error));
    assert(client.SendFrame(
        static_cast<uint16_t>(AppMessageType::APP_HELLO), hello_payload, 1U));

    assert(PumpUntil(server, [&] { return server.Statistics().rejected_peers >= 1U; }));
    // A refused peer must never reach the delegate: that is the whole point of
    // admission control on a socket ordinary apps can open.
    assert(delegate.hello_calls == 0);
    assert(!server.Connected());
    assert(server.Statistics().accepted_connections == 0U);

    // Restoring the default rule lets this process's own uid back in.
    server.SetAllowedPeer({});
    FakeAppClient allowed;
    assert(allowed.Connect(server.SocketName()));
    assert(allowed.SendFrame(
        static_cast<uint16_t>(AppMessageType::APP_HELLO), hello_payload, 2U));
    assert(PumpUntil(server, [&] { return delegate.hello_calls == 1; }));
}

void TestDefaultAdmissionRule() {
    using Peer = AppEventServer::PeerCredentials;
    // Root and the daemon's own uid: the daemon and its tests talk to themselves.
    assert(AppEventServer::DefaultAllowedPeer(Peer{0U, 0U, 1}));
    assert(AppEventServer::DefaultAllowedPeer(
        Peer{static_cast<uint32_t>(::getuid()), 0U, 1}));
    // Ordinary app uids in the owner profile and in a work profile.
    assert(AppEventServer::DefaultAllowedPeer(Peer{10234U, 10234U, 1}));
    assert(AppEventServer::DefaultAllowedPeer(Peer{1010234U, 1010234U, 1}));
    // System services must not be able to drive this endpoint.
    assert(!AppEventServer::DefaultAllowedPeer(Peer{1000U, 1000U, 1}));
    assert(!AppEventServer::DefaultAllowedPeer(Peer{1041U, 1041U, 1}));
    assert(!AppEventServer::DefaultAllowedPeer(Peer{2000U, 2000U, 1}));
    // Isolated processes (appid 99000+) are the most likely hostile caller.
    assert(!AppEventServer::DefaultAllowedPeer(Peer{99001U, 99001U, 1}));
}

} // namespace

int main() {
    TestHelloRoundTripCarriesDelegateState();
    TestRouteReportReachesDelegateAndAcks();
    TestRejectedRouteReportStillAcks();
    TestSnapshotCommandsAreForwarded();
    TestRejectedSnapshotCommandReportsError();
    TestMalformedAndUnknownFramesAreCountedNotFatal();
    TestReconnectReplacesClient();
    TestPollWithoutClientIsANoOp();
    TestRefusedPeerIsCountedAndDropped();
    TestDefaultAdmissionRule();
    std::printf("AppEventServerTest: all cases passed\n");
    return 0;
}
