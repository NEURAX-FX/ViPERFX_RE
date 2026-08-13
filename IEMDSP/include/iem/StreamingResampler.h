#pragma once

#include "iem/AlignedPlanarBuffer.h"

#include <cstddef>
#include <cstdint>

namespace iem {

class StreamingResampler final {
public:
    static constexpr uint32_t kTapCount = 64;
    static constexpr uint32_t kPhaseCount = 1024;

    StreamingResampler() = default;
    ~StreamingResampler();

    StreamingResampler(const StreamingResampler &) = delete;
    StreamingResampler &operator=(const StreamingResampler &) = delete;
    StreamingResampler(StreamingResampler &&other) noexcept;
    StreamingResampler &operator=(StreamingResampler &&other) noexcept;

    bool Prepare(
        uint32_t input_rate,
        uint32_t output_rate,
        uint32_t channels,
        std::size_t max_input_frames
    ) noexcept;
    std::size_t Process(
        const float *const *input,
        std::size_t input_frames,
        float *const *output,
        std::size_t output_capacity
    ) noexcept;

    std::size_t MaxOutputFrames(std::size_t input_frames) const noexcept;
    uint32_t LatencyInputFrames() const noexcept;
    void Reset() noexcept;
    bool IsPrepared() const noexcept;
    bool Failed() const noexcept { return failed_; }

private:
    static constexpr int32_t kLeftTaps = 31;
    static constexpr int32_t kRightTaps = 32;

    bool BuildCoefficients() noexcept;
    void ReleaseCoefficients() noexcept;
    void DiscardConsumedInput() noexcept;
    float InputSample(uint32_t channel, int64_t absolute_index) const noexcept;

    AlignedPlanarBuffer input_ring_;
    float *coefficients_ = nullptr;
    uint32_t input_rate_ = 0;
    uint32_t output_rate_ = 0;
    uint32_t channels_ = 0;
    std::size_t max_input_frames_ = 0;
    std::size_t ring_capacity_ = 0;
    std::size_t ring_start_slot_ = 0;
    std::size_t ring_count_ = 0;
    uint64_t ring_start_index_ = 0;
    uint64_t total_input_frames_ = 0;
    uint64_t next_source_numerator_ = 0;
    bool failed_ = false;
};

} // namespace iem
