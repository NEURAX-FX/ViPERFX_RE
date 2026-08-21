#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace viper::daemon {

// Wire format for Snapshot::parameters and Snapshot::iem_parameters.
//
// The driver's only parameter entry point is ViperContext::DispatchRawParam, so a
// snapshot carries exactly the raw records needed to replay it. `payload_length`
// is explicit and independent of `arr_size` because some parameters consume more
// bytes than `arr_size` alone implies (see RequiredPayloadBytes).
constexpr uint16_t kParameterStreamVersion = 1;
constexpr std::size_t kParameterStreamHeaderSize = 12;
constexpr std::size_t kParameterRecordHeaderSize = 24;
constexpr std::size_t kMaxParameterRecords = 512;
// Matches the largest effect_param_t vsize the driver accepts (8192 bytes).
constexpr std::size_t kMaxRecordPayloadBytes = 8192;
constexpr std::size_t kMaxParameterStreamBytes = 1024U * 1024U;

struct RawParamRecord {
    int32_t param = 0;
    int32_t val1 = 0;
    int32_t val2 = 0;
    int32_t val3 = 0;
    uint32_t arr_size = 0;
    std::vector<uint8_t> payload;
};

// Minimum payload bytes the driver will read for `param` given `arr_size`.
// Returns 0 when the parameter takes no array. Overflow-safe.
std::size_t RequiredPayloadBytes(int32_t param, uint32_t arr_size) noexcept;

// Rejects records whose payload cannot satisfy the driver's read pattern.
bool ValidateRawParamRecord(const RawParamRecord &record, std::string *error);

bool EncodeParameterStream(
    const std::vector<RawParamRecord> &records,
    std::vector<uint8_t> *out,
    std::string *error
);

bool DecodeParameterStream(
    std::span<const uint8_t> bytes,
    std::vector<RawParamRecord> *records,
    std::string *error
);

} // namespace viper::daemon
