#include "IemGraphSlots.h"

#include <cmath>
#include <cstdio>

namespace {

bool Check(bool condition, const char *message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}

bool TestInitialAndPendingPublication() {
    viper::audio::IemGraphSlots slots;
    viper::audio::IemResources resources;
    iem::IemParams params{};
    if (!Check(slots.PrepareInitial({48000, 8192, 1}, params, resources), "prepare initial graph")) {
        return false;
    }
    if (!Check(slots.Active() != nullptr, "publish initial active graph")) return false;
    if (!Check(slots.Active()->Config().generation == 1, "initial generation")) return false;
    if (!Check(slots.Active()->ResourceGeneration() == 0, "initial resource generation")) return false;

    if (!Check(slots.PreparePending({96000, 8192, 2}, params, resources), "prepare pending graph")) {
        return false;
    }
    if (!Check(slots.Active()->Config().generation == 1, "pending does not replace active early")) {
        return false;
    }
    const auto swap = slots.ConsumePending();
    return Check(swap.changed, "consume pending change")
        && Check(swap.sample_rate_changed, "report sample-rate change")
        && Check(swap.active != nullptr && swap.active->Config().generation == 2, "activate generation 2")
        && Check(swap.previous != nullptr && swap.previous->Config().generation == 1, "retain generation 1");
}

bool TestPublicationGuardsAndRelease() {
    viper::audio::IemGraphSlots slots;
    viper::audio::IemResources resources;
    iem::IemParams params{};
    if (!slots.PrepareInitial({48000, 8192, 1}, params, resources)) return false;
    if (!slots.PreparePending({48000, 8192, 2}, params, resources)) return false;
    if (!Check(!slots.PreparePending({48000, 8192, 3}, params, resources), "reject second pending graph")) {
        return false;
    }
    slots.ConsumePending();
    if (!Check(slots.PreparePending({48000, 8192, 3}, params, resources),
            "allow third-slot preparation while previous retained")) {
        return false;
    }
    if (!Check(slots.Pending() != nullptr
            && slots.Pending()->Config().generation == 3,
            "publish third-slot pending graph")) return false;
    if (!Check(slots.CancelPending(), "cancel third-slot pending graph")) return false;
    slots.ReleasePrevious();
    if (!Check(slots.Previous() == nullptr, "release previous graph")) return false;
    if (!Check(!slots.PreparePending({48000, 8192, 2}, params, resources), "reject equal generation")) {
        return false;
    }
    if (!Check(!slots.PreparePending({48000, 8192, 1}, params, resources), "reject older generation")) {
        return false;
    }
    return Check(slots.PreparePending({48000, 8192, 3}, params, resources), "allow newer generation after release");
}

bool TestPendingCancellationAllowsLatestReplacement() {
    viper::audio::IemGraphSlots slots;
    viper::audio::IemResources resources;
    iem::IemParams params{};
    if (!slots.PrepareInitial({48000, 8192, 1}, params, resources)) return false;
    params.encoder_mode = iem::EncoderMode::MULTI;
    if (!slots.PreparePending({48000, 8192, 2}, params, resources)) return false;
    if (!Check(slots.CancelPending(), "cancel unconsumed pending graph")) return false;
    if (!Check(slots.Pending() == nullptr, "pending graph removed from publication")) return false;
    params.encoder_mode = iem::EncoderMode::GRANULAR;
    if (!Check(slots.PreparePending({48000, 8192, 3}, params, resources),
            "prepare latest replacement")) return false;
    return Check(slots.Pending()->Engine().Params().encoder_mode
            == iem::EncoderMode::GRANULAR, "latest structural snapshot wins");
}

bool TestInvalidReplacementPreservesActive() {
    viper::audio::IemGraphSlots slots;
    viper::audio::IemResources resources;
    iem::IemParams params{};
    if (!slots.PrepareInitial({48000, 8192, 1}, params, resources)) return false;
    if (!Check(!slots.PreparePending({7999, 8192, 2}, params, resources), "reject invalid rate")) return false;
    if (!Check(!slots.PreparePending({48000, 8193, 2}, params, resources), "reject invalid block")) return false;
    return Check(slots.Active() != nullptr && slots.Active()->Config().generation == 1,
                 "invalid replacement preserves active graph");
}

bool TestParamsAndResourcesStayCoherentAcrossTransition() {
    viper::audio::IemGraphSlots slots;
    viper::audio::IemResources resources;
    iem::IemParams params{};
    params.wet = 0.25F;
    if (!slots.PrepareInitial({48000, 8192, 1}, params, resources)) return false;
    if (!resources.CaptureRaw(iem::kParamIemResourceReset, 1)) return false;
    params.wet = 0.75F;
    params.order = 2;
    if (!slots.PreparePending({48000, 8192, 2}, params, resources)) return false;
    const auto swap = slots.ConsumePending();
    if (!Check(swap.active->ResourceGeneration() == 1, "apply replacement resource generation")) return false;
    swap.active->ApplyParams(params);
    swap.previous->ApplyParams(params);
    return Check(std::fabs(swap.active->Engine().Params().wet - 0.75F) < 1.0e-6F,
                 "apply latest params to active")
        && Check(swap.active->Engine().Params().order == 2, "apply replacement order to active")
        && Check(std::fabs(swap.previous->Engine().Params().wet - 0.75F) < 1.0e-6F,
                 "apply latest params to previous")
        && Check(swap.previous->Engine().Params().order == 3,
            "preserve previous graph structural order");
}

} // namespace

int main() {
    if (!TestInitialAndPendingPublication()) return 1;
    if (!TestPublicationGuardsAndRelease()) return 1;
    if (!TestPendingCancellationAllowsLatestReplacement()) return 1;
    if (!TestInvalidReplacementPreservesActive()) return 1;
    if (!TestParamsAndResourcesStayCoherentAcrossTransition()) return 1;
    std::puts("IEM graph slot tests passed");
    return 0;
}
