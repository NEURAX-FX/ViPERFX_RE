#pragma once

#include "iem/FannDialogNet.h"
#include "iem/HaloStft.h"
#include "iem/IemParams.h"

#include <cstddef>
#include <cstdint>

namespace iem {

struct HaloDialogFeatureFrame {
    float a[516]{};
    float d[516]{};
};

struct HaloDialogFrame {
    float centre_re[HaloStft::kBins]{};
    float centre_im[HaloStft::kBins]{};
    float residual_l_re[HaloStft::kBins]{};
    float residual_l_im[HaloStft::kBins]{};
    float residual_r_re[HaloStft::kBins]{};
    float residual_r_im[HaloStft::kBins]{};
};

void ComputeHaloDialogAd(
    const float left_re[HaloStft::kBins],
    const float left_im[HaloStft::kBins],
    const float right_re[HaloStft::kBins],
    const float right_im[HaloStft::kBins],
    HaloDialogFeatureFrame &out
) noexcept;

void BuildHaloDialogFeatures(
    const HaloDialogFeatureFrame slots[HaloStft::kHistoryFrames],
    uint32_t bin,
    float features[kDialogNetInputs]
) noexcept;

class HaloDialogExtractor {
public:
    bool Prepare() noexcept;
    void ApplyParams(const HaloParams &params) noexcept;
    void Reset() noexcept;
    void ProcessFrame(
        const float left_re[HaloStft::kBins],
        const float left_im[HaloStft::kBins],
        const float right_re[HaloStft::kBins],
        const float right_im[HaloStft::kBins],
        HaloDialogFrame &out
    ) noexcept;

private:
    void RotateHistory() noexcept;

    bool prepared_ = false;
    FannDialogNet net_{};
    HaloParams params_{};
    HaloDialogFeatureFrame slots_[HaloStft::kHistoryFrames]{};
    uint32_t map_[HaloStft::kHistoryFrames]{};
    float envelope_[HaloStft::kBins]{};
    float left_history_re_[HaloStft::kBins]{};
    float left_history_im_[HaloStft::kBins]{};
    float right_history_re_[HaloStft::kBins]{};
    float right_history_im_[HaloStft::kBins]{};
    bool have_target_ = false;
};

} // namespace iem
