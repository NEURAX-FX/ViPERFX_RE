#include "DspResources.h"
#include "DspGraph.h"
#include "viper/utils/Crc32.h"
#include <algorithm>
#include <cstring>

namespace viper::audio {
namespace {

void ClearPendingConvolver(
    std::vector<float> &buffer,
    size_t &size,
    uint32_t &channels
) {
    buffer.clear();
    size = 0;
    channels = 0;
}

} // namespace

ResourceCaptureResult DspResources::CaptureRaw(
    int param,
    int val1,
    int val2,
    int val3,
    uint32_t arr_size,
    const signed char *arr
) {
    using namespace viper::params;
    switch (param) {
        case kParamConvolverPrepareBuffer:
            if (val3 != 0) {
                ClearPendingConvolver(
                    pending_convolver_,
                    pending_convolver_size_,
                    pending_convolver_channels_
                );
                convolver_kernel_.clear();
                convolver_channels_ = 0;
                convolver_kernel_id_ = 0;
                return ResourceCaptureResult::CLEARED;
            }
            if (val1 < 16 || (val2 != 1 && val2 != 2)) {
                ClearPendingConvolver(
                    pending_convolver_,
                    pending_convolver_size_,
                    pending_convolver_channels_
                );
                return ResourceCaptureResult::INVALID;
            }
            pending_convolver_.assign(static_cast<size_t>(val1), 0.0F);
            pending_convolver_size_ = 0;
            pending_convolver_channels_ = static_cast<uint32_t>(val2);
            return ResourceCaptureResult::UPDATED;

        case kParamConvolverSetBuffer: {
            if (arr == nullptr || arr_size == 0 || pending_convolver_.empty()) {
                return ResourceCaptureResult::INVALID;
            }
            const size_t remaining = pending_convolver_.size() - pending_convolver_size_;
            const size_t copy_size = std::min<size_t>(arr_size, remaining);
            std::memcpy(
                pending_convolver_.data() + pending_convolver_size_,
                arr,
                copy_size * sizeof(float)
            );
            pending_convolver_size_ += copy_size;
            return ResourceCaptureResult::UPDATED;
        }

        case kParamConvolverCommitBuffer: {
            const bool complete = val1 > 0
                && static_cast<size_t>(val1) == pending_convolver_.size()
                && pending_convolver_size_ == pending_convolver_.size();
            const uint32_t crc = complete
                ? Crc32(
                    reinterpret_cast<const uint8_t *>(pending_convolver_.data()),
                    static_cast<uint32_t>(pending_convolver_size_ * sizeof(float))
                )
                : 0;
            if (!complete || crc != static_cast<uint32_t>(val2)) {
                ClearPendingConvolver(
                    pending_convolver_,
                    pending_convolver_size_,
                    pending_convolver_channels_
                );
                return ResourceCaptureResult::INVALID;
            }
            convolver_kernel_ = pending_convolver_;
            convolver_channels_ = pending_convolver_channels_;
            convolver_kernel_id_ = val3;
            ClearPendingConvolver(
                pending_convolver_,
                pending_convolver_size_,
                pending_convolver_channels_
            );
            return ResourceCaptureResult::COMMITTED;
        }

        case kParamDdcCoefficients: {
            if (arr == nullptr || arr_size < 5 || arr_size > 80 || arr_size % 5 != 0) {
                return ResourceCaptureResult::INVALID;
            }
            const auto *coefficients = reinterpret_cast<const float *>(arr);
            const size_t section_count = arr_size / 5;
            ddc_44100_.resize(section_count);
            ddc_48000_.resize(section_count);
            for (size_t section = 0; section < section_count; ++section) {
                std::memcpy(
                    ddc_44100_[section].data(),
                    coefficients + section * 5,
                    5 * sizeof(float)
                );
                std::memcpy(
                    ddc_48000_[section].data(),
                    coefficients + arr_size + section * 5,
                    5 * sizeof(float)
                );
            }
            return ResourceCaptureResult::COMMITTED;
        }

        default:
            return ResourceCaptureResult::NOT_RESOURCE;
    }
}

bool DspResources::ApplyTo(DspGraph &graph) const {
    if (HasDdcCoefficients()) {
        graph.Engine().LoadDdcCoefficients(
            ddc_44100_.data(),
            ddc_48000_.data(),
            static_cast<uint32_t>(ddc_44100_.size())
        );
    }
    if (HasConvolverKernel()) {
        const size_t frames = convolver_kernel_.size() / convolver_channels_;
        graph.Engine().LoadConvolverKernel(
            convolver_kernel_.data(),
            static_cast<int>(frames),
            static_cast<int>(convolver_channels_),
            convolver_kernel_id_
        );
        if (graph.Engine().GetConvolverKernelID() != convolver_kernel_id_) return false;
    }
    return true;
}

bool DspResources::HasConvolverKernel() const noexcept {
    return !convolver_kernel_.empty()
        && (convolver_channels_ == 1 || convolver_channels_ == 2)
        && convolver_kernel_.size() % convolver_channels_ == 0;
}

bool DspResources::HasDdcCoefficients() const noexcept {
    return !ddc_44100_.empty() && ddc_44100_.size() == ddc_48000_.size();
}

} // namespace viper::audio
