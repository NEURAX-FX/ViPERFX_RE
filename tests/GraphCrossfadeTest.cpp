#include "GraphCrossfade.h"
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

bool Near(float actual, float expected, float tolerance = 1.0e-5F) {
    return std::fabs(actual - expected) <= tolerance;
}

bool TestPreparation() {
    viper::audio::GraphCrossfade crossfade;
    if (!Check(!crossfade.Prepare(7999), "reject unsupported sample rate")) return false;
    if (!Check(crossfade.Prepare(48000), "prepare 48 kHz transition")) return false;
    if (!Check(crossfade.TransitionFrames() == 240, "5 ms at 48 kHz")) return false;
    if (!Check(crossfade.Prepare(384000), "prepare 384 kHz transition")) return false;
    return Check(crossfade.TransitionFrames() == 1920, "5 ms at 384 kHz");
}

bool TestCorrelationSafeCurve() {
    viper::audio::GraphCrossfade crossfade;
    if (!crossfade.Prepare(48000)) return false;
    crossfade.StartDryToWet();

    std::vector<float> dry(crossfade.TransitionFrames() * 2, 0.25F);
    std::vector<float> wet(crossfade.TransitionFrames() * 2, 0.75F);
    crossfade.Apply(wet.data(), dry.data(), crossfade.TransitionFrames());

    if (!Check(Near(wet[0], 0.25F), "transition begins dry")) return false;
    const size_t middle_frame = crossfade.TransitionFrames() / 2;
    const size_t middle = middle_frame * 2;
    const float progress = static_cast<float>(middle_frame)
        / static_cast<float>(crossfade.TransitionFrames() - 1);
    const float dry_gain = std::cos(progress * 1.57079632679489661923F);
    const float wet_gain = std::sin(progress * 1.57079632679489661923F);
    const float expected_middle =
        (0.25F * dry_gain + 0.75F * wet_gain) / (dry_gain + wet_gain);
    if (!Check(Near(wet[middle], expected_middle), "correlation-safe midpoint")) {
        return false;
    }
    if (!Check(Near(wet[wet.size() - 2], 0.75F), "transition ends wet")) return false;
    if (!Check(!crossfade.IsActive(), "transition completes")) return false;
    return Check(crossfade.RemainingFrames() == 0, "no remaining frames");
}

bool TestBlockContinuity() {
    viper::audio::GraphCrossfade crossfade;
    if (!crossfade.Prepare(8000, 1.0F)) return false;
    crossfade.StartDryToWet();

    std::array<float, 16> dry{};
    std::array<float, 16> wet{};
    dry.fill(1.0F);
    wet.fill(0.0F);
    crossfade.Apply(wet.data(), dry.data(), 3);
    if (!Check(crossfade.RemainingFrames() == 5, "position persists across blocks")) {
        return false;
    }
    crossfade.Apply(wet.data() + 6, dry.data() + 6, 5);
    return Check(!crossfade.IsActive(), "second block completes transition");
}

bool TestCorrelatedSignalsDoNotBoost() {
    viper::audio::GraphCrossfade crossfade;
    if (!crossfade.Prepare(48000)) return false;
    crossfade.StartDryToWet();
    std::vector<float> dry(crossfade.TransitionFrames() * 2, 0.6F);
    std::vector<float> wet(crossfade.TransitionFrames() * 2, 0.6F);

    crossfade.Apply(wet.data(), dry.data(), crossfade.TransitionFrames());

    for (float sample : wet) {
        if (!Check(Near(sample, 0.6F), "correlated transition preserves level")) {
            return false;
        }
    }
    return true;
}

bool TestFadeThroughSilence() {
    viper::audio::GraphCrossfade transition;
    if (!transition.Prepare(48000)) return false;
    transition.StartFadeThroughSilence();
    std::vector<float> old_signal(transition.TransitionFrames() * 2, 0.5F);
    std::vector<float> new_signal(transition.TransitionFrames() * 2, 0.75F);
    transition.Apply(new_signal.data(), old_signal.data(), transition.TransitionFrames());
    const std::size_t middle = transition.TransitionFrames() / 2U * 2U;
    return Check(Near(new_signal[0], 0.5F), "fade-through begins old")
        && Check(std::fabs(new_signal[middle]) < 0.01F, "fade-through reaches silence")
        && Check(Near(new_signal[new_signal.size() - 2U], 0.75F),
            "fade-through ends new")
        && Check(!transition.IsActive(), "fade-through completes");
}

} // namespace

int main() {
    if (!TestPreparation()) return 1;
    if (!TestCorrelationSafeCurve()) return 1;
    if (!TestBlockContinuity()) return 1;
    if (!TestCorrelatedSignalsDoNotBoost()) return 1;
    if (!TestFadeThroughSilence()) return 1;
    std::puts("Graph crossfade tests passed");
    return 0;
}
