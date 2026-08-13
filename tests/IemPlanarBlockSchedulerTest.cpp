#include "iem/PlanarBlockScheduler.h"

#include <array>
#include <cstdio>
#include <vector>

namespace {

bool Check(bool condition, const char *message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}

bool PushRange(
    iem::PlanarBlockScheduler &scheduler,
    std::size_t start,
    std::size_t frames
) {
    std::array<std::vector<float>, 2> input{
        std::vector<float>(frames), std::vector<float>(frames)
    };
    for (std::size_t frame = 0; frame < frames; ++frame) {
        input[0][frame] = static_cast<float>(start + frame);
        input[1][frame] = -static_cast<float>(start + frame);
    }
    const float *pointers[2]{input[0].data(), input[1].data()};
    return scheduler.Push(pointers, frames);
}

bool TestChunkedBlocksAndWrap() {
    iem::PlanarBlockScheduler scheduler;
    if (!Check(scheduler.Prepare(2, 1024), "prepare scheduler")) return false;
    std::size_t start = 0;
    for (const std::size_t frames : {17U, 300U, 1U, 511U}) {
        if (!Check(PushRange(scheduler, start, frames), "push chunk")) return false;
        start += frames;
    }
    if (!Check(scheduler.HasBlock(), "expose complete block")) return false;
    for (std::size_t frame = 0; frame < 256; ++frame) {
        if (scheduler.ChannelBlock(0)[frame] != static_cast<float>(frame)
            || scheduler.ChannelBlock(1)[frame] != -static_cast<float>(frame)) {
            return Check(false, "first block keeps ramp order");
        }
    }
    scheduler.ConsumeBlock();
    if (!Check(scheduler.AvailableFrames() == 573, "consume exactly one block")) return false;

    if (!Check(PushRange(scheduler, start, 400), "push wrapping chunk")) return false;
    std::array<std::vector<float>, 2> output{
        std::vector<float>(700), std::vector<float>(700)
    };
    float *output_ptrs[2]{output[0].data(), output[1].data()};
    if (!Check(scheduler.Pop(output_ptrs, 700), "pop across wrap")) return false;
    for (std::size_t frame = 0; frame < 700; ++frame) {
        const float expected = static_cast<float>(256 + frame);
        if (output[0][frame] != expected || output[1][frame] != -expected) {
            return Check(false, "wrapped pop keeps sample order");
        }
    }
    return true;
}

bool TestOverflowDoesNotModifyData() {
    iem::PlanarBlockScheduler scheduler;
    if (!scheduler.Prepare(2, 256)) return false;
    if (!PushRange(scheduler, 0, 200)) return false;
    const std::size_t before = scheduler.AvailableFrames();
    if (!Check(!PushRange(scheduler, 200, 100), "reject overflow")) return false;
    if (!Check(scheduler.AvailableFrames() == before, "overflow preserves count")) return false;
    std::array<std::vector<float>, 2> output{
        std::vector<float>(200), std::vector<float>(200)
    };
    float *pointers[2]{output[0].data(), output[1].data()};
    if (!scheduler.Pop(pointers, 200)) return false;
    for (std::size_t frame = 0; frame < 200; ++frame) {
        if (output[0][frame] != static_cast<float>(frame)) {
            return Check(false, "overflow preserves queued samples");
        }
    }
    return true;
}

bool TestResetAndInvalidShapes() {
    iem::PlanarBlockScheduler scheduler;
    if (!Check(!scheduler.Prepare(0, 256), "reject zero channels")) return false;
    if (!Check(!scheduler.Prepare(17, 256), "reject too many channels")) return false;
    if (!Check(!scheduler.Prepare(2, 255), "reject sub-block capacity")) return false;
    if (!scheduler.Prepare(2, 300)) return false;
    if (!Check(scheduler.CapacityFrames() == 512, "round capacity to block multiple")) return false;
    if (!PushRange(scheduler, 0, 300)) return false;
    scheduler.Reset();
    return Check(scheduler.AvailableFrames() == 0, "reset clears frames")
        && Check(!scheduler.HasBlock(), "reset clears block readiness");
}

} // namespace

int main() {
    if (!TestChunkedBlocksAndWrap()) return 1;
    if (!TestOverflowDoesNotModifyData()) return 1;
    if (!TestResetAndInvalidShapes()) return 1;
    std::puts("IEM planar block scheduler tests passed");
    return 0;
}
