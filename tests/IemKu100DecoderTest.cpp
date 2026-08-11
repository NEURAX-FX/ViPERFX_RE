#include "IemResourceManifest.h"
#include "iem/HeadphoneEq.h"
#include "iem/Ku100Decoder.h"

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

std::atomic<uint64_t> g_new_calls{0};
std::atomic<bool> g_count_new{false};

bool Check(bool condition, const char *message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}

bool TestKu100Impulses(uint32_t order, uint32_t partition) {
    const auto *resource = iem::resources::FindKu100(order);
    if (resource == nullptr) return false;
    for (uint32_t impulse_channel = 0; impulse_channel < resource->input_channels;
            ++impulse_channel) {
        iem::Ku100Decoder decoder;
        if (!Check(decoder.Prepare(order, partition), "prepare KU100 decoder")) return false;
        const std::size_t total = partition + resource->frames + 7U;
        std::vector<std::vector<float>> input(resource->input_channels,
            std::vector<float>(total, 0.0F));
        input[impulse_channel][0] = 1.0F;
        std::array<std::vector<float>, 2> output{
            std::vector<float>(total), std::vector<float>(total)};
        constexpr std::array<std::size_t, 6> chunks{1, 17, 64, 5, 129, 31};
        std::size_t position = 0;
        std::size_t chunk = 0;
        while (position < total) {
            const std::size_t frames = std::min(chunks[chunk++ % chunks.size()],
                total - position);
            std::array<const float *, iem::kMaxAmbisonicsChannels> inputs{};
            for (uint32_t channel = 0; channel < resource->input_channels; ++channel) {
                inputs[channel] = input[channel].data() + position;
            }
            float *outputs[2]{output[0].data() + position, output[1].data() + position};
            if (!decoder.Process(inputs.data(), outputs, frames)) return false;
            position += frames;
        }
        for (uint32_t ear = 0; ear < 2; ++ear) {
            const float *expected = resource->ir
                + (static_cast<std::size_t>(ear) * resource->input_channels
                    + impulse_channel) * resource->frames;
            for (uint32_t frame = 0; frame < resource->frames; ++frame) {
                if (!Check(std::fabs(output[ear][partition + frame] - expected[frame])
                        <= 3.0e-4F, "KU100 impulse response")) return false;
            }
        }
    }
    return true;
}

bool TestHeadphoneEqModels() {
    constexpr uint32_t partition = 128;
    for (int32_t id = 0; id < 23; ++id) {
        const auto *resource = iem::resources::FindHeadphoneEq(id);
        iem::HeadphoneEq equalizer;
        if (!Check(equalizer.Prepare(id, partition) && equalizer.UsesConvolution(),
                "prepare headphone EQ")) return false;
        const std::size_t total = partition + resource->frames + 3U;
        std::array<std::vector<float>, 2> input{
            std::vector<float>(total), std::vector<float>(total)};
        std::array<std::vector<float>, 2> output{
            std::vector<float>(total), std::vector<float>(total)};
        input[0][0] = 1.0F;
        input[1][0] = 1.0F;
        const float *inputs[2]{input[0].data(), input[1].data()};
        float *outputs[2]{output[0].data(), output[1].data()};
        if (!equalizer.Process(inputs, outputs, total)) return false;
        for (uint32_t channel = 0; channel < 2; ++channel) {
            const float *expected = resource->impulse
                + static_cast<std::size_t>(channel) * resource->frames;
            for (uint32_t frame = 0; frame < resource->frames; ++frame) {
                if (!Check(std::fabs(output[channel][partition + frame] - expected[frame])
                        <= 3.0e-4F, "headphone EQ impulse response")) return false;
            }
        }
    }
    return true;
}

