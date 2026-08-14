#include "iem/HaloLfeSynth.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

bool Check(bool condition, const char *message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}

bool Near(float left, float right, float tolerance) {
    return std::fabs(left - right) <= tolerance;
}

iem::HaloLfeParams Params(
    bool enabled,
    int frequency,
    int split,
    int gain
) {
    iem::IemParams params{};
    iem::UpdateIemParameterSnapshot(
        params, iem::kParamHaloLfeEnable, enabled ? 1 : 0, 0, 0);
    iem::UpdateIemParameterSnapshot(
        params, iem::kParamHaloLfeFrequency, frequency, 0, 0);
    iem::UpdateIemParameterSnapshot(
        params, iem::kParamHaloLfeSplit, split, 0, 0);
    iem::UpdateIemParameterSnapshot(
        params, iem::kParamHaloLfeGain, gain, 0, 0);
    return params.halo.lfe;
}

bool TestMappingsAndCoefficients() {
    if (!Check(Near(iem::HaloLfeCutoffHz(0), 10.0F, 1.0e-5F),
            "frequency minimum")) return false;
    if (!Check(Near(iem::HaloLfeCutoffHz(1000000), 200.0F, 1.0e-3F),
            "frequency maximum")) return false;
    if (!Check(Near(iem::HaloLfeGainDb(0), -45.0F, 1.0e-6F),
            "gain minimum")) return false;
    if (!Check(Near(iem::HaloLfeGainDb(1000000), 10.0F, 1.0e-6F),
            "gain maximum")) return false;
    if (!Check(Near(iem::HaloLfeGainDb(272727), -30.000015F, 1.0e-4F),
            "gain default")) return false;

    const auto at48 = iem::MakeHaloLfeLowPass(48000, 750000);
    if (!Check(Near(at48.b0, 0.0000380900201F, 1.0e-10F)
            && Near(at48.b1, 0.0000761800402F, 1.0e-10F)
            && Near(at48.fb1, 1.98754442F, 1.0e-7F)
            && Near(at48.fb2, -0.987696767F, 1.0e-7F),
            "48 kHz binary fixture")) return false;

    const auto at96 = iem::MakeHaloLfeLowPass(96000, 750000);
    if (!Check(Near(at96.b0, 0.00000953702965F, 1.0e-11F)
            && Near(at96.b1, 0.0000190740593F, 1.0e-11F)
            && Near(at96.fb1, 1.99379110F, 1.0e-7F)
            && Near(at96.fb2, -0.993829250F, 1.0e-7F),
            "96 kHz project fixture")) return false;

    iem::IemParams snapshot{};
    if (!Check(iem::UpdateIemParameterSnapshot(
            snapshot, iem::kParamHaloLfeFrequency, 1000000, 0, 0)
            == iem::ParamUpdate::UPDATED,
            "frequency snapshot update")) return false;
    const auto expected = iem::MakeHaloLfeLowPass(96000, 1000000);
    if (!Check(Near(snapshot.halo.lfe.coefficients_96k.b0, expected.b0, 1.0e-10F),
            "snapshot stores derived coefficients")) return false;
    if (!Check(iem::UpdateIemParameterSnapshot(
            snapshot, iem::kParamHaloLfeGain, 1000000, 0, 0)
            == iem::ParamUpdate::UPDATED,
            "gain snapshot update")) return false;
    return Check(Near(snapshot.halo.lfe.gain_linear,
        iem::HaloLfeGainLinear(1000000), 1.0e-7F),
        "snapshot stores derived gain");
}

