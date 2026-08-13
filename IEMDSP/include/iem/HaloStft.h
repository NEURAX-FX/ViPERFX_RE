#pragma once

#include <cstddef>
#include <cstdint>

struct PFFFT_Setup;

namespace iem {

class HaloStft {
public:
    static constexpr uint32_t kFftSize = 1024;
    static constexpr uint32_t kHop = 512;
    static constexpr uint32_t kBins = 513;
    static constexpr uint32_t kReportedLatency = 1024;
    static constexpr uint32_t kHistoryFrames = 6;

    using FrameCallback = void (*)(
        const float left_re[kBins],
        const float left_im[kBins],
        const float right_re[kBins],
        const float right_im[kBins],
        void *user
    );

    HaloStft() = default;
    ~HaloStft();

    HaloStft(const HaloStft &) = delete;
    HaloStft &operator=(const HaloStft &) = delete;

    bool Prepare(std::size_t max_frames) noexcept;
    bool Process(
        const float *left,
        const float *right,
        std::size_t frames,
        FrameCallback on_frame,
        void *user
    ) noexcept;
    bool InverseAdd(
        const float re[kBins],
        const float im[kBins],
        float *dst,
        std::size_t dst_frames
    ) noexcept;
    uint32_t LatencyFrames() const noexcept { return kReportedLatency; }

private:
    void Release() noexcept;
    void AnalyzeChannel(const float *time, float *re, float *im) noexcept;
    bool InverseOne(const float re[kBins], const float im[kBins], float *ola) noexcept;

    bool prepared_ = false;
    PFFFT_Setup *setup_ = nullptr;
    float *work_ = nullptr;
    float *fft_in_ = nullptr;
    float *fft_out_ = nullptr;
    float *window_ = nullptr;
    float *analysis_left_ = nullptr;
    float *analysis_right_ = nullptr;
    float *synth_ = nullptr;
    float *left_re_ = nullptr;
    float *left_im_ = nullptr;
    float *right_re_ = nullptr;
    float *right_im_ = nullptr;
    std::size_t analysis_fill_ = 0;
    std::size_t synth_fill_ = 0;
    uint32_t history_map_[kHistoryFrames]{};
};

} // namespace iem
