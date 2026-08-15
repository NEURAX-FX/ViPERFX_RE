#include "DspResources.h"
#include "DspGraph.h"
#include "viper/utils/Crc32.h"
#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>

namespace viper::audio {
namespace {

constexpr int kMaxConvolverSamples = 4 * 1024 * 1024;

void ClearPendingConvolver(
    std::vector<float> &buffer,
    size_t &size,
    uint32_t &channels
) {
    buffer.clear();
    size = 0;
    channels = 0;
}

CommittedDspResourceSnapshot CopyCommitted(
    const CommittedDspResourcePtr &committed
) {
    return committed != nullptr
        ? *committed
        : CommittedDspResourceSnapshot{};
}

CommittedDspResourcePtr PublishCommitted(
    CommittedDspResourceSnapshot snapshot
) {
    return std::make_shared<const CommittedDspResourceSnapshot>(
        std::move(snapshot));
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
                auto next = CopyCommitted(committed_);
                next.convolver_kernel.clear();
                next.convolver_channels = 0;
                next.convolver_kernel_id = 0;
                committed_ = PublishCommitted(std::move(next));
                return ResourceCaptureResult::CLEARED;
            }
            if ((val2 != 1 && val2 != 2 && val2 != 4)
                || val1 > kMaxConvolverSamples
                || val1 < 16 * val2 || val1 % val2 != 0) {
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
                && pending_convolver_size_ == pending_convolver_.size()
                && pending_convolver_channels_ != 0
                && pending_convolver_.size() % pending_convolver_channels_ == 0;
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
            auto next = CopyCommitted(committed_);
            next.convolver_kernel = pending_convolver_;
            next.convolver_channels = pending_convolver_channels_;
            next.convolver_kernel_id = val3;
            committed_ = PublishCommitted(std::move(next));
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
            auto next = CopyCommitted(committed_);
            next.ddc_44100.resize(section_count);
            next.ddc_48000.resize(section_count);
            for (size_t section = 0; section < section_count; ++section) {
                std::memcpy(
                    next.ddc_44100[section].data(),
                    coefficients + section * 5,
                    5 * sizeof(float)
                );
                std::memcpy(
                    next.ddc_48000[section].data(),
                    coefficients + arr_size + section * 5,
                    5 * sizeof(float)
                );
            }
            committed_ = PublishCommitted(std::move(next));
            return ResourceCaptureResult::COMMITTED;
        }

        default:
            return ResourceCaptureResult::NOT_RESOURCE;
    }
}

bool DspResources::ApplyTo(DspGraph &graph) const {
    if (HasDdcCoefficients()) {
        graph.Engine().LoadDdcCoefficients(
            committed_->ddc_44100.data(),
            committed_->ddc_48000.data(),
            static_cast<uint32_t>(committed_->ddc_44100.size())
        );
    }
    if (HasConvolverKernel()) {
        const size_t frames = committed_->convolver_kernel.size()
            / committed_->convolver_channels;
        graph.Engine().LoadConvolverKernel(
            committed_->convolver_kernel.data(),
            static_cast<int>(frames),
            static_cast<int>(committed_->convolver_channels),
            committed_->convolver_kernel_id
        );
        if (graph.Engine().GetConvolverKernelID()
            != committed_->convolver_kernel_id) return false;
    }
    return true;
}

bool DspResources::HasConvolverKernel() const noexcept {
    return committed_ != nullptr
        && !committed_->convolver_kernel.empty()
        && (committed_->convolver_channels == 1
            || committed_->convolver_channels == 2
            || committed_->convolver_channels == 4)
        && committed_->convolver_kernel.size()
            % committed_->convolver_channels == 0;
}

bool DspResources::HasDdcCoefficients() const noexcept {
    return committed_ != nullptr
        && !committed_->ddc_44100.empty()
        && committed_->ddc_44100.size() == committed_->ddc_48000.size();
}

CommittedDspResourcePtr DspResources::CommittedSnapshot() const noexcept {
    return committed_;
}

void DspResources::RestoreCommittedSnapshot(
    CommittedDspResourcePtr snapshot
) noexcept {
    committed_ = std::move(snapshot);
}

} // namespace viper::audio
