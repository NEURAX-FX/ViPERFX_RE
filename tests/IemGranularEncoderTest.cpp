#include "iem/GranularEncoder.h"
#include "reference/PinnedGranularReference.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace {

constexpr std::size_t kFrames = 128;
constexpr uint32_t kSampleRate = 96000;

using Input = std::array<float, kFrames>;
using Channel = std::array<float, kFrames>;
using Output = std::array<Channel, iem::kMaxAmbisonicsChannels>;

std::atomic<uint64_t> g_new_calls{0};
std::atomic<bool> g_count_new{false};

bool Check(bool condition, const char *message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}

bool Near(float left, float right, float tolerance = 2.0e-5F) {
    return std::fabs(left - right) <= tolerance;
}

bool Process(
    iem::GranularEncoder &encoder,
    const Input &left,
    const Input &right,
    Output &output,
    std::size_t frames = kFrames
) {
    const float *inputs[2]{left.data(), right.data()};
    float *outputs[iem::kMaxAmbisonicsChannels]{};
    for (std::size_t channel = 0; channel < output.size(); ++channel) {
        outputs[channel] = output[channel].data();
    }
    return encoder.Process(inputs, outputs, frames);
}

void FillInput(Input &left, Input &right, uint64_t start_frame) {
    constexpr float kTwoPi = 6.2831853071795864769F;
    for (std::size_t frame = 0; frame < kFrames; ++frame) {
        const float time = static_cast<float>(start_frame + frame)
            / static_cast<float>(kSampleRate);
        left[frame] = std::sin(kTwoPi * 440.0F * time);
        right[frame] = std::sin(kTwoPi * 660.0F * time);
    }
}

bool TestGrainReference() {
    std::array<float, 8> left{0.0F, 1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F};
    std::array<float, 8> right{};
    iem::Grain grain;
    iem::GrainStart start{};
    start.start_position = 1;
    start.length_samples = 5;
    start.pitch_factor = 0.5F;
    start.attack_fraction = 0.5F;
    start.decay_fraction = 0.5F;
    start.gain = 0.75F;
    grain.Start(start);
    for (uint32_t index = 0; index < start.length_samples; ++index) {
        const float progress = static_cast<float>(index)
            / static_cast<float>(start.length_samples - 1U);
        const float expected = iem_test::PinnedInterpolatedSample(
            left.data(), left.size(), 1.0F + static_cast<float>(index) * 0.5F
        ) * iem_test::PinnedWindowValue(progress, 0.5F, 0.5F) * 0.75F;
        if (!Check(Near(grain.RenderSample(left.data(), right.data(), left.size()), expected),
                "grain scalar reference")) return false;
    }
    return Check(!grain.IsActive(), "grain completes at requested length");
}

bool TestDeterministicRender() {
    iem::GranularEncoder first;
    iem::GranularEncoder second;
    const iem::EncoderConfig config{kSampleRate, kFrames, 3, 0x6D2B79F5U};
    if (!Check(first.Prepare(config) && second.Prepare(config),
            "prepare deterministic granular encoders")) return false;
    iem::IemParams params{};
    params.encoder_mode = iem::EncoderMode::GRANULAR;
    params.granular.mix_tenths_percent = 1000;
    params.granular.read_position_us = 0;
    params.granular.position_mod_us = 0;
    first.ApplyParams(params);
    second.ApplyParams(params);

    Input left{};
    Input right{};
    Output first_output{};
    Output second_output{};
    uint64_t rendered = 0;
    while (rendered < 24000) {
        FillInput(left, right, rendered);
        first_output = {};
        second_output = {};
        if (!Process(first, left, right, first_output)
            || !Process(second, left, right, second_output)) return false;
        for (std::size_t channel = 0; channel < first_output.size(); ++channel) {
            for (std::size_t frame = 0; frame < kFrames; ++frame) {
                if (!Check(first_output[channel][frame] == second_output[channel][frame],
                        "fixed seed produces deterministic output")) return false;
                if (!Check(std::isfinite(first_output[channel][frame]),
                        "granular output remains finite")) return false;
            }
        }
        rendered += kFrames;
    }
    return Check(first.ActiveGrainCount() > 0, "deterministic render creates grains");
}

