#include "IemGraphSlots.h"

namespace viper::audio {
namespace {

constexpr uint32_t kNone = 3U;

constexpr uint32_t Pack(uint32_t active, uint32_t pending, uint32_t previous) {
    return active | (pending << 2U) | (previous << 4U);
}

constexpr uint32_t ActiveIndex(uint32_t state) { return state & 3U; }
constexpr uint32_t PendingIndex(uint32_t state) { return (state >> 2U) & 3U; }
constexpr uint32_t PreviousIndex(uint32_t state) { return (state >> 4U) & 3U; }

} // namespace

bool IemGraphSlots::PrepareInitial(
    const IemGraphConfig &config,
    const iem::IemParams &params,
    const IemResources &resources
) noexcept {
    uint32_t expected = Pack(kNone, kNone, kNone);
    if (state_.load(std::memory_order_acquire) != expected) return false;
    if (!graphs_[0].Prepare(config, params) || !resources.ApplyTo(graphs_[0])) {
        return false;
    }
    return state_.compare_exchange_strong(
        expected,
        Pack(0, kNone, kNone),
        std::memory_order_release,
        std::memory_order_acquire
    );
}

bool IemGraphSlots::PreparePending(
    const IemGraphConfig &config,
    const iem::IemParams &params,
    const IemResources &resources
) noexcept {
    uint32_t expected = state_.load(std::memory_order_acquire);
    const uint32_t active = ActiveIndex(expected);
    const uint32_t previous = PreviousIndex(expected);
    if (active == kNone
        || PendingIndex(expected) != kNone
        || config.generation <= graphs_[active].Config().generation) {
        return false;
    }
    uint32_t target = 0;
    while (target == active || target == previous) ++target;
    if (target >= graphs_.size()) return false;
    if (!graphs_[target].Prepare(config, params)
        || !resources.ApplyTo(graphs_[target])) {
        return false;
    }
    return state_.compare_exchange_strong(
        expected,
        Pack(active, target, kNone),
        std::memory_order_release,
        std::memory_order_acquire
    );
}

IemGraphSwapResult IemGraphSlots::ConsumePending() noexcept {
    uint32_t expected = state_.load(std::memory_order_acquire);
    for (;;) {
        const uint32_t active = ActiveIndex(expected);
        const uint32_t pending = PendingIndex(expected);
        const uint32_t previous = PreviousIndex(expected);
        if (pending == kNone) {
            return {
                active != kNone ? &graphs_[active] : nullptr,
                previous != kNone ? &graphs_[previous] : nullptr,
                false,
                false,
            };
        }
        const uint32_t desired = Pack(pending, kNone, active);
        if (state_.compare_exchange_weak(
                expected,
                desired,
                std::memory_order_acq_rel,
                std::memory_order_acquire
            )) {
            return {
                &graphs_[pending],
                active != kNone ? &graphs_[active] : nullptr,
                true,
                active != kNone
                    && graphs_[active].Config().sample_rate
                        != graphs_[pending].Config().sample_rate,
            };
        }
    }
}

bool IemGraphSlots::CancelPending() noexcept {
    uint32_t expected = state_.load(std::memory_order_acquire);
    for (;;) {
        const uint32_t pending = PendingIndex(expected);
        if (pending == kNone) return true;
        const uint32_t desired = Pack(
            ActiveIndex(expected), kNone, PreviousIndex(expected));
        if (state_.compare_exchange_weak(
                expected,
                desired,
                std::memory_order_acq_rel,
                std::memory_order_acquire
            )) {
            return true;
        }
    }
}

void IemGraphSlots::ReleasePrevious() noexcept {
    uint32_t expected = state_.load(std::memory_order_acquire);
    for (;;) {
        if (PreviousIndex(expected) == kNone) return;
        const uint32_t desired = Pack(
            ActiveIndex(expected), PendingIndex(expected), kNone
        );
        if (state_.compare_exchange_weak(
                expected,
                desired,
                std::memory_order_acq_rel,
                std::memory_order_acquire
            )) {
            return;
        }
    }
}

IemGraph *IemGraphSlots::Active() noexcept {
    const uint32_t index = ActiveIndex(state_.load(std::memory_order_acquire));
    return index != kNone ? &graphs_[index] : nullptr;
}

const IemGraph *IemGraphSlots::Active() const noexcept {
    const uint32_t index = ActiveIndex(state_.load(std::memory_order_acquire));
    return index != kNone ? &graphs_[index] : nullptr;
}

IemGraph *IemGraphSlots::Pending() noexcept {
    const uint32_t index = PendingIndex(state_.load(std::memory_order_acquire));
    return index != kNone ? &graphs_[index] : nullptr;
}

const IemGraph *IemGraphSlots::Pending() const noexcept {
    const uint32_t index = PendingIndex(state_.load(std::memory_order_acquire));
    return index != kNone ? &graphs_[index] : nullptr;
}

IemGraph *IemGraphSlots::Previous() noexcept {
    const uint32_t index = PreviousIndex(state_.load(std::memory_order_acquire));
    return index != kNone ? &graphs_[index] : nullptr;
}

const IemGraph *IemGraphSlots::Previous() const noexcept {
    const uint32_t index = PreviousIndex(state_.load(std::memory_order_acquire));
    return index != kNone ? &graphs_[index] : nullptr;
}

} // namespace viper::audio
