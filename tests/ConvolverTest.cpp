#include "viper/effects/Convolver.h"
#include "viper/utils/Crc32.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

bool Near(float actual, float expected) {
    return std::fabs(actual - expected) <= 1.0e-4F;
}

bool Check(bool condition, const char *message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}

bool Upload(Convolver &convolver, const float *data, uint32_t samples, uint32_t channels, uint32_t id) {
    convolver.PrepareKernelBuffer(samples, channels, false);
    convolver.SetKernelBuffer(data, samples);
    convolver.CommitKernelBuffer(
        samples,
        Crc32(reinterpret_cast<const uint8_t *>(data), samples * sizeof(float)),
        id
    );
    return convolver.GetKernelID() == id;
}

bool TestFourChannelUploadAndRouting() {
    Convolver convolver;
    convolver.SetSamplingRate(48000);
    std::array<float, 16 * 4> ir{};
    ir[0] = 1.0F;
    ir[1] = 2.0F;
    ir[2] = 3.0F;
    ir[3] = 4.0F;
    if (!Check(Upload(convolver, ir.data(), ir.size(), 4, 77), "commit four-channel kernel")) {
        return false;
    }

    convolver.SetCrossChannel(1.0F);
    convolver.SetWet(1.0F);
    convolver.SetOutputGain(0.0F);
    convolver.SetRouting(MatrixRouting::DIRECT_AND_CROSS);
    convolver.SetCrossDelay(0.0F);
    convolver.SetEnable(true);

    std::vector<float> audio(2304 * 2, 0.0F);
    audio[0] = 1.0F;
    audio[32 * 2 + 1] = 1.0F;
    if (!Check(convolver.Process(audio.data(), audio.data(), 2304) == 2304, "process all frames")) {
        return false;
    }

    constexpr uint32_t latency = 1024;
    return Check(Near(audio[latency * 2], 0.5F), "wrapper H_LL")
        && Check(Near(audio[latency * 2 + 1], 1.0F), "wrapper H_RL")
        && Check(Near(audio[(latency + 32) * 2], 1.5F), "wrapper H_LR")
        && Check(Near(audio[(latency + 32) * 2 + 1], 2.0F), "wrapper H_RR");
}

bool TestDelayMillisecondsConvertAtCurrentSampleRate() {
    Convolver convolver;
    convolver.SetSamplingRate(96000);
    std::array<float, 16 * 2> ir{};
    ir[0] = 1.0F;
    ir[1] = 2.0F;
    if (!Upload(convolver, ir.data(), ir.size(), 2, 78)) return false;

    convolver.SetCrossChannel(1.0F);
    convolver.SetWet(1.0F);
    convolver.SetOutputGain(0.0F);
    convolver.SetRouting(MatrixRouting::DIRECT_AND_CROSS);
    convolver.SetCrossDelay(0.3125F);
    convolver.SetEnable(true);

    std::vector<float> audio(2304 * 2, 0.0F);
    audio[0] = 1.0F;
    convolver.Process(audio.data(), audio.data(), 2304);
    constexpr uint32_t latency = 1024;
    return Check(Near(audio[latency * 2], 0.5F), "delay control keeps direct branch")
        && Check(Near(audio[(latency + 30) * 2 + 1], 1.0F), "0.3125 ms rounds to 30 frames at 96 kHz");
}

bool TestRejectedUploadPreservesKernel() {
    Convolver convolver;
    std::array<float, 16> valid{};
    valid[0] = 1.0F;
    if (!Upload(convolver, valid.data(), valid.size(), 1, 88)) return false;

    std::array<float, 48> invalid{};
    convolver.PrepareKernelBuffer(invalid.size(), 3, false);
    convolver.SetKernelBuffer(invalid.data(), invalid.size());
    convolver.CommitKernelBuffer(
        invalid.size(),
        Crc32(reinterpret_cast<const uint8_t *>(invalid.data()), invalid.size() * sizeof(float)),
        99
    );
    return Check(convolver.GetKernelID() == 88, "rejected upload preserves active kernel ID");
}

} // namespace

int main() {
    if (!TestFourChannelUploadAndRouting()) return 1;
    if (!TestDelayMillisecondsConvertAtCurrentSampleRate()) return 1;
    if (!TestRejectedUploadPreservesKernel()) return 1;
    std::puts("Convolver wrapper tests passed");
    return 0;
}
