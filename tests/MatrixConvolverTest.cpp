#include "viper/utils/MatrixConvolver.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

bool Check(bool condition, const char *message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}

bool Near(float actual, float expected, float tolerance = 1.0e-4F) {
    return std::fabs(actual - expected) <= tolerance;
}

MatrixConvolverMix FullWetMix(
    float cross = 1.0F,
    MatrixRouting routing = MatrixRouting::DIRECT_AND_CROSS,
    uint32_t cross_delay_frames = 0
) {
    return MatrixConvolverMix{cross, 1.0F, 1.0F, routing, cross_delay_frames};
}

bool TestCanonicalFourChannelRouting() {
    MatrixConvolver engine;
    std::array<float, 16 * 4> ir{};
    ir[0] = 1.0F;
    ir[1] = 2.0F;
    ir[2] = 3.0F;
    ir[3] = 4.0F;
    if (!Check(engine.LoadInterleaved(ir.data(), 16, 4, 48000), "load four-channel IR")) {
        return false;
    }
    if (!Check(engine.LatencyFrames() == 1024, "fixed 1024-frame latency")) return false;

    std::vector<float> input(2304 * 2, 0.0F);
    std::vector<float> output(input.size(), 0.0F);
    input[0] = 1.0F;
    input[32 * 2 + 1] = 1.0F;
    engine.Process(input.data(), output.data(), 2304, FullWetMix());

    const uint32_t latency = engine.LatencyFrames();
    return Check(Near(output[latency * 2], 0.5F), "H_LL routes left input to left output")
        && Check(Near(output[latency * 2 + 1], 1.0F), "H_RL routes left input to right output")
        && Check(Near(output[(latency + 32) * 2], 1.5F), "H_LR routes right input to left output")
        && Check(Near(output[(latency + 32) * 2 + 1], 2.0F), "H_RR routes right input to right output");
}

bool TestUserControlledCrossDelay() {
    for (const auto [sample_rate, expected_delay] :
         std::array<std::pair<uint32_t, uint32_t>, 2>{{{48000, 15}, {96000, 30}}}) {
        MatrixConvolver engine;
        std::array<float, 16 * 2> ir{};
        ir[0] = 1.0F;
        ir[1] = 2.0F;
        if (!Check(
                engine.LoadInterleaved(ir.data(), 16, 2, sample_rate),
                "load stereo IR"
            )) {
            return false;
        }

        std::vector<float> input(2304 * 2, 0.0F);
        std::vector<float> output(input.size(), 0.0F);
        input[0] = 1.0F;
        engine.Process(
            input.data(), output.data(), 2304,
            FullWetMix(1.0F, MatrixRouting::DIRECT_AND_CROSS, expected_delay)
        );

        const uint32_t latency = engine.LatencyFrames();
        if (!Check(Near(output[latency * 2], 0.5F), "stereo direct impulse")) {
            return false;
        }
        if (!Check(
                Near(output[(latency + expected_delay) * 2 + 1], 1.0F),
                "stereo cross impulse delay"
            )) {
            return false;
        }
    }
    return true;
}

bool TestRoutingModes() {
    MatrixConvolver engine;
    std::array<float, 16 * 4> ir{};
    ir[0] = 1.0F;
    ir[1] = 2.0F;
    ir[2] = 3.0F;
    ir[3] = 4.0F;
    if (!engine.LoadInterleaved(ir.data(), 16, 4, 48000)) return false;

    const auto render = [&](MatrixConvolverMix mix) {
        engine.Reset();
        std::vector<float> input(2048 * 2, 0.0F);
        std::vector<float> output(input.size(), 0.0F);
        input[0] = 1.0F;
        input[1] = 1.0F;
        engine.Process(input.data(), output.data(), 2048, mix);
        return std::array<float, 2>{
            output[engine.LatencyFrames() * 2],
            output[engine.LatencyFrames() * 2 + 1],
        };
    };

    const auto direct = render(FullWetMix(0.25F, MatrixRouting::DIRECT_ONLY));
    const auto cross = render(FullWetMix(0.25F, MatrixRouting::CROSS_ONLY));
    return Check(Near(direct[0], 1.0F), "direct-only left")
        && Check(Near(direct[1], 4.0F), "direct-only right")
        && Check(Near(cross[0], 3.0F), "cross-only left normalizes branch gain")
        && Check(Near(cross[1], 2.0F), "cross-only right normalizes branch gain");
}

bool TestWetGainAndArbitraryCallbacks() {
    MatrixConvolver engine;
    std::array<float, 16> ir{};
    ir[0] = 2.0F;
    if (!engine.LoadInterleaved(ir.data(), 16, 1, 48000)) return false;

    std::vector<float> input(2304 * 2, 0.0F);
    std::vector<float> output(input.size(), 0.0F);
    input[0] = 1.0F;
    input[1] = 1.0F;
    const MatrixConvolverMix mix{
        0.0F, 0.5F, 2.0F, MatrixRouting::DIRECT_ONLY, 0
    };

    uint32_t offset = 0;
    for (const uint32_t frames : std::array<uint32_t, 4>{333, 777, 986, 208}) {
        engine.Process(input.data() + offset * 2, output.data() + offset * 2, frames, mix);
        offset += frames;
    }

    const uint32_t latency = engine.LatencyFrames();
    return Check(offset == 2304, "callback fixture covers all frames")
        && Check(Near(output[latency * 2], 3.0F), "wet and gain align with delayed dry left")
        && Check(Near(output[latency * 2 + 1], 3.0F), "wet and gain align with delayed dry right");
}

bool TestResetClearsHistory() {
    MatrixConvolver engine;
    std::array<float, 2048> ir{};
    ir[1024] = 1.0F;
    if (!engine.LoadInterleaved(ir.data(), 2048, 1, 48000)) return false;

    std::vector<float> impulse(1024 * 2, 0.0F);
    std::vector<float> discarded(1024 * 2, 0.0F);
    impulse[0] = 1.0F;
    impulse[1] = 1.0F;
    engine.Process(impulse.data(), discarded.data(), 1024, FullWetMix(0.0F, MatrixRouting::DIRECT_ONLY));
    engine.Reset();

    std::vector<float> silence(3072 * 2, 0.0F);
    std::vector<float> output(silence.size(), 1.0F);
    engine.Process(silence.data(), output.data(), 3072, FullWetMix(0.0F, MatrixRouting::DIRECT_ONLY));
    for (const float sample : output) {
        if (!Near(sample, 0.0F)) return Check(false, "reset clears input, overlap, and output history");
    }
    return true;
}

} // namespace

int main() {
    if (!TestCanonicalFourChannelRouting()) return 1;
    if (!TestUserControlledCrossDelay()) return 1;
    if (!TestRoutingModes()) return 1;
    if (!TestWetGainAndArbitraryCallbacks()) return 1;
    if (!TestResetClearsHistory()) return 1;
    std::puts("Matrix convolver tests passed");
    return 0;
}