bool TestOffDelayResetAndErrors() {
    constexpr uint32_t partition = 64;
    iem::HeadphoneEq equalizer;
    if (!Check(equalizer.Prepare(-1, partition) && !equalizer.UsesConvolution(),
            "prepare matched-delay EQ off")) return false;
    std::array<float, 129> left{};
    std::array<float, 129> right{};
    std::array<float, 129> output_left{};
    std::array<float, 129> output_right{};
    left[0] = 1.0F;
    right[1] = -0.5F;
    const float *inputs[2]{left.data(), right.data()};
    float *outputs[2]{output_left.data(), output_right.data()};
    if (!equalizer.Process(inputs, outputs, left.size())) return false;
    if (!Check(output_left[partition] == 1.0F
            && output_right[partition + 1U] == -0.5F, "EQ off matched delay")) return false;
    equalizer.Reset();
    output_left.fill(1.0F);
    output_right.fill(1.0F);
    if (!equalizer.Process(inputs, outputs, partition)) return false;
    if (!Check(std::all_of(output_left.begin(), output_left.begin() + partition,
            [](float value) { return value == 0.0F; }), "EQ reset clears delay")) return false;

    iem::Ku100Decoder decoder;
    if (!Check(!decoder.Prepare(0, partition)
            && decoder.Error() == iem::IemResourceError::INVALID_ORDER,
            "reject invalid decoder order")) return false;
    if (!Check(!equalizer.Prepare(23, partition)
            && equalizer.Error() == iem::IemResourceError::INVALID_EQ,
            "reject invalid EQ id")) return false;

    if (!decoder.Prepare(1, partition)) return false;
    std::array<float, 1> finite{};
    std::array<float, 1> nonfinite{std::numeric_limits<float>::quiet_NaN()};
    std::array<const float *, iem::kMaxAmbisonicsChannels> decoder_inputs{};
    decoder_inputs[0] = nonfinite.data();
    decoder_inputs[1] = finite.data();
    decoder_inputs[2] = finite.data();
    decoder_inputs[3] = finite.data();
    std::array<float, 1> decoder_left{};
    std::array<float, 1> decoder_right{};
    float *decoder_outputs[2]{decoder_left.data(), decoder_right.data()};
    return Check(!decoder.Process(decoder_inputs.data(), decoder_outputs, 1)
            && decoder.Error() == iem::IemResourceError::PROCESS_NONFINITE,
        "reject non-finite decoder input");
}

bool TestNoCallbackAllocation() {
    iem::Ku100Decoder decoder;
    iem::HeadphoneEq equalizer;
    if (!decoder.Prepare(3, 128) || !equalizer.Prepare(0, 128)) return false;
    std::array<std::array<float, 128>, iem::kMaxAmbisonicsChannels> bus{};
    std::array<const float *, iem::kMaxAmbisonicsChannels> inputs{};
    for (std::size_t channel = 0; channel < bus.size(); ++channel) {
        inputs[channel] = bus[channel].data();
    }
    std::array<float, 128> left{};
    std::array<float, 128> right{};
    std::array<float, 128> eq_left{};
    std::array<float, 128> eq_right{};
    float *outputs[2]{left.data(), right.data()};
    const uint64_t before = g_new_calls.load(std::memory_order_relaxed);
    g_count_new.store(true, std::memory_order_release);
    const bool decode_ok = decoder.Process(inputs.data(), outputs, left.size());
    const float *eq_inputs[2]{left.data(), right.data()};
    float *eq_outputs[2]{eq_left.data(), eq_right.data()};
    const bool eq_ok = equalizer.Process(eq_inputs, eq_outputs, left.size());
    g_count_new.store(false, std::memory_order_release);
    const uint64_t after = g_new_calls.load(std::memory_order_relaxed);
    return Check(decode_ok && eq_ok, "decoder allocation fixtures")
        && Check(before == after, "decoder and EQ callbacks do not allocate");
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
    if (!TestKu100Impulses(1, 64)) return 1;
    if (!TestKu100Impulses(2, 128)) return 1;
    if (!TestKu100Impulses(3, 256)) return 1;
    if (!TestHeadphoneEqModels()) return 1;
    if (!TestOffDelayResetAndErrors()) return 1;
    if (!TestNoCallbackAllocation()) return 1;
    std::puts("IEM KU100 decoder and headphone EQ tests passed");
    return 0;
}
