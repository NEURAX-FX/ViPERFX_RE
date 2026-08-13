#include "iem/HaloStft.h"

#include "pffft.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace iem {
namespace {

constexpr float kPi = 3.14159265358979323846F;

float *AllocateFloats(std::size_t count) noexcept {
    return static_cast<float *>(pffft_aligned_malloc(count * sizeof(float)));
}

void ClearFloats(float *data, std::size_t count) noexcept {
    if (data != nullptr) std::memset(data, 0, count * sizeof(float));
}

} // namespace

HaloStft::~HaloStft() {
    Release();
}

void HaloStft::Release() noexcept {
    if (work_ != nullptr) pffft_aligned_free(work_);
    if (fft_in_ != nullptr) pffft_aligned_free(fft_in_);
    if (fft_out_ != nullptr) pffft_aligned_free(fft_out_);
    if (window_ != nullptr) pffft_aligned_free(window_);
    if (analysis_left_ != nullptr) pffft_aligned_free(analysis_left_);
    if (analysis_right_ != nullptr) pffft_aligned_free(analysis_right_);
    if (synth_ != nullptr) pffft_aligned_free(synth_);
    if (left_re_ != nullptr) pffft_aligned_free(left_re_);
    if (left_im_ != nullptr) pffft_aligned_free(left_im_);
    if (right_re_ != nullptr) pffft_aligned_free(right_re_);
    if (right_im_ != nullptr) pffft_aligned_free(right_im_);
    if (setup_ != nullptr) pffft_destroy_setup(setup_);
    work_ = fft_in_ = fft_out_ = window_ = nullptr;
    analysis_left_ = analysis_right_ = synth_ = nullptr;
    left_re_ = left_im_ = right_re_ = right_im_ = nullptr;
    setup_ = nullptr;
    prepared_ = false;
    analysis_fill_ = 0;
    synth_fill_ = 0;
    synth_frames_ = 0;
}

bool HaloStft::Prepare(std::size_t max_frames) noexcept {
    Release();
    if (max_frames == 0) return false;
    setup_ = pffft_new_setup(static_cast<int>(kFftSize), PFFFT_REAL);
    work_ = AllocateFloats(kFftSize);
    fft_in_ = AllocateFloats(kFftSize);
    fft_out_ = AllocateFloats(kFftSize);
    window_ = AllocateFloats(kFftSize * 2U);
    analysis_left_ = AllocateFloats(kFftSize);
    analysis_right_ = AllocateFloats(kFftSize);
    synth_ = AllocateFloats(kFftSize + max_frames);
    left_re_ = AllocateFloats(kBins);
    left_im_ = AllocateFloats(kBins);
    right_re_ = AllocateFloats(kBins);
    right_im_ = AllocateFloats(kBins);
    if (setup_ == nullptr || work_ == nullptr || fft_in_ == nullptr || fft_out_ == nullptr
        || window_ == nullptr || analysis_left_ == nullptr || analysis_right_ == nullptr
        || synth_ == nullptr || left_re_ == nullptr || left_im_ == nullptr
        || right_re_ == nullptr || right_im_ == nullptr) {
        Release();
        return false;
    }
    for (uint32_t index = 0; index < kFftSize; ++index) {
        window_[index] = 0.5F - 0.5F * std::cos(
            2.0F * kPi * static_cast<float>(index) / static_cast<float>(kFftSize));
        window_[index + kFftSize] = window_[index];
    }
    ClearFloats(analysis_left_, kFftSize);
    ClearFloats(analysis_right_, kFftSize);
    ClearFloats(synth_, kFftSize + max_frames);
    for (uint32_t index = 0; index < kHistoryFrames; ++index) {
        history_map_[index] = index;
    }
    analysis_fill_ = 0;
    synth_fill_ = 0;
    synth_frames_ = kFftSize + max_frames;
    prepared_ = true;
    return true;
}