bool TestDisabledAndSplitRouting() {
    constexpr std::size_t kFrames = 1024;
    std::vector<float> left(kFrames, 0.0F);
    std::vector<float> right(kFrames, 0.0F);
    std::vector<float> centre(kFrames, 0.0F);
    std::vector<float> lfe(kFrames, 1.0F);
    left[0] = 1.0F;
    right[1] = -0.5F;
    centre[2] = 0.25F;
    const auto original_left = left;
    const auto original_right = right;
    const auto original_centre = centre;

    iem::HaloLfeSynth disabled;
    if (!Check(disabled.Prepare(96000), "prepare disabled synth")) return false;
    disabled.ApplyParams(Params(false, 750000, 1000000, 1000000));
    disabled.Process(left.data(), right.data(), centre.data(), lfe.data(), kFrames);
    if (!Check(left == original_left && right == original_right
            && centre == original_centre,
            "disabled leaves directional sources unchanged")) return false;
    if (!Check(std::all_of(lfe.begin(), lfe.end(), [](float value) { return value == 0.0F; }),
            "disabled clears LFE")) return false;

    left = original_left;
    right = original_right;
    centre = original_centre;
    std::fill(lfe.begin(), lfe.end(), 0.0F);
    iem::HaloLfeSynth split_zero;
    if (!Check(split_zero.Prepare(96000), "prepare split-zero synth")) return false;
    split_zero.ApplyParams(Params(true, 750000, 0, 1000000));
    split_zero.Process(left.data(), right.data(), centre.data(), lfe.data(), kFrames);
    if (!Check(left == original_left && right == original_right
            && centre == original_centre,
            "split zero preserves source arrays")) return false;
    if (!Check(std::any_of(lfe.begin(), lfe.end(), [](float value) {
            return std::fabs(value) > 1.0e-8F;
        }), "enabled synth produces LFE")) return false;

    left = original_left;
    right = original_right;
    centre = original_centre;
    std::fill(lfe.begin(), lfe.end(), 0.0F);
    iem::HaloLfeSynth split_full;
    if (!Check(split_full.Prepare(96000), "prepare full-split synth")) return false;
    split_full.ApplyParams(Params(true, 750000, 1000000, 1000000));
    split_full.Process(left.data(), right.data(), centre.data(), lfe.data(), kFrames);
    return Check(left != original_left && right != original_right
            && centre != original_centre,
            "split one subtracts each filtered source");
}

bool TestChunkingAndReset() {
    constexpr std::size_t kFrames = 1024;
    std::vector<float> source_left(kFrames);
    std::vector<float> source_right(kFrames);
    std::vector<float> source_centre(kFrames);
    for (std::size_t frame = 0; frame < kFrames; ++frame) {
        source_left[frame] = std::sin(static_cast<float>(frame) * 0.017F) * 0.2F;
        source_right[frame] = std::cos(static_cast<float>(frame) * 0.013F) * 0.15F;
        source_centre[frame] = std::sin(static_cast<float>(frame) * 0.007F) * 0.1F;
    }
    const auto params = Params(true, 630000, 400000, 700000);

    auto one_left = source_left;
    auto one_right = source_right;
    auto one_centre = source_centre;
    std::vector<float> one_lfe(kFrames);
    iem::HaloLfeSynth one_block;
    if (!Check(one_block.Prepare(96000), "prepare one-block synth")) return false;
    one_block.ApplyParams(params);
    one_block.Process(one_left.data(), one_right.data(), one_centre.data(),
        one_lfe.data(), kFrames);

    auto chunk_left = source_left;
    auto chunk_right = source_right;
    auto chunk_centre = source_centre;
    std::vector<float> chunk_lfe(kFrames);
    iem::HaloLfeSynth chunks;
    if (!Check(chunks.Prepare(96000), "prepare chunked synth")) return false;
    chunks.ApplyParams(params);
    for (std::size_t offset = 0; offset < kFrames; offset += 128) {
        chunks.Process(chunk_left.data() + offset, chunk_right.data() + offset,
            chunk_centre.data() + offset, chunk_lfe.data() + offset, 128);
    }
    for (std::size_t frame = 0; frame < kFrames; ++frame) {
        if (!Near(one_left[frame], chunk_left[frame], 1.0e-7F)
            || !Near(one_right[frame], chunk_right[frame], 1.0e-7F)
            || !Near(one_centre[frame], chunk_centre[frame], 1.0e-7F)
            || !Near(one_lfe[frame], chunk_lfe[frame], 1.0e-7F)) {
            return Check(false, "processing is chunk invariant");
        }
    }

    chunks.Reset();
    chunks.ApplyParams(params);
    auto reset_left = source_left;
    auto reset_right = source_right;
    auto reset_centre = source_centre;
    std::vector<float> reset_lfe(kFrames);
    chunks.Process(reset_left.data(), reset_right.data(), reset_centre.data(),
        reset_lfe.data(), kFrames);
    for (std::size_t frame = 0; frame < kFrames; ++frame) {
        if (!Near(one_lfe[frame], reset_lfe[frame], 1.0e-7F)) {
            return Check(false, "reset restores initial response");
        }
    }
    return true;
}

} // namespace

int main() {
    if (!TestMappingsAndCoefficients()) return 1;
    if (!TestDisabledAndSplitRouting()) return 1;
    if (!TestChunkingAndReset()) return 1;
    std::puts("IEM Halo LFE synth tests passed");
    return 0;
}
