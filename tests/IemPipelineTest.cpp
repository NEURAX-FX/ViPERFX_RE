#include "iem/IemPipeline.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <vector>

namespace {

constexpr std::size_t kBlock = 256;
std::atomic<uint64_t> g_new_calls{0};
std::atomic<bool> g_count_new{false};

bool Check(bool condition, const char *message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}

bool Render(iem::IemPipeline &pipeline, const std::vector<float> &left,
    const std::vector<float> &right, std::vector<float> &output_left,
    std::vector<float> &output_right) {
    std::size_t position = 0;
    while (position < left.size()) {
        const std::size_t frames = std::min(kBlock, left.size() - position);
        const float *inputs[2]{left.data() + position, right.data() + position};
        float *outputs[2]{output_left.data() + position, output_right.data() + position};
        if (!pipeline.Process(inputs, outputs, frames)) return false;
        position += frames;
    }
    return true;
}

bool TestProfileBoundsAndModes() {
    uint32_t previous_latency = 0;
    for (uint32_t profile = 0; profile < 3; ++profile) {
        for (uint32_t order = 1; order <= 3; ++order) {
            for (uint32_t mode = 0; mode < 3; ++mode) {
                iem::IemParams params{};
                params.order = order;
                params.encoder_mode = static_cast<iem::EncoderMode>(mode);
                params.latency_profile = static_cast<iem::LatencyProfile>(profile);
                params.decoder.headphone_eq = iem::HeadphoneEqId::OFF;
                iem::IemPipeline pipeline;
                if (!Check(pipeline.Prepare(params, kBlock),
                        "prepare pipeline mode/order/profile")) return false;
                if (!Check(pipeline.LatencyFrames()
                        <= iem::kLatencyProfiles[profile].maximum_latency_ms * 96U,
                        "pipeline profile latency cap")) return false;
                if (order == 1 && mode == 0) {
                    if (!Check(pipeline.LatencyFrames() >= previous_latency,
                            "profile latency is monotonic")) return false;
                    previous_latency = pipeline.LatencyFrames();
                }
            }
        }
    }
    for (uint32_t profile = 0; profile < 3; ++profile) {
        iem::IemParams params{};
        params.encoder_mode = iem::EncoderMode::HALO;
        params.render_mode = iem::RenderMode::SIMPLE;
        params.latency_profile = static_cast<iem::LatencyProfile>(profile);
        params.limiter.enabled = false;
        iem::IemPipeline pipeline;
        if (!Check(pipeline.Prepare(params, kBlock),
                "prepare Halo Downmix in every latency profile")) return false;
        if (!Check(pipeline.LatencyFrames()
                <= iem::kLatencyProfiles[profile].maximum_latency_ms * 96U,
                "Halo Downmix profile latency cap")) return false;
    }
    return Check(iem::kLatencyProfiles[0].partition_frames
            < iem::kLatencyProfiles[1].partition_frames
            && iem::kLatencyProfiles[1].partition_frames
                < iem::kLatencyProfiles[2].partition_frames,
        "profile partitions are monotonic");
}

bool TestDryAlignmentGainAndLimiter() {
    iem::IemParams params{};
    params.order = 1;
    params.latency_profile = iem::LatencyProfile::LOW;
    params.wet = 0.0F;
    params.limiter.enabled = false;
    iem::IemPipeline pipeline;
    if (!pipeline.Prepare(params, kBlock)) return false;
    const std::size_t total = pipeline.LatencyFrames() + 512U;
    std::vector<float> left(total, 0.0F), right(total, 0.0F);
    std::vector<float> output_left(total), output_right(total);
    left[0] = 1.0F;
    if (!Render(pipeline, left, right, output_left, output_right)) return false;
    if (!Check(std::fabs(output_left[pipeline.LatencyFrames()] - 1.0F) <= 1.0e-6F,
            "wet zero returns latency-aligned dry")) return false;
    if (!Check(output_right[pipeline.LatencyFrames()] == 0.0F,
            "dry alignment preserves channels")) return false;

    params.output_gain_db = 6.0F;
    pipeline.ApplyParams(params);
    pipeline.Reset();
    std::fill(output_left.begin(), output_left.end(), 0.0F);
    left[0] = 1.0F;
    if (!Render(pipeline, left, right, output_left, output_right)) return false;
    if (!Check(std::fabs(output_left[pipeline.LatencyFrames()]
            - std::pow(10.0F, 6.0F / 20.0F)) <= 2.0e-5F,
            "pipeline output gain")) return false;

    params.output_gain_db = 0.0F;
    params.limiter.enabled = true;
    params.limiter.ceiling_centidb = -30;
    pipeline.ApplyParams(params);
    pipeline.Reset();
    left.assign(total, 0.0F);
    right.assign(total, 0.0F);
    output_left.assign(total, 0.0F);
    output_right.assign(total, 0.0F);
    left[0] = 2.0F;
    if (!Render(pipeline, left, right, output_left, output_right)) return false;
    const float ceiling = std::pow(10.0F, -0.3F / 20.0F);
    return Check(std::fabs(output_left[pipeline.LatencyFrames()]) <= ceiling + 1.0e-6F,
        "pipeline limiter ceiling");
}

bool TestWetPathEqFreezeAndReset() {
    iem::IemParams params{};
    params.encoder_mode = iem::EncoderMode::GRANULAR;
    params.order = 3;
    params.wet = 1.0F;
    params.decoder.headphone_eq = iem::HeadphoneEqId::AKG_K1000_CLOSED;
    params.limiter.enabled = false;
    iem::IemPipeline pipeline;
    if (!pipeline.Prepare(params, kBlock)) return false;
    const std::size_t total = pipeline.LatencyFrames() + 2048U;
    std::vector<float> left(total, 0.1F), right(total, -0.05F);
    std::vector<float> output_left(total), output_right(total);
    if (!Render(pipeline, left, right, output_left, output_right)) return false;
    if (!Check(std::any_of(output_left.begin() + pipeline.LatencyFrames(),
            output_left.end(), [](float value) { return std::fabs(value) > 1.0e-7F; }),
            "wet KU100/EQ path produces output")) return false;
    pipeline.SetFreeze(true);
    if (!Check(pipeline.IsFrozen(), "pipeline routes Freeze to granular encoder")) return false;
    pipeline.Reset();
    if (!Check(!pipeline.IsFrozen() && pipeline.ActiveGrainCount() == 0,
            "pipeline reset clears granular runtime")) return false;
    left.assign(total, 0.0F);
    right.assign(total, 0.0F);
    output_left.assign(total, 1.0F);
    output_right.assign(total, 1.0F);
    if (!Render(pipeline, left, right, output_left, output_right)) return false;
    return Check(std::all_of(output_left.begin(), output_left.end(),
            [](float value) { return value == 0.0F; }), "pipeline reset clears histories");
}

bool TestRenderModesAndHaloBranch() {
    constexpr std::size_t kTotal = 8192;
    std::vector<float> left(kTotal, 0.0F), right(kTotal, 0.0F);
    left[0] = 1.0F;
    right[0] = -0.5F;

    iem::IemParams off_params{};
    off_params.encoder_mode = iem::EncoderMode::HALO;
    off_params.render_mode = iem::RenderMode::OFF;
    off_params.order = 1;
    off_params.latency_profile = iem::LatencyProfile::STABLE;
    off_params.wet = 1.0F;
    off_params.limiter.enabled = false;
    iem::IemPipeline off;
    if (!Check(off.Prepare(off_params, kBlock), "prepare Halo Off pipeline")) return false;
    if (!Check(off.WetLatencyFrames() == 1536U, "Halo Off includes encoder latency")) return false;
    std::vector<float> off_left(kTotal), off_right(kTotal);
    if (!Render(off, left, right, off_left, off_right)) return false;

    iem::IemParams ku100_params = off_params;
    ku100_params.render_mode = iem::RenderMode::KU100;
    iem::IemPipeline ku100;
    if (!Check(ku100.Prepare(ku100_params, kBlock), "prepare Halo KU100 pipeline")) return false;
    if (!Check(ku100.WetLatencyFrames() > off.WetLatencyFrames(),
            "KU100 adds decoder latency")) return false;
    std::vector<float> ku100_left(kTotal), ku100_right(kTotal);
    if (!Render(ku100, left, right, ku100_left, ku100_right)) return false;
    if (!Check(off_left != ku100_left || off_right != ku100_right,
            "Halo Off and KU100 produce distinct stereo")) return false;

    iem::IemParams simple_params = off_params;
    simple_params.render_mode = iem::RenderMode::SIMPLE;
    iem::IemPipeline simple_zero;
    if (!Check(simple_zero.Prepare(simple_params, kBlock), "prepare Halo Simple pipeline")) return false;
    if (!Check(simple_zero.WetLatencyFrames() == 1536U + 3072U,
            "Halo Downmix adds fixed decoder latency")) return false;
    std::vector<float> simple_zero_left(kTotal), simple_zero_right(kTotal);
    if (!Render(simple_zero, left, right, simple_zero_left, simple_zero_right)) return false;

    simple_params.rotation.yaw_centidegrees = 9000;
    iem::IemPipeline simple_yaw;
    if (!Check(simple_yaw.Prepare(simple_params, kBlock), "prepare rotated Halo Simple pipeline")) return false;
    std::vector<float> simple_yaw_left(kTotal), simple_yaw_right(kTotal);
    if (!Render(simple_yaw, left, right, simple_yaw_left, simple_yaw_right)) return false;
    if (!Check(simple_zero_left != simple_yaw_left || simple_zero_right != simple_yaw_right,
            "Simple mode applies scene rotation")) return false;

    iem::IemParams stereo_off{};
    stereo_off.render_mode = iem::RenderMode::OFF;
    stereo_off.decoder.headphone_eq = static_cast<iem::HeadphoneEqId>(22);
    stereo_off.wet = 1.0F;
    stereo_off.limiter.enabled = false;
    iem::IemPipeline non_halo_off;
    return Check(non_halo_off.Prepare(stereo_off, kBlock),
        "Stereo Off skips KU100 and headphone-EQ resource preparation")
        && Check(non_halo_off.WetLatencyFrames() == 3072U,
            "Stereo Off uses Halo Downmix decoder latency");
}

struct LfeDeltaRender {
    std::vector<float> left;
    std::vector<float> right;
    uint32_t latency = 0;
};

bool RenderHaloLfeDelta(iem::IemParams params, LfeDeltaRender &result) {
    constexpr std::size_t kTotal = 16384;
    params.encoder_mode = iem::EncoderMode::HALO;
    params.latency_profile = iem::LatencyProfile::STABLE;
    params.wet = 1.0F;
    params.limiter.enabled = false;
    params.halo.dialog_isolate_thousandths = 0;
    params.halo.divergence_thousandths = 1000;
    params.halo.fade_thousandths = 0;
    params.halo.fade_rears_thousandths = 0;
    params.halo.diffusion_thousandths = 0;
    params.halo.space_thousandths = 0;
    params.halo.rear_shelf_enable = false;
    params.halo.lfe.enabled = true;
    params.halo.lfe.split_millionths = 0;
    if (iem::UpdateIemParameterSnapshot(
            params, iem::kParamHaloLfeGain, 1000000, 0, 0)
            != iem::ParamUpdate::UPDATED) return false;

    iem::IemParams disabled_params = params;
    disabled_params.halo.lfe.enabled = false;
    iem::IemPipeline enabled;
    iem::IemPipeline disabled;
    if (!enabled.Prepare(params, kBlock) || !disabled.Prepare(disabled_params, kBlock)) {
        return false;
    }

    std::vector<float> input_left(kTotal, 0.0F);
    std::vector<float> input_right(kTotal, 0.0F);
    input_left[733] = 0.5F;
    input_right[733] = 0.5F;
    std::vector<float> enabled_left(kTotal), enabled_right(kTotal);
    std::vector<float> disabled_left(kTotal), disabled_right(kTotal);
    if (!Render(enabled, input_left, input_right, enabled_left, enabled_right)
        || !Render(disabled, input_left, input_right, disabled_left, disabled_right)) {
        return false;
    }

    result.left.resize(kTotal);
    result.right.resize(kTotal);
    for (std::size_t frame = 0; frame < kTotal; ++frame) {
        result.left[frame] = enabled_left[frame] - disabled_left[frame];
        result.right[frame] = enabled_right[frame] - disabled_right[frame];
    }
    result.latency = enabled.WetLatencyFrames();
    return true;
}

std::size_t PeakFrame(const std::vector<float> &values) {
    std::size_t peak_frame = 0;
    float peak = 0.0F;
    for (std::size_t frame = 0; frame < values.size(); ++frame) {
        if (std::fabs(values[frame]) > peak) {
            peak = std::fabs(values[frame]);
            peak_frame = frame;
        }
    }
    return peak_frame;
}

bool SameSignal(const std::vector<float> &left, const std::vector<float> &right,
    float tolerance = 1.0e-6F) {
    if (left.size() != right.size()) return false;
    for (std::size_t frame = 0; frame < left.size(); ++frame) {
        if (std::fabs(left[frame] - right[frame]) > tolerance) return false;
    }
    return true;
}

bool SameSignalShifted(
    const std::vector<float> &earlier,
    const std::vector<float> &later,
    std::size_t shift,
    float tolerance = 1.0e-5F
) {
    if (earlier.size() != later.size() || shift >= earlier.size()) return false;
    for (std::size_t frame = 0; frame + shift < earlier.size(); ++frame) {
        if (std::fabs(earlier[frame] - later[frame + shift]) > tolerance) return false;
    }
    return true;
}

bool TestHaloLfeRenderRouting() {
    iem::IemParams off_params{};
    off_params.render_mode = iem::RenderMode::OFF;
    off_params.order = 1;
    LfeDeltaRender off;
    if (!Check(RenderHaloLfeDelta(off_params, off), "render Halo Off LFE delta")) {
        return false;
    }
    if (!Check(SameSignal(off.left, off.right), "Off mixes LFE equally to L/R")) {
        return false;
    }

    iem::IemParams simple_params = off_params;
    simple_params.render_mode = iem::RenderMode::SIMPLE;
    LfeDeltaRender simple;
    if (!Check(RenderHaloLfeDelta(simple_params, simple),
            "render Halo Simple LFE delta")) return false;
    if (!Check(simple.latency - off.latency == 3072U,
            "Halo Downmix adds 3072 samples to LFE")) return false;
    if (!Check(SameSignalShifted(off.left, simple.left, 3072U),
            "Halo Downmix delays LFE exactly once")) return false;

    simple_params.rotation.yaw_centidegrees = 9000;
    LfeDeltaRender rotated;
    if (!Check(RenderHaloLfeDelta(simple_params, rotated),
            "render rotated Halo LFE delta")) return false;
    if (!Check(SameSignal(simple.left, rotated.left),
            "scene rotation does not alter LFE")) return false;

    simple_params.rotation.yaw_centidegrees = 0;
    simple_params.order = 3;
    LfeDeltaRender order_three;
    if (!Check(RenderHaloLfeDelta(simple_params, order_three),
            "render order-three Halo LFE delta")) return false;
    if (!Check(SameSignal(simple.left, order_three.left),
            "Ambisonics order does not alter LFE")) return false;

    iem::IemParams ku100_params = off_params;
    ku100_params.render_mode = iem::RenderMode::KU100;
    ku100_params.decoder.headphone_eq = iem::HeadphoneEqId::OFF;
    LfeDeltaRender ku100;
    if (!Check(RenderHaloLfeDelta(ku100_params, ku100),
            "render Halo KU100 LFE delta")) return false;
    const std::size_t off_peak = PeakFrame(off.left);
    const std::size_t ku100_peak = PeakFrame(ku100.left);
    return Check(ku100_peak > off_peak, "KU100 delays LFE after Halo encoding")
        && Check(ku100_peak - off_peak == ku100.latency - off.latency,
            "KU100 LFE delay matches the directional render latency")
        && Check(SameSignal(ku100.left, ku100.right),
            "KU100 mixes aligned LFE equally before headphone EQ");
}

bool TestFaultAndNoAllocation() {
    iem::IemParams params{};
    params.wet = 0.0F;
    params.limiter.enabled = false;
    iem::IemPipeline pipeline;
    if (!pipeline.Prepare(params, kBlock)) return false;
    std::array<float, kBlock> left{};
    std::array<float, kBlock> right{};
    std::array<float, kBlock> output_left{};
    std::array<float, kBlock> output_right{};
    const float *inputs[2]{left.data(), right.data()};
    float *outputs[2]{output_left.data(), output_right.data()};
    const uint64_t before = g_new_calls.load(std::memory_order_relaxed);
    g_count_new.store(true, std::memory_order_release);
    const bool ok = pipeline.Process(inputs, outputs, kBlock);
    g_count_new.store(false, std::memory_order_release);
    const uint64_t after = g_new_calls.load(std::memory_order_relaxed);
    if (!Check(ok && before == after, "pipeline callback allocation audit")) return false;
    left[0] = std::numeric_limits<float>::quiet_NaN();
    return Check(!pipeline.Process(inputs, outputs, 1)
            && pipeline.Error() == iem::IemResourceError::PROCESS_NONFINITE,
        "pipeline rejects non-finite input");
}

} // namespace

void *operator new(std::size_t size) {
    if (g_count_new.load(std::memory_order_acquire)) {
        g_new_calls.fetch_add(1, std::memory_order_relaxed);
    }
    if (void *memory = std::malloc(size)) return memory;
    std::abort();
}
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }

int main() {
    if (!TestProfileBoundsAndModes()) return 1;
    if (!TestDryAlignmentGainAndLimiter()) return 1;
    if (!TestWetPathEqFreezeAndReset()) return 1;
    if (!TestRenderModesAndHaloBranch()) return 1;
    if (!TestHaloLfeRenderRouting()) return 1;
    if (!TestFaultAndNoAllocation()) return 1;
    std::puts("IEM pipeline tests passed");
    return 0;
}
