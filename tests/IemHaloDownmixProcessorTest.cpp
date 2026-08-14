#include "iem/HaloDownmixProcessor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr std::size_t kFrames = 4096;

bool Check(bool condition, const char *message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}

bool Near(float left, float right, float tolerance = 1.0e-5F) {
    return std::fabs(left - right) <= tolerance;
}

iem::HaloDownmixParams UnityParams() {
    iem::HaloDownmixParams params{};
    params.front_mid_trim_millionths = 777778;
    params.front_side_trim_millionths = 777778;
    params.center_trim_millionths = 777778;
    params.surround_mid_trim_millionths = 777778;
    params.surround_side_trim_millionths = 777778;
    params.rear_mid_trim_millionths = 777778;
    params.rear_side_trim_millionths = 777778;
    params.lfe_trim_millionths = 777778;
    params.output_left_trim_millionths = 777778;
    params.output_right_trim_millionths = 777778;
    iem::RefreshHaloDownmixDerived(params);
    return params;
}

struct RenderResult {
    RenderResult() : left(kFrames), right(kFrames) {}

    std::vector<float> left;
    std::vector<float> right;
};

bool RenderRole(
    iem::HaloDownmixRole role,
    const iem::HaloDownmixParams &params,
    RenderResult &result
) {
    std::array<std::vector<float>, iem::HaloDownmixProcessor::kRoleCount> storage{};
    std::array<const float *, iem::HaloDownmixProcessor::kRoleCount> inputs{};
    for (uint32_t index = 0; index < iem::HaloDownmixProcessor::kRoleCount; ++index) {
        storage[index].assign(kFrames, 0.0F);
        inputs[index] = storage[index].data();
    }
    storage[static_cast<uint32_t>(role)][0] = 1.0F;
    float *outputs[2]{result.left.data(), result.right.data()};
    iem::HaloDownmixProcessor processor;
    if (!processor.Prepare(kFrames)) return false;
    processor.ApplyParams(params);
    return processor.Process(inputs.data(), outputs, kFrames);
}

bool TestRoleRouting() {
    const auto params = UnityParams();
    struct Expected {
        iem::HaloDownmixRole role;
        float left;
        float right;
    };
    constexpr Expected expected[] = {
        {iem::HaloDownmixRole::L, 1.0F, 0.0F},
        {iem::HaloDownmixRole::R, 0.0F, 1.0F},
        {iem::HaloDownmixRole::C, 1.0F, 1.0F},
        {iem::HaloDownmixRole::LFE, 1.0F, 1.0F},
        {iem::HaloDownmixRole::Ls, 1.0F, 0.0F},
        {iem::HaloDownmixRole::Rs, 0.0F, 1.0F},
        {iem::HaloDownmixRole::Lsr, 1.0F, 0.0F},
        {iem::HaloDownmixRole::Rsr, 0.0F, 1.0F},
    };
    for (const auto &item : expected) {
        RenderResult result;
        if (!Check(RenderRole(item.role, params, result), "render role impulse")) {
            return false;
        }
        if (!Near(result.left[3072], item.left, 2.0e-5F)
            || !Near(result.right[3072], item.right, 2.0e-5F)) {
            std::fprintf(stderr, "role %u: left=%g right=%g expected=%g/%g\n",
                static_cast<unsigned>(item.role), result.left[3072],
                result.right[3072], item.left, item.right);
            return Check(false, "role routing at base latency");
        }
    }
    return true;
}

bool TestScalingAndRelativeDelay() {
    auto params = UnityParams();
    params.scale_input_by_output_count = true;
    RenderResult center;
    if (!Check(RenderRole(iem::HaloDownmixRole::C, params, center),
            "render scaled center")) return false;
    if (!Check(Near(center.left[3072], 0.5F, 2.0e-5F)
            && Near(center.right[3072], 0.5F, 2.0e-5F),
            "scale shared center by output count")) return false;

    params.scale_input_by_output_count = false;
    params.ls_delay_us = 10000;
    RenderResult side;
    if (!Check(RenderRole(iem::HaloDownmixRole::Ls, params, side),
            "render advanced side")) return false;
    return Check(std::fabs(side.left[2112]) > 0.99F
            && std::fabs(side.left[3072]) < 1.0e-6F,
            "relative side delay advances against common latency");
}

