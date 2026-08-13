#include "iem/AlignedPlanarBuffer.h"
#include <cstdint>
#include <cstdio>

namespace {

bool Check(bool condition, const char *message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}

bool TestPrepareMaximumShape() {
    iem::AlignedPlanarBuffer buffer;
    if (!Check(buffer.Prepare(16, 256), "prepare 16x256 storage")) return false;
    return Check(buffer.IsPrepared(), "report prepared state")
        && Check(buffer.Channels() == 16, "report channel count")
        && Check(buffer.Frames() == 256, "report frame count");
}

bool TestChannelAlignmentAndIsolation() {
    iem::AlignedPlanarBuffer buffer;
    if (!Check(buffer.Prepare(16, 256), "prepare storage")) return false;

    for (uint32_t channel = 0; channel < 16; ++channel) {
        float *data = buffer.ChannelData(channel);
        if (!Check(data != nullptr, "channel pointer is valid")) return false;
        if (!Check(reinterpret_cast<uintptr_t>(data) % 64 == 0, "channel is 64-byte aligned")) {
            return false;
        }
        data[17] = static_cast<float>(channel + 1);
    }

    for (uint32_t channel = 0; channel < 16; ++channel) {
        const float *data = buffer.ChannelData(channel);
        if (!Check(data[17] == static_cast<float>(channel + 1), "channels do not alias")) {
            return false;
        }
    }
    return true;
}

bool TestClearZeroesEveryChannel() {
    iem::AlignedPlanarBuffer buffer;
    if (!Check(buffer.Prepare(4, 128), "prepare storage")) return false;
    for (uint32_t channel = 0; channel < 4; ++channel) {
        buffer.ChannelData(channel)[17] = 1.5F;
    }
    buffer.Clear();
    for (uint32_t channel = 0; channel < 4; ++channel) {
        if (!Check(buffer.ChannelData(channel)[17] == 0.0F, "clear zeroes channel data")) {
            return false;
        }
    }
    return true;
}

bool TestRejectsInvalidShapes() {
    iem::AlignedPlanarBuffer buffer;
    return Check(!buffer.Prepare(0, 256), "reject zero channels")
        && Check(!buffer.Prepare(17, 256), "reject channels above kMaxChannels")
        && Check(!buffer.Prepare(2, 0), "reject zero frames")
        && Check(!buffer.IsPrepared(), "stay unprepared after rejected shapes");
}

bool TestRejectedPrepareKeepsExistingStorage() {
    iem::AlignedPlanarBuffer buffer;
    if (!Check(buffer.Prepare(2, 256), "prepare initial storage")) return false;
    buffer.ChannelData(1)[5] = 0.75F;
    if (!Check(!buffer.Prepare(17, 256), "reject oversized reprepare")) return false;
    return Check(buffer.Channels() == 2, "keep previous channel count")
        && Check(buffer.Frames() == 256, "keep previous frame count")
        && Check(buffer.ChannelData(1)[5] == 0.75F, "keep previous channel data");
}

bool TestOutOfRangeChannelReturnsNull() {
    iem::AlignedPlanarBuffer buffer;
    if (!Check(buffer.Prepare(2, 64), "prepare storage")) return false;
    const iem::AlignedPlanarBuffer &const_buffer = buffer;
    return Check(buffer.ChannelData(2) == nullptr, "reject out-of-range channel")
        && Check(const_buffer.ChannelData(2) == nullptr, "reject out-of-range const channel");
}

bool TestUnpreparedBufferIsSafe() {
    iem::AlignedPlanarBuffer buffer;
    buffer.Clear();
    return Check(!buffer.IsPrepared(), "default buffer is unprepared")
        && Check(buffer.Channels() == 0, "default channel count is zero")
        && Check(buffer.Frames() == 0, "default frame count is zero")
        && Check(buffer.ChannelData(0) == nullptr, "default channel pointer is null");
}

} // namespace

int main() {
    if (!TestPrepareMaximumShape()) return 1;
    if (!TestChannelAlignmentAndIsolation()) return 1;
    if (!TestClearZeroesEveryChannel()) return 1;
    if (!TestRejectsInvalidShapes()) return 1;
    if (!TestRejectedPrepareKeepsExistingStorage()) return 1;
    if (!TestOutOfRangeChannelReturnsNull()) return 1;
    if (!TestUnpreparedBufferIsSafe()) return 1;
    std::puts("IEM aligned planar buffer tests passed");
    return 0;
}
