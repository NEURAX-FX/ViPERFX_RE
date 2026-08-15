#include "Session0StateCache.h"

#include <cstdio>
#include <memory>

namespace {

bool Check(bool condition, const char *message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}

bool TestDefaultsAreSafeBypass() {
    viper::audio::Session0StateCache cache;
    const auto state = cache.Load();
    return Check(!state.initialized, "cache starts uninitialized")
        && Check(!state.active, "session 0 starts inactive")
        && Check(state.generation == 0, "cache generation starts at zero")
        && Check(state.dsp_resources == nullptr, "cache starts without resources");
}

bool TestValidatedStateRoundTrips() {
    viper::audio::Session0StateCache cache;
    viper::ViPERParams params{};
    params.equalizer.enable = true;
    iem::IemParams iem{};
    iem.enable = true;
    auto mutable_resources =
        std::make_shared<viper::audio::CommittedDspResourceSnapshot>();
    mutable_resources->convolver_kernel_id = 91;
    const viper::audio::CommittedDspResourcePtr resources = mutable_resources;

    if (!Check(cache.StoreActive(true) == 1, "active store increments generation")) {
        return false;
    }
    if (!Check(cache.StoreParams(params) == 2, "param store increments generation")) {
        return false;
    }
    if (!Check(cache.StoreIem(iem, 7) == 3, "IEM store increments generation")) {
        return false;
    }
    if (!Check(cache.StoreResources(resources) == 4,
            "resource store increments generation")) return false;

    const auto restored = cache.Load();
    return Check(restored.initialized, "cache initializes on first store")
        && Check(restored.active, "active flag round-trips")
        && Check(restored.generation == 4, "latest generation round-trips")
        && Check(restored.params.equalizer.enable, "ViPER params round-trip")
        && Check(restored.iem_params.enable, "IEM params round-trip")
        && Check(restored.iem_resource_generation == 7,
            "IEM generation round-trips")
        && Check(restored.dsp_resources == resources,
            "resources remain shared")
        && Check(restored.dsp_resources->convolver_kernel_id == 91,
            "shared resource data round-trips");
}

bool TestStoresReplaceOnlyOwnedFields() {
    viper::audio::Session0StateCache cache;
    viper::ViPERParams params{};
    params.reverb.enable = true;
    cache.StoreParams(params);
    cache.StoreActive(true);
    const auto state = cache.Load();
    return Check(state.params.reverb.enable, "active store preserves params")
        && Check(state.active, "active store updates active flag");
}

bool TestSessionGateAppliesOnlyToOutputMix() {
    return Check(viper::audio::IsSession0(0), "session zero is output mix")
        && Check(!viper::audio::IsSession0(42), "dynamic session is not output mix")
        && Check(viper::audio::ShouldBypassSession0(0, false),
            "inactive output mix is bypassed")
        && Check(!viper::audio::ShouldBypassSession0(0, true),
            "active output mix processes")
        && Check(!viper::audio::ShouldBypassSession0(42, false),
            "dynamic session ignores global gate");
}

} // namespace

int main() {
    if (!TestDefaultsAreSafeBypass()) return 1;
    if (!TestValidatedStateRoundTrips()) return 1;
    if (!TestStoresReplaceOnlyOwnedFields()) return 1;
    if (!TestSessionGateAppliesOnlyToOutputMix()) return 1;
    std::puts("Session 0 state cache tests passed");
    return 0;
}