bool TestImageAndFilters() {
    auto params = UnityParams();
    params.center_divergence_millionths = 1000000;
    RenderResult divergent;
    if (!Check(RenderRole(iem::HaloDownmixRole::C, params, divergent),
            "render divergent center")) return false;
    if (!Check(Near(divergent.left[3072], 0.5F, 2.0e-5F)
            && Near(divergent.right[3072], 0.5F, 2.0e-5F),
            "full divergence moves center into front pair")) return false;

    params = UnityParams();
    params.lfe_lpf_enable = true;
    RenderResult filtered_lfe;
    if (!Check(RenderRole(iem::HaloDownmixRole::LFE, params, filtered_lfe),
            "render filtered LFE")) return false;
    if (!Check(std::fabs(filtered_lfe.left[3072]) < 0.001F
            && std::fabs(filtered_lfe.left[3073]) > 0.0F,
            "LFE low-pass produces recursive response")) return false;

    params = UnityParams();
    params.output_hpf_enable = true;
    RenderResult high_passed;
    if (!Check(RenderRole(iem::HaloDownmixRole::L, params, high_passed),
            "render high-passed output")) return false;
    return Check(std::fabs(high_passed.left[3073]) > 0.0F,
        "output high-pass produces recursive response");
}

bool TestChunkingAndReset() {
    auto params = UnityParams();
    constexpr std::size_t kTotal = 8192;
    std::array<std::vector<float>, iem::HaloDownmixProcessor::kRoleCount> one_storage{};
    std::array<std::vector<float>, iem::HaloDownmixProcessor::kRoleCount> chunk_storage{};
    std::array<const float *, iem::HaloDownmixProcessor::kRoleCount> one_inputs{};
    std::array<const float *, iem::HaloDownmixProcessor::kRoleCount> chunk_inputs{};
    for (uint32_t role = 0; role < iem::HaloDownmixProcessor::kRoleCount; ++role) {
        one_storage[role].resize(kTotal);
        for (std::size_t frame = 0; frame < kTotal; ++frame) {
            one_storage[role][frame] = std::sin(
                static_cast<float>(frame + role * 17U) * 0.009F) * 0.05F;
        }
        chunk_storage[role] = one_storage[role];
        one_inputs[role] = one_storage[role].data();
        chunk_inputs[role] = chunk_storage[role].data();
    }
    std::vector<float> one_left(kTotal), one_right(kTotal);
    std::vector<float> chunk_left(kTotal), chunk_right(kTotal);
    float *one_outputs[2]{one_left.data(), one_right.data()};
    iem::HaloDownmixProcessor one;
    if (!Check(one.Prepare(kTotal), "prepare one-block processor")) return false;
    one.ApplyParams(params);
    if (!Check(one.Process(one_inputs.data(), one_outputs, kTotal),
            "process one block")) return false;

    iem::HaloDownmixProcessor chunked;
    if (!Check(chunked.Prepare(256), "prepare chunked processor")) return false;
    chunked.ApplyParams(params);
    for (std::size_t offset = 0; offset < kTotal; offset += 256) {
        std::array<const float *, iem::HaloDownmixProcessor::kRoleCount> block_inputs{};
        for (uint32_t role = 0; role < iem::HaloDownmixProcessor::kRoleCount; ++role) {
            block_inputs[role] = chunk_inputs[role] + offset;
        }
        float *block_outputs[2]{chunk_left.data() + offset, chunk_right.data() + offset};
        if (!Check(chunked.Process(block_inputs.data(), block_outputs, 256),
                "process chunk")) return false;
    }
    for (std::size_t frame = 0; frame < kTotal; ++frame) {
        if (!Near(one_left[frame], chunk_left[frame], 1.0e-6F)
            || !Near(one_right[frame], chunk_right[frame], 1.0e-6F)) {
            return Check(false, "processing is chunk invariant");
        }
    }

    chunked.Reset();
    std::vector<float> reset_left(kTotal), reset_right(kTotal);
    for (std::size_t offset = 0; offset < kTotal; offset += 256) {
        std::array<const float *, iem::HaloDownmixProcessor::kRoleCount> block_inputs{};
        for (uint32_t role = 0; role < iem::HaloDownmixProcessor::kRoleCount; ++role) {
            block_inputs[role] = chunk_inputs[role] + offset;
        }
        float *block_outputs[2]{reset_left.data() + offset, reset_right.data() + offset};
        if (!Check(chunked.Process(block_inputs.data(), block_outputs, 256),
                "process reset chunk")) return false;
    }
    return Check(std::equal(reset_left.begin(), reset_left.end(), chunk_left.begin())
            && std::equal(reset_right.begin(), reset_right.end(), chunk_right.begin()),
            "reset restores deterministic response");
}

