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
    if (!TestFaultAndNoAllocation()) return 1;
    std::puts("IEM pipeline tests passed");
    return 0;
}
