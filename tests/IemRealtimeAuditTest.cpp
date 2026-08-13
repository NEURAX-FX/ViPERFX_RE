#include "iem/IemPipeline.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace {

constexpr std::size_t kMaxFrames = 256;
constexpr std::size_t kIterations = 1000;
std::atomic<uint64_t> g_new_calls{0};
std::atomic<bool> g_count_new{false};

bool Check(bool condition, const char *message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}

bool TestMaximumConfigurationRealtimePath() {
    iem::IemParams params{};
    params.encoder_mode = iem::EncoderMode::HALO;
    params.render_mode = iem::RenderMode::KU100;
    params.order = 3;
    params.latency_profile = iem::LatencyProfile::STABLE;
    params.decoder.headphone_eq = iem::HeadphoneEqId::SHURE_SRH940;
    params.limiter.enabled = true;
    iem::IemPipeline pipeline;
    if (!Check(pipeline.Prepare(params, kMaxFrames), "prepare maximum IEM pipeline")) {
        return false;
    }

    std::array<float, kMaxFrames> left{};
    std::array<float, kMaxFrames> right{};
    std::array<float, kMaxFrames> output_left{};
    std::array<float, kMaxFrames> output_right{};
    std::array<uint64_t, kIterations> durations{};
    uint32_t random = 0xA341316CU;
    const uint64_t before = g_new_calls.load(std::memory_order_relaxed);
    g_count_new.store(true, std::memory_order_release);
    for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
        random ^= random << 13U;
        random ^= random >> 17U;
        random ^= random << 5U;
        const std::size_t frames = 8U + random % (kMaxFrames - 7U);
        for (std::size_t frame = 0; frame < frames; ++frame) {
            left[frame] = std::sin(static_cast<float>(iteration * kMaxFrames + frame) * 0.013F);
            right[frame] = std::cos(static_cast<float>(iteration * kMaxFrames + frame) * 0.017F);
        }
        const float *inputs[2]{left.data(), right.data()};
        float *outputs[2]{output_left.data(), output_right.data()};
        const auto start = std::chrono::steady_clock::now();
        if (!pipeline.Process(inputs, outputs, frames)) {
            g_count_new.store(false, std::memory_order_release);
            return false;
        }
        durations[iteration] = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start).count());
        for (std::size_t frame = 0; frame < frames; ++frame) {
            if (!std::isfinite(output_left[frame]) || !std::isfinite(output_right[frame])) {
                g_count_new.store(false, std::memory_order_release);
                return false;
            }
        }
    }
    g_count_new.store(false, std::memory_order_release);
    const uint64_t after = g_new_calls.load(std::memory_order_relaxed);
    std::sort(durations.begin(), durations.end());
    const uint64_t p99 = durations[kIterations * 99U / 100U];
    std::printf("IEM maximum-config host p99=%llu ns\n",
        static_cast<unsigned long long>(p99));
    return Check(before == after, "maximum IEM callback performs no operator new allocation")
        && Check(p99 != 0, "record maximum-config p99 timing");
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
    if (!TestMaximumConfigurationRealtimePath()) return 1;
    std::puts("IEM realtime audit tests passed");
    return 0;
}
