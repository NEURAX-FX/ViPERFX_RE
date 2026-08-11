#include "iem/PartitionedMatrixConvolver.h"

#include "pffft.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace iem {
namespace {

bool IsPowerOfTwo(uint32_t value) noexcept {
    return value != 0 && (value & (value - 1U)) == 0;
}

bool CheckedMultiply(std::size_t left, std::size_t right,
    std::size_t &result) noexcept {
    if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) return false;
    result = left * right;
    return true;
}

float *AllocateFloats(std::size_t count) noexcept {
    std::size_t bytes = 0;
    if (!CheckedMultiply(count, sizeof(float), bytes)) return nullptr;
    return static_cast<float *>(pffft_aligned_malloc(bytes));
}

void ClearFloats(float *data, std::size_t count) noexcept {
    if (data != nullptr) std::memset(data, 0, count * sizeof(float));
}

} // namespace

PartitionedMatrixConvolver::~PartitionedMatrixConvolver() { Release(); }

PartitionedMatrixConvolver::PartitionedMatrixConvolver(
    PartitionedMatrixConvolver &&other) noexcept {
    Swap(other);
}

PartitionedMatrixConvolver &PartitionedMatrixConvolver::operator=(
    PartitionedMatrixConvolver &&other) noexcept {
    if (this == &other) return *this;
    Release();
    Swap(other);
    return *this;
}

bool PartitionedMatrixConvolver::Prepare(uint32_t input_channels,
    uint32_t ir_frames, uint32_t partition_frames,
    const float *output_input_frame_ir) noexcept {
    PartitionedMatrixConvolver candidate;
    if (!candidate.Build(input_channels, ir_frames, partition_frames,
            output_input_frame_ir)) return false;
    Swap(candidate);
    return true;
}

bool PartitionedMatrixConvolver::Build(uint32_t input_channels,
    uint32_t ir_frames, uint32_t partition_frames,
    const float *output_input_frame_ir) noexcept {
    if (input_channels == 0 || input_channels > 16 || ir_frames == 0
        || partition_frames < 64 || partition_frames > 4096
        || !IsPowerOfTwo(partition_frames) || output_input_frame_ir == nullptr) return false;
    input_channels_ = input_channels;
    ir_frames_ = ir_frames;
    partition_frames_ = partition_frames;
    fft_frames_ = partition_frames * 2U;
    segment_count_ = static_cast<uint32_t>((static_cast<uint64_t>(ir_frames)
        + partition_frames - 1U) / partition_frames);
    if (segment_count_ == 0) return false;

    std::size_t ir_count = 0;
    if (!CheckedMultiply(2U, input_channels_, ir_count)
        || !CheckedMultiply(ir_count, ir_frames_, ir_count)) return false;
    for (std::size_t index = 0; index < ir_count; ++index) {
        if (!std::isfinite(output_input_frame_ir[index])) return false;
    }

    std::size_t kernel_count = 0;
    std::size_t history_count = 0;
    std::size_t partition_count = 0;
    if (!CheckedMultiply(2U, input_channels_, kernel_count)
        || !CheckedMultiply(kernel_count, segment_count_, kernel_count)
        || !CheckedMultiply(kernel_count, fft_frames_, kernel_count)
        || !CheckedMultiply(input_channels_, segment_count_, history_count)
        || !CheckedMultiply(history_count, fft_frames_, history_count)
        || !CheckedMultiply(input_channels_, partition_frames_, partition_count)) return false;

    setup_ = pffft_new_setup(static_cast<int>(fft_frames_), PFFFT_REAL);
    if (setup_ == nullptr) return false;
    work_ = AllocateFloats(fft_frames_);
    kernel_spectra_ = AllocateFloats(kernel_count);
    input_history_ = AllocateFloats(history_count);
    overlap_ = AllocateFloats(partition_count);
    input_block_ = AllocateFloats(partition_count);
    fft_input_ = AllocateFloats(fft_frames_);
    accum_ = AllocateFloats(static_cast<std::size_t>(2U) * fft_frames_);
    output_block_ = AllocateFloats(static_cast<std::size_t>(2U) * partition_frames_);
    if (work_ == nullptr || kernel_spectra_ == nullptr || input_history_ == nullptr
        || overlap_ == nullptr || input_block_ == nullptr || fft_input_ == nullptr
        || accum_ == nullptr || output_block_ == nullptr) return false;
    ClearFloats(work_, fft_frames_);
    ClearFloats(kernel_spectra_, kernel_count);
    ClearFloats(input_history_, history_count);
    ClearFloats(overlap_, partition_count);
    ClearFloats(input_block_, partition_count);
    ClearFloats(fft_input_, fft_frames_);
    ClearFloats(accum_, static_cast<std::size_t>(2U) * fft_frames_);
    ClearFloats(output_block_, static_cast<std::size_t>(2U) * partition_frames_);

    for (uint32_t output = 0; output < 2; ++output) {
        for (uint32_t input = 0; input < input_channels_; ++input) {
            const std::size_t source_base = (static_cast<std::size_t>(output)
                * input_channels_ + input) * ir_frames_;
            for (uint32_t segment = 0; segment < segment_count_; ++segment) {
                ClearFloats(fft_input_, fft_frames_);
                const uint32_t offset = segment * partition_frames_;
                const uint32_t count = std::min(partition_frames_, ir_frames_ - offset);
                std::memcpy(fft_input_, output_input_frame_ir + source_base + offset,
                    static_cast<std::size_t>(count) * sizeof(float));
                pffft_transform(setup_, fft_input_,
                    kernel_spectra_ + KernelOffset(output, input, segment),
                    work_, PFFFT_FORWARD);
            }
        }
    }
    prepared_ = true;
    Reset();
    return true;
}

