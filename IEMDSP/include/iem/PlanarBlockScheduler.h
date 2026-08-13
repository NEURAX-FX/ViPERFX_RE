#pragma once

#include "iem/AlignedPlanarBuffer.h"

#include <cstddef>
#include <cstdint>

namespace iem {

class PlanarBlockScheduler final {
public:
    static constexpr std::size_t kBlockFrames = 256;

    bool Prepare(uint32_t channels, std::size_t capacity_frames) noexcept;
    bool Push(const float *const *input, std::size_t frames) noexcept;
    bool Pop(float *const *output, std::size_t frames) noexcept;

    bool HasBlock() const noexcept { return count_ >= kBlockFrames; }
    const float *ChannelBlock(uint32_t channel) const noexcept;
    void ConsumeBlock() noexcept;

    std::size_t AvailableFrames() const noexcept { return count_; }
    std::size_t FreeFrames() const noexcept { return capacity_ - count_; }
    uint32_t Channels() const noexcept { return channels_; }
    std::size_t CapacityFrames() const noexcept { return capacity_; }
    bool IsPrepared() const noexcept { return ring_.IsPrepared(); }
    void Reset() noexcept;

private:
    AlignedPlanarBuffer ring_;
    uint32_t channels_ = 0;
    std::size_t capacity_ = 0;
    std::size_t read_index_ = 0;
    std::size_t write_index_ = 0;
    std::size_t count_ = 0;
};

} // namespace iem
