#include "iem/PlanarBlockScheduler.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace iem {

bool PlanarBlockScheduler::Prepare(
    uint32_t channels,
    std::size_t capacity_frames
) noexcept {
    if (channels == 0 || channels > AlignedPlanarBuffer::kMaxChannels
        || capacity_frames < kBlockFrames) {
        return false;
    }
    if (capacity_frames > std::numeric_limits<std::size_t>::max() - (kBlockFrames - 1)) {
        return false;
    }
    const std::size_t rounded_capacity =
        (capacity_frames + kBlockFrames - 1) / kBlockFrames * kBlockFrames;
    AlignedPlanarBuffer replacement;
    if (!replacement.Prepare(channels, rounded_capacity)) return false;

    ring_ = std::move(replacement);
    channels_ = channels;
    capacity_ = rounded_capacity;
    Reset();
    return true;
}

bool PlanarBlockScheduler::Push(
    const float *const *input,
    std::size_t frames
) noexcept {
    if (!IsPrepared() || input == nullptr || frames > FreeFrames()) return false;
    for (uint32_t channel = 0; channel < channels_; ++channel) {
        if (input[channel] == nullptr) return false;
    }
    if (frames == 0) return true;

    const std::size_t first = std::min(frames, capacity_ - write_index_);
    const std::size_t second = frames - first;
    for (uint32_t channel = 0; channel < channels_; ++channel) {
        float *destination = ring_.ChannelData(channel);
        std::memcpy(
            destination + write_index_, input[channel], first * sizeof(float)
        );
        if (second != 0) {
            std::memcpy(destination, input[channel] + first, second * sizeof(float));
        }
    }
    write_index_ = (write_index_ + frames) % capacity_;
    count_ += frames;
    return true;
}

bool PlanarBlockScheduler::Pop(
    float *const *output,
    std::size_t frames
) noexcept {
    if (!IsPrepared() || output == nullptr || frames > count_) return false;
    for (uint32_t channel = 0; channel < channels_; ++channel) {
        if (output[channel] == nullptr) return false;
    }
    if (frames == 0) return true;

    const std::size_t first = std::min(frames, capacity_ - read_index_);
    const std::size_t second = frames - first;
    for (uint32_t channel = 0; channel < channels_; ++channel) {
        const float *source = ring_.ChannelData(channel);
        std::memcpy(output[channel], source + read_index_, first * sizeof(float));
        if (second != 0) {
            std::memcpy(output[channel] + first, source, second * sizeof(float));
        }
    }
    read_index_ = (read_index_ + frames) % capacity_;
    count_ -= frames;
    return true;
}

const float *PlanarBlockScheduler::ChannelBlock(uint32_t channel) const noexcept {
    if (!HasBlock() || channel >= channels_) return nullptr;
    return ring_.ChannelData(channel) + read_index_;
}

void PlanarBlockScheduler::ConsumeBlock() noexcept {
    if (!HasBlock()) return;
    read_index_ = (read_index_ + kBlockFrames) % capacity_;
    count_ -= kBlockFrames;
}

void PlanarBlockScheduler::Reset() noexcept {
    ring_.Clear();
    read_index_ = 0;
    write_index_ = 0;
    count_ = 0;
}

} // namespace iem
