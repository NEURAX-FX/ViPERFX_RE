#include "ViperDaemonProtocol.h"

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using viper::daemon::DecodeFrame;
using viper::daemon::EncodeFrame;
using viper::daemon::FrameError;
using viper::daemon::FrameHeader;

void TestRoundTrip() {
    FrameHeader input{};
    input.protocol_version = viper::daemon::kProtocolVersion;
    input.message_type = 7;
    input.flags = 3;
    input.request_id = 0x0102030405060708ULL;
    input.sequence = 0x1112131415161718ULL;
    const std::string payload("viper daemon payload");

    std::vector<uint8_t> encoded;
    std::string error;
    assert(EncodeFrame(input, payload, &encoded, &error));
    assert(encoded.size() == viper::daemon::kFrameHeaderSize + payload.size());

    FrameHeader decoded{};
    std::vector<uint8_t> decoded_payload;
    assert(DecodeFrame(encoded, &decoded, &decoded_payload, &error));
    assert(decoded.protocol_version == input.protocol_version);
    assert(decoded.message_type == input.message_type);
    assert(decoded.flags == input.flags);
    assert(decoded.request_id == input.request_id);
    assert(decoded.sequence == input.sequence);
    assert(decoded.payload_length == payload.size());
    assert(std::string(decoded_payload.begin(), decoded_payload.end()) == payload);
}

void TestLittleEndianHeader() {
    FrameHeader input{};
    input.protocol_version = viper::daemon::kProtocolVersion;
    input.message_type = 0x1122;
    input.flags = 0x00005566;
    input.request_id = 0x778899AABBCCDDEEULL;
    input.sequence = 0x0102030405060708ULL;

    std::vector<uint8_t> encoded;
    std::string error;
    assert(EncodeFrame(input, "", &encoded, &error));
    assert(encoded[0] == 'V');
    assert(encoded[1] == '4');
    assert(encoded[2] == 'A');
    assert(encoded[3] == 'D');
    assert(encoded[4] == 0x01);
    assert(encoded[5] == 0x00);
    assert(encoded[6] == 0x22);
    assert(encoded[7] == 0x11);
    assert(encoded[8] == 0x66);
    assert(encoded[9] == 0x55);
    assert(encoded[10] == 0x00);
    assert(encoded[11] == 0x00);
    assert(encoded[12] == 0xEE);
    assert(encoded[13] == 0xDD);
    assert(encoded[19] == 0x77);
}

void TestRejectsMalformedFrames() {
    FrameHeader input{};
    input.protocol_version = viper::daemon::kProtocolVersion;
    std::vector<uint8_t> encoded;
    std::string error;
    assert(EncodeFrame(input, "payload", &encoded, &error));

    FrameHeader decoded{};
    std::vector<uint8_t> payload;
    encoded[0] = 'X';
    assert(!DecodeFrame(encoded, &decoded, &payload, &error));
    assert(error == viper::daemon::FrameErrorMessage(FrameError::BAD_MAGIC));

    assert(EncodeFrame(input, "payload", &encoded, &error));
    encoded[4] = 0x7F;
    encoded[5] = 0x7F;
    assert(!DecodeFrame(encoded, &decoded, &payload, &error));
    assert(error == viper::daemon::FrameErrorMessage(FrameError::UNSUPPORTED_VERSION));

    assert(EncodeFrame(input, "payload", &encoded, &error));
    encoded.back() ^= 0xFF;
    assert(!DecodeFrame(encoded, &decoded, &payload, &error));
    assert(error == viper::daemon::FrameErrorMessage(FrameError::CRC_MISMATCH));

    assert(EncodeFrame(input, "payload", &encoded, &error));
    encoded.push_back(0);
    assert(!DecodeFrame(encoded, &decoded, &payload, &error));
    assert(error == viper::daemon::FrameErrorMessage(FrameError::TRAILING_BYTES));
}

void TestRejectsOversizedPayload() {
    FrameHeader input{};
    input.protocol_version = viper::daemon::kProtocolVersion;
    const std::string payload(viper::daemon::kMaxPayloadSize + 1, 'x');
    std::vector<uint8_t> encoded;
    std::string error;
    assert(!EncodeFrame(input, payload, &encoded, &error));
    assert(error == viper::daemon::FrameErrorMessage(FrameError::PAYLOAD_TOO_LARGE));
}

} // namespace

int main() {
    TestRoundTrip();
    TestLittleEndianHeader();
    TestRejectsMalformedFrames();
    TestRejectsOversizedPayload();
    return 0;
}
