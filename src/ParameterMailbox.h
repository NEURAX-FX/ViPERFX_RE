#ifndef VIPER_PARAMETER_MAILBOX_H
#define VIPER_PARAMETER_MAILBOX_H

#include "ViPERParams.h"
#include <array>
#include <atomic>
#include <cstdint>

namespace viper::audio {

class ParameterMailbox final {
public:
    uint64_t Publish(const viper::ViPERParams &params) noexcept;
    bool ConsumeLatest(
        uint64_t &last_generation,
        viper::ViPERParams &params
    ) noexcept;

private:
    struct Slot {
        viper::ViPERParams params{};
        uint64_t generation = 0;
    };

    std::array<Slot, 3> slots_{};
    std::atomic<int> published_index_{0};
    std::atomic<int> reading_index_{-1};
    uint64_t next_generation_ = 0;
};

} // namespace viper::audio

#endif
