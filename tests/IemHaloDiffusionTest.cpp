#include "iem/HaloDiffusion.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

bool Check(bool condition, const char *message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}

bool TestMappings() {
    return Check(iem::HaloDiffusionGain(0.0F) == 0.0F, "zero diffusion")
        && Check(iem::HaloDiffusionGain(1.0F) == 1.0F, "full diffusion")
        && Check(std::fabs(iem::HaloDiffusionGain(0.5F) - iem::DbToLin(-15.0F)) < 1.0e-6F,
            "mid diffusion maps to -15 dB")
        && Check(iem::HaloSpaceDelayA(0.8F) == 2000U, "space delay A")
        && Check(iem::HaloSpaceDelayB(0.8F) == 500U, "space delay B")
        && Check(std::fabs(iem::HaloRearShelfFrequency(0.0F) - 1000.0F) < 1.0e-3F,
            "rear shelf minimum")
        && Check(std::fabs(iem::HaloRearShelfFrequency(1.0F) - 22000.0F) < 1.0e-2F,
            "rear shelf maximum");
}

bool TestDisabledShelfMatchesZeroDbShelf() {
    constexpr std::size_t kFrames = 4096;
    std::vector<float> disabled_storage[7];
    std::vector<float> zero_db_storage[7];
    float *disabled[7]{};
    float *zero_db[7]{};
    for (int channel = 0; channel < 7; ++channel) {
        disabled_storage[channel].assign(kFrames, 0.0F);
        zero_db_storage[channel].assign(kFrames, 0.0F);
        disabled[channel] = disabled_storage[channel].data();
        zero_db[channel] = zero_db_storage[channel].data();
    }
    disabled[5][0] = zero_db[5][0] = 1.0F;
    disabled[6][0] = zero_db[6][0] = -1.0F;

    iem::HaloParams disabled_params{};
    disabled_params.diffusion_thousandths = 0;
    disabled_params.space_thousandths = 0;
    disabled_params.rear_shelf_enable = false;

    iem::HaloParams zero_db_params = disabled_params;
    zero_db_params.rear_shelf_enable = true;
    zero_db_params.rear_shelf_gain_thousandths = 500;

    iem::HaloDiffusion disabled_effect;
    iem::HaloDiffusion zero_db_effect;
    if (!Check(disabled_effect.Prepare(96000) && zero_db_effect.Prepare(96000),
            "prepare diffusion pair")) return false;
    disabled_effect.ApplyParams(disabled_params);
    zero_db_effect.ApplyParams(zero_db_params);
    disabled_effect.Process(disabled, kFrames);
    zero_db_effect.Process(zero_db, kFrames);

    for (int channel = 0; channel < 7; ++channel) {
        for (std::size_t frame = 0; frame < kFrames; ++frame) {
            if (std::fabs(disabled[channel][frame] - zero_db[channel][frame]) > 1.0e-6F) {
                return Check(false, "disabled shelf matches zero dB shelf");
            }
        }
    }
    return true;
}

bool TestSpaceDelaysRearEnergy() {
    constexpr std::size_t kFrames = 2200;
    std::vector<float> storage[7];
    float *bed[7]{};
    for (int channel = 0; channel < 7; ++channel) {
        storage[channel].assign(kFrames, 0.0F);
        bed[channel] = storage[channel].data();
    }
    bed[5][0] = 1.0F;

    iem::HaloParams params{};
    params.diffusion_thousandths = 1000;
    params.space_thousandths = 800;
    params.rear_shelf_enable = false;

    iem::HaloDiffusion effect;
    if (!Check(effect.Prepare(96000), "prepare diffusion")) return false;
    effect.ApplyParams(params);
    effect.Process(bed, kFrames);
    return Check(std::fabs(bed[5][2000]) > 1.0e-5F,
        "space delay A produces delayed rear energy");
}

} // namespace

int main() {
    if (!TestMappings()) return 1;
    if (!TestDisabledShelfMatchesZeroDbShelf()) return 1;
    if (!TestSpaceDelaysRearEnergy()) return 1;
    std::puts("IEM Halo diffusion tests passed");
    return 0;
}
