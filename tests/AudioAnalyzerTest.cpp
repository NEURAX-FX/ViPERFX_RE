#include "viper/effects/AudioAnalyzer.h"
#include "TelemetryProtocol.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr float kPi = 3.14159265358979323846F;

bool Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
    return condition;
}

std::vector<float> StereoSignal(
    uint32_t frames, uint32_t sample_rate, float frequency, float amplitude
) {
    std::vector<float> samples(frames * 2);
    for (uint32_t frame = 0; frame < frames; ++frame) {
        const float value = amplitude
            * std::sin(2.0F * kPi * frequency * static_cast<float>(frame)
                       / static_cast<float>(sample_rate));
        samples[frame * 2] = value;
        samples[frame * 2 + 1] = value;
    }
    return samples;
}

bool TestDisabledAndSilence() {
    viper::AudioAnalyzer analyzer;
    if (!Check(analyzer.Configure(48000, 2), "configure succeeds")) return false;

    const std::vector<float> silence(2048 * 2, 0.0F);
    analyzer.Push(silence.data(), 2048);

    viper::AnalyzerSnapshot snapshot{};
    analyzer.ReadTelemetry(&snapshot);
    if (!Check((snapshot.valid_mask & viper::kSpectrumValid) == 0,
               "disabled analyzer ignores samples")) {
        return false;
    }

    analyzer.Push(silence.data(), 2048);
    analyzer.ReadTelemetry(&snapshot);
    if (!Check((snapshot.valid_mask & viper::kSpectrumValid) != 0,
               "armed analyzer produces a spectrum")) {
        return false;
    }
    return Check(
        std::all_of(snapshot.spectrum_db.begin(), snapshot.spectrum_db.end(), [](float value) {
            return std::abs(value + 96.0F) < 0.01F;
        }),
        "silence remains at the dB floor"
    );
}

bool TestTonePeak() {
    viper::AudioAnalyzer analyzer;
    if (!Check(analyzer.Configure(48000, 2), "configure succeeds")) return false;

    viper::AnalyzerSnapshot snapshot{};
    analyzer.ReadTelemetry(&snapshot);
    const auto tone = StereoSignal(2048, 48000, 1000.0F, 1.0F);
    analyzer.Push(tone.data(), 2048);
    analyzer.ReadTelemetry(&snapshot);

    const auto peak = std::max_element(snapshot.spectrum_db.begin(), snapshot.spectrum_db.end());
    const size_t peak_index = static_cast<size_t>(peak - snapshot.spectrum_db.begin());
    const float band_center = viper::AudioAnalyzer::BandCenterFrequency(peak_index, 48000);
    return Check((snapshot.valid_mask & viper::kSpectrumValid) != 0, "tone is valid")
        && Check(*peak > -3.0F, "full-scale tone peak is near 0 dBFS")
        && Check(band_center > 850.0F && band_center < 1200.0F,
                 "1 kHz tone peaks in the expected log band");
}

bool TestResetAndOverrun() {
    viper::AudioAnalyzer analyzer;
    if (!Check(analyzer.Configure(48000, 2), "configure succeeds")) return false;

    viper::AnalyzerSnapshot snapshot{};
    analyzer.ReadTelemetry(&snapshot);
    const auto signal = StereoSignal(12000, 48000, 440.0F, 0.5F);
    analyzer.Push(signal.data(), 12000);
    analyzer.ReadTelemetry(&snapshot);
    if (!Check(snapshot.overrun_count > 0, "overrun is counted without blocking")) return false;

    analyzer.Reset();
    analyzer.Push(signal.data(), 2048);
    analyzer.ReadTelemetry(&snapshot);
    return Check((snapshot.valid_mask & viper::kSpectrumValid) == 0,
                 "reset disables capture and invalidates stale data");
}

bool TestMetersAndWireLayout() {
    viper::AudioAnalyzer analyzer;
    if (!Check(analyzer.Configure(48000, 2), "configure succeeds")) return false;
    analyzer.SetMeter(0, 7.5F);
    analyzer.SetMeter(1, -4.0F);
    analyzer.SetMeter(2, INFINITY);

    viper::AnalyzerSnapshot snapshot{};
    analyzer.ReadTelemetry(&snapshot);
    const auto wire = viper::MakeTelemetryWire(snapshot);
    return Check(sizeof(wire) == 320, "wire layout stays fixed at 320 bytes")
        && Check(wire.version == viper::kTelemetryVersion, "wire version is explicit")
        && Check(wire.spectrum_band_count == 64, "wire spectrum count is explicit")
        && Check(wire.meter_count == 8, "wire meter count is explicit")
        && Check((wire.valid_mask & viper::kMetersValid) != 0, "meter validity is exposed")
        && Check(std::abs(wire.meter_db[0] - 7.5F) < 0.001F, "meter ordering is stable")
        && Check(wire.meter_db[1] == 0.0F, "negative reduction is clamped")
        && Check(wire.meter_db[2] == 0.0F, "non-finite reduction is clamped");
}

}  // namespace

int main() {
    const bool passed = TestDisabledAndSilence() && TestTonePeak() && TestResetAndOverrun()
        && TestMetersAndWireLayout();
    if (passed) std::puts("AudioAnalyzer tests passed");
    return passed ? 0 : 1;
}
