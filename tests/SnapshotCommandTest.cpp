#include "SnapshotCommand.h"

#include "DriverEvent.h"
#include "SnapshotSchema.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using viper::daemon::DecodeSnapshotAbort;
using viper::daemon::DecodeSnapshotBegin;
using viper::daemon::DecodeSnapshotChunk;
using viper::daemon::DecodeSnapshotCommit;
using viper::daemon::EncodeSnapshotAbort;
using viper::daemon::EncodeSnapshotBegin;
using viper::daemon::EncodeSnapshotChunk;
using viper::daemon::EncodeSnapshotCommit;
using viper::daemon::SnapshotAbort;
using viper::daemon::SnapshotBegin;
using viper::daemon::SnapshotChunk;
using viper::daemon::SnapshotCommandType;
using viper::daemon::SnapshotCommit;

const std::string kHash(64, 'a');

SnapshotBegin MakeBegin() {
    SnapshotBegin begin{};
    begin.app_generation = 5;
    begin.daemon_generation = 7;
    begin.total_size = 4096;
    begin.crc32 = 0xDEADBEEFU;
    begin.device_key_hash = kHash;
    return begin;
}

void TestBeginRoundTrip() {
    const SnapshotBegin begin = MakeBegin();
    std::vector<uint8_t> encoded;
    std::string error;
    assert(EncodeSnapshotBegin(begin, &encoded, &error));
    assert(encoded.size() == viper::daemon::kSnapshotBeginWireSize);

    SnapshotBegin decoded{};
    assert(DecodeSnapshotBegin(encoded, &decoded, &error));
    assert(decoded.version == begin.version);
    assert(decoded.app_generation == begin.app_generation);
    assert(decoded.daemon_generation == begin.daemon_generation);
    assert(decoded.total_size == begin.total_size);
    assert(decoded.crc32 == begin.crc32);
    assert(decoded.device_key_hash == begin.device_key_hash);

    // Deterministic encoding keeps the wire stable across daemon versions.
    std::vector<uint8_t> again;
    assert(EncodeSnapshotBegin(decoded, &again, &error));
    assert(again == encoded);
}

void TestBeginRejectsInvalidFields() {
    std::vector<uint8_t> encoded;
    std::string error;

    SnapshotBegin no_size = MakeBegin();
    no_size.total_size = 0;
    assert(!EncodeSnapshotBegin(no_size, &encoded, &error));

    SnapshotBegin huge = MakeBegin();
    huge.total_size = static_cast<uint32_t>(viper::daemon::kMaxSnapshotSize) + 1U;
    assert(!EncodeSnapshotBegin(huge, &encoded, &error));

    SnapshotBegin no_app = MakeBegin();
    no_app.app_generation = 0;
    assert(!EncodeSnapshotBegin(no_app, &encoded, &error));

    SnapshotBegin no_daemon = MakeBegin();
    no_daemon.daemon_generation = 0;
    assert(!EncodeSnapshotBegin(no_daemon, &encoded, &error));

    SnapshotBegin short_hash = MakeBegin();
    short_hash.device_key_hash = "abcd";
    assert(!EncodeSnapshotBegin(short_hash, &encoded, &error));

    SnapshotBegin upper_hash = MakeBegin();
    upper_hash.device_key_hash = std::string(64, 'A');
    assert(!EncodeSnapshotBegin(upper_hash, &encoded, &error));

    SnapshotBegin bad_version = MakeBegin();
    bad_version.version = 2;
    assert(!EncodeSnapshotBegin(bad_version, &encoded, &error));
}