bool PartitionedMatrixConvolver::Process(const float *const *input,
    float *const output[2], std::size_t frames) noexcept {
    if (!prepared_ || input == nullptr || output == nullptr
        || output[0] == nullptr || output[1] == nullptr) return false;
    for (uint32_t channel = 0; channel < input_channels_; ++channel) {
        if (input[channel] == nullptr) return false;
    }
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (uint32_t channel = 0; channel < 2; ++channel) {
            output[channel][frame] = output_index_ < partition_frames_
                ? output_block_[static_cast<std::size_t>(channel) * partition_frames_
                    + output_index_] : 0.0F;
        }
        if (output_index_ < partition_frames_) ++output_index_;
        for (uint32_t channel = 0; channel < input_channels_; ++channel) {
            input_block_[static_cast<std::size_t>(channel) * partition_frames_ + input_fill_]
                = input[channel][frame];
        }
        if (++input_fill_ == partition_frames_) {
            ProcessBlock();
            input_fill_ = 0;
        }
    }
    return true;
}

void PartitionedMatrixConvolver::ProcessBlock() noexcept {
    for (uint32_t input = 0; input < input_channels_; ++input) {
        const std::size_t offset = static_cast<std::size_t>(input) * partition_frames_;
        std::memcpy(fft_input_, overlap_ + offset, partition_frames_ * sizeof(float));
        std::memcpy(fft_input_ + partition_frames_, input_block_ + offset,
            partition_frames_ * sizeof(float));
        std::memcpy(overlap_ + offset, input_block_ + offset,
            partition_frames_ * sizeof(float));
        pffft_transform(setup_, fft_input_,
            input_history_ + HistoryOffset(input, delay_line_index_),
            work_, PFFFT_FORWARD);
    }
    const float scale = 1.0F / static_cast<float>(fft_frames_);
    for (uint32_t output = 0; output < 2; ++output) {
        float *accumulator = accum_ + static_cast<std::size_t>(output) * fft_frames_;
        ClearFloats(accumulator, fft_frames_);
        for (uint32_t input = 0; input < input_channels_; ++input) {
            for (uint32_t segment = 0; segment < segment_count_; ++segment) {
                const uint32_t history_segment = (delay_line_index_ + segment_count_
                    - segment) % segment_count_;
                pffft_zconvolve_accumulate(setup_,
                    input_history_ + HistoryOffset(input, history_segment),
                    kernel_spectra_ + KernelOffset(output, input, segment),
                    accumulator, scale);
            }
        }
        pffft_transform(setup_, accumulator, accumulator, work_, PFFFT_BACKWARD);
        std::memcpy(output_block_ + static_cast<std::size_t>(output) * partition_frames_,
            accumulator + partition_frames_, partition_frames_ * sizeof(float));
    }
    delay_line_index_ = (delay_line_index_ + 1U) % segment_count_;
    output_index_ = 0;
}

