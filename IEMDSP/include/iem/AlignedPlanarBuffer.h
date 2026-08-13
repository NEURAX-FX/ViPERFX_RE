#pragma once

#include <cstddef>
#include <cstdint>

namespace iem {

class AlignedPlanarBuffer final {
public:
    static constexpr uint32_t kMaxChannels = 16;

    AlignedPlanarBuffer() = default;
    ~AlignedPlanarBuffer();

    AlignedPlanarBuffer(const AlignedPlanarBuffer &) = delete;
    AlignedPlanarBuffer &operator=(const AlignedPlanarBuffer &) = delete;
    AlignedPlanarBuffer(AlignedPlanarBuffer &&other) noexcept;
    AlignedPlanarBuffer &operator=(AlignedPlanarBuffer &&other) noexcept;

    bool Prepare(uint32_t channels, std::size_t frames) noexcept;
    void Clear() noexcept;

    float *ChannelData(uint32_t channel) noexcept;
    const float *ChannelData(uint32_t channel) const noexcept;

    uint32_t Channels() const noexcept { return channels_; }
    std::size_t Frames() const noexcept { return frames_; }
    bool IsPrepared() const noexcept { return storage_ != nullptr; }

private:
    void ResetStorage() noexcept;

    float *storage_ = nullptr;
    uint32_t channels_ = 0;
    std::size_t frames_ = 0;
    std::size_t stride_ = 0;
};

} // namespace iem
