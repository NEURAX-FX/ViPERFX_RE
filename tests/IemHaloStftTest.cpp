#include "iem/HaloStft.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

bool Check(bool condition, const char *message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}

bool TestLatencyAndImpulse() {
    iem::HaloStft stft;
    if (!Check(stft.Prepare(256), "prepare STFT")) return false;
    if (!Check(stft.LatencyFrames() == 1024, "reported latency is 1024")) return false;

    std::vector<float> left(256, 0.0F);
    std::vector<float> right(256, 0.0F);
    left[0] = 1.0F;
    if (!Check(stft.Process(left.data(), right.data(), left.size(), nullptr, nullptr),
            "process impulse block")) return false;

    std::vector<float> output(2048, 0.0F);
    float re[iem::HaloStft::kBins]{};
    float im[iem::HaloStft::kBins]{};
    re[0] = 1.0F;
    if (!Check(stft.InverseAdd(re, im, output.data(), output.size()),
            "inverse-add DC bin")) return false;
    return Check(std::isfinite(output[1024]), "impulse appears after reported latency");
}

bool TestSineRoundTrip() {
    iem::HaloStft analysis;
    iem::HaloStft synthesis;
    if (!Check(analysis.Prepare(512) && synthesis.Prepare(512), "prepare pair")) return false;

    constexpr float kTwoPi = 6.283185307179586F;
    std::vector<float> left(2048);
    std::vector<float> right(2048);
    for (std::size_t index = 0; index < left.size(); ++index) {
        left[index] = std::sin(kTwoPi * 8.0F * static_cast<float>(index)
            / static_cast<float>(iem::HaloStft::kFftSize));
        right[index] = left[index];
    }

    struct Capture {
        float left_re[iem::HaloStft::kBins]{};
        float left_im[iem::HaloStft::kBins]{};
        bool have = false;
    } capture;

    const auto on_frame = [](const float left_re[iem::HaloStft::kBins],
        const float left_im[iem::HaloStft::kBins],
        const float *, const float *, void *user) {
        auto *out = static_cast<Capture *>(user);
        if (out->have) return;
        for (uint32_t bin = 0; bin < iem::HaloStft::kBins; ++bin) {
            out->left_re[bin] = left_re[bin];
            out->left_im[bin] = left_im[bin];
        }
        out->have = true;
    };

    if (!Check(analysis.Process(left.data(), right.data(), left.size(), on_frame, &capture)
            && capture.have, "capture analysis frame")) return false;

    std::vector<float> reconstructed(left.size() + iem::HaloStft::kReportedLatency, 0.0F);
    if (!Check(synthesis.InverseAdd(capture.left_re, capture.left_im,
            reconstructed.data(), reconstructed.size()), "reconstruct sine")) {
        return false;
    }

    double energy = 0.0;
    for (float sample : reconstructed) energy += static_cast<double>(sample) * sample;
    return Check(energy > 1.0e-6, "round-trip energy is non-zero");
}

} // namespace

int main() {
    if (!TestLatencyAndImpulse()) return 1;
    if (!TestSineRoundTrip()) return 1;
    std::puts("IEM Halo STFT tests passed");
    return 0;
}
