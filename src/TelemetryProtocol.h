#pragma once

#include "viper/effects/AudioAnalyzer.h"

#include <cstddef>
#include <cstdint>

namespace viper {

constexpr uint32_t kTelemetryVersion = 1;
constexpr uint32_t kTelemetryWireSize = 320;

struct TelemetryWire {
    uint32_t version;
    uint32_t sequence;
    uint32_t sample_rate;
    uint32_t fft_size;
    uint32_t spectrum_band_count;
    uint32_t meter_count;
    uint32_t valid_mask;
    uint32_t overrun_count;
    float spectrum_db[AudioAnalyzer::kSpectrumBandCount];
    float meter_db[AudioAnalyzer::kMeterCount];
};

static_assert(sizeof(float) == sizeof(uint32_t), "Telemetry requires 32-bit floats");
static_assert(sizeof(TelemetryWire) == kTelemetryWireSize, "Telemetry wire layout changed");
static_assert(offsetof(TelemetryWire, spectrum_db) == 32, "Telemetry header layout changed");

inline TelemetryWire MakeTelemetryWire(const AnalyzerSnapshot& snapshot) {
    TelemetryWire wire{};
    wire.version = kTelemetryVersion;
    wire.sequence = snapshot.sequence;
    wire.sample_rate = snapshot.sample_rate;
    wire.fft_size = snapshot.fft_size;
    wire.spectrum_band_count = AudioAnalyzer::kSpectrumBandCount;
    wire.meter_count = AudioAnalyzer::kMeterCount;
    wire.valid_mask = snapshot.valid_mask;
    wire.overrun_count = snapshot.overrun_count;
    for (size_t i = 0; i < AudioAnalyzer::kSpectrumBandCount; ++i) {
        wire.spectrum_db[i] = snapshot.spectrum_db[i];
    }
    for (size_t i = 0; i < AudioAnalyzer::kMeterCount; ++i) {
        wire.meter_db[i] = snapshot.meter_db[i];
    }
    return wire;
}

}  // namespace viper
