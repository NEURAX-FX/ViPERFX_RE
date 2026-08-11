#include "iem/PartitionedMatrixConvolver.h"

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

std::vector<float> MakeIr(uint32_t inputs, uint32_t frames) {
    std::vector<float> ir(static_cast<std::size_t>(2U) * inputs * frames);
    for (uint32_t output = 0; output < 2; ++output) {
        for (uint32_t input = 0; input < inputs; ++input) {
            float *kernel = ir.data()
                + (static_cast<std::size_t>(output) * inputs + input) * frames;
            for (uint32_t frame = 0; frame < frames; ++frame) {
                kernel[frame] = std::sin(static_cast<float>((output + 1U) * (input + 2U)
                    * (frame + 1U)) * 0.017F) * 0.002F;
            }
            kernel[0] += static_cast<float>((output + 1U) * (input + 1U)) * 0.01F;
        }
    }
    return ir;
}

void DirectConvolution(const std::vector<std::vector<float>> &input,
    const std::vector<float> &ir, uint32_t ir_frames,
    std::array<std::vector<float>, 2> &output) {
    const uint32_t inputs = static_cast<uint32_t>(input.size());
    for (uint32_t out = 0; out < 2; ++out) {
        output[out].assign(input[0].size() + ir_frames - 1U, 0.0F);
        for (uint32_t in = 0; in < inputs; ++in) {
            const float *kernel = ir.data()
                + (static_cast<std::size_t>(out) * inputs + in) * ir_frames;
            for (std::size_t frame = 0; frame < input[in].size(); ++frame) {
                for (uint32_t tap = 0; tap < ir_frames; ++tap) {
                    output[out][frame + tap] += input[in][frame] * kernel[tap];
                }
            }
        }
    }
}

bool RunCase(uint32_t inputs, uint32_t ir_frames, uint32_t partition_frames) {
    constexpr std::size_t kInputFrames = 389;
    const auto ir = MakeIr(inputs, ir_frames);
    std::vector<std::vector<float>> input(inputs, std::vector<float>(kInputFrames));
    for (uint32_t channel = 0; channel < inputs; ++channel) {
        for (std::size_t frame = 0; frame < kInputFrames; ++frame) {
            input[channel][frame] = std::sin(
                static_cast<float>((channel + 1U) * (frame + 3U)) * 0.031F) * 0.1F;
        }
    }
    std::array<std::vector<float>, 2> reference;
    DirectConvolution(input, ir, ir_frames, reference);

    iem::PartitionedMatrixConvolver convolver;
    if (!Check(convolver.Prepare(inputs, ir_frames, partition_frames, ir.data()),
            "prepare matrix convolver")) return false;
    if (!Check(convolver.InputChannels() == inputs
            && convolver.LatencyFrames() == partition_frames, "convolver metadata")) return false;

    const std::size_t total_frames = kInputFrames + ir_frames + partition_frames + 8U;
    std::vector<std::vector<float>> padded(inputs, std::vector<float>(total_frames));
    for (uint32_t channel = 0; channel < inputs; ++channel) {
        std::copy(input[channel].begin(), input[channel].end(), padded[channel].begin());
    }
    std::array<std::vector<float>, 2> actual{
        std::vector<float>(total_frames), std::vector<float>(total_frames)};
    constexpr std::array<std::size_t, 8> chunks{1, 7, 64, 255, 3, 128, 19, 384};
    std::size_t position = 0;
    std::size_t chunk_index = 0;
    while (position < total_frames) {
        const std::size_t frames = std::min(
            chunks[chunk_index++ % chunks.size()], total_frames - position);
        std::array<const float *, 16> input_pointers{};
        for (uint32_t channel = 0; channel < inputs; ++channel) {
            input_pointers[channel] = padded[channel].data() + position;
        }
        float *output_pointers[2]{actual[0].data() + position, actual[1].data() + position};
        if (!Check(convolver.Process(input_pointers.data(), output_pointers, frames),
                "stream matrix convolution")) return false;
        position += frames;
    }
    for (uint32_t output = 0; output < 2; ++output) {
        for (std::size_t frame = 0; frame < reference[output].size(); ++frame) {
            const float value = actual[output][frame + partition_frames];
            if (!Check(std::isfinite(value), "finite convolver output")) return false;
            if (!Check(std::fabs(value - reference[output][frame]) <= 3.0e-4F,
                    "partitioned convolution matches direct reference")) return false;
        }
    }

    convolver.Reset();
    const std::size_t reset_frames = partition_frames * 2U;
    std::vector<std::vector<float>> zeros(inputs, std::vector<float>(reset_frames));
    std::array<std::vector<float>, 2> reset_output{
        std::vector<float>(reset_frames, 1.0F), std::vector<float>(reset_frames, 1.0F)};
    std::array<const float *, 16> zero_pointers{};
    for (uint32_t channel = 0; channel < inputs; ++channel) {
        zero_pointers[channel] = zeros[channel].data();
    }
    float *reset_pointers[2]{reset_output[0].data(), reset_output[1].data()};
    if (!convolver.Process(zero_pointers.data(), reset_pointers, reset_frames)) return false;
    for (const auto &channel : reset_output) {
        for (float value : channel) {
            if (!Check(value == 0.0F, "reset clears convolution state")) return false;
        }
    }
    return true;
}

bool TestValidationAndNoAllocation() {
    iem::PartitionedMatrixConvolver convolver;
    const std::array<float, 4> ir{1.0F, 0.0F, 1.0F, 0.0F};
    if (!Check(!convolver.Prepare(0, 1, 64, ir.data())
            && !convolver.Prepare(17, 1, 64, ir.data())
            && !convolver.Prepare(1, 0, 64, ir.data())
            && !convolver.Prepare(1, 1, 63, ir.data())
            && !convolver.Prepare(1, 1, 64, nullptr), "reject invalid config")) return false;

    const std::array<float, 2> valid_ir{1.0F, -1.0F};
    if (!convolver.Prepare(1, 1, 64, valid_ir.data())) return false;
    std::array<float, 128> input{};
    std::array<float, 128> left{};
    std::array<float, 128> right{};
    const float *inputs[1]{input.data()};
    float *outputs[2]{left.data(), right.data()};
    const uint64_t before = g_new_calls.load(std::memory_order_relaxed);
    g_count_new.store(true, std::memory_order_release);
    const bool ok = convolver.Process(inputs, outputs, input.size());
    g_count_new.store(false, std::memory_order_release);
    const uint64_t after = g_new_calls.load(std::memory_order_relaxed);
    return Check(ok, "allocation process")
        && Check(before == after, "convolver Process performs no operator new allocation");
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
    if (!RunCase(4, 1, 64)) return 1;
    if (!RunCase(9, 236, 128)) return 1;
    if (!RunCase(16, 514, 256)) return 1;
    if (!TestValidationAndNoAllocation()) return 1;
    std::puts("IEM partitioned matrix convolver tests passed");
    return 0;
}
