#include "iem/MultiEncoder.h"
#include "iem/StereoEncoder.h"
#include "reference/PinnedEncoderReference.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace {

constexpr std::size_t kFrames = 8;
constexpr float kDegreesToRadians = 0.017453292519943295F;

std::atomic<uint64_t> g_new_calls{0};
std::atomic<bool> g_count_new{false};

using InputChannel = std::array<float, kFrames>;
using OutputChannel = std::array<float, kFrames>;
using OutputBlock = std::array<OutputChannel, iem::kMaxAmbisonicsChannels>;

bool Check(bool condition, const char *message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}

bool Near(float left, float right, float tolerance = 2.0e-5F) {
    return std::fabs(left - right) <= tolerance;
}

bool Process(
    iem::IemEncoder &encoder,
    const InputChannel &left,
    const InputChannel &right,
    OutputBlock &output,
    std::size_t frames = kFrames
) {
    const float *inputs[2]{left.data(), right.data()};
    float *outputs[iem::kMaxAmbisonicsChannels]{};
    for (std::size_t channel = 0; channel < output.size(); ++channel) {
        outputs[channel] = output[channel].data();
    }
    return encoder.Process(inputs, outputs, frames);
}

bool TestStereoDefaultsAndOrderClearing() {
    iem::StereoEncoder encoder;
    if (!Check(encoder.Prepare({96000, kFrames, 3}), "prepare stereo encoder")) {
        return false;
    }
    iem::IemParams params{};
    encoder.ApplyParams(params);

    InputChannel left{};
    InputChannel right{};
    left[0] = 1.0F;
    OutputBlock output{};
    if (!Check(Process(encoder, left, right, output), "process stereo left impulse")) {
        return false;
    }
    const auto expected_left = iem_test::FirstOrderSn3d(30.0F * kDegreesToRadians, 0.0F);
    for (std::size_t channel = 0; channel < expected_left.size(); ++channel) {
        if (!Check(Near(output[channel][0], expected_left[channel]),
                "stereo left direction")) return false;
    }

    encoder.Reset();
    left.fill(0.0F);
    right[0] = 1.0F;
    output = {};
    if (!Check(Process(encoder, left, right, output), "process stereo right impulse")) {
        return false;
    }
    const auto expected_right = iem_test::FirstOrderSn3d(-30.0F * kDegreesToRadians, 0.0F);
    for (std::size_t channel = 0; channel < expected_right.size(); ++channel) {
        if (!Check(Near(output[channel][0], expected_right[channel]),
                "stereo right direction")) return false;
    }

    iem::StereoEncoder first_order;
    if (!Check(first_order.Prepare({96000, kFrames, 1}), "prepare first-order stereo")) {
        return false;
    }
    first_order.ApplyParams(params);
    output = {};
    output[15].fill(9.0F);
    if (!Check(Process(first_order, left, right, output), "process first-order stereo")) {
        return false;
    }
    for (std::size_t channel = 4; channel < output.size(); ++channel) {
        for (float sample : output[channel]) {
            if (!Check(sample == 0.0F, "clear inactive stereo order channels")) {
                return false;
            }
        }
    }
    return true;
}

bool TestStereoSmoothingModes() {
    iem::StereoEncoder encoder;
    if (!Check(encoder.Prepare({96000, kFrames, 1}), "prepare smoothing stereo")) {
        return false;
    }
    iem::IemParams params{};
    params.stereo.width_centidegrees = 0;
    encoder.ApplyParams(params);
    InputChannel left{};
    InputChannel right{};
    left.fill(1.0F);
    OutputBlock output{};
    if (!Check(Process(encoder, left, right, output), "prime stereo position")) return false;

    params.stereo.azimuth_centidegrees = 9000;
    encoder.ApplyParams(params);
    output = {};
    if (!Check(Process(encoder, left, right, output), "block-wise stereo smoothing")) {
        return false;
    }
    if (!Check(output[3][0] > output[3][kFrames - 1], "block-wise X ramp")) {
        return false;
    }
    if (!Check(Near(output[1][kFrames - 1], 1.0F)
            && Near(output[3][kFrames - 1], 0.0F), "block-wise target endpoint")) {
        return false;
    }

    params.stereo.sample_wise = true;
    params.stereo.azimuth_centidegrees = 0;
    encoder.ApplyParams(params);
    output = {};
    if (!Check(Process(encoder, left, right, output), "sample-wise stereo smoothing")) {
        return false;
    }
    return Check(output[1][0] > output[1][kFrames - 1]
            && Near(output[1][kFrames - 1], 0.0F)
            && Near(output[3][kFrames - 1], 1.0F),
        "sample-wise target endpoint");
}

