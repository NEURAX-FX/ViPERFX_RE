#include "ParameterStream.h"

#include "ViPERParams.h"
#include "ViperDaemonProtocol.h"

#include <limits>

namespace viper::daemon {
namespace {

constexpr std::size_t kMagicSize = 4;
constexpr uint8_t kMagic[kMagicSize] = {'V', '4', 'A', 'P'};

void SetError(std::string *error, const char *message) {
    if (error != nullptr) error->assign(message);
}

void PutU16(std::vector<uint8_t> &out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8U));
}

void PutU32(std::vector<uint8_t> &out, uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<uint8_t>(value >> shift));
    }
}

void PutI32(std::vector<uint8_t> &out, int32_t value) {
    PutU32(out, static_cast<uint32_t>(value));
}

uint16_t ReadU16(std::span<const uint8_t> bytes, std::size_t &offset) noexcept {
    const uint16_t value = static_cast<uint16_t>(bytes[offset])
        | static_cast<uint16_t>(bytes[offset + 1U]) << 8U;
    offset += 2U;
    return value;
}

uint32_t ReadU32(std::span<const uint8_t> bytes, std::size_t &offset) noexcept {
    uint32_t value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        value |= static_cast<uint32_t>(bytes[offset + shift / 8U]) << shift;
    }
    offset += 4U;
    return value;
}

int32_t ReadI32(std::span<const uint8_t> bytes, std::size_t &offset) noexcept {
    return static_cast<int32_t>(ReadU32(bytes, offset));
}

} // namespace

std::size_t RequiredPayloadBytes(int32_t param, uint32_t arr_size) noexcept {
    using namespace viper::params;
    switch (param) {
        case kParamConvolverSetBuffer:
            // Consumed as float samples: DspResources copies arr_size floats.
            return static_cast<std::size_t>(arr_size) * sizeof(float);
        case kParamDdcCoefficients:
            // Two coefficient banks (44100/48000) of arr_size floats each.
            return static_cast<std::size_t>(arr_size) * 2U * sizeof(float);
        case kParamEqualizerBandLevels:
            return static_cast<std::size_t>(arr_size) * sizeof(float);
        default:
            return 0;
    }
}

bool ValidateRawParamRecord(const RawParamRecord &record, std::string *error) {
    if (record.payload.size() > kMaxRecordPayloadBytes) {
        SetError(error, "parameter record payload is too large");
        return false;
    }
    if (record.arr_size > kMaxRecordPayloadBytes) {
        SetError(error, "parameter record arr_size is too large");
        return false;
    }

    const std::size_t required = RequiredPayloadBytes(record.param, record.arr_size);
    if (required == 0U) {
        // Scalar parameter: an array payload would never be read.
        if (!record.payload.empty() || record.arr_size != 0U) {
            SetError(error, "scalar parameter must not carry an array payload");
            return false;
        }
        return true;
    }
    if (required > kMaxRecordPayloadBytes) {
        SetError(error, "parameter record requires more payload than the driver accepts");
        return false;
    }
    // The driver reads `required` bytes unconditionally; a short payload would
    // make it read past the buffer.
    if (record.payload.size() != required) {
        SetError(error, "parameter record payload does not match arr_size");
        return false;
    }
    return true;
}

bool EncodeParameterStream(
    const std::vector<RawParamRecord> &records,
    std::vector<uint8_t> *out,
    std::string *error
) {
    if (out == nullptr) {
        SetError(error, "null output");
        return false;
    }
    if (records.size() > kMaxParameterRecords) {
        SetError(error, "too many parameter records");
        return false;
    }

    std::size_t total = kParameterStreamHeaderSize;
    for (const auto &record : records) {
        if (!ValidateRawParamRecord(record, error)) return false;
        total += kParameterRecordHeaderSize + record.payload.size();
        if (total > kMaxParameterStreamBytes) {
            SetError(error, "parameter stream is too large");
            return false;
        }
    }

    out->clear();
    out->reserve(total);
    out->insert(out->end(), kMagic, kMagic + kMagicSize);
    PutU16(*out, kParameterStreamVersion);
    PutU16(*out, 0U); // reserved, must be zero
    PutU32(*out, static_cast<uint32_t>(records.size()));
    for (const auto &record : records) {
        PutI32(*out, record.param);
        PutI32(*out, record.val1);
        PutI32(*out, record.val2);
        PutI32(*out, record.val3);
        PutU32(*out, record.arr_size);
        PutU32(*out, static_cast<uint32_t>(record.payload.size()));
        out->insert(out->end(), record.payload.begin(), record.payload.end());
    }
    return out->size() == total;
}

bool DecodeParameterStream(
    std::span<const uint8_t> bytes,
    std::vector<RawParamRecord> *records,
    std::string *error
) {
    if (records == nullptr) {
        SetError(error, "null output");
        return false;
    }
    records->clear();

    // An absent stream is valid and means "no parameters".
    if (bytes.empty()) {
        if (error != nullptr) error->clear();
        return true;
    }
    if (bytes.size() > kMaxParameterStreamBytes) {
        SetError(error, "parameter stream is too large");
        return false;
    }
    if (bytes.size() < kParameterStreamHeaderSize) {
        SetError(error, "parameter stream is truncated");
        return false;
    }

    std::size_t offset = 0;
    for (std::size_t index = 0; index < kMagicSize; ++index) {
        if (bytes[offset + index] != kMagic[index]) {
            SetError(error, "bad parameter stream magic");
            return false;
        }
    }
    offset += kMagicSize;

    if (ReadU16(bytes, offset) != kParameterStreamVersion) {
        SetError(error, "unsupported parameter stream version");
        return false;
    }
    if (ReadU16(bytes, offset) != 0U) {
        SetError(error, "reserved parameter stream field must be zero");
        return false;
    }

    const uint32_t count = ReadU32(bytes, offset);
    if (count > kMaxParameterRecords) {
        SetError(error, "too many parameter records");
        return false;
    }

    records->reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        if (bytes.size() - offset < kParameterRecordHeaderSize) {
            SetError(error, "parameter record header is truncated");
            return false;
        }
        RawParamRecord record{};
        record.param = ReadI32(bytes, offset);
        record.val1 = ReadI32(bytes, offset);
        record.val2 = ReadI32(bytes, offset);
        record.val3 = ReadI32(bytes, offset);
        record.arr_size = ReadU32(bytes, offset);
        const uint32_t payload_length = ReadU32(bytes, offset);
        if (payload_length > kMaxRecordPayloadBytes) {
            SetError(error, "parameter record payload is too large");
            return false;
        }
        if (bytes.size() - offset < payload_length) {
            SetError(error, "parameter record payload is truncated");
            return false;
        }
        record.payload.assign(
            bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            bytes.begin() + static_cast<std::ptrdiff_t>(offset + payload_length)
        );
        offset += payload_length;
        if (!ValidateRawParamRecord(record, error)) return false;
        records->push_back(std::move(record));
    }

    if (offset != bytes.size()) {
        SetError(error, "parameter stream has trailing bytes");
        return false;
    }
    if (error != nullptr) error->clear();
    return true;
}

} // namespace viper::daemon
