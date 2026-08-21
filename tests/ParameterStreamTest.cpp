#include "ParameterStream.h"

#include "ViPERParams.h"

#include <cassert>
#include <cstdio>
#include <cstring>

namespace {

using viper::daemon::DecodeParameterStream;
using viper::daemon::EncodeParameterStream;
using viper::daemon::RawParamRecord;
using viper::daemon::RequiredPayloadBytes;

RawParamRecord Scalar(int32_t param, int32_t val1) {
    RawParamRecord record{};
    record.param = param;
    record.val1 = val1;
    return record;
}

RawParamRecord FloatArray(int32_t param, uint32_t arr_size, std::size_t payload_floats) {
    RawParamRecord record{};
    record.param = param;
    record.arr_size = arr_size;
    record.payload.assign(payload_floats * sizeof(float), 0U);
    for (std::size_t index = 0; index < payload_floats; ++index) {
        const float value = static_cast<float>(index) + 0.5F;
        std::memcpy(record.payload.data() + index * sizeof(float), &value, sizeof(float));
    }
    return record;
}

void TestRoundTripPreservesRecords() {
    using namespace viper::params;
    std::vector<RawParamRecord> records{
        Scalar(kParamMasterLimiterOutputVolume, 4096),
        Scalar(kParamEqualizerEnable, 1),
        FloatArray(kParamEqualizerBandLevels, 10, 10),
        FloatArray(kParamConvolverSetBuffer, 64, 64),
        // DDC carries two banks of arr_size floats.
        FloatArray(kParamDdcCoefficients, 5, 10),
    };

    std::vector<uint8_t> encoded;
    std::string error;
    assert(EncodeParameterStream(records, &encoded, &error));
    assert(error.empty());

    std::vector<RawParamRecord> decoded;
    assert(DecodeParameterStream(encoded, &decoded, &error));
    assert(decoded.size() == records.size());
    for (std::size_t index = 0; index < records.size(); ++index) {
        assert(decoded[index].param == records[index].param);
        assert(decoded[index].val1 == records[index].val1);
        assert(decoded[index].val2 == records[index].val2);
        assert(decoded[index].val3 == records[index].val3);
        assert(decoded[index].arr_size == records[index].arr_size);
        assert(decoded[index].payload == records[index].payload);
    }

    // Encoding is deterministic, so snapshots hash stably.
    std::vector<uint8_t> again;
    assert(EncodeParameterStream(decoded, &again, &error));
    assert(again == encoded);
}

void TestEmptyStreamMeansNoParameters() {
    std::vector<RawParamRecord> decoded{Scalar(1, 1)};
    std::string error;
    assert(DecodeParameterStream({}, &decoded, &error));
    assert(decoded.empty());

    std::vector<uint8_t> encoded;
    assert(EncodeParameterStream({}, &encoded, &error));
    assert(encoded.size() == viper::daemon::kParameterStreamHeaderSize);

    std::vector<RawParamRecord> round_trip{Scalar(2, 2)};
    assert(DecodeParameterStream(encoded, &round_trip, &error));
    assert(round_trip.empty());
}

void TestRequiredPayloadMatchesDriverReads() {
    using namespace viper::params;
    assert(RequiredPayloadBytes(kParamConvolverSetBuffer, 8) == 8U * sizeof(float));
    assert(RequiredPayloadBytes(kParamEqualizerBandLevels, 10) == 10U * sizeof(float));
    // DspResources reads coefficients + arr_size for the second bank.
    assert(RequiredPayloadBytes(kParamDdcCoefficients, 5) == 10U * sizeof(float));
    assert(RequiredPayloadBytes(kParamMasterLimiterThreshold, 0) == 0U);
    // Scalar parameters take no array even when a caller claims one.
    assert(RequiredPayloadBytes(kParamMasterLimiterThreshold, 16) == 0U);
}

void TestShortPayloadIsRejected() {
    using namespace viper::params;
    // arr_size claims 64 floats but only 32 are present: accepting this would
    // make DspResources read past the buffer.
    RawParamRecord record = FloatArray(kParamConvolverSetBuffer, 64, 32);
    std::vector<uint8_t> encoded;
    std::string error;
    assert(!EncodeParameterStream({record}, &encoded, &error));
    assert(!error.empty());

    // Same rejection on the decode side, where the bytes come from the daemon.
    RawParamRecord honest = FloatArray(kParamConvolverSetBuffer, 32, 32);
    assert(EncodeParameterStream({honest}, &encoded, &error));
    // Rewrite arr_size in place: 32 floats staged, 64 claimed.
    const uint32_t lie = 64;
    std::memcpy(
        encoded.data() + viper::daemon::kParameterStreamHeaderSize + 16,
        &lie,
        sizeof(lie)
    );
    std::vector<RawParamRecord> decoded;
    assert(!DecodeParameterStream(encoded, &decoded, &error));
}

void TestDdcShortSecondBankIsRejected() {
    using namespace viper::params;
    // A single bank of 5 floats: the driver would read a second bank that is not
    // there.
    RawParamRecord record = FloatArray(kParamDdcCoefficients, 5, 5);
    std::vector<uint8_t> encoded;
    std::string error;
    assert(!EncodeParameterStream({record}, &encoded, &error));
}

void TestScalarWithPayloadIsRejected() {
    using namespace viper::params;
    RawParamRecord record = Scalar(kParamMasterLimiterThreshold, 1);
    record.payload.assign(4, 0U);
    std::vector<uint8_t> encoded;
    std::string error;
    assert(!EncodeParameterStream({record}, &encoded, &error));

    RawParamRecord claims_array = Scalar(kParamMasterLimiterThreshold, 1);
    claims_array.arr_size = 4;
    assert(!EncodeParameterStream({claims_array}, &encoded, &error));
}

void TestOversizedRecordIsRejected() {
    using namespace viper::params;
    // 8192 bytes is the largest vsize the driver accepts.
    const std::size_t too_many_floats = viper::daemon::kMaxRecordPayloadBytes / sizeof(float) + 1;
    RawParamRecord record = FloatArray(
        kParamConvolverSetBuffer,
        static_cast<uint32_t>(too_many_floats),
        too_many_floats
    );
    std::vector<uint8_t> encoded;
    std::string error;
    assert(!EncodeParameterStream({record}, &encoded, &error));
}

void TestTooManyRecordsIsRejected() {
    using namespace viper::params;
    std::vector<RawParamRecord> records;
    records.reserve(viper::daemon::kMaxParameterRecords + 1);
    for (std::size_t index = 0; index <= viper::daemon::kMaxParameterRecords; ++index) {
        records.push_back(Scalar(kParamMasterLimiterThreshold, static_cast<int32_t>(index)));
    }
    std::vector<uint8_t> encoded;
    std::string error;
    assert(!EncodeParameterStream(records, &encoded, &error));
}

void TestMalformedStreamsAreRejected() {
    using namespace viper::params;
    std::vector<RawParamRecord> records{
        Scalar(kParamBassEnable, 1),
        FloatArray(kParamEqualizerBandLevels, 4, 4),
    };
    std::vector<uint8_t> encoded;
    std::string error;
    assert(EncodeParameterStream(records, &encoded, &error));

    std::vector<RawParamRecord> decoded;

    std::vector<uint8_t> bad_magic = encoded;
    bad_magic[0] = 'X';
    assert(!DecodeParameterStream(bad_magic, &decoded, &error));

    std::vector<uint8_t> bad_version = encoded;
    bad_version[4] = 0x02;
    assert(!DecodeParameterStream(bad_version, &decoded, &error));

    std::vector<uint8_t> reserved_set = encoded;
    reserved_set[6] = 0x01;
    assert(!DecodeParameterStream(reserved_set, &decoded, &error));

    std::vector<uint8_t> truncated = encoded;
    truncated.pop_back();
    assert(!DecodeParameterStream(truncated, &decoded, &error));

    std::vector<uint8_t> trailing = encoded;
    trailing.push_back(0U);
    assert(!DecodeParameterStream(trailing, &decoded, &error));

    std::vector<uint8_t> header_only(encoded.begin(), encoded.begin() + 6);
    assert(!DecodeParameterStream(header_only, &decoded, &error));

    // A count larger than the payload can satisfy.
    std::vector<uint8_t> lying_count = encoded;
    const uint32_t huge = 400;
    std::memcpy(lying_count.data() + 8, &huge, sizeof(huge));
    assert(!DecodeParameterStream(lying_count, &decoded, &error));
}

void TestNullOutputsAreRejected() {
    using namespace viper::params;
    std::string error;
    assert(!EncodeParameterStream({Scalar(kParamBassEnable, 1)}, nullptr, &error));

    std::vector<uint8_t> encoded;
    assert(EncodeParameterStream({Scalar(kParamBassEnable, 1)}, &encoded, &error));
    assert(!DecodeParameterStream(encoded, nullptr, &error));
}

} // namespace

int main() {
    TestRoundTripPreservesRecords();
    TestEmptyStreamMeansNoParameters();
    TestRequiredPayloadMatchesDriverReads();
    TestShortPayloadIsRejected();
    TestDdcShortSecondBankIsRejected();
    TestScalarWithPayloadIsRejected();
    TestOversizedRecordIsRejected();
    TestTooManyRecordsIsRejected();
    TestMalformedStreamsAreRejected();
    TestNullOutputsAreRejected();
    std::puts("parameter stream tests passed");
    return 0;
}
