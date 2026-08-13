#include "IemParameterMailbox.h"

namespace viper::audio {

uint64_t IemParameterMailbox::Publish(const iem::IemParams &params) noexcept {
    const int published = published_index_.load(std::memory_order_acquire);
    const int reading = reading_index_.load(std::memory_order_acquire);
    int target = 0;
    while (target == published || target == reading) ++target;

    Slot &slot = slots_[target];
    slot.params = params;
    slot.generation = ++next_generation_;
    published_index_.store(target, std::memory_order_release);
    return slot.generation;
}

bool IemParameterMailbox::ConsumeLatest(
    uint64_t &last_generation,
    iem::IemParams &params
) noexcept {
    for (;;) {
        const int published = published_index_.load(std::memory_order_acquire);
        reading_index_.store(published, std::memory_order_release);
        if (published != published_index_.load(std::memory_order_acquire)) {
            reading_index_.store(-1, std::memory_order_release);
            continue;
        }

        const Slot &slot = slots_[published];
        const uint64_t generation = slot.generation;
        if (generation <= last_generation) {
            reading_index_.store(-1, std::memory_order_release);
            return false;
        }

        params = slot.params;
        reading_index_.store(-1, std::memory_order_release);
        last_generation = generation;
        return true;
    }
}

} // namespace viper::audio
