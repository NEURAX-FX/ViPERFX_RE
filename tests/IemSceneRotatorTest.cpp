#include "iem/SceneRotator.h"
#include "reference/PinnedRotatorReference.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace {

constexpr std::size_t kFrames = 32;
using Channel = std::array<float, kFrames>;
using Block = std::array<Channel, iem::kMaxAmbisonicsChannels>;

std::atomic<uint64_t> g_new_calls{0};
std::atomic<bool> g_count_new{false};

bool Check(bool condition, const char *message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}

bool Near(float left, float right, float tolerance = 3.0e-5F) {
    return std::fabs(left - right) <= tolerance;
}

bool Process(iem::SceneRotator &rotator, const Block &input, Block &output, bool in_place = false) {
    const float *inputs[iem::kMaxAmbisonicsChannels]{};
    float *outputs[iem::kMaxAmbisonicsChannels]{};
    if (in_place) {
        Block &mutable_input = const_cast<Block &>(input);
        for (std::size_t channel = 0; channel < input.size(); ++channel) {
            inputs[channel] = mutable_input[channel].data();
            outputs[channel] = mutable_input[channel].data();
        }
    } else {
        for (std::size_t channel = 0; channel < input.size(); ++channel) {
            inputs[channel] = input[channel].data();
            outputs[channel] = output[channel].data();
        }
    }
    return rotator.Process(inputs, outputs, kFrames);
}

void FillBlock(Block &block, uint32_t order) {
    const uint32_t channels = iem::AmbisonicsChannelCount(order);
    for (uint32_t channel = 0; channel < channels; ++channel) {
        for (std::size_t frame = 0; frame < kFrames; ++frame) {
            block[channel][frame] = std::sin(
                static_cast<float>((channel + 1U) * (frame + 1U)) * 0.173F
            );
        }
    }
}

float Energy(const Block &block, uint32_t channels, std::size_t frame) {
    float energy = 0.0F;
    for (uint32_t channel = 0; channel < channels; ++channel) {
        energy += block[channel][frame] * block[channel][frame];
    }
    return energy;
}

bool TestIdentityAndEnergy() {
    for (uint32_t order = 1; order <= 3; ++order) {
        iem::SceneRotator rotator;
        if (!Check(rotator.Prepare({96000, kFrames, order}), "prepare rotator order")) {
            return false;
        }
        Block input{};
        Block output{};
        FillBlock(input, order);
        if (!Check(Process(rotator, input, output), "process identity rotation")) return false;
        const uint32_t channels = iem::AmbisonicsChannelCount(order);
        for (uint32_t channel = 0; channel < channels; ++channel) {
            for (std::size_t frame = 0; frame < kFrames; ++frame) {
                if (!Check(Near(output[channel][frame], input[channel][frame]),
                        "identity rotation")) return false;
            }
        }

        iem::IemParams params{};
        params.rotation.yaw_centidegrees = 3700;
        params.rotation.pitch_centidegrees = -2100;
        params.rotation.roll_centidegrees = 5800;
        rotator.ApplyParams(params);
        rotator.Reset();
        output = {};
        if (!Process(rotator, input, output)) return false;
        for (std::size_t frame = 0; frame < kFrames; ++frame) {
            if (!Check(Near(Energy(output, channels, frame), Energy(input, channels, frame),
                    2.0e-4F), "rotation preserves per-degree energy")) return false;
        }
    }
    return true;
}

bool TestKnownYawAndReference() {
    iem::SceneRotator rotator;
    if (!rotator.Prepare({96000, kFrames, 1})) return false;
    iem::IemParams params{};
    params.rotation.yaw_centidegrees = 9000;
    rotator.ApplyParams(params);
    rotator.Reset();
    Block input{};
    Block output{};
    input[3].fill(1.0F);
    if (!Process(rotator, input, output)) return false;
    if (!Check(Near(output[1][0], 1.0F) && Near(output[3][0], 0.0F),
            "+90 yaw rotates X into Y")) return false;

    const auto reference = iem_test::FirstOrderRotation(params.rotation);
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            Block basis{};
            basis[column + 1U].fill(1.0F);
            output = {};
            if (!Process(rotator, basis, output)) return false;
            if (!Check(Near(output[row + 1U][0], reference[row][column]),
                    "first-order pinned matrix reference")) return false;
        }
    }
    return true;
}

