#include "DspGraph.h"
#include "ViPERParams.h"
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

bool TestConfigurationValidation() {
    viper::audio::DspGraph graph;
    if (!Check(
            !graph.Prepare({7999, 256, 1}),
            "reject sample rate below contract"
        )) {
        return false;
    }
    if (!Check(
            !graph.Prepare({384001, 256, 1}),
            "reject sample rate above contract"
        )) {
        return false;
    }
    if (!Check(!graph.Prepare({48000, 0, 1}), "reject zero block size")) return false;
    return Check(!graph.IsPrepared(), "invalid configuration stays unprepared");
}

bool TestPrepareAndProcessAt384Khz() {
    viper::audio::DspGraph graph;
    const viper::audio::DspGraphConfig config{384000, 8192, 42};
    if (!Check(graph.Prepare(config), "prepare 384 kHz graph")) return false;
    if (!Check(graph.IsPrepared(), "prepared flag")) return false;
    if (!Check(graph.Config().sample_rate == 384000, "retain sample rate")) return false;
    if (!Check(graph.Config().generation == 42, "retain generation")) return false;
    if (!Check(
            graph.Transition().TransitionFrames() == 1920,
            "prepare 5 ms transition table at graph rate"
        )) {
        return false;
    }

    std::vector<float> silence(256 * 2, 0.0F);
    if (!Check(graph.Process(silence.data(), 256), "process valid block")) return false;
    if (!Check(
            std::all_of(silence.begin(), silence.end(), [](float value) {
                return std::isfinite(value);
            }),
            "384 kHz output remains finite"
        )) {
        return false;
    }
    return Check(!graph.Process(silence.data(), 8193), "reject oversized block");
}

bool TestRawParametersSurviveGraphRebuild() {
    using namespace viper::params;
    viper::audio::DspGraph active;
    if (!active.Prepare({48000, 8192, 1})) return false;
    active.Engine().DispatchRawParam(kParamBassEnable, 1, 0, 0, 0, nullptr);
    active.Engine().DispatchRawParam(kParamBassFrequency, 90, 0, 0, 0, nullptr);
    active.Engine().DispatchRawParam(kParamReverbWet, 40, 0, 0, 0, nullptr);
    const viper::ViPERParams snapshot = active.Engine().CurrentParams();
    if (!Check(snapshot.bass.enable, "raw dispatch updates snapshot enable")) return false;
    if (!Check(snapshot.bass.frequency == 90, "raw dispatch updates snapshot value")) {
        return false;
    }

    viper::audio::DspGraph replacement;
    if (!Check(
            replacement.Prepare({96000, 8192, 2}, snapshot),
            "prepare replacement from immutable snapshot"
        )) {
        return false;
    }
    const auto &restored = replacement.Engine().CurrentParams();
    return Check(restored.bass.enable, "restore enabled effect")
        && Check(restored.bass.frequency == 90, "restore effect value")
        && Check(std::fabs(restored.reverb.wet - 0.4F) < 1.0e-6F, "restore disabled effect value");
}

bool TestPrepareClearsPreviousDdcResource() {
    viper::ViPERParams params{};
    params.ddc.enable = true;
    viper::audio::DspGraph graph;
    if (!graph.Prepare({48000, 8192, 1}, params)) return false;
    const std::array<viper::BiquadSection, 1> coefficients{
        viper::BiquadSection{0.5F, 0.0F, 0.0F, 0.0F, 0.0F},
    };
    graph.Engine().LoadDdcCoefficients(coefficients.data(), coefficients.data(), 1);
    if (!graph.Prepare({48000, 8192, 2}, params)) return false;

    std::vector<float> frames(1024, 0.5F);
    if (!graph.Process(frames.data(), 512)) return false;
    return Check(
        std::fabs(frames.back() - 0.5F) < 1.0e-6F,
        "reprepared graph does not retain old DDC coefficients"
    );
}

} // namespace

int main() {
    if (!TestConfigurationValidation()) return 1;
    if (!TestPrepareAndProcessAt384Khz()) return 1;
    if (!TestRawParametersSurviveGraphRebuild()) return 1;
    if (!TestPrepareClearsPreviousDdcResource()) return 1;
    std::puts("DspGraph tests passed");
    return 0;
}
