#include "iem/HaloBed.h"
#include "iem/HaloEncoder.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

bool Check(bool condition, const char *message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}

bool TestCentreEncodingAndFold() {
    constexpr std::size_t kFrames = 1;
    float storage[7][kFrames]{};
    const float *bed[7]{};
    for (int channel = 0; channel < 7; ++channel) bed[channel] = storage[channel];
    storage[static_cast<int>(iem::HaloBedChannel::C)][0] = 1.0F;

    float ambi_storage[iem::kMaxAmbisonicsChannels][kFrames]{};
    float *ambi[iem::kMaxAmbisonicsChannels]{};
    for (uint32_t channel = 0; channel < iem::kMaxAmbisonicsChannels; ++channel) {
        ambi[channel] = ambi_storage[channel];
    }
    iem::EncodeHaloBedToSn3d(1, bed, ambi, kFrames);
    if (!Check(std::fabs(ambi[0][0]) > 0.1F, "centre contributes W")) return false;
    if (!Check(std::fabs(ambi[1][0]) < 1.0e-5F, "centre has no Y")) return false;
    if (!Check(std::fabs(ambi[2][0]) < 1.0e-5F, "centre has no Z")) return false;

    float stereo_storage[2][kFrames]{};
    float *stereo[2]{stereo_storage[0], stereo_storage[1]};
    iem::FoldHaloBedToStereo(bed, stereo, kFrames);
    constexpr float kCentre = 0.7071067811865476F;
    return Check(std::fabs(stereo[0][0] - kCentre) < 1.0e-6F,
            "centre folds equally left")
        && Check(std::fabs(stereo[1][0] - kCentre) < 1.0e-6F,
            "centre folds equally right");
}

bool TestLeftDirection() {
    float storage[7][1]{};
    const float *bed[7]{};
    for (int channel = 0; channel < 7; ++channel) bed[channel] = storage[channel];
    storage[static_cast<int>(iem::HaloBedChannel::L)][0] = 1.0F;
    float ambi_storage[iem::kMaxAmbisonicsChannels][1]{};
    float *ambi[iem::kMaxAmbisonicsChannels]{};
    for (uint32_t channel = 0; channel < iem::kMaxAmbisonicsChannels; ++channel) {
        ambi[channel] = ambi_storage[channel];
    }
    iem::EncodeHaloBedToSn3d(1, bed, ambi, 1);
    return Check(ambi[1][0] < 0.0F, "-30 degree source has negative Y")
        && Check(ambi[3][0] > 0.0F, "-30 degree source has positive X");
}

bool TestPreparedSilence() {
    constexpr std::size_t kFrames = 256;
    iem::HaloEncoder encoder;
    iem::EncoderConfig config{};
    config.max_frames = kFrames;
    if (!Check(encoder.Prepare(config), "prepare HaloEncoder")) return false;
    encoder.ApplyParams(iem::IemParams{});
    if (!Check(encoder.PreparedBytes() > 0, "prepared storage is reported")) return false;

    float input_storage[2][kFrames]{};
    const float *input[2]{input_storage[0], input_storage[1]};
    float bed_storage[7][kFrames]{};
    float *bed[7]{};
    for (int channel = 0; channel < 7; ++channel) bed[channel] = bed_storage[channel];

    for (int block = 0; block < 8; ++block) {
        if (!Check(encoder.ProcessBed(input, bed, kFrames), "process silent block")) return false;
        for (int channel = 0; channel < 7; ++channel) {
            for (float sample : bed_storage[channel]) {
                if (!std::isfinite(sample) || sample != 0.0F) {
                    return Check(false, "silent Halo output is finite zero");
                }
            }
        }
    }
    return Check(encoder.StftLatencyFrames() == 1024, "Halo reports STFT latency");
}

} // namespace

int main() {
    if (!TestCentreEncodingAndFold()) return 1;
    if (!TestLeftDirection()) return 1;
    if (!TestPreparedSilence()) return 1;
    std::puts("IEM Halo encoder tests passed");
    return 0;
}