void HaloStft::Reset() noexcept {
    if (!prepared_) return;
    ClearFloats(analysis_left_, kFftSize);
    ClearFloats(analysis_right_, kFftSize);
    ClearFloats(synth_, synth_frames_);
    ClearFloats(left_re_, kBins);
    ClearFloats(left_im_, kBins);
    ClearFloats(right_re_, kBins);
    ClearFloats(right_im_, kBins);
    analysis_fill_ = 0;
    synth_fill_ = 0;
    for (uint32_t index = 0; index < kHistoryFrames; ++index) {
        history_map_[index] = index;
    }
}

void HaloStft::AnalyzeChannel(const float *time, float *re, float *im) noexcept {
    std::copy(time, time + kFftSize, fft_in_);
    pffft_transform_ordered(setup_, fft_in_, fft_out_, work_, PFFFT_FORWARD);
    re[0] = fft_out_[0];
    im[0] = 0.0F;
    for (uint32_t bin = 1; bin < kBins - 1U; ++bin) {
        re[bin] = fft_out_[2U * bin];
        im[bin] = fft_out_[2U * bin + 1U];
    }
    re[kBins - 1U] = fft_out_[1];
    im[kBins - 1U] = 0.0F;
}

bool HaloStft::InverseOne(const float re[kBins], const float im[kBins], float *ola) noexcept {
    fft_in_[0] = re[0];
    fft_in_[1] = re[kBins - 1U];
    for (uint32_t bin = 1; bin < kBins - 1U; ++bin) {
        fft_in_[2U * bin] = re[bin];
        fft_in_[2U * bin + 1U] = im[bin];
    }
    pffft_transform_ordered(setup_, fft_in_, fft_out_, work_, PFFFT_BACKWARD);
    const float scale = 1.0F / static_cast<float>(kFftSize);
    for (uint32_t index = 0; index < kFftSize; ++index) {
        ola[index] += fft_out_[index] * scale * window_[index];
    }
    return true;
}

bool HaloStft::Process(
    const float *left,
    const float *right,
    std::size_t frames,
    FrameCallback on_frame,
    void *user
) noexcept {
    if (!prepared_ || left == nullptr || right == nullptr) return false;
    std::size_t consumed = 0;
    while (consumed < frames) {
        const std::size_t room = kFftSize - analysis_fill_;
        const std::size_t take = std::min(room, frames - consumed);
        std::copy(left + consumed, left + consumed + take, analysis_left_ + analysis_fill_);
        std::copy(right + consumed, right + consumed + take, analysis_right_ + analysis_fill_);
        analysis_fill_ += take;
        consumed += take;
        if (analysis_fill_ < kFftSize) break;
        AnalyzeChannel(analysis_left_, left_re_, left_im_);
        AnalyzeChannel(analysis_right_, right_re_, right_im_);
        if (on_frame != nullptr) {
            on_frame(left_re_, left_im_, right_re_, right_im_, user);
        }
        const uint32_t first = history_map_[0];
        for (uint32_t index = 0; index + 1U < kHistoryFrames; ++index) {
            history_map_[index] = history_map_[index + 1U];
        }
        history_map_[kHistoryFrames - 1U] = first;
        std::copy(analysis_left_ + kHop, analysis_left_ + kFftSize, analysis_left_);
        std::copy(analysis_right_ + kHop, analysis_right_ + kFftSize, analysis_right_);
        analysis_fill_ = kHop;
    }
    return true;
}

bool HaloStft::InverseAdd(
    const float re[kBins],
    const float im[kBins],
    float *dst,
    std::size_t dst_frames
) noexcept {
    if (!prepared_ || re == nullptr || im == nullptr || dst == nullptr) return false;
    if (dst_frames < kReportedLatency) return false;
    ClearFloats(synth_, dst_frames);
    if (!InverseOne(re, im, synth_)) return false;
    std::copy(synth_, synth_ + dst_frames, dst);
    return true;
}

} // namespace iem
