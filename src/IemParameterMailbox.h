#pragma once

#include "iem/IemParams.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace viper::audio {

class IemParameterMailbox final {
public:
    uint64_t Publish(const iem::IemParams &params) noexcept;
    bool ConsumeLatest(
        uint64_t &last_generation,
        iem::IemParams &params
    ) noexcept;

private:
    struct Slot {
        iem::IemParams params{};
        uint64_t generation = 0;
    };

    std::array<Slot, 3> slots_{};
    std::atomic<int> published_index_{0};
    std::atomic<int> reading_index_{-1};
    uint64_t next_generation_ = 0;
};

} // namespace viper::audio
