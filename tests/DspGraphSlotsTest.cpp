#include "DspGraphSlots.h"
#include "viper/utils/Crc32.h"
#include <array>
#include <cstdio>

namespace {

bool Check(bool condition, const char *message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}

viper::audio::DspResources ConvolverResources(int kernel_id) {
    using namespace viper::params;
    viper::audio::DspResources resources;
    std::array<float, 16> kernel{};
    kernel[0] = 1.0F;
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
    resources.CaptureRaw(kParamConvolverCommitBuffer, 16, crc, kernel_id, 0, nullptr);
    return resources;
}

bool TestPendingGraphPublishesOnlyAtBoundary() {
    viper::audio::DspGraphSlots slots;
    const viper::ViPERParams params{};
    const viper::audio::DspResources resources{};
    if (!Check(
            slots.PrepareInitial({48000, 8192, 1}, params, resources),
            "prepare initial graph"
        )) {
        return false;
    }
    if (!Check(slots.Active()->Config().generation == 1, "initial active generation")) {
        return false;
    }
    if (!Check(
            slots.PreparePending({96000, 8192, 2}, params, resources),
            "prepare pending graph"
        )) {
        return false;
    }
    if (!Check(slots.Active()->Config().generation == 1, "pending does not switch early")) {
        return false;
    }

    const auto swap = slots.ConsumePending();
    return Check(swap.changed, "boundary consumes pending graph")
        && Check(swap.sample_rate_changed, "reports sample rate change")
        && Check(swap.active->Config().generation == 2, "new generation becomes active")
        && Check(swap.previous->Config().generation == 1, "old generation retained");
}

bool TestPreviousSlotCannotBeReusedDuringTransition() {
    viper::audio::DspGraphSlots slots;
    const viper::ViPERParams params{};
    const viper::audio::DspResources resources{};
    if (!slots.PrepareInitial({48000, 8192, 1}, params, resources)) return false;
    if (!slots.PreparePending({48000, 8192, 2}, params, resources)) return false;
    slots.ConsumePending();
    if (!Check(
            !slots.PreparePending({48000, 8192, 3}, params, resources),
            "retained previous slot blocks overwrite"
        )) {
        return false;
    }
    slots.ReleasePrevious();
    return Check(
        slots.PreparePending({48000, 8192, 3}, params, resources),
        "released slot accepts next graph"
    );
}

bool TestResourcesAreReadyBeforePublication() {
    viper::audio::DspGraphSlots slots;
    const viper::ViPERParams params{};
    const viper::audio::DspResources empty{};
    if (!slots.PrepareInitial({48000, 8192, 1}, params, empty)) return false;
    const auto resources = ConvolverResources(91);
    if (!slots.PreparePending({48000, 8192, 2}, params, resources)) return false;
    if (!Check(
            slots.Pending()->Engine().GetConvolverKernelID() == 91,
            "pending graph already contains resources"
        )) {
        return false;
    }
    slots.ConsumePending();
    return Check(
        slots.Active()->Engine().GetConvolverKernelID() == 91,
        "published graph retains resource"
    );
}

bool TestInvalidOrStalePendingGraphIsRejected() {
    viper::audio::DspGraphSlots slots;
    const viper::ViPERParams params{};
    const viper::audio::DspResources resources{};
    if (!slots.PrepareInitial({48000, 8192, 7}, params, resources)) return false;
    return Check(
               !slots.PreparePending({384001, 8192, 8}, params, resources),
               "reject invalid pending config"
           )
        && Check(
            !slots.PreparePending({96000, 8192, 7}, params, resources),
            "reject stale generation"
        )
        && Check(slots.Active()->Config().generation == 7, "active graph unchanged");
}

bool TestReusedSlotDoesNotRetainOldResources() {
    viper::audio::DspGraphSlots slots;
    const viper::ViPERParams params{};
    const auto resources = ConvolverResources(33);
    const viper::audio::DspResources empty{};
    if (!slots.PrepareInitial({48000, 8192, 1}, params, resources)) return false;
    if (!slots.PreparePending({48000, 8192, 2}, params, empty)) return false;
    slots.ConsumePending();
    slots.ReleasePrevious();
    if (!slots.PreparePending({48000, 8192, 3}, params, empty)) return false;

    return Check(
        slots.Pending()->Engine().GetConvolverKernelID() == 0,
        "reused slot clears previous convolution kernel"
    );
}

} // namespace

int main() {
    if (!TestPendingGraphPublishesOnlyAtBoundary()) return 1;
    if (!TestPreviousSlotCannotBeReusedDuringTransition()) return 1;
    if (!TestResourcesAreReadyBeforePublication()) return 1;
    if (!TestInvalidOrStalePendingGraphIsRejected()) return 1;
    if (!TestReusedSlotDoesNotRetainOldResources()) return 1;
    std::puts("DSP graph slot tests passed");
    return 0;
}
