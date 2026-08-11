#include "iem/LinkedLookaheadLimiter.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace {

constexpr std::size_t kFrames = 8000;
std::atomic<uint64_t> g_new_calls{0};
std::atomic<bool> g_count_new{false};

bool Check(bool condition, const char *message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}

bool TestLimiter() {
    iem::LinkedLookaheadLimiter limiter;
    if (!Check(limiter.Prepare(96000), "prepare limiter")) return false;
    limiter.SetCeilingCentidb(-30);
    std::array<float, kFrames> left{};
    std::array<float, kFrames> right{};
    std::array<float, kFrames> output_left{};
    std::array<float, kFrames> output_right{};
    left[200] = 2.0F;
    right[200] = 1.0F;
    const float *inputs[2]{left.data(), right.data()};
    float *outputs[2]{output_left.data(), output_right.data()};
    const uint64_t before = g_new_calls.load(std::memory_order_relaxed);
    g_count_new.store(true, std::memory_order_release);
    const bool ok = limiter.Process(inputs, outputs, kFrames);
    g_count_new.store(false, std::memory_order_release);
    const uint64_t after = g_new_calls.load(std::memory_order_relaxed);
    if (!Check(ok && before == after, "limiter callback allocation audit")) return false;
    const float ceiling = std::pow(10.0F, -0.3F / 20.0F);
    if (!Check(std::abs(output_left[200 + limiter.LatencyFrames()]) <= ceiling + 1.0e-6F,
            "limiter enforces ceiling")) return false;
    if (!Check(std::abs(output_right[200 + limiter.LatencyFrames()]
            / output_left[200 + limiter.LatencyFrames()] - 0.5F) <= 1.0e-6F,
            "limiter preserves stereo ratio")) return false;
    if (!Check(limiter.Gain() > 0.85F && limiter.Gain() < 1.0F,
            "limiter follows 50 ms exponential release")) return false;

    limiter.Reset();
    limiter.SetEnabled(false);
    left.fill(0.0F);
    right.fill(0.0F);
    output_left.fill(0.0F);
    output_right.fill(0.0F);
    left[0] = 0.75F;
    if (!limiter.Process(inputs, outputs, 256)) return false;
    return Check(output_left[limiter.LatencyFrames()] == 0.75F,
            "disabled limiter keeps matched delay")
        && Check(limiter.Gain() == 1.0F, "disabled limiter uses unity gain")
        && Check(!limiter.Prepare(48000), "reject non-internal sample rate");
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
    if (!TestLimiter()) return 1;
    std::puts("IEM limiter tests passed");
    return 0;
}
