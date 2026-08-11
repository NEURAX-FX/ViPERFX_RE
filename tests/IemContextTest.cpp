#include "DspGraph.h"
#include "IemContext.h"
#include "iem/IemParams.h"
#include "ViPERParams.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <random>
#include <vector>

namespace {

bool Check(bool condition, const char *message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}

bool TestDisabledFixturesStayBitIdentical() {
    for (const uint32_t rate : {44100U, 48000U, 96000U, 192000U, 384000U}) {
        for (int fixture = 0; fixture < 4; ++fixture) {
            viper::audio::IemContext context;
            if (!Check(context.Prepare(rate, 1024), "prepare IEM context")) return false;
            std::vector<float> audio(1024 * 2, 0.0F);
            if (fixture == 0) {
                audio[0] = 1.0F;
                audio[1] = -1.0F;
            } else if (fixture == 1) {
                std::mt19937 generator(0x493E4D + rate);
                std::uniform_real_distribution<float> distribution(-1.0F, 1.0F);
                for (float &sample : audio) sample = distribution(generator);
            } else if (fixture == 2) {
                std::fill(audio.begin(), audio.end(), 0.0F);
            } else {
                const float maximum = std::numeric_limits<float>::max() / 4.0F;
                for (std::size_t index = 0; index < audio.size(); ++index) {
                    audio[index] = index % 2 == 0 ? maximum : -maximum;
                }
            }
            const auto original = audio;
            if (!Check(context.Process(audio.data(), 1024), "process disabled context")) return false;
            if (!Check(audio == original, "disabled context preserves fixture")) return false;
        }
    }
    return true;
}

bool TestPostViperBufferIsTheIemInput() {
    viper::ViPERParams params{};
    params.master_limiter.output_volume = 0.5F;
    viper::audio::DspGraph viper_graph;
    if (!viper_graph.Prepare({48000, 1024, 1}, params)) return false;
    viper::audio::IemContext context;
    if (!context.Prepare(48000, 1024)) return false;

    std::vector<float> audio(1024 * 2, 0.25F);
    if (!viper_graph.Process(audio.data(), 1024)) return false;
    const auto post_viper = audio;
    if (!Check(post_viper[0] != 0.25F, "ViPER changes the fixture")) return false;
    if (!Check(context.Process(audio.data(), 1024), "process post-ViPER buffer")) return false;
    return Check(audio == post_viper, "disabled IEM preserves post-ViPER output");
}

bool TestDispatchIsolationAndResourceGeneration() {
    viper::audio::IemContext context;
    if (!context.Prepare(48000, 1024)) return false;
    if (!Check(!context.DispatchRawParam(0x101B9, 0, 0, 0), "leave ViPER ID unhandled")) {
        return false;
    }
    if (!Check(context.DispatchRawParam(iem::kParamIemWet, 65, 0, 0), "handle IEM wet")) {
        return false;
    }
    if (!Check(std::fabs(context.Params().wet - 0.65F) < 1.0e-6F, "update only IEM snapshot")) {
        return false;
    }
    if (!Check(context.DispatchRawParam(iem::kParamIemResourceReset, 1, 0, 0), "handle resource reset")) {
        return false;
    }
    if (!Check(context.ResourceGeneration() == 1, "increment resource generation")) return false;
    std::vector<float> audio(1024 * 2, 0.1F);
    return Check(context.Process(audio.data(), 1024), "consume resource replacement graph");
}

bool TestOversizedFailurePreservesInput() {
    viper::audio::IemContext context;
    if (!context.Prepare(48000, 256)) return false;
    std::vector<float> audio(257 * 2, 0.375F);
    const auto original = audio;
    return Check(!context.Process(audio.data(), 257), "reject oversized block")
        && Check(audio == original, "failure preserves post-ViPER input");
}

bool TestResetKeepsViPERStateIndependent() {
    viper::ViPERParams params{};
    params.master_limiter.output_volume = 0.5F;
    viper::audio::DspGraph viper_graph;
    if (!viper_graph.Prepare({48000, 256, 1}, params)) return false;
    viper::audio::IemContext context;
    if (!context.Prepare(48000, 256)) return false;
    std::vector<float> audio(256 * 2, 0.2F);
    if (!viper_graph.Process(audio.data(), 256)) return false;
    const auto config_before = viper_graph.Config();
    context.Reset();
    return Check(viper_graph.IsPrepared(), "IEM reset keeps ViPER graph prepared")
        && Check(viper_graph.Config().sample_rate == config_before.sample_rate,
                 "IEM reset keeps ViPER sample rate")
        && Check(viper_graph.Config().generation == config_before.generation,
                 "IEM reset keeps ViPER generation")
        && Check(viper_graph.Process(audio.data(), 256),
                 "ViPER graph remains processable after IEM reset");
}

bool TestStructuralDeferralAndLatestWins() {
    viper::audio::IemContext context;
    if (!context.Prepare(48000, 256)) return false;
    const uint64_t initial_generation = context.GraphGeneration();
    if (!context.DispatchRawParam(iem::kParamIemOrder, 2, 0, 0)
        || !context.DispatchRawParam(iem::kParamIemEncoderMode, 1, 0, 0)
        || !context.DispatchRawParam(iem::kParamHeadphoneEq, 0, 0, 0)
        || !context.DispatchRawParam(iem::kParamIemLatencyProfile, 2, 0, 0)) {
        return false;
    }
    if (!Check(context.GraphGeneration() == initial_generation
            && context.PendingGraphForTest() == nullptr,
            "disabled structural updates defer graph preparation")) return false;
    if (!context.DispatchRawParam(iem::kParamIemEnable, 1, 0, 0)) return false;
    if (!Check(context.GraphGeneration() == initial_generation + 1U,
            "enable prepares one complete structural graph")) return false;
    const auto *pending = context.PendingGraphForTest();
    if (!Check(pending != nullptr && pending->Engine().Params().order == 2
            && pending->Engine().Params().encoder_mode == iem::EncoderMode::MULTI
            && pending->Engine().Params().latency_profile == iem::LatencyProfile::STABLE
            && pending->Engine().Params().decoder.headphone_eq
                == iem::HeadphoneEqId::AKG_K1000_CLOSED,
            "pending graph contains complete restored structure")) return false;
    iem::IemTelemetrySnapshot telemetry{};
    if (!Check(context.ReadTelemetry(telemetry)
            && telemetry.graph_generation
                == context.ActiveGraphForTest()->Config().generation
            && telemetry.preparation_result == iem::IemPreparationResult::SUCCESS,
            "context overlays graph preparation telemetry")) return false;

    viper::audio::IemContext rapid;
    if (!rapid.Prepare(48000, 256)
        || !rapid.DispatchRawParam(iem::kParamIemEnable, 1, 0, 0)
        || !rapid.DispatchRawParam(iem::kParamIemEncoderMode, 1, 0, 0)
        || !rapid.DispatchRawParam(iem::kParamIemEncoderMode, 2, 0, 0)) return false;
    return Check(rapid.PendingGraphForTest() != nullptr
            && rapid.PendingGraphForTest()->Engine().Params().encoder_mode
                == iem::EncoderMode::GRANULAR,
        "rapid structural updates replace pending graph with latest");
}

bool TestCommandsInvalidValuesAndFaultFallback() {
    viper::audio::IemContext context;
    if (!context.Prepare(48000, 256)) return false;
    const auto before = context.Params();
    if (!Check(context.DispatchRawParam(iem::kParamIemEnable, 2, 0, 0)
            && context.Params().enable == before.enable
            && context.LastPreparationResult()
                == iem::IemPreparationResult::INVALID_PARAMETER,
            "invalid IEM value is handled without mutation")) return false;
    if (!context.DispatchRawParam(iem::kParamRotationYaw, 9000, 0, 0)
        || !context.DispatchRawParam(iem::kCommandResetRotation, 1, 0, 0)) return false;
    if (!Check(context.Params().rotation.yaw_centidegrees == 0,
            "rotation reset updates persistent snapshot")) return false;

    if (!context.DispatchRawParam(iem::kParamIemEncoderMode, 2, 0, 0)
        || !context.DispatchRawParam(iem::kParamIemEnable, 1, 0, 0)) return false;
    std::vector<float> audio(256 * 2, 0.1F);
    if (!context.Process(audio.data(), 256)) return false;
    if (!context.DispatchRawParam(iem::kCommandGranularFreeze, 1, 0, 0)) return false;
    if (!Check(context.ActiveGraphForTest()->Engine().IsFrozen(),
            "Freeze command reaches active granular graph")) return false;
    if (!context.DispatchRawParam(iem::kCommandResetIemRuntime, 1, 0, 0)) return false;
    if (!Check(!context.ActiveGraphForTest()->Engine().IsFrozen(),
            "runtime reset clears Freeze")) return false;

    audio.assign(256 * 2, 0.25F);
    audio[0] = std::numeric_limits<float>::quiet_NaN();
    if (!Check(!context.Process(audio.data(), 256), "non-finite pipeline fails safely")) {
        return false;
    }
    return Check(std::all_of(audio.begin(), audio.end(),
            [](float value) { return std::isfinite(value); }),
        "fault fallback never returns non-finite samples");
}

} // namespace

int main() {
    if (!TestDisabledFixturesStayBitIdentical()) return 1;
    if (!TestPostViperBufferIsTheIemInput()) return 1;
    if (!TestDispatchIsolationAndResourceGeneration()) return 1;
    if (!TestOversizedFailurePreservesInput()) return 1;
    if (!TestResetKeepsViPERStateIndependent()) return 1;
    if (!TestStructuralDeferralAndLatestWins()) return 1;
    if (!TestCommandsInvalidValuesAndFaultFallback()) return 1;
    std::puts("IEM context tests passed");
    return 0;
}
