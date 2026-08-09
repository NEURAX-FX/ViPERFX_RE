#include "DspGraph.h"
#include <algorithm>
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

} // namespace

int main() {
    if (!TestConfigurationValidation()) return 1;
    if (!TestPrepareAndProcessAt384Khz()) return 1;
    std::puts("DspGraph tests passed");
    return 0;
}