void PartitionedMatrixConvolver::Reset() noexcept {
    if (!prepared_) return;
    ClearFloats(input_history_, static_cast<std::size_t>(input_channels_)
        * segment_count_ * fft_frames_);
    ClearFloats(overlap_, static_cast<std::size_t>(input_channels_) * partition_frames_);
    ClearFloats(input_block_, static_cast<std::size_t>(input_channels_) * partition_frames_);
    ClearFloats(fft_input_, fft_frames_);
    ClearFloats(accum_, static_cast<std::size_t>(2U) * fft_frames_);
    ClearFloats(output_block_, static_cast<std::size_t>(2U) * partition_frames_);
    ClearFloats(work_, fft_frames_);
    delay_line_index_ = 0;
    input_fill_ = 0;
    output_index_ = partition_frames_;
}

void PartitionedMatrixConvolver::Release() noexcept {
    prepared_ = false;
    pffft_aligned_free(work_);
    pffft_aligned_free(kernel_spectra_);
    pffft_aligned_free(input_history_);
    pffft_aligned_free(overlap_);
    pffft_aligned_free(input_block_);
    pffft_aligned_free(fft_input_);
    pffft_aligned_free(accum_);
    pffft_aligned_free(output_block_);
    work_ = kernel_spectra_ = input_history_ = overlap_ = nullptr;
    input_block_ = fft_input_ = accum_ = output_block_ = nullptr;
    if (setup_ != nullptr) pffft_destroy_setup(setup_);
    setup_ = nullptr;
    input_channels_ = ir_frames_ = partition_frames_ = fft_frames_ = 0;
    segment_count_ = delay_line_index_ = input_fill_ = output_index_ = 0;
}

void PartitionedMatrixConvolver::Swap(PartitionedMatrixConvolver &other) noexcept {
    using std::swap;
    swap(prepared_, other.prepared_);
    swap(input_channels_, other.input_channels_);
    swap(ir_frames_, other.ir_frames_);
    swap(partition_frames_, other.partition_frames_);
    swap(fft_frames_, other.fft_frames_);
    swap(segment_count_, other.segment_count_);
    swap(delay_line_index_, other.delay_line_index_);
    swap(input_fill_, other.input_fill_);
    swap(output_index_, other.output_index_);
    swap(setup_, other.setup_);
    swap(work_, other.work_);
    swap(kernel_spectra_, other.kernel_spectra_);
    swap(input_history_, other.input_history_);
    swap(overlap_, other.overlap_);
    swap(input_block_, other.input_block_);
    swap(fft_input_, other.fft_input_);
    swap(accum_, other.accum_);
    swap(output_block_, other.output_block_);
}

std::size_t PartitionedMatrixConvolver::KernelOffset(uint32_t output,
    uint32_t input, uint32_t segment) const noexcept {
    return ((static_cast<std::size_t>(output) * input_channels_ + input)
        * segment_count_ + segment) * fft_frames_;
}

std::size_t PartitionedMatrixConvolver::HistoryOffset(uint32_t input,
    uint32_t segment) const noexcept {
    return (static_cast<std::size_t>(input) * segment_count_ + segment) * fft_frames_;
}

} // namespace iem
