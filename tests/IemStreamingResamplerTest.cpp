#include "iem/StreamingResampler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

namespace {

std::atomic<uint64_t> g_new_calls{0};
std::atomic<bool> g_count_new{false};

bool Check(bool condition, const char *message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}

std::vector<float> RenderChunked(
    uint32_t input_rate,
    uint32_t output_rate,
    const std::vector<float> &input,
    const std::vector<std::size_t> &chunks
) {
    iem::StreamingResampler resampler;
    if (!resampler.Prepare(input_rate, output_rate, 1, input.size())) return {};
    std::vector<float> result;
    result.reserve(resampler.MaxOutputFrames(input.size()));
    std::vector<float> scratch(resampler.MaxOutputFrames(input.size()));
    std::size_t offset = 0;
    std::size_t chunk_index = 0;
    while (offset < input.size()) {
        const std::size_t requested = chunks[chunk_index % chunks.size()];
        const std::size_t count = std::min(requested, input.size() - offset);
        const float *input_ptrs[1]{input.data() + offset};
        float *output_ptrs[1]{scratch.data()};
        const std::size_t produced = resampler.Process(
            input_ptrs, count, output_ptrs, scratch.size()
        );
        result.insert(result.end(), scratch.begin(), scratch.begin() + produced);
        offset += count;
        ++chunk_index;
    }
    return result;
}

bool TestConstantUpsample() {
    iem::StreamingResampler resampler;
    if (!Check(resampler.Prepare(48000, 96000, 2, 4096), "prepare 48-to-96 resampler")) {
        return false;
    }
    const std::size_t capacity = resampler.MaxOutputFrames(4096);
    std::array<std::vector<float>, 2> input{
        std::vector<float>(4096, 0.25F),
        std::vector<float>(4096, 0.25F),
    };
    std::array<std::vector<float>, 2> output{
        std::vector<float>(capacity),
        std::vector<float>(capacity),
    };
    const float *input_ptrs[2]{input[0].data(), input[1].data()};
    float *output_ptrs[2]{output[0].data(), output[1].data()};
    const std::size_t produced = resampler.Process(
        input_ptrs, input[0].size(), output_ptrs, capacity
    );
    if (!Check(produced > 512 && produced <= capacity, "produce bounded output")) return false;
    for (std::size_t frame = 256; frame < produced; ++frame) {
        if (std::fabs(output[0][frame] - 0.25F) > 1.0e-4F
            || std::fabs(output[1][frame] - 0.25F) > 1.0e-4F) {
            return Check(false, "preserve settled DC level");
        }
    }
    return true;
}

bool TestDownsampleSineRms() {
    constexpr uint32_t kInputRate = 384000;
    constexpr uint32_t kOutputRate = 96000;
    std::vector<float> input(16384);
    for (std::size_t frame = 0; frame < input.size(); ++frame) {
        input[frame] = std::sin(
            2.0 * 3.14159265358979323846 * 1000.0 * frame / kInputRate
        );
    }
    const auto output = RenderChunked(kInputRate, kOutputRate, input, {input.size()});
    if (!Check(output.size() > 1024, "produce downsampled sine")) return false;
    double energy = 0.0;
    std::size_t count = 0;
    for (std::size_t frame = 128; frame < output.size(); ++frame) {
        energy += static_cast<double>(output[frame]) * output[frame];
        ++count;
    }
    const double rms = std::sqrt(energy / count);
    const double error_db = 20.0 * std::log10(rms / std::sqrt(0.5));
    return Check(std::fabs(error_db) < 0.15, "preserve 1 kHz RMS within 0.15 dB");
}

bool TestExtremeUpsampleImpulseIsFinite() {
    std::vector<float> input(512, 0.0F);
    input[100] = 1.0F;
    const auto output = RenderChunked(8000, 96000, input, {512});
    if (!Check(!output.empty(), "produce 8-to-96 output")) return false;
    for (float sample : output) {
        if (!std::isfinite(sample)) return Check(false, "extreme-ratio output stays finite");
    }
    return true;
}

bool TestChunkInvariance() {
    std::vector<float> input(4096);
    for (std::size_t frame = 0; frame < input.size(); ++frame) {
        input[frame] = std::sin(0.013 * static_cast<double>(frame));
    }
    const auto single = RenderChunked(44100, 96000, input, {4096});
    const auto split = RenderChunked(44100, 96000, input, {17, 31, 257, 509});
    if (!Check(single.size() == split.size(), "chunking preserves output count")) return false;
    for (std::size_t frame = 0; frame < single.size(); ++frame) {
        if (std::fabs(single[frame] - split[frame]) > 2.0e-5F) {
            return Check(false, "chunking preserves output samples");
        }
    }
    return true;
}

bool TestResetReproducesImpulse() {
    iem::StreamingResampler resampler;
    if (!resampler.Prepare(48000, 96000, 1, 1024)) return false;
    std::vector<float> input(1024, 0.0F);
    input[100] = 1.0F;
    std::vector<float> first(resampler.MaxOutputFrames(input.size()));
    std::vector<float> second(first.size());
    const float *input_ptrs[1]{input.data()};
    float *first_ptrs[1]{first.data()};
    const std::size_t first_count = resampler.Process(
        input_ptrs, input.size(), first_ptrs, first.size()
    );
    resampler.Reset();
    float *second_ptrs[1]{second.data()};
    const std::size_t second_count = resampler.Process(
        input_ptrs, input.size(), second_ptrs, second.size()
    );
    return Check(first_count == second_count, "reset preserves output count")
        && Check(
            std::equal(first.begin(), first.begin() + first_count, second.begin()),
            "reset reproduces impulse output"
        );
}

bool TestProcessDoesNotAllocate() {
    iem::StreamingResampler resampler;
    if (!resampler.Prepare(48000, 96000, 2, 1024)) return false;
    const std::size_t capacity = resampler.MaxOutputFrames(1024);
    std::array<std::vector<float>, 2> input{
        std::vector<float>(1024, 0.1F),
        std::vector<float>(1024, -0.1F),
    };
    std::array<std::vector<float>, 2> output{
        std::vector<float>(capacity),
        std::vector<float>(capacity),
    };
    const float *input_ptrs[2]{input[0].data(), input[1].data()};
    float *output_ptrs[2]{output[0].data(), output[1].data()};
    const uint64_t before = g_new_calls.load(std::memory_order_relaxed);
    g_count_new.store(true, std::memory_order_release);
    const std::size_t produced = resampler.Process(
        input_ptrs, input[0].size(), output_ptrs, capacity
    );
    g_count_new.store(false, std::memory_order_release);
    const uint64_t after = g_new_calls.load(std::memory_order_relaxed);
    return Check(produced > 0, "produce output during allocation test")
        && Check(before == after, "Process performs no operator new allocation");
}

} // namespace

void *operator new(std::size_t size) {
    if (g_count_new.load(std::memory_order_acquire)) {
        g_new_calls.fetch_add(1, std::memory_order_relaxed);
    }
    if (void *memory = std::malloc(size)) return memory;
    std::abort();
}

void operator delete(void *memory) noexcept {
    std::free(memory);
}

void operator delete(void *memory, std::size_t) noexcept {
    std::free(memory);
}

int main() {
    if (!TestConstantUpsample()) return 1;
    if (!TestDownsampleSineRms()) return 1;
    if (!TestExtremeUpsampleImpulseIsFinite()) return 1;
    if (!TestChunkInvariance()) return 1;
    if (!TestResetReproducesImpulse()) return 1;
    if (!TestProcessDoesNotAllocate()) return 1;
    std::puts("IEM streaming resampler tests passed");
    return 0;
}
