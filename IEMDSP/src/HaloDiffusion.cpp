#include "iem/HaloDiffusion.h"

#include <algorithm>
#include <cmath>

namespace iem {
namespace {

constexpr float kMaxShelfDb = 12.0F;
constexpr float kShelfMinHz = 1000.0F;
constexpr float kShelfMaxHz = 22000.0F;

float Thousandths(int32_t value) noexcept {
    return static_cast<float>(value) * 0.001F;
}

float Clamp01(float value) noexcept {
    return std::clamp(value, 0.0F, 1.0F);
}

} // namespace

float DbToLin(float db) noexcept {
    return std::pow(10.0F, db / 20.0F);
}

float HaloDiffusionGain(float normalized) noexcept {
    const float value = Clamp01(normalized);
    if (value == 0.0F) return 0.0F;
    if (value == 1.0F) return 1.0F;
    return DbToLin(30.0F * value - 30.0F);
}

uint32_t HaloSpaceDelayA(float normalized) noexcept {
    return static_cast<uint32_t>(2500.0F * Clamp01(normalized));
}

uint32_t HaloSpaceDelayB(float normalized) noexcept {
    return static_cast<uint32_t>(625.0F * Clamp01(normalized));
}

float HaloRearShelfFrequency(float normalized) noexcept {
    const float value = Clamp01(normalized);
    return std::exp(std::log(kShelfMinHz)
        + value * (std::log(kShelfMaxHz) - std::log(kShelfMinHz)));
}

bool HaloDiffusion::Prepare(uint32_t sample_rate) noexcept {
    if (sample_rate == 0) return false;
    sample_rate_ = sample_rate;
    delay_ = {};
    shelf_lowpass_ = {};
    write_ = 0;
    prepared_ = true;
    return true;
}

void HaloDiffusion::ApplyParams(const HaloParams &params) noexcept {
    params_ = params;
}

void HaloDiffusion::Reset() noexcept {
    delay_ = {};
    shelf_lowpass_ = {};
    write_ = 0;
}

void HaloDiffusion::Process(float *const bed[7], std::size_t frames) noexcept {
    if (!prepared_ || bed == nullptr) return;

    const float diffusion = HaloDiffusionGain(Thousandths(params_.diffusion_thousandths));
    const uint32_t delay_a = HaloSpaceDelayA(Thousandths(params_.space_thousandths));
    const uint32_t delay_b = HaloSpaceDelayB(Thousandths(params_.space_thousandths));
    const float shelf_db = params_.rear_shelf_enable
        ? (Thousandths(params_.rear_shelf_gain_thousandths) - 0.5F) * 2.0F * kMaxShelfDb
        : 0.0F;
    const float shelf_gain = DbToLin(shelf_db);
    // HaloMixRE confirms logarithmic mapping but not independent rear-shelf
    // endpoints. Keep the current 1..22 kHz mapping explicit and testable.
    const float shelf_frequency = HaloRearShelfFrequency(
        Thousandths(params_.rear_shelf_freq_thousandths));
    const float shelf_alpha = 1.0F - std::exp(
        -6.283185307179586F * shelf_frequency / static_cast<float>(sample_rate_));

    for (std::size_t frame = 0; frame < frames; ++frame) {
        const uint32_t index = write_;
        const uint32_t read_a = (index + kMaxDelay + 1U - delay_a) % (kMaxDelay + 1U);
        const uint32_t read_b = (index + kMaxDelay + 1U - delay_b) % (kMaxDelay + 1U);

        for (uint32_t channel = 0; channel < 7; ++channel) {
            const float input = bed[channel][frame];
            delay_[channel][index] = input;
            const bool rear = channel == 5U || channel == 6U;
            const float delayed = delay_[channel][read_a];
            const float short_delayed = delay_[channel][read_b];
            shelf_lowpass_[channel] += shelf_alpha * (delayed - shelf_lowpass_[channel]);
            const float shelf_input = rear
                ? shelf_lowpass_[channel] + shelf_gain * (delayed - shelf_lowpass_[channel])
                : delayed;
            const float decorrelated = rear
                ? 0.5F * (shelf_input + short_delayed)
                : delayed;
            bed[channel][frame] = input + diffusion * decorrelated;
        }
        write_ = (write_ + 1U) % (kMaxDelay + 1U);
    }
}

} // namespace iem