void TestBeginDecodeRejectsMalformedBytes() {
    std::vector<uint8_t> encoded;
    std::string error;
    assert(EncodeSnapshotBegin(MakeBegin(), &encoded, &error));

    SnapshotBegin decoded{};

    std::vector<uint8_t> truncated = encoded;
    truncated.pop_back();
    assert(!DecodeSnapshotBegin(truncated, &decoded, &error));

    std::vector<uint8_t> trailing = encoded;
    trailing.push_back(0U);
    assert(!DecodeSnapshotBegin(trailing, &decoded, &error));

    std::vector<uint8_t> bad_version = encoded;
    bad_version[0] = 0x02;
    assert(!DecodeSnapshotBegin(bad_version, &decoded, &error));

    // Reserved fields must be zero so future flags cannot be silently ignored.
    std::vector<uint8_t> reserved_head = encoded;
    reserved_head[2] = 0x01;
    assert(!DecodeSnapshotBegin(reserved_head, &decoded, &error));

    std::vector<uint8_t> reserved_tail = encoded;
    reserved_tail[28] = 0x01;
    assert(!DecodeSnapshotBegin(reserved_tail, &decoded, &error));

    // A hash with a non-hex byte must not be accepted as a route identity.
    std::vector<uint8_t> bad_hash = encoded;
    bad_hash[viper::daemon::kSnapshotBeginWireSize - 1] = 'z';
    assert(!DecodeSnapshotBegin(bad_hash, &decoded, &error));

    std::vector<uint8_t> zero_size = encoded;
    std::memset(zero_size.data() + 4, 0, sizeof(uint32_t));
    assert(!DecodeSnapshotBegin(zero_size, &decoded, &error));

    std::vector<uint8_t> zero_generation = encoded;
    std::memset(zero_generation.data() + 8, 0, sizeof(uint64_t));
    assert(!DecodeSnapshotBegin(zero_generation, &decoded, &error));
}

void TestChunkRoundTrip() {
    SnapshotChunk chunk{};
    chunk.offset = 8192;
    chunk.data.assign(1024, 0x5AU);

    std::vector<uint8_t> encoded;
    std::string error;
    assert(EncodeSnapshotChunk(chunk, &encoded, &error));
    assert(encoded.size() == viper::daemon::kSnapshotChunkHeaderSize + chunk.data.size());

    SnapshotChunk decoded{};
    assert(DecodeSnapshotChunk(encoded, &decoded, &error));
    assert(decoded.offset == chunk.offset);
    assert(decoded.data == chunk.data);
}

void TestChunkRejectsBadSizes() {
    std::vector<uint8_t> encoded;
    std::string error;

    SnapshotChunk empty{};
    empty.offset = 0;
    assert(!EncodeSnapshotChunk(empty, &encoded, &error));

    SnapshotChunk oversized{};
    oversized.data.assign(viper::daemon::kMaxSnapshotChunkBytes + 1U, 0U);
    assert(!EncodeSnapshotChunk(oversized, &encoded, &error));

    SnapshotChunk good{};
    good.offset = 16;
    good.data.assign(64, 0x11U);
    assert(EncodeSnapshotChunk(good, &encoded, &error));

    SnapshotChunk decoded{};

    std::vector<uint8_t> header_only(
        encoded.begin(), encoded.begin() + viper::daemon::kSnapshotChunkHeaderSize);
    assert(!DecodeSnapshotChunk(header_only, &decoded, &error));

    std::vector<uint8_t> truncated_header(encoded.begin(), encoded.begin() + 8);
    assert(!DecodeSnapshotChunk(truncated_header, &decoded, &error));

    // A length field larger than the frame body must be rejected, not clamped:
    // otherwise the driver stages fewer bytes than the daemon believes it sent.
    std::vector<uint8_t> lying_length = encoded;
    const uint32_t bigger = 4096;
    std::memcpy(lying_length.data() + 4, &bigger, sizeof(bigger));
    assert(!DecodeSnapshotChunk(lying_length, &decoded, &error));

    // A shorter declared length leaves trailing bytes, which is equally wrong.
    std::vector<uint8_t> short_length = encoded;
    const uint32_t smaller = 32;
    std::memcpy(short_length.data() + 4, &smaller, sizeof(smaller));
    assert(!DecodeSnapshotChunk(short_length, &decoded, &error));

    std::vector<uint8_t> reserved_set = encoded;
    reserved_set[8] = 0x01;
    assert(!DecodeSnapshotChunk(reserved_set, &decoded, &error));
}

void TestCommitRoundTrip() {
    SnapshotCommit commit{};
    commit.app_generation = 11;
    commit.daemon_generation = 12;

    std::vector<uint8_t> encoded;
    std::string error;
    assert(EncodeSnapshotCommit(commit, &encoded, &error));
    assert(encoded.size() == viper::daemon::kSnapshotCommitWireSize);

    SnapshotCommit decoded{};
    assert(DecodeSnapshotCommit(encoded, &decoded, &error));
    assert(decoded.app_generation == commit.app_generation);
    assert(decoded.daemon_generation == commit.daemon_generation);

    SnapshotCommit zero{};
    assert(!EncodeSnapshotCommit(zero, &encoded, &error));

    std::vector<uint8_t> good;
    assert(EncodeSnapshotCommit(commit, &good, &error));

    std::vector<uint8_t> truncated = good;
    truncated.pop_back();
    assert(!DecodeSnapshotCommit(truncated, &decoded, &error));

    std::vector<uint8_t> reserved_set = good;
    reserved_set[16] = 0x01;
    assert(!DecodeSnapshotCommit(reserved_set, &decoded, &error));

    std::vector<uint8_t> zero_generation = good;
    std::memset(zero_generation.data(), 0, sizeof(uint64_t));
    assert(!DecodeSnapshotCommit(zero_generation, &decoded, &error));
}