bool TestMultiMappingGainAndMute() {
    iem::MultiEncoder encoder;
    if (!Check(encoder.Prepare({96000, kFrames, 3}), "prepare multi encoder")) {
        return false;
    }
    iem::IemParams params{};
    encoder.ApplyParams(params);
    InputChannel left{};
    InputChannel right{};
    left[0] = 1.0F;
    OutputBlock output{};
    if (!Check(Process(encoder, left, right, output), "process multi left impulse")) {
        return false;
    }
    const auto expected_left = iem_test::FirstOrderSn3d(-30.0F * kDegreesToRadians, 0.0F);
    for (std::size_t channel = 0; channel < expected_left.size(); ++channel) {
        if (!Check(Near(output[channel][0], expected_left[channel]),
                "multi source zero direction")) return false;
    }

    iem::MultiEncoder right_encoder;
    if (!Check(right_encoder.Prepare({96000, kFrames, 3}),
            "prepare right multi mapping")) return false;
    right_encoder.ApplyParams(params);
    left.fill(0.0F);
    right[0] = 1.0F;
    output = {};
    if (!Check(Process(right_encoder, left, right, output),
            "process multi right impulse")) return false;
    const auto expected_right = iem_test::FirstOrderSn3d(
        30.0F * kDegreesToRadians, 0.0F
    );
    for (std::size_t channel = 0; channel < expected_right.size(); ++channel) {
        if (!Check(Near(output[channel][0], expected_right[channel]),
                "multi source one direction")) return false;
    }

    params.multi.gain_decidb[0] = -60;
    encoder.ApplyParams(params);
    left.fill(1.0F);
    output = {};
    if (!Check(Process(encoder, left, right, output), "process multi gain ramp")) return false;
    if (!Check(Near(output[0][kFrames - 1], std::pow(10.0F, -0.3F)),
            "multi gain endpoint")) return false;

    params.multi.mute[0] = true;
    encoder.ApplyParams(params);
    output = {};
    if (!Check(Process(encoder, left, right, output), "process multi mute ramp")) return false;
    if (!Check(Near(output[0][kFrames - 1], 0.0F), "multi mute endpoint")) return false;

    encoder.Reset();
    right.fill(1.0F);
    params.multi.mute[1] = true;
    encoder.ApplyParams(params);
    output = {};
    if (!Check(Process(encoder, left, right, output), "process both muted sources")) {
        return false;
    }
    return Check(Near(output[0][kFrames - 1], 0.0F), "both sources muted");
}

bool TestValidation() {
    iem::StereoEncoder unprepared;
    InputChannel left{};
    InputChannel right{};
    OutputBlock output{};
    if (!Check(!Process(unprepared, left, right, output), "reject unprepared process")) {
        return false;
    }
    iem::MultiEncoder invalid;
    return Check(!invalid.Prepare({96000, 0, 3})
            && !invalid.Prepare({96000, kFrames, 4}),
        "reject invalid encoder config");
}

bool TestProcessDoesNotAllocate() {
    iem::StereoEncoder stereo;
    iem::MultiEncoder multi;
    if (!stereo.Prepare({96000, kFrames, 3})
        || !multi.Prepare({96000, kFrames, 3})) {
        return false;
    }
    const iem::IemParams params{};
    stereo.ApplyParams(params);
    multi.ApplyParams(params);
    InputChannel left{};
    InputChannel right{};
    left.fill(0.1F);
    right.fill(-0.1F);
    OutputBlock output{};

    const uint64_t before = g_new_calls.load(std::memory_order_relaxed);
    g_count_new.store(true, std::memory_order_release);
    const bool stereo_ok = Process(stereo, left, right, output);
    const bool multi_ok = Process(multi, left, right, output);
    g_count_new.store(false, std::memory_order_release);
    const uint64_t after = g_new_calls.load(std::memory_order_relaxed);
    return Check(stereo_ok && multi_ok, "process allocation fixtures")
        && Check(before == after, "encoder Process performs no operator new allocation");
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
    if (!TestStereoDefaultsAndOrderClearing()) return 1;
    if (!TestStereoSmoothingModes()) return 1;
    if (!TestMultiMappingGainAndMute()) return 1;
    if (!TestValidation()) return 1;
    if (!TestProcessDoesNotAllocate()) return 1;
    std::puts("IEM encoder tests passed");
    return 0;
}