bool TestAxisInversions() {
    for (int axis = 0; axis < 3; ++axis) {
        iem::SceneRotator rotator;
        if (!rotator.Prepare({96000, kFrames, 1})) return false;
        iem::IemParams params{};
        if (axis == 0) {
            params.rotation.yaw_centidegrees = 4100;
            params.rotation.invert_yaw = true;
        } else if (axis == 1) {
            params.rotation.pitch_centidegrees = -3700;
            params.rotation.invert_pitch = true;
        } else {
            params.rotation.roll_centidegrees = 5300;
            params.rotation.invert_roll = true;
        }
        rotator.ApplyParams(params);
        rotator.Reset();
        const auto reference = iem_test::FirstOrderRotation(params.rotation);
        for (std::size_t column = 0; column < 3; ++column) {
            Block basis{};
            Block output{};
            basis[column + 1U].fill(1.0F);
            if (!Process(rotator, basis, output)) return false;
            for (std::size_t row = 0; row < 3; ++row) {
                if (!Check(Near(output[row + 1U][0], reference[row][column]),
                        "axis inversion reference")) return false;
            }
        }
    }
    return true;
}

bool TestInversionsSequencesAndReset() {
    iem::SceneRotator normal;
    iem::SceneRotator inverse;
    if (!normal.Prepare({96000, kFrames, 3}) || !inverse.Prepare({96000, kFrames, 3})) {
        return false;
    }
    iem::IemParams params{};
    params.rotation.yaw_centidegrees = 3200;
    params.rotation.pitch_centidegrees = -1900;
    params.rotation.roll_centidegrees = 4700;
    normal.ApplyParams(params);
    normal.Reset();
    params.rotation.invert_overall = true;
    inverse.ApplyParams(params);
    inverse.Reset();

    Block input{};
    Block rotated{};
    FillBlock(input, 3);
    if (!Process(normal, input, rotated)) return false;
    Block restored = rotated;
    if (!Process(inverse, restored, restored, true)) return false;
    const uint32_t channels = iem::AmbisonicsChannelCount(3);
    for (uint32_t channel = 0; channel < channels; ++channel) {
        for (std::size_t frame = 0; frame < kFrames; ++frame) {
            if (!Check(Near(restored[channel][frame], input[channel][frame], 8.0e-5F),
                    "overall inverse restores input")) return false;
        }
    }

    iem::SceneRotator sequences[2];
    Block sequence_output[2]{};
    for (int sequence = 0; sequence < 2; ++sequence) {
        if (!sequences[sequence].Prepare({96000, kFrames, 3})) return false;
        params.rotation.invert_overall = false;
        params.rotation.sequence = static_cast<iem::RotationSequence>(sequence);
        params.rotation.invert_yaw = true;
        params.rotation.invert_pitch = true;
        params.rotation.invert_roll = true;
        sequences[sequence].ApplyParams(params);
        sequences[sequence].Reset();
        if (!Process(sequences[sequence], input, sequence_output[sequence])) return false;
    }
    if (!Check(!Near(sequence_output[1][0][0], sequence_output[0][0][0], 1.0e-4F)
            || !Near(sequence_output[1][1][0], sequence_output[0][1][0], 1.0e-4F),
            "rotation sequences produce distinct transforms")) return false;

    const uint64_t before = sequences[1].MatrixRecomputeCountForTest();
    sequences[1].ApplyParams(params);
    if (!Check(sequences[1].MatrixRecomputeCountForTest() == before,
            "unchanged params do not recompute matrix")) return false;
    sequences[1].ResetAngles();
    sequences[1].Reset();
    Block reset_output{};
    if (!Process(sequences[1], input, reset_output)) return false;
    for (uint32_t channel = 0; channel < channels; ++channel) {
        if (!Check(Near(reset_output[channel][0], input[channel][0]),
                "reset angles restores identity while keeping toggles")) return false;
    }
    return true;
}

bool TestInPlaceAndNoAllocation() {
    iem::SceneRotator rotator;
    if (!rotator.Prepare({96000, kFrames, 3})) return false;
    iem::IemParams params{};
    params.rotation.yaw_centidegrees = 1200;
    rotator.ApplyParams(params);
    rotator.Reset();
    Block input{};
    FillBlock(input, 3);
    const uint64_t before = g_new_calls.load(std::memory_order_relaxed);
    g_count_new.store(true, std::memory_order_release);
    const bool ok = Process(rotator, input, input, true);
    g_count_new.store(false, std::memory_order_release);
    const uint64_t after = g_new_calls.load(std::memory_order_relaxed);
    return Check(ok, "in-place rotation")
        && Check(before == after, "SceneRotator Process performs no operator new allocation");
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
    if (!TestIdentityAndEnergy()) return 1;
    if (!TestKnownYawAndReference()) return 1;
    if (!TestAxisInversions()) return 1;
    if (!TestInversionsSequencesAndReset()) return 1;
    if (!TestInPlaceAndNoAllocation()) return 1;
    std::puts("IEM scene rotator tests passed");
    return 0;
}
