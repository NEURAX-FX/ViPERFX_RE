#pragma once

#include <cstdint>

namespace iem {

class FixedRandom {
public:
    explicit constexpr FixedRandom(uint32_t seed = kFallbackSeed) noexcept
        : state_(seed == 0 ? kFallbackSeed : seed) {}

    uint32_t NextU32() noexcept {
        uint32_t value = state_;
        value ^= value << 13U;
        value ^= value >> 17U;
        value ^= value << 5U;
        state_ = value;
        return value;
    }

    float NextUnit() noexcept {
        return static_cast<float>(NextU32() >> 8U) * (1.0F / 16777216.0F);
    }

    float NextBipolar() noexcept {
        return NextUnit() * 2.0F - 1.0F;
    }

private:
    static constexpr uint32_t kFallbackSeed = 0x6D2B79F5U;
    uint32_t state_;
};

} // namespace iem
