#include "OwnerProtocol.h"

#include "ViperDaemonProtocol.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace viper::owner;

void TestMessageRangeAndSelectors() {
    assert(IsOwnerMessage(static_cast<uint16_t>(OwnerMessage::OWNER_HELLO)));
    assert(IsOwnerMessage(static_cast<uint16_t>(OwnerMessage::SESSION_DELTA)));
    assert(!IsOwnerMessage(299));
    assert(!IsOwnerMessage(308));

    assert(IsAllowedEffectSelector(static_cast<uint16_t>(EffectTypeSelector::HIDL)));
    assert(IsAllowedEffectSelector(static_cast<uint16_t>(EffectTypeSelector::AIDL)));
    assert(!IsAllowedEffectSelector(0));
    assert(!IsAllowedEffectSelector(3));
}

void TestHelloRoundTrip() {
    OwnerHello hello{};
    hello.owner_pid = 0x11223344U;
    hello.boot_id = 0x0102030405060708ULL;

    std::vector<uint8_t> bytes;
    std::string error;
    assert(EncodeOwnerHello(hello, &bytes, &error));
    assert(bytes.size() == kOwnerHelloWireSize);
    assert(bytes[0] == 1 && bytes[1] == 0);
    assert(bytes[4] == 0x44 && bytes[7] == 0x11);
    assert(bytes[12] == 0x08 && bytes[19] == 0x01);

    OwnerHello decoded{};
    assert(DecodeOwnerHello(bytes, &decoded, &error));
    assert(decoded.owner_pid == hello.owner_pid);
    assert(decoded.boot_id == hello.boot_id);

    hello.owner_pid = 0;
    assert(!EncodeOwnerHello(hello, &bytes, &error));
}

void TestHelloAckRoundTrip() {
    OwnerHelloAck ack{};
    ack.accepted = true;
    ack.daemon_generation = 9;

    std::vector<uint8_t> bytes;
    std::string error;
    assert(EncodeOwnerHelloAck(ack, &bytes, &error));
    assert(bytes.size() == kOwnerHelloAckWireSize);

    OwnerHelloAck decoded{};
    assert(DecodeOwnerHelloAck(bytes, &decoded, &error));
    assert(decoded.accepted);
    assert(decoded.daemon_generation == 9);

    ack.daemon_generation = 0;
    assert(!EncodeOwnerHelloAck(ack, &bytes, &error));
}

void TestOwnSessionRoundTripAndSelectorRejection() {
    OwnSession own{};
    own.audio_session_id = 0;
    own.selector = EffectTypeSelector::HIDL;

    std::vector<uint8_t> bytes;
    std::string error;
    assert(EncodeOwnSession(own, &bytes, &error));
    assert(bytes.size() == kOwnSessionWireSize);

    OwnSession decoded{};
    assert(DecodeOwnSession(bytes, &decoded, &error));
    assert(decoded.audio_session_id == 0);
    assert(decoded.selector == EffectTypeSelector::HIDL);

    own.selector = static_cast<EffectTypeSelector>(3);
    assert(!EncodeOwnSession(own, &bytes, &error));

    own.selector = EffectTypeSelector::AIDL;
    assert(EncodeOwnSession(own, &bytes, &error));
    bytes[2] = 0;
    bytes[3] = 0;
    assert(!DecodeOwnSession(bytes, &decoded, &error));
}

void TestOwnedRoundTrip() {
    Owned owned{};
    owned.audio_session_id = 0;
    owned.effect_id = 77;
    owned.has_control = true;

    std::vector<uint8_t> bytes;
    std::string error;
    assert(EncodeOwned(owned, &bytes, &error));
    assert(bytes.size() == kOwnedWireSize);

    Owned decoded{};
    assert(DecodeOwned(bytes, &decoded, &error));
    assert(decoded.audio_session_id == 0);
    assert(decoded.effect_id == 77);
    assert(decoded.has_control);

    owned.effect_id = 0;
    assert(!EncodeOwned(owned, &bytes, &error));
}

