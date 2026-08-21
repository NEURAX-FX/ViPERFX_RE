// Emits framed owner messages as hex so the ART owner's Java codec can be
// diffed against the native codec byte for byte.
//
// The owner and the daemon are separate programs with independently written
// codecs; a silent layout drift between them would only surface on a device as
// a rejected frame. This emitter is the fixture side of that contract check.
#include "OwnerProtocol.h"
#include "ViperDaemonProtocol.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace viper::owner;

// Fixed, non-trivial header values: a byte-order mistake in request_id or
// sequence stays invisible with zeroes.
constexpr uint64_t kRequestId = 0x1122334455667788ULL;
constexpr uint64_t kSequence = 0x0807060504030201ULL;

int failures = 0;

void Emit(const char *name, OwnerMessage message, const std::vector<uint8_t> &payload) {
    viper::daemon::FrameHeader header{};
    header.message_type = static_cast<uint16_t>(message);
    header.request_id = kRequestId;
    header.sequence = kSequence;

    const std::string_view payload_view(
        reinterpret_cast<const char *>(payload.data()), payload.size());
    std::vector<uint8_t> frame;
    std::string error;
    if (!viper::daemon::EncodeFrame(header, payload_view, &frame, &error)) {
        std::fprintf(stderr, "encode frame failed for %s: %s\n", name, error.c_str());
        failures++;
        return;
    }

    std::printf("%s ", name);
    for (const uint8_t byte : frame) std::printf("%02x", byte);
    std::printf("\n");
}

template <typename Value, typename Encoder>
void EmitPayload(const char *name, OwnerMessage message, const Value &value, Encoder encoder) {
    std::vector<uint8_t> payload;
    std::string error;
    if (!encoder(value, &payload, &error)) {
        std::fprintf(stderr, "encode payload failed for %s: %s\n", name, error.c_str());
        failures++;
        return;
    }
    Emit(name, message, payload);
}

} // namespace

int main() {
    OwnerHello hello{};
    hello.owner_pid = 0x11223344ULL;
    hello.boot_id = 0x0102030405060708ULL;
    EmitPayload("owner_hello", OwnerMessage::OWNER_HELLO, hello, EncodeOwnerHello);

    OwnerHelloAck ack{};
    ack.accepted = true;
    ack.daemon_generation = 7ULL;
    EmitPayload("owner_hello_ack", OwnerMessage::OWNER_HELLO_ACK, ack, EncodeOwnerHelloAck);

    OwnSession own{};
    own.audio_session_id = 0;
    own.selector = EffectTypeSelector::HIDL;
    EmitPayload("own_session_hidl", OwnerMessage::OWN_SESSION, own, EncodeOwnSession);

    own.selector = EffectTypeSelector::AIDL;
    EmitPayload("own_session_aidl", OwnerMessage::OWN_SESSION, own, EncodeOwnSession);

    Owned owned{};
    owned.audio_session_id = 0;
    owned.effect_id = 66139U;
    owned.has_control = true;
    EmitPayload("owned", OwnerMessage::OWNED, owned, EncodeOwned);

    OwnerFailed failed{};
    failed.audio_session_id = 0;
    failed.reason_code = 5U;
    EmitPayload("own_failed", OwnerMessage::OWN_FAILED, failed, EncodeOwnerFailed);

    ReleaseSession release{};
    release.audio_session_id = 0;
    EmitPayload("release_session", OwnerMessage::RELEASE_SESSION, release, EncodeReleaseSession);

    Released released{};
    released.audio_session_id = 0;
    EmitPayload("released", OwnerMessage::RELEASED, released, EncodeReleased);

    SessionDelta delta{};
    delta.audio_session_id = 42U;
    delta.client_uid = 10123U;
    delta.appeared = true;
    EmitPayload("session_delta_appeared", OwnerMessage::SESSION_DELTA, delta, EncodeSessionDelta);

    delta.appeared = false;
    EmitPayload("session_delta_gone", OwnerMessage::SESSION_DELTA, delta, EncodeSessionDelta);

    return failures == 0 ? 0 : 1;
}
