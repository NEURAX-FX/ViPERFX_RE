#ifndef VIPER_DSP_RESOURCES_H
#define VIPER_DSP_RESOURCES_H

#include "ViPERParams.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace viper::audio {

class DspGraph;

enum class ResourceCaptureResult {
    NOT_RESOURCE,
    UPDATED,
    COMMITTED,
    CLEARED,
    INVALID,
};

struct CommittedDspResourceSnapshot final {
    std::vector<float> convolver_kernel;
    uint32_t convolver_channels = 0;
    int convolver_kernel_id = 0;
    std::vector<viper::BiquadSection> ddc_44100;
    std::vector<viper::BiquadSection> ddc_48000;
};

using CommittedDspResourcePtr =
    std::shared_ptr<const CommittedDspResourceSnapshot>;

class DspResources final {
public:
    ResourceCaptureResult CaptureRaw(
        int param,
        int val1,
        int val2,
        int val3,
        uint32_t arr_size,
        const signed char *arr
    );

    bool ApplyTo(DspGraph &graph) const;
    bool HasConvolverKernel() const noexcept;
    bool HasDdcCoefficients() const noexcept;
    CommittedDspResourcePtr CommittedSnapshot() const noexcept;
    void RestoreCommittedSnapshot(CommittedDspResourcePtr snapshot) noexcept;

private:
    std::vector<float> pending_convolver_;
    size_t pending_convolver_size_ = 0;
    uint32_t pending_convolver_channels_ = 0;

    CommittedDspResourcePtr committed_{};
};

} // namespace viper::audio

#endif
