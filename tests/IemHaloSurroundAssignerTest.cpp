#include "iem/HaloSurroundAssigner.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

bool Check(bool condition, const char *message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}

void FillInPhaseResidual(iem::HaloDialogFrame &frame, float amplitude) {
    std::memset(&frame, 0, sizeof(frame));
    frame.residual_l_re[8] = amplitude;
    frame.residual_r_re[8] = amplitude;
}

float Mag(float re, float im) {
    return std::hypot(re, im);
}

bool TestFrontStayAndRearMove() {
    iem::HaloSurroundAssigner assigner;
    if (!Check(assigner.Prepare(), "prepare assigner")) return false;

    iem::HaloParams front{};
    front.divergence_thousandths = 0;
    front.fade_thousandths = 0;
    front.fade_rears_thousandths = 0;
    front.back_boost = false;
    assigner.ApplyParams(front);
    assigner.Reset();

    iem::HaloDialogFrame in{};
    FillInPhaseResidual(in, 1.0F);
    in.residual_r_re[8] = -1.0F;
    float bed_re[7][iem::HaloStft::kBins]{};
    float bed_im[7][iem::HaloStft::kBins]{};
    for (int frame = 0; frame < 1024; ++frame) {
        assigner.ProcessFrame(in, bed_re, bed_im);
    }

    const float front_l = Mag(bed_re[static_cast<int>(iem::HaloBedChannel::L)][8],
        bed_im[static_cast<int>(iem::HaloBedChannel::L)][8]);
    const float front_r = Mag(bed_re[static_cast<int>(iem::HaloBedChannel::R)][8],
        bed_im[static_cast<int>(iem::HaloBedChannel::R)][8]);
    const float rear_l = Mag(bed_re[static_cast<int>(iem::HaloBedChannel::Lsr)][8],
        bed_im[static_cast<int>(iem::HaloBedChannel::Lsr)][8]);
    if (!Check(front_l + front_r > 0.4F, "Fade=0 keeps residual in front")) return false;
    if (!Check(rear_l < 1.0e-4F, "Fade=0 has no rear Lsr")) return false;

    iem::HaloParams rear{};
    rear.divergence_thousandths = 0;
    rear.fade_thousandths = 1000;
    rear.fade_rears_thousandths = 0;
    rear.back_boost = false;
    assigner.ApplyParams(rear);
    assigner.Reset();
    FillInPhaseResidual(in, 1.0F);
    in.residual_r_re[8] = -1.0F;
    for (int frame = 0; frame < 1024; ++frame) {
        assigner.ProcessFrame(in, bed_re, bed_im);
    }

    return Check(Mag(bed_re[static_cast<int>(iem::HaloBedChannel::Lsr)][8],
            bed_im[static_cast<int>(iem::HaloBedChannel::Lsr)][8]) > 0.2F,
            "Divergence=0 Fade=1 BackBoost=0 moves residual to Lsr")
        && Check(Mag(bed_re[static_cast<int>(iem::HaloBedChannel::Rsr)][8],
            bed_im[static_cast<int>(iem::HaloBedChannel::Rsr)][8]) > 0.2F,
            "Divergence=0 Fade=1 BackBoost=0 moves residual to Rsr");
}

bool TestBackBoostChangesOnlyRear() {
    iem::HaloSurroundAssigner boosted;
    iem::HaloSurroundAssigner unboosted;
    if (!Check(boosted.Prepare() && unboosted.Prepare(), "prepare pair")) return false;

    iem::HaloParams params{};
    params.divergence_thousandths = 0;
    params.fade_thousandths = 1000;
    params.fade_rears_thousandths = 500;
    params.back_boost = true;
    boosted.ApplyParams(params);
    params.back_boost = false;
    unboosted.ApplyParams(params);

    iem::HaloDialogFrame in{};
    FillInPhaseResidual(in, 1.0F);
    in.residual_r_re[8] = -1.0F;
    float boost_re[7][iem::HaloStft::kBins]{};
    float boost_im[7][iem::HaloStft::kBins]{};
    float plain_re[7][iem::HaloStft::kBins]{};
    float plain_im[7][iem::HaloStft::kBins]{};
    for (int frame = 0; frame < 1024; ++frame) {
        boosted.ProcessFrame(in, boost_re, boost_im);
        unboosted.ProcessFrame(in, plain_re, plain_im);
    }

    const float boost_rear = Mag(boost_re[static_cast<int>(iem::HaloBedChannel::Lsr)][8],
        boost_im[static_cast<int>(iem::HaloBedChannel::Lsr)][8]);
    const float plain_rear = Mag(plain_re[static_cast<int>(iem::HaloBedChannel::Lsr)][8],
        plain_im[static_cast<int>(iem::HaloBedChannel::Lsr)][8]);
    const float boost_front = Mag(boost_re[static_cast<int>(iem::HaloBedChannel::L)][8],
        boost_im[static_cast<int>(iem::HaloBedChannel::L)][8]);
    const float plain_front = Mag(plain_re[static_cast<int>(iem::HaloBedChannel::L)][8],
        plain_im[static_cast<int>(iem::HaloBedChannel::L)][8]);
    return Check(boost_rear > plain_rear, "BackBoost increases rear gain")
        && Check(std::fabs(boost_front - plain_front) < 1.0e-5F,
            "BackBoost does not change front gain");
}

} // namespace

int main() {
    if (!TestFrontStayAndRearMove()) return 1;
    if (!TestBackBoostChangesOnlyRear()) return 1;
    std::puts("IEM Halo surround assigner tests passed");
    return 0;
}
