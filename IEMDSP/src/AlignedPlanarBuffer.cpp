#include "iem/AlignedPlanarBuffer.h"

#include <cstdlib>
#include <cstring>
#include <limits>

namespace iem {
namespace {

constexpr std::size_t kAlignmentBytes = 64;
constexpr std::size_t kFloatsPerAlignment = kAlignmentBytes / sizeof(float);

bool RoundUp(std::size_t value, std::size_t multiple, std::size_t &result) noexcept {
    if (multiple == 0 || value > std::numeric_limits<std::size_t>::max() - (multiple - 1)) {
        return false;
    }
    result = (value + multiple - 1) / multiple * multiple;
    return true;
}

} // namespace

AlignedPlanarBuffer::~AlignedPlanarBuffer() {
    ResetStorage();
}

AlignedPlanarBuffer::AlignedPlanarBuffer(AlignedPlanarBuffer &&other) noexcept
    : storage_(other.storage_),
      channels_(other.channels_),
      frames_(other.frames_),
      stride_(other.stride_) {
    other.storage_ = nullptr;
    other.channels_ = 0;
    other.frames_ = 0;
    other.stride_ = 0;
}

AlignedPlanarBuffer &AlignedPlanarBuffer::operator=(AlignedPlanarBuffer &&other) noexcept {
    if (this == &other) return *this;
    ResetStorage();
    storage_ = other.storage_;
    channels_ = other.channels_;
    frames_ = other.frames_;
    stride_ = other.stride_;
    other.storage_ = nullptr;
    other.channels_ = 0;
    other.frames_ = 0;
    other.stride_ = 0;
    return *this;
}

bool AlignedPlanarBuffer::Prepare(uint32_t channels, std::size_t frames) noexcept {
    if (channels == 0 || channels > kMaxChannels || frames == 0) return false;

    std::size_t stride = 0;
    if (!RoundUp(frames, kFloatsPerAlignment, stride)) return false;
    if (channels > std::numeric_limits<std::size_t>::max() / stride) return false;
    const std::size_t float_count = static_cast<std::size_t>(channels) * stride;
    if (float_count > std::numeric_limits<std::size_t>::max() / sizeof(float)) return false;

    void *raw = nullptr;
    if (posix_memalign(&raw, kAlignmentBytes, float_count * sizeof(float)) != 0 || raw == nullptr) {
        return false;
    }
    std::memset(raw, 0, float_count * sizeof(float));

    ResetStorage();
    storage_ = static_cast<float *>(raw);
    channels_ = channels;
    frames_ = frames;
    stride_ = stride;
    return true;
}

void AlignedPlanarBuffer::Clear() noexcept {
    if (storage_ == nullptr) return;
    std::memset(storage_, 0, static_cast<std::size_t>(channels_) * stride_ * sizeof(float));
}

float *AlignedPlanarBuffer::ChannelData(uint32_t channel) noexcept {
    if (storage_ == nullptr || channel >= channels_) return nullptr;
    return storage_ + static_cast<std::size_t>(channel) * stride_;
}

const float *AlignedPlanarBuffer::ChannelData(uint32_t channel) const noexcept {
    if (storage_ == nullptr || channel >= channels_) return nullptr;
    return storage_ + static_cast<std::size_t>(channel) * stride_;
}

void AlignedPlanarBuffer::ResetStorage() noexcept {
    std::free(storage_);
    storage_ = nullptr;
    channels_ = 0;
    frames_ = 0;
    stride_ = 0;
}

} // namespace iem