void TestOwnerFailedRoundTrip() {
    OwnerFailed failed{};
    failed.audio_session_id = 0;
    failed.reason_code = 4;

    std::vector<uint8_t> bytes;
    std::string error;
    assert(EncodeOwnerFailed(failed, &bytes, &error));
    assert(bytes.size() == kOwnerFailedWireSize);

    OwnerFailed decoded{};
    assert(DecodeOwnerFailed(bytes, &decoded, &error));
    assert(decoded.reason_code == 4);

    failed.reason_code = 0;
    assert(!EncodeOwnerFailed(failed, &bytes, &error));
}

void TestReleaseRoundTrips() {
    ReleaseSession release{};
    release.audio_session_id = 0;
    Released released{};
    released.audio_session_id = 0;

    std::vector<uint8_t> bytes;
    std::string error;
    assert(EncodeReleaseSession(release, &bytes, &error));
    assert(bytes.size() == kReleaseSessionWireSize);
    ReleaseSession decoded_release{};
    assert(DecodeReleaseSession(bytes, &decoded_release, &error));

    assert(EncodeReleased(released, &bytes, &error));
    assert(bytes.size() == kReleasedWireSize);
    Released decoded_released{};
    assert(DecodeReleased(bytes, &decoded_released, &error));
}

void TestSessionDeltaRoundTrip() {
    SessionDelta delta{};
    delta.audio_session_id = 123;
    delta.client_uid = 10438;
    delta.appeared = true;

    std::vector<uint8_t> bytes;
    std::string error;
    assert(EncodeSessionDelta(delta, &bytes, &error));
    assert(bytes.size() == kSessionDeltaWireSize);

    SessionDelta decoded{};
    assert(DecodeSessionDelta(bytes, &decoded, &error));
    assert(decoded.audio_session_id == 123);
    assert(decoded.client_uid == 10438);
    assert(decoded.appeared);

    delta.audio_session_id = 0;
    assert(!EncodeSessionDelta(delta, &bytes, &error));
}

void TestMalformedPayloadsAreRejected() {
    std::vector<uint8_t> bytes;
    std::string error;
    OwnSession own{};
    own.selector = EffectTypeSelector::HIDL;
    assert(EncodeOwnSession(own, &bytes, &error));

    OwnSession decoded{};
    std::vector<uint8_t> truncated = bytes;
    truncated.pop_back();
    assert(!DecodeOwnSession(truncated, &decoded, &error));

    std::vector<uint8_t> trailing = bytes;
    trailing.push_back(0);
    assert(!DecodeOwnSession(trailing, &decoded, &error));

    std::vector<uint8_t> bad_version = bytes;
    bad_version[0] = 2;
    assert(!DecodeOwnSession(bad_version, &decoded, &error));

    std::vector<uint8_t> reserved = bytes;
    reserved[8] = 1;
    assert(!DecodeOwnSession(reserved, &decoded, &error));
}

void TestPayloadFitsBoundedFrame() {
    static_assert(kOwnerMaxPayloadBytes <= viper::daemon::kMaxPayloadSize);
    static_assert(kOwnerHelloWireSize <= kOwnerMaxPayloadBytes);
    static_assert(kSessionDeltaWireSize <= kOwnerMaxPayloadBytes);
}

} // namespace

int main() {
    TestMessageRangeAndSelectors();
    TestHelloRoundTrip();
    TestHelloAckRoundTrip();
    TestOwnSessionRoundTripAndSelectorRejection();
    TestOwnedRoundTrip();
    TestOwnerFailedRoundTrip();
    TestReleaseRoundTrips();
    TestSessionDeltaRoundTrip();
    TestMalformedPayloadsAreRejected();
    TestPayloadFitsBoundedFrame();
    std::puts("owner protocol tests passed");
    return 0;
}
