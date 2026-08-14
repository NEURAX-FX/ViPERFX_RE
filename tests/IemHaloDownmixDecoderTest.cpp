#include "iem/HaloDownmixDecoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

bool Check(bool condition, const char *message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}

bool Near(float left, float right, float tolerance = 1.0e-6F) {
    return std::fabs(left - right) <= tolerance;
}

bool TestInvalidPreparation() {
    iem::HaloDownmixDecoder decoder;
    return Check(!decoder.Prepare(0, 96000, 256), "order zero rejected")
        && Check(!decoder.Prepare(iem::kMaxAmbisonicsOrder + 1U, 96000, 256),
            "order four rejected")
        && Check(!decoder.Prepare(3, 48000, 256), "non-internal sample rate rejected")
        && Check(!decoder.Prepare(3, 96000, 0), "zero block size rejected");
}

bool TestDirectionalSource() {
    constexpr std::size_t kFrames = 4096;
    iem::HaloDownmixDecoder decoder;
    if (!Check(decoder.Prepare(3, 96000, kFrames), "prepare directional decoder")) {
        return false;
    }
    std::array<std::vector<float>, iem::kMaxAmbisonicsChannels> storage{};
    std::array<const float *, iem::kMaxAmbisonicsChannels> inputs{};
    for (uint32_t channel = 0; channel < iem::kMaxAmbisonicsChannels; ++channel) {
        storage[channel].assign(kFrames, 0.0F);
        inputs[channel] = storage[channel].data();
    }
    float source[iem::kMaxAmbisonicsChannels]{};
    iem::EvaluateSn3d(3, 30.0F * 0.017453292519943295F, 0.0F, source);
    for (uint32_t channel = 0; channel < iem::kMaxAmbisonicsChannels; ++channel) {
        storage[channel][0] = source[channel];
    }
    std::vector<float> left(kFrames), right(kFrames);
    float *outputs[2]{left.data(), right.data()};
    if (!Check(decoder.Process(inputs.data(), nullptr, outputs, kFrames),
            "decode directional source")) return false;
    float left_energy = 0.0F;
    float right_energy = 0.0F;
    for (std::size_t frame = decoder.LatencyFrames(); frame < kFrames; ++frame) {
        left_energy += left[frame] * left[frame];
        right_energy += right[frame] * right[frame];
    }
    return Check(right_energy > left_energy, "+30 degree source favours right");
}

bool TestWOnlyAndLfe() {
    constexpr std::size_t kFrames = 4096;
    iem::HaloDownmixDecoder decoder;
    if (!Check(decoder.Prepare(1, 96000, kFrames), "prepare first-order decoder")) {
        return false;
    }
    std::array<std::vector<float>, iem::kMaxAmbisonicsChannels> storage{};
    std::array<const float *, iem::kMaxAmbisonicsChannels> inputs{};
    for (uint32_t channel = 0; channel < iem::kMaxAmbisonicsChannels; ++channel) {
        storage[channel].assign(kFrames, 0.0F);
        inputs[channel] = storage[channel].data();
    }
    storage[0][0] = 1.0F;
    std::vector<float> left(kFrames), right(kFrames);
    float *outputs[2]{left.data(), right.data()};
    if (!Check(decoder.Process(inputs.data(), nullptr, outputs, kFrames),
            "decode W-only input")) return false;
    if (!Check(std::isfinite(left[3072]) && std::isfinite(right[3072])
            && std::fabs(left[3072]) > 0.0F && std::fabs(right[3072]) > 0.0F,
            "W-only output finite and non-zero")) return false;

    decoder.Reset();
    for (auto &channel : storage) std::fill(channel.begin(), channel.end(), 0.0F);
    std::vector<float> lfe(kFrames, 0.0F);
    lfe[0] = 1.0F;
    std::fill(left.begin(), left.end(), 0.0F);
    std::fill(right.begin(), right.end(), 0.0F);
    if (!Check(decoder.Process(inputs.data(), lfe.data(), outputs, kFrames),
            "decode explicit LFE")) return false;
    return Check(Near(left[3072], right[3072])
            && std::fabs(left[3072]) > 0.99F,
            "LFE routes equally at decoder latency");
}

bool TestChunkInvariant() {
    constexpr std::size_t kFrames = 8192;
    std::array<std::vector<float>, iem::kMaxAmbisonicsChannels> storage{};
    std::array<const float *, iem::kMaxAmbisonicsChannels> inputs{};
    for (uint32_t channel = 0; channel < iem::kMaxAmbisonicsChannels; ++channel) {
        storage[channel].resize(kFrames);
        for (std::size_t frame = 0; frame < kFrames; ++frame) {
            storage[channel][frame] = std::sin(
                static_cast<float>(frame + channel * 11U) * 0.013F) * 0.01F;
        }
        inputs[channel] = storage[channel].data();
    }
    std::vector<float> lfe(kFrames);
    for (std::size_t frame = 0; frame < kFrames; ++frame) {
        lfe[frame] = std::cos(static_cast<float>(frame) * 0.007F) * 0.02F;
    }

    iem::HaloDownmixDecoder one;
    if (!Check(one.Prepare(3, 96000, kFrames), "prepare one-block decoder")) {
        return false;
    }
    std::vector<float> one_left(kFrames), one_right(kFrames);
    float *one_outputs[2]{one_left.data(), one_right.data()};
    if (!Check(one.Process(inputs.data(), lfe.data(), one_outputs, kFrames),
            "process one decoder block")) return false;

    iem::HaloDownmixDecoder chunked;
    if (!Check(chunked.Prepare(3, 96000, 256), "prepare chunked decoder")) {
        return false;
    }
    std::vector<float> chunk_left(kFrames), chunk_right(kFrames);
    for (std::size_t offset = 0; offset < kFrames; offset += 256) {
        std::array<const float *, iem::kMaxAmbisonicsChannels> block_inputs{};
        for (uint32_t channel = 0; channel < iem::kMaxAmbisonicsChannels; ++channel) {
            block_inputs[channel] = inputs[channel] + offset;
        }
        float *block_outputs[2]{chunk_left.data() + offset, chunk_right.data() + offset};
        if (!Check(chunked.Process(block_inputs.data(), lfe.data() + offset,
                block_outputs, 256), "process decoder chunk")) return false;
    }
    for (std::size_t frame = 0; frame < kFrames; ++frame) {
        if (!Near(one_left[frame], chunk_left[frame])
            || !Near(one_right[frame], chunk_right[frame])) {
            return Check(false, "decoder is chunk invariant");
        }
    }
    return true;
}

} // namespace

int main() {
    if (!TestInvalidPreparation()) return 1;
    if (!TestDirectionalSource()) return 1;
    if (!TestWOnlyAndLfe()) return 1;
    if (!TestChunkInvariant()) return 1;
    std::puts("IEM Halo Downmix decoder tests passed");
    return 0;
}
