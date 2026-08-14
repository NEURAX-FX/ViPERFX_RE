#pragma once

#include "iem/AlignedPlanarBuffer.h"
#include "iem/HaloBed.h"
#include "iem/HaloDialogExtractor.h"
#include "iem/HaloDiffusion.h"
#include "iem/HaloLfeSynth.h"
#include "iem/HaloStft.h"
#include "iem/HaloSurroundAssigner.h"
#include "iem/IemEncoder.h"

#include <cstddef>
#include <cstdint>

namespace iem {

class HaloEncoder {
public:
    bool Prepare(const EncoderConfig &config) noexcept;
    void ApplyParams(const IemParams &params) noexcept;
    void Reset() noexcept;
    bool ProcessBed(
        const float *const stereo[2],
        float *const bed[kHaloBedChannels],
        std::size_t frames
    ) noexcept;
    bool ProcessBed(
        const float *const stereo[2],
        HaloBedView bed,
        std::size_t frames
    ) noexcept;
    uint32_t StftLatencyFrames() const noexcept {
        return HaloStft::kReportedLatency + HaloStft::kHop;
    }
    bool Prepared() const noexcept { return prepared_; }
    std::size_t PreparedBytes() const noexcept;

private:
    static void OnAnalysisFrame(
        const float left_re[HaloStft::kBins],
        const float left_im[HaloStft::kBins],
        const float right_re[HaloStft::kBins],
        const float right_im[HaloStft::kBins],
        void *user
    ) noexcept;
    void RenderFrame(
        const float left_re[HaloStft::kBins],
        const float left_im[HaloStft::kBins],
        const float right_re[HaloStft::kBins],
        const float right_im[HaloStft::kBins]
    ) noexcept;

    EncoderConfig config_{};
    bool prepared_ = false;
    HaloStft analysis_{};
    HaloStft synthesis_[kHaloBedChannels]{};
    HaloDialogExtractor dialog_{};
    HaloSurroundAssigner surround_{};
    HaloDiffusion diffusion_{};
    HaloLfeSynth lfe_{};
    AlignedPlanarBuffer frame_time_{};
    AlignedPlanarBuffer lfe_buffer_{};
    AlignedPlanarBuffer output_ring_{};
    float bed_re_[kHaloBedChannels][HaloStft::kBins]{};
    float bed_im_[kHaloBedChannels][HaloStft::kBins]{};
    std::size_t ring_frames_ = 0;
    uint64_t read_frame_ = 0;
    uint64_t synthesis_frame_ = HaloStft::kReportedLatency;
};

} // namespace iem
