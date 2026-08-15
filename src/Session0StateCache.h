#ifndef VIPER_SESSION0_STATE_CACHE_H
#define VIPER_SESSION0_STATE_CACHE_H

#include "DspResources.h"
#include "ViPERParams.h"
#include "iem/IemParams.h"

#include <cstdint>
#include <mutex>

namespace viper::audio {

constexpr int kParamDriverSession0Active = 0x120F0;

constexpr bool IsSession0(int32_t session_id) noexcept {
    return session_id == 0;
}

constexpr bool ShouldBypassSession0(
    int32_t session_id,
    bool active
) noexcept {
    return IsSession0(session_id) && !active;
}

struct Session0CachedState final {
    bool initialized = false;
    bool active = false;
    uint64_t generation = 0;
    viper::ViPERParams params{};
    iem::IemParams iem_params{};
    uint64_t iem_resource_generation = 0;
    CommittedDspResourcePtr dsp_resources{};
};

class Session0StateCache final {
public:
    static Session0StateCache &Instance() noexcept;

    Session0CachedState Load() const;
    uint64_t StoreActive(bool active);
    uint64_t StoreParams(const viper::ViPERParams &params);
    uint64_t StoreIem(
        const iem::IemParams &params,
        uint64_t resource_generation
    );
    uint64_t StoreResources(CommittedDspResourcePtr resources);

private:
    uint64_t MarkUpdatedLocked() noexcept;

    mutable std::mutex mutex_{};
    Session0CachedState state_{};
};

} // namespace viper::audio

#endif
