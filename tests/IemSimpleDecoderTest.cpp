#include "iem/HaloBed.h"
#include "iem/SimpleDecoder.h"

#include <cmath>
#include <cstdio>

namespace {

bool Check(bool condition, const char *message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}

bool TestInvalidOrder() {
    iem::SimpleDecoder decoder;
    return Check(!decoder.Prepare(0), "order zero rejected")
        && Check(!decoder.Prepare(iem::kMaxAmbisonicsOrder + 1U), "order four rejected");
}

bool TestDirectionalSource() {
    iem::SimpleDecoder decoder;
    if (!Check(decoder.Prepare(3), "prepare simple decoder")) return false;

    float ambi_storage[iem::kMaxAmbisonicsChannels][1]{};
    float *ambi[iem::kMaxAmbisonicsChannels]{};
    for (uint32_t channel = 0; channel < iem::kMaxAmbisonicsChannels; ++channel) {
        ambi[channel] = ambi_storage[channel];
    }
    float source[iem::kMaxAmbisonicsChannels]{};
    iem::EvaluateSn3d(3, 30.0F * 0.017453292519943295F, 0.0F, source);
    for (uint32_t channel = 0; channel < iem::kMaxAmbisonicsChannels; ++channel) {
        ambi[channel][0] = source[channel];
    }
    float left[1]{};
    float right[1]{};
    float *stereo[2]{left, right};
    if (!Check(decoder.Process(ambi, stereo, 1), "decode directional source")) return false;
    return Check(right[0] > left[0], "+30 degree source favours right");
}

bool TestWOnlyFinite() {
    iem::SimpleDecoder decoder;
    if (!Check(decoder.Prepare(1), "prepare first-order decoder")) return false;
    float ambi_storage[iem::kMaxAmbisonicsChannels][8]{};
    float *ambi[iem::kMaxAmbisonicsChannels]{};
    for (uint32_t channel = 0; channel < iem::kMaxAmbisonicsChannels; ++channel) {
        ambi[channel] = ambi_storage[channel];
    }
    for (int frame = 0; frame < 8; ++frame) ambi[0][frame] = 1.0F;
    float left[8]{};
    float right[8]{};
    float *stereo[2]{left, right};
    if (!Check(decoder.Process(ambi, stereo, 8), "decode W-only input")) return false;
    for (int frame = 0; frame < 8; ++frame) {
        if (!std::isfinite(left[frame]) || !std::isfinite(right[frame])) {
            return Check(false, "W-only output finite");
        }
    }
    return Check(std::fabs(left[0]) > 0.0F && std::fabs(right[0]) > 0.0F,
        "W-only output non-zero");
}

} // namespace

int main() {
    if (!TestInvalidOrder()) return 1;
    if (!TestDirectionalSource()) return 1;
    if (!TestWOnlyFinite()) return 1;
    std::puts("IEM simple decoder tests passed");
    return 0;
}
