#include "Session0StateCache.h"

#include <utility>

namespace viper::audio {

Session0StateCache &Session0StateCache::Instance() noexcept {
    static Session0StateCache cache;
    return cache;
}

Session0CachedState Session0StateCache::Load() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

uint64_t Session0StateCache::StoreActive(bool active) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.active = active;
    return MarkUpdatedLocked();
}

uint64_t Session0StateCache::StoreParams(const viper::ViPERParams &params) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.params = params;
    return MarkUpdatedLocked();
}

uint64_t Session0StateCache::StoreIem(
    const iem::IemParams &params,
    uint64_t resource_generation
) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.iem_params = params;
    state_.iem_resource_generation = resource_generation;
    return MarkUpdatedLocked();
}

uint64_t Session0StateCache::StoreResources(
    CommittedDspResourcePtr resources
) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.dsp_resources = std::move(resources);
    return MarkUpdatedLocked();
}

uint64_t Session0StateCache::MarkUpdatedLocked() noexcept {
    state_.initialized = true;
    return ++state_.generation;
}

} // namespace viper::audio
