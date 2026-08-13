#include "iem/HaloDialogExtractor.h"
#include "iem/IemParams.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

bool Check(bool condition, const char *message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}

void ZeroSpectra(float *left_re, float *left_im, float *right_re, float *right_im) {
    std::memset(left_re, 0, sizeof(float) * iem::HaloStft::kBins);
    std::memset(left_im, 0, sizeof(float) * iem::HaloStft::kBins);
    std::memset(right_re, 0, sizeof(float) * iem::HaloStft::kBins);
    std::memset(right_im, 0, sizeof(float) * iem::HaloStft::kBins);
}

bool TestLogFrequencyAndFeatures() {
    float left_re[iem::HaloStft::kBins]{};
    float left_im[iem::HaloStft::kBins]{};
    float right_re[iem::HaloStft::kBins]{};
    float right_im[iem::HaloStft::kBins]{};
    left_re[8] = 1.0F;
    right_re[8] = 1.0F;

    iem::HaloDialogFeatureFrame frame{};
    iem::ComputeHaloDialogAd(left_re, left_im, right_re, right_im, frame);

    iem::HaloDialogFeatureFrame slots[6];
    for (auto &slot : slots) slot = frame;

    float bin0[37]{};
    float bin8[37]{};
    iem::BuildHaloDialogFeatures(slots, 0, bin0);
    iem::BuildHaloDialogFeatures(slots, 8, bin8);

    return Check(bin0[36] == 0.0F, "bin 0 log-frequency feature clamps to 0")
        && Check(bin8[36] >= 0.0F && bin8[36] <= 1.0F,
            "in-range log-frequency feature stays in 0..1")
        && Check(frame.a[8] > frame.a[0], "occupied bin is louder than silence");
}

bool TestIdenticalDialogMovesEnergyToCentre() {
    iem::HaloDialogExtractor extractor;
    if (!Check(extractor.Prepare(), "prepare extractor")) return false;

    iem::HaloParams params{};
    params.dialog_isolate_thousandths = 1000;
    params.dialog_aggress_thousandths = 500;
    params.dialog_attack_thousandths = 1000;
    params.dialog_release_thousandths = 0;
    params.dialog_mix_in_thousandths = 1000;
    extractor.ApplyParams(params);

    float left_re[iem::HaloStft::kBins];
    float left_im[iem::HaloStft::kBins];
    float right_re[iem::HaloStft::kBins];
    float right_im[iem::HaloStft::kBins];
    ZeroSpectra(left_re, left_im, right_re, right_im);
    left_re[8] = 1.0F;
    right_re[8] = 1.0F;

    iem::HaloDialogFrame out{};
    extractor.ProcessFrame(left_re, left_im, right_re, right_im, out);
    extractor.ProcessFrame(left_re, left_im, right_re, right_im, out);

    const float residual = std::hypot(out.residual_l_re[8], out.residual_l_im[8]);
    const float centre = std::hypot(out.centre_re[8], out.centre_im[8]);
    return Check(residual < 1.0F, "identical L/R residual is reduced")
        && Check(centre > 0.0F, "identical L/R energy moves to centre");
}

} // namespace

int main() {
    if (!TestLogFrequencyAndFeatures()) return 1;
    if (!TestIdenticalDialogMovesEnergyToCentre()) return 1;
    std::puts("IEM Halo dialog extractor tests passed");
    return 0;
}
