#pragma once

#include "iem/HaloDialogExtractor.h"
#include "iem/HaloStft.h"
#include "iem/IemParams.h"

#include <cstdint>

namespace iem {

enum class HaloBedChannel : uint32_t {
    L = 0,
    R = 1,
    C = 2,
    Ls = 3,
    Rs = 4,
    Lsr = 5,
    Rsr = 6,
    kCount = 7
};

class HaloSurroundAssigner {
public:
    bool Prepare() noexcept;
    void ApplyParams(const HaloParams &params) noexcept;
    void Reset() noexcept;
    void ProcessFrame(
        const HaloDialogFrame &in,
        float bed_re[7][HaloStft::kBins],
        float bed_im[7][HaloStft::kBins]
    ) noexcept;

private:
    struct AlphaAverager {
        float history[8]{};
        uint32_t filled = 0;
        uint32_t write = 0;

        void Reset() noexcept;
        float Push(float value) noexcept;
    };

    bool prepared_ = false;
    HaloParams params_{};
    float energy_prev_[HaloStft::kBins]{};
    float transient_[HaloStft::kBins]{};
    float c_prev_[HaloStft::kBins]{};
    float f_prev_[HaloStft::kBins]{};
    float b_prev_[HaloStft::kBins]{};
    float dialog_alpha_prev_ = 0.0F;
    AlphaAverager alpha_{};
};

} // namespace iem
