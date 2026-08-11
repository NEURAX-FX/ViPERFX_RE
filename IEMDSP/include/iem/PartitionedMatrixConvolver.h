#pragma once

#include <cstddef>
#include <cstdint>

typedef struct PFFFT_Setup PFFFT_Setup;

namespace iem {

class PartitionedMatrixConvolver {
public:
    PartitionedMatrixConvolver() = default;
    ~PartitionedMatrixConvolver();
    PartitionedMatrixConvolver(const PartitionedMatrixConvolver &) = delete;
    PartitionedMatrixConvolver &operator=(const PartitionedMatrixConvolver &) = delete;
    PartitionedMatrixConvolver(PartitionedMatrixConvolver &&other) noexcept;
    PartitionedMatrixConvolver &operator=(PartitionedMatrixConvolver &&other) noexcept;

    bool Prepare(uint32_t input_channels, uint32_t ir_frames,
        uint32_t partition_frames, const float *output_input_frame_ir) noexcept;
    bool Process(const float *const *input, float *const output[2],
        std::size_t frames) noexcept;
    void Reset() noexcept;

    bool IsPrepared() const noexcept { return prepared_; }
    uint32_t InputChannels() const noexcept { return input_channels_; }
    uint32_t LatencyFrames() const noexcept { return partition_frames_; }

private:
    bool Build(uint32_t input_channels, uint32_t ir_frames,
        uint32_t partition_frames, const float *output_input_frame_ir) noexcept;
    void ProcessBlock() noexcept;
    void Release() noexcept;
    void Swap(PartitionedMatrixConvolver &other) noexcept;
    std::size_t KernelOffset(uint32_t output, uint32_t input,
        uint32_t segment) const noexcept;
    std::size_t HistoryOffset(uint32_t input, uint32_t segment) const noexcept;

    bool prepared_ = false;
    uint32_t input_channels_ = 0;
    uint32_t ir_frames_ = 0;
    uint32_t partition_frames_ = 0;
    uint32_t fft_frames_ = 0;
    uint32_t segment_count_ = 0;
    uint32_t delay_line_index_ = 0;
    uint32_t input_fill_ = 0;
    uint32_t output_index_ = 0;
    PFFFT_Setup *setup_ = nullptr;
    float *work_ = nullptr;
    float *kernel_spectra_ = nullptr;
    float *input_history_ = nullptr;
    float *overlap_ = nullptr;
    float *input_block_ = nullptr;
    float *fft_input_ = nullptr;
    float *accum_ = nullptr;
    float *output_block_ = nullptr;
};

} // namespace iem