bool TestFreezeAndReset() {
    iem::GranularEncoder encoder;
    if (!encoder.Prepare({kSampleRate, kFrames, 3, 123U})) return false;
    iem::IemParams params{};
    encoder.ApplyParams(params);
    encoder.SetFreeze(true);
    if (!Check(!encoder.IsFrozen(), "freeze requires valid history")) return false;
    Input left{};
    Input right{};
    left.fill(0.25F);
    right.fill(-0.25F);
    Output output{};
    if (!Process(encoder, left, right, output)) return false;
    encoder.SetFreeze(true);
    if (!Check(encoder.IsFrozen(), "freeze activates after history")) return false;
    const uint32_t frozen_head = encoder.WriteHeadForTest();
    if (!Process(encoder, left, right, output)) return false;
    if (!Check(encoder.WriteHeadForTest() == frozen_head, "freeze stops write head")) {
        return false;
    }
    encoder.Reset();
    return Check(!encoder.IsFrozen(), "reset clears freeze")
        && Check(encoder.WriteHeadForTest() == 0, "reset clears write head")
        && Check(encoder.ActiveGrainCount() == 0, "reset clears grains");
}

bool TestTwoDimensionalElevation() {
    iem::GranularEncoder encoder;
    if (!encoder.Prepare({kSampleRate, kFrames, 3, 456U})) return false;
    iem::IemParams params{};
    params.granular.spatial_mode = iem::GranularSpatialMode::TWO_D;
    params.granular.elevation_centidegrees = 3000;
    params.granular.size_centidegrees = 36000;
    params.granular.delta_time_us = 1000;
    encoder.ApplyParams(params);
    Input left{};
    Input right{};
    Output output{};
    if (!Process(encoder, left, right, output)) return false;
    return Check(Near(encoder.LastSpawnedDirectionForTest().z, 0.5F),
        "2D spatialization preserves center elevation");
}

bool TestPoolExhaustionAndNoAllocation() {
    iem::GranularEncoder encoder;
    if (!encoder.Prepare({kSampleRate, kFrames, 3, 789U})) return false;
    iem::IemParams params{};
    params.granular.delta_time_us = 1000;
    params.granular.grain_length_us = 2000000;
    params.granular.position_mod_us = 0;
    params.granular.mix_tenths_percent = 1000;
    encoder.ApplyParams(params);
    Input left{};
    Input right{};
    left.fill(0.1F);
    right.fill(-0.1F);
    Output output{};

    const uint64_t before = g_new_calls.load(std::memory_order_relaxed);
    g_count_new.store(true, std::memory_order_release);
    for (int block = 0; block < 520; ++block) {
        if (!Process(encoder, left, right, output)) {
            g_count_new.store(false, std::memory_order_release);
            return false;
        }
    }
    g_count_new.store(false, std::memory_order_release);
    const uint64_t after = g_new_calls.load(std::memory_order_relaxed);
    return Check(encoder.ActiveGrainCount() == iem::GranularEncoder::kMaxGrains,
            "grain pool reaches fixed capacity")
        && Check(encoder.PoolExhaustionCount() > 0, "grain pool exhaustion counted")
        && Check(before == after, "Granular Process performs no operator new allocation");
}

bool TestHistoryWrapAndExtremes() {
    iem::GranularEncoder encoder;
    if (!encoder.Prepare({kSampleRate, kFrames, 1, 0xABCDEFU})) return false;
    iem::IemParams params{};
    params.granular.delta_time_us = 2000000;
    params.granular.grain_length_us = 1000;
    params.granular.shape_tenths = -100;
    params.granular.size_centidegrees = 36000;
    params.granular.pitch_millisem = -12000;
    params.granular.pitch_mod_millisem = 12000;
    params.granular.attack_tenths_percent = 0;
    params.granular.decay_tenths_percent = 0;
    params.granular.sample_wise = true;
    encoder.ApplyParams(params);
    Input left{};
    Input right{};
    Output output{};
    const uint32_t history_frames = kSampleRate * iem::GranularEncoder::kHistorySeconds;
    uint32_t rendered = 0;
    while (rendered < history_frames + kFrames) {
        if (!Process(encoder, left, right, output)) return false;
        rendered += kFrames;
    }
    return Check(encoder.ValidHistoryFramesForTest() == history_frames,
            "history valid count saturates")
        && Check(encoder.WriteHeadForTest() == kFrames,
            "history write head wraps exactly");
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
    if (!TestGrainReference()) return 1;
    if (!TestDeterministicRender()) return 1;
    if (!TestFreezeAndReset()) return 1;
    if (!TestTwoDimensionalElevation()) return 1;
    if (!TestPoolExhaustionAndNoAllocation()) return 1;
    if (!TestHistoryWrapAndExtremes()) return 1;
    std::puts("IEM granular encoder tests passed");
    return 0;
}
