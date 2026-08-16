#include "DspGraph.h"
#include "DspResources.h"
#include "viper/utils/Crc32.h"
#include <array>
#include <cmath>
#include <cstdio>

namespace {

bool Check(bool condition, const char *message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}

bool TestConvolverKernelReplaysIntoReplacementGraph() {
    using namespace viper::params;
    viper::audio::DspResources resources;
    std::array<float, 16 * 4> kernel{};
    kernel[0] = 1.0F;
    kernel[1] = 2.0F;
    kernel[2] = 3.0F;
    kernel[3] = 4.0F;
    const uint32_t crc = Crc32(
        reinterpret_cast<const uint8_t *>(kernel.data()),
        kernel.size() * sizeof(float)
    );

    if (!Check(
            resources.CaptureRaw(
                kParamConvolverPrepareBuffer,
                kernel.size(),
                4,
                0,
                0,
                nullptr
            ) == viper::audio::ResourceCaptureResult::UPDATED,
            "prepare convolver snapshot"
        )) {
        return false;
    }
    if (!Check(
            resources.CaptureRaw(
                kParamConvolverSetBuffer,
                0,
                0,
                0,
                kernel.size(),
                reinterpret_cast<const signed char *>(kernel.data())
            ) == viper::audio::ResourceCaptureResult::UPDATED,
            "append convolver snapshot"
        )) {
        return false;
    }
    if (!Check(
            resources.CaptureRaw(
                kParamConvolverCommitBuffer,
                kernel.size(),
                crc,
                77,
                0,
                nullptr
            ) == viper::audio::ResourceCaptureResult::COMMITTED,
            "commit convolver snapshot"
        )) {
        return false;
    }

    viper::audio::DspGraph replacement;
    if (!replacement.Prepare({48000, 8192, 2})) return false;
    if (!Check(resources.ApplyTo(replacement), "replay convolver resource")) return false;
    return Check(
        replacement.Engine().GetConvolverKernelID() == 77,
        "replacement graph receives kernel ID"
    );
}

bool TestUnsupportedChannelCountPreservesCommittedState() {
    using namespace viper::params;
    viper::audio::DspResources resources;
    const std::array<float, 16> kernel{1.0F};
    const uint32_t crc = Crc32(
        reinterpret_cast<const uint8_t *>(kernel.data()),
        kernel.size() * sizeof(float)
    );
    resources.CaptureRaw(kParamConvolverPrepareBuffer, kernel.size(), 1, 0, 0, nullptr);
    resources.CaptureRaw(
        kParamConvolverSetBuffer,
        0,
        0,
        0,
        kernel.size(),
        reinterpret_cast<const signed char *>(kernel.data())
    );
    resources.CaptureRaw(
        kParamConvolverCommitBuffer, kernel.size(), crc, 23, 0, nullptr
    );

    if (!Check(
            resources.CaptureRaw(kParamConvolverPrepareBuffer, 80, 5, 0, 0, nullptr)
                == viper::audio::ResourceCaptureResult::INVALID,
            "reject five-channel resource"
        )) {
        return false;
    }

    viper::audio::DspGraph replacement;
    if (!replacement.Prepare({48000, 8192, 6})) return false;
    if (!resources.ApplyTo(replacement)) return false;
    return Check(
        replacement.Engine().GetConvolverKernelID() == 23,
        "unsupported channel count preserves committed kernel"
    );
}

bool TestDdcCoefficientsAffectReplacementGraph() {
    using namespace viper::params;
    viper::audio::DspResources resources;
    const std::array<float, 10> coefficients{
        0.5F, 0.0F, 0.0F, 0.0F, 0.0F,
        0.5F, 0.0F, 0.0F, 0.0F, 0.0F,
    };
    if (!Check(
            resources.CaptureRaw(
                kParamDdcCoefficients,
                0,
                0,
                0,
                5,
                reinterpret_cast<const signed char *>(coefficients.data())
            ) == viper::audio::ResourceCaptureResult::COMMITTED,
            "capture DDC coefficients"
        )) {
        return false;
    }

    viper::ViPERParams params{};
    params.ddc.enable = true;
    viper::audio::DspGraph replacement;
    if (!replacement.Prepare({48000, 8192, 3}, params)) return false;
    if (!Check(resources.ApplyTo(replacement), "replay DDC resource")) return false;
    std::array<float, 1024> frames{};
    for (size_t i = 0; i < frames.size(); i += 2) {
        frames[i] = 1.0F;
        frames[i + 1] = -1.0F;
    }
    if (!Check(replacement.Process(frames.data(), 512), "process DDC block")) return false;
    return Check(
               std::fabs(frames[frames.size() - 2] - 0.5F) < 1.0e-6F,
               "DDC left coefficient applied"
           )
        && Check(
            std::fabs(frames[frames.size() - 1] + 0.5F) < 1.0e-6F,
            "DDC right coefficient applied"
        );
}

bool TestInvalidResourceDoesNotReplaceCommittedState() {
    using namespace viper::params;
    viper::audio::DspResources resources;
    const std::array<float, 16> kernel{1.0F};
    const uint32_t crc = Crc32(
        reinterpret_cast<const uint8_t *>(kernel.data()),
        kernel.size() * sizeof(float)
    );
    resources.CaptureRaw(kParamConvolverPrepareBuffer, 16, 1, 0, 0, nullptr);
    resources.CaptureRaw(
        kParamConvolverSetBuffer,
        0,
        0,
        0,
        16,
        reinterpret_cast<const signed char *>(kernel.data())
    );
    resources.CaptureRaw(kParamConvolverCommitBuffer, 16, crc, 4, 0, nullptr);

    resources.CaptureRaw(kParamConvolverPrepareBuffer, 16, 1, 0, 0, nullptr);
    resources.CaptureRaw(
        kParamConvolverSetBuffer,
        0,
        0,
        0,
        16,
        reinterpret_cast<const signed char *>(kernel.data())
    );
    if (!Check(
            resources.CaptureRaw(kParamConvolverCommitBuffer, 16, crc + 1, 9, 0, nullptr)
                == viper::audio::ResourceCaptureResult::INVALID,
            "reject bad resource CRC"
        )) {
        return false;
    }

    viper::audio::DspGraph replacement;
    if (!replacement.Prepare({48000, 8192, 5})) return false;
    if (!resources.ApplyTo(replacement)) return false;
    return Check(
        replacement.Engine().GetConvolverKernelID() == 4,
        "bad update preserves committed kernel"
    );
}

} // namespace

int main() {
    if (!TestConvolverKernelReplaysIntoReplacementGraph()) return 1;
    if (!TestDdcCoefficientsAffectReplacementGraph()) return 1;
    if (!TestInvalidResourceDoesNotReplaceCommittedState()) return 1;
    if (!TestUnsupportedChannelCountPreservesCommittedState()) return 1;
    std::puts("DSP resource tests passed");
    return 0;
}
