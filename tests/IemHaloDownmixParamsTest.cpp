#include "iem/HaloDownmixParams.h"
#include "iem/IemParams.h"

#include <cmath>
#include <cstdio>

namespace {

bool Check(bool condition, const char *message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}

bool Near(float left, float right, float tolerance) {
    return std::fabs(left - right) <= tolerance;
}

bool TestMappings() {
    return Check(Near(iem::HaloDownmixFrequencyHz(0), 20.0F, 1.0e-5F),
            "frequency minimum")
        && Check(Near(iem::HaloDownmixFrequencyHz(1000000), 22000.0F, 0.02F),
            "frequency maximum")
        && Check(Near(iem::HaloDownmixFrequencyHz(229819), 100.0F, 0.01F),
            "frequency default")
        && Check(Near(iem::HaloDownmixGainDb(0), -70.0F, 1.0e-6F),
            "gain minimum")
        && Check(Near(iem::HaloDownmixGainDb(1000000), 20.0F, 1.0e-6F),
            "gain maximum")
        && Check(Near(iem::HaloDownmixGainDb(744444), -3.00004F, 1.0e-4F),
            "center gain default");
}

bool TestDefaultsAndDerivedValues() {
    const iem::HaloDownmixParams params{};
    if (!Check(params.delay_enable, "delay defaults enabled")) return false;
    if (!Check(params.ls_delay_us == 0 && params.rsr_delay_us == 0,
            "relative delays default to zero")) return false;
    if (!Check(params.center_trim_millionths == 744444
            && params.rear_mid_trim_millionths == 711111,
            "trim defaults")) return false;
    if (!Check(!params.lfe_lpf_enable && !params.output_hpf_enable,
            "filters default disabled")) return false;
    if (!Check(Near(params.derived.center_gain, 0.7079425F, 1.0e-5F)
            && Near(params.derived.rear_mid_gain, 0.5011815F, 1.0e-5F),
            "derived trim gains")) return false;
    if (!Check(Near(params.derived.balance.left_from_left, 1.0F, 1.0e-6F)
            && Near(params.derived.balance.right_from_right, 1.0F, 1.0e-6F)
            && Near(params.derived.balance.left_from_right, 0.0F, 1.0e-6F),
            "default balance is transparent")) return false;

    const auto low_pass = iem::MakeHaloDownmixLowPass(96000U, 328797);
    return Check(Near(low_pass.b0, 0.0000425576816F, 2.0e-9F)
            && Near(low_pass.b1, 0.0000851153632F, 4.0e-9F)
            && Near(low_pass.a1, -1.98682529F, 2.0e-6F)
            && Near(low_pass.a2, 0.986995516F, 2.0e-6F),
            "default LFE low-pass fixture");
}

bool TestRefresh() {
    iem::HaloDownmixParams params{};
    params.pan_left_millionths = 0;
    params.pan_right_millionths = 0;
    params.output_left_trim_millionths = 1000000;
    iem::RefreshHaloDownmixDerived(params);
    return Check(params.derived.balance.left_from_left > 0.70F
            && params.derived.balance.left_from_right < -0.70F,
            "balance refresh")
        && Check(Near(params.derived.output_left_gain, 10.0F, 1.0e-5F),
            "gain refresh");
}

} // namespace

int main() {
    if (!TestMappings()) return 1;
    if (!TestDefaultsAndDerivedValues()) return 1;
    if (!TestRefresh()) return 1;
    std::puts("IEM Halo Downmix parameter tests passed");
    return 0;
}
