#pragma once

#include "iem/IemParams.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace iem {

float DbToLin(float db) noexcept;
float HaloDiffusionGain(float normalized) noexcept;
uint32_t HaloSpaceDelayA(float normalized) noexcept;
uint32_t HaloSpaceDelayB(float normalized) noexcept;
float HaloRearShelfFrequency(float normalized) noexcept;

class HaloDiffusion {
public:
    bool Prepare(uint32_t sample_rate) noexcept;
    void ApplyParams(const HaloParams &params) noexcept;
    void Reset() noexcept;
    void Process(float *const bed[7], std::size_t frames) noexcept;

private:
    static constexpr uint32_t kMaxDelay = 2500;

    bool prepared_ = false;
    uint32_t sample_rate_ = 0;
    uint32_t write_ = 0;
    HaloParams params_{};
    std::array<std::array<float, kMaxDelay + 1>, 7> delay_{};
    std::array<float, 7> shelf_lowpass_{};
};

} // namespace iem
