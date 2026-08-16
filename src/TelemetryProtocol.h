#pragma once

#include "iem/IemTelemetry.h"
#include "viper/effects/AudioAnalyzer.h"

#include <cstddef>
#include <cstdint>

namespace viper {

constexpr uint32_t kTelemetryVersion = 1;
constexpr uint32_t kTelemetryWireSize = 320;
constexpr uint32_t kIemTelemetryVersion = 3;
constexpr uint32_t kIemTelemetryWireSize = 168;

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

struct IemTelemetryWire {
    uint32_t version;
    uint32_t wire_size;
    uint64_t sequence;
    uint64_t processed_frames;
    uint64_t latest_process_ns;
    uint64_t average_process_ns;
    uint64_t max_process_ns;
    uint64_t deadline_misses;
    uint64_t output_underflows;
    uint64_t input_overflows;
    uint64_t output_overflows;
    uint64_t grain_pool_exhaustions;
    uint64_t graph_generation;
    uint32_t host_sample_rate;
    uint32_t internal_sample_rate;
    uint32_t latency_frames;
    uint32_t bypass_reason;
    uint32_t enabled;
    uint32_t prepared;
    uint32_t active_grains;
    uint32_t encoder_mode;
    uint32_t render_mode;
    uint32_t ambisonics_order;
    uint32_t halo_prepared;
    uint32_t halo_stft_latency_frames;
    uint32_t dialog_net_result;
    uint32_t fault_code;
    uint32_t preparation_result;
    float latency_ms;
    float limiter_gain_reduction_db;
};

static_assert(
    sizeof(IemTelemetryWire) == kIemTelemetryWireSize,
    "IEM telemetry wire layout changed"
);

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

inline IemTelemetryWire MakeIemTelemetryWire(
    const iem::IemTelemetrySnapshot &snapshot
) {
    IemTelemetryWire wire{};
    wire.version = kIemTelemetryVersion;
    wire.wire_size = kIemTelemetryWireSize;
    wire.sequence = snapshot.sequence;
    wire.processed_frames = snapshot.processed_frames;
    wire.latest_process_ns = snapshot.latest_process_ns;
    wire.average_process_ns = snapshot.average_process_ns;
    wire.max_process_ns = snapshot.max_process_ns;
    wire.deadline_misses = snapshot.deadline_misses;
    wire.output_underflows = snapshot.output_underflows;
    wire.input_overflows = snapshot.input_overflows;
    wire.output_overflows = snapshot.output_overflows;
    wire.grain_pool_exhaustions = snapshot.grain_pool_exhaustions;
    wire.graph_generation = snapshot.graph_generation;
    wire.host_sample_rate = snapshot.host_sample_rate;
    wire.internal_sample_rate = snapshot.internal_sample_rate;
    wire.latency_frames = snapshot.latency_frames;
    wire.bypass_reason = static_cast<uint32_t>(snapshot.bypass_reason);
    wire.enabled = snapshot.enabled ? 1U : 0U;
    wire.prepared = snapshot.prepared ? 1U : 0U;
    wire.active_grains = snapshot.active_grains;
    wire.encoder_mode = snapshot.encoder_mode;
    wire.render_mode = snapshot.render_mode;
    wire.ambisonics_order = snapshot.ambisonics_order;
    wire.halo_prepared = snapshot.halo_prepared;
    wire.halo_stft_latency_frames = snapshot.halo_stft_latency_frames;
    wire.dialog_net_result = static_cast<uint32_t>(snapshot.dialog_net_result);
    wire.fault_code = snapshot.fault_code;
    wire.preparation_result = static_cast<uint32_t>(snapshot.preparation_result);
    wire.latency_ms = snapshot.latency_ms;
    wire.limiter_gain_reduction_db = snapshot.limiter_gain_reduction_db;
    return wire;
}

}  // namespace viper