bool TestDynamicRamps() {
    constexpr std::size_t kWarmup = 4096;
    constexpr std::size_t kTransition = 10000;
    std::array<std::vector<float>, iem::HaloDownmixProcessor::kRoleCount> warmup{};
    std::array<std::vector<float>, iem::HaloDownmixProcessor::kRoleCount> transition{};
    std::array<const float *, iem::HaloDownmixProcessor::kRoleCount> warmup_inputs{};
    std::array<const float *, iem::HaloDownmixProcessor::kRoleCount> transition_inputs{};
    for (uint32_t role = 0; role < iem::HaloDownmixProcessor::kRoleCount; ++role) {
        warmup[role].assign(kWarmup, 0.0F);
        transition[role].assign(kTransition, 0.0F);
        warmup_inputs[role] = warmup[role].data();
        transition_inputs[role] = transition[role].data();
    }
    const uint32_t side = static_cast<uint32_t>(iem::HaloDownmixRole::Ls);
    for (std::size_t frame = 0; frame < kWarmup; ++frame) {
        warmup[side][frame] = static_cast<float>(frame) * 0.0001F;
    }
    for (std::size_t frame = 0; frame < kTransition; ++frame) {
        transition[side][frame] = static_cast<float>(kWarmup + frame) * 0.0001F;
    }

    iem::HaloDownmixProcessor processor;
    if (!Check(processor.Prepare(kTransition), "prepare dynamic processor")) return false;
    auto params = UnityParams();
    processor.ApplyParams(params);
    std::vector<float> warmup_left(kWarmup), warmup_right(kWarmup);
    float *warmup_outputs[2]{warmup_left.data(), warmup_right.data()};
    if (!Check(processor.Process(warmup_inputs.data(), warmup_outputs, kWarmup),
            "warm up delay ring")) return false;

    params.ls_delay_us = 10000;
    processor.ApplyParams(params);
    std::vector<float> transition_left(kTransition), transition_right(kTransition);
    float *transition_outputs[2]{transition_left.data(), transition_right.data()};
    if (!Check(processor.Process(
            transition_inputs.data(), transition_outputs, kTransition),
            "process delay crossfade")) return false;
    if (!Check(Near(transition_left[0], 0.1024F, 2.0e-5F),
            "delay crossfade starts on old tap")) return false;
    if (!Check(Near(transition_left[9999], 1.19829F, 3.0e-4F),
            "delay crossfade reaches new tap after 10000 samples")) return false;

    processor.Reset();
    params = UnityParams();
    processor.ApplyParams(params);
    std::array<std::vector<float>, iem::HaloDownmixProcessor::kRoleCount> constant{};
    std::array<const float *, iem::HaloDownmixProcessor::kRoleCount> constant_inputs{};
    for (uint32_t role = 0; role < iem::HaloDownmixProcessor::kRoleCount; ++role) {
        constant[role].assign(kWarmup, 0.0F);
        constant_inputs[role] = constant[role].data();
    }
    std::fill(constant[static_cast<uint32_t>(iem::HaloDownmixRole::L)].begin(),
        constant[static_cast<uint32_t>(iem::HaloDownmixRole::L)].end(), 1.0F);
    if (!Check(processor.Process(constant_inputs.data(), warmup_outputs, kWarmup),
            "warm up gain ramp input")) return false;
    params.output_left_trim_millionths = 1000000;
    iem::RefreshHaloDownmixDerived(params);
    processor.ApplyParams(params);
    std::array<std::vector<float>, iem::HaloDownmixProcessor::kRoleCount> gain_input{};
    std::array<const float *, iem::HaloDownmixProcessor::kRoleCount> gain_inputs{};
    for (uint32_t role = 0; role < iem::HaloDownmixProcessor::kRoleCount; ++role) {
        gain_input[role].assign(1024, 0.0F);
        gain_inputs[role] = gain_input[role].data();
    }
    std::fill(gain_input[static_cast<uint32_t>(iem::HaloDownmixRole::L)].begin(),
        gain_input[static_cast<uint32_t>(iem::HaloDownmixRole::L)].end(), 1.0F);
    std::vector<float> gain_left(1024), gain_right(1024);
    float *gain_outputs[2]{gain_left.data(), gain_right.data()};
    if (!Check(processor.Process(gain_inputs.data(), gain_outputs, 1024),
            "process output gain ramp")) return false;
    return Check(gain_left[0] > 1.0F && gain_left[0] < 1.02F,
            "gain ramp starts smoothly")
        && Check(Near(gain_left[1023], 10.0F, 2.0e-4F),
            "gain ramp reaches target after 1024 samples");
}

} // namespace

int main() {
    if (!TestRoleRouting()) return 1;
    if (!TestScalingAndRelativeDelay()) return 1;
    if (!TestImageAndFilters()) return 1;
    if (!TestChunkingAndReset()) return 1;
    if (!TestDynamicRamps()) return 1;
    std::puts("IEM Halo Downmix processor tests passed");
    return 0;
}