void TestAbortRoundTrip() {
    SnapshotAbort abort{};
    abort.reason = 12;

    std::vector<uint8_t> encoded;
    std::string error;
    assert(EncodeSnapshotAbort(abort, &encoded, &error));
    assert(encoded.size() == viper::daemon::kSnapshotAbortWireSize);

    SnapshotAbort decoded{};
    assert(DecodeSnapshotAbort(encoded, &decoded, &error));
    assert(decoded.reason == abort.reason);

    // Reason 0 is legal: the daemon may abort without a specific cause.
    SnapshotAbort plain{};
    assert(EncodeSnapshotAbort(plain, &encoded, &error));
    assert(DecodeSnapshotAbort(encoded, &decoded, &error));
    assert(decoded.reason == 0);

    std::vector<uint8_t> truncated = encoded;
    truncated.pop_back();
    assert(!DecodeSnapshotAbort(truncated, &decoded, &error));

    std::vector<uint8_t> reserved_set = encoded;
    reserved_set[4] = 0x01;
    assert(!DecodeSnapshotAbort(reserved_set, &decoded, &error));
}

void TestCommandTypesDoNotCollideWithDriverEvents() {
    // A decoder must be able to tell a daemon command from a driver event using
    // only the frame header, so the two ranges must stay disjoint.
    const uint16_t types[] = {
        static_cast<uint16_t>(SnapshotCommandType::SNAPSHOT_BEGIN),
        static_cast<uint16_t>(SnapshotCommandType::SNAPSHOT_CHUNK),
        static_cast<uint16_t>(SnapshotCommandType::SNAPSHOT_COMMIT),
        static_cast<uint16_t>(SnapshotCommandType::SNAPSHOT_ABORT),
        static_cast<uint16_t>(SnapshotCommandType::ROUTE_ANNOUNCE),
    };
    for (const uint16_t type : types) {
        assert(viper::daemon::IsSnapshotCommandType(type));
        assert(!viper::daemon::IsKnownDriverEventType(type));
    }
    assert(!viper::daemon::IsSnapshotCommandType(
        static_cast<uint16_t>(viper::daemon::DriverEventType::TELEMETRY)));
    assert(!viper::daemon::IsSnapshotCommandType(99));
    // One past ROUTE_ANNOUNCE: the range must stay closed so an unknown future type
    // is refused rather than silently decoded as an announce.
    assert(!viper::daemon::IsSnapshotCommandType(105));
}

void TestNullOutputsAreRejected() {
    std::string error;
    assert(!EncodeSnapshotBegin(MakeBegin(), nullptr, &error));
    assert(!DecodeSnapshotBegin({}, nullptr, &error));

    SnapshotChunk chunk{};
    chunk.data.assign(4, 0U);
    assert(!EncodeSnapshotChunk(chunk, nullptr, &error));
    assert(!DecodeSnapshotChunk({}, nullptr, &error));

    SnapshotCommit commit{};
    commit.app_generation = 1;
    commit.daemon_generation = 1;
    assert(!EncodeSnapshotCommit(commit, nullptr, &error));
    assert(!DecodeSnapshotCommit({}, nullptr, &error));

    assert(!EncodeSnapshotAbort(SnapshotAbort{}, nullptr, &error));
    assert(!DecodeSnapshotAbort({}, nullptr, &error));
}

} // namespace

int main() {
    TestBeginRoundTrip();
    TestBeginRejectsInvalidFields();
    TestBeginDecodeRejectsMalformedBytes();
    TestChunkRoundTrip();
    TestChunkRejectsBadSizes();
    TestCommitRoundTrip();
    TestAbortRoundTrip();
    TestCommandTypesDoNotCollideWithDriverEvents();
    TestNullOutputsAreRejected();
    std::puts("snapshot command tests passed");
    return 0;
}
