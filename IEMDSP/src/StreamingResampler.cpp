#include "iem/StreamingResampler.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>

namespace iem {
namespace {

constexpr std::size_t kAlignmentBytes = 64;
constexpr double kPi = 3.1415926535897932384626433832795;
constexpr double kKaiserBeta = 8.6;

double BesselI0(double value) noexcept {
    const double x = std::fabs(value);
    if (x < 3.75) {
        const double y = value / 3.75;
        const double y2 = y * y;
        return 1.0
            + y2 * (3.5156229
                    + y2 * (3.0899424
                            + y2 * (1.2067492
                                    + y2 * (0.2659732
                                            + y2 * (0.0360768
                                                    + y2 * 0.0045813)))));
    }
    const double y = 3.75 / x;
    return std::exp(x) / std::sqrt(x)
        * (0.39894228
           + y * (0.01328592
                  + y * (0.00225319
                         + y * (-0.00157565
                                + y * (0.00916281
                                       + y * (-0.02057706
                                              + y * (0.02635537
                                                     + y * (-0.01647633
                                                            + y * 0.00392377))))))));
}

double Sinc(double value) noexcept {
    if (std::fabs(value) < 1.0e-12) return 1.0;
    const double angle = kPi * value;
    return std::sin(angle) / angle;
}

} // namespace

StreamingResampler::~StreamingResampler() {
    ReleaseCoefficients();
}

StreamingResampler::StreamingResampler(StreamingResampler &&other) noexcept
    : input_ring_(std::move(other.input_ring_)),
      coefficients_(other.coefficients_),
      input_rate_(other.input_rate_),
      output_rate_(other.output_rate_),
      channels_(other.channels_),
      max_input_frames_(other.max_input_frames_),
      ring_capacity_(other.ring_capacity_),
      ring_start_slot_(other.ring_start_slot_),
      ring_count_(other.ring_count_),
      ring_start_index_(other.ring_start_index_),
      total_input_frames_(other.total_input_frames_),
      next_source_numerator_(other.next_source_numerator_),
      failed_(other.failed_) {
    other.coefficients_ = nullptr;
    other.input_rate_ = 0;
    other.output_rate_ = 0;
    other.channels_ = 0;
    other.max_input_frames_ = 0;
    other.ring_capacity_ = 0;
    other.ring_start_slot_ = 0;
    other.ring_count_ = 0;
    other.ring_start_index_ = 0;
    other.total_input_frames_ = 0;
    other.next_source_numerator_ = 0;
    other.failed_ = false;
}

StreamingResampler &StreamingResampler::operator=(StreamingResampler &&other) noexcept {
    if (this == &other) return *this;
    ReleaseCoefficients();
    input_ring_ = std::move(other.input_ring_);
    coefficients_ = other.coefficients_;
    input_rate_ = other.input_rate_;
    output_rate_ = other.output_rate_;
    channels_ = other.channels_;
    max_input_frames_ = other.max_input_frames_;
    ring_capacity_ = other.ring_capacity_;
    ring_start_slot_ = other.ring_start_slot_;
    ring_count_ = other.ring_count_;
    ring_start_index_ = other.ring_start_index_;
    total_input_frames_ = other.total_input_frames_;
    next_source_numerator_ = other.next_source_numerator_;
    failed_ = other.failed_;
    other.coefficients_ = nullptr;
    other.input_rate_ = 0;
    other.output_rate_ = 0;
    other.channels_ = 0;
    other.max_input_frames_ = 0;
    other.ring_capacity_ = 0;
    other.ring_start_slot_ = 0;
    other.ring_count_ = 0;
    other.ring_start_index_ = 0;
    other.total_input_frames_ = 0;
    other.next_source_numerator_ = 0;
    other.failed_ = false;
    return *this;
}

bool StreamingResampler::Prepare(
    uint32_t input_rate,
    uint32_t output_rate,
    uint32_t channels,
    std::size_t max_input_frames
) noexcept {
    if (input_rate == 0 || output_rate == 0 || channels == 0
        || channels > AlignedPlanarBuffer::kMaxChannels || max_input_frames == 0) {
        return false;
    }
    if (max_input_frames > std::numeric_limits<std::size_t>::max() - kTapCount * 2U) {
        return false;
    }

    AlignedPlanarBuffer replacement_ring;
    const std::size_t replacement_capacity = max_input_frames + kTapCount * 2U;
    if (!replacement_ring.Prepare(channels, replacement_capacity)) return false;

    if (kPhaseCount > std::numeric_limits<std::size_t>::max() / kTapCount) return false;
    const std::size_t coefficient_count = static_cast<std::size_t>(kPhaseCount) * kTapCount;
    if (coefficient_count > std::numeric_limits<std::size_t>::max() / sizeof(float)) return false;
    void *raw_coefficients = nullptr;
    if (posix_memalign(
            &raw_coefficients,
            kAlignmentBytes,
            coefficient_count * sizeof(float)
        ) != 0 || raw_coefficients == nullptr) {
        return false;
    }

    ReleaseCoefficients();
    input_ring_ = std::move(replacement_ring);
    coefficients_ = static_cast<float *>(raw_coefficients);
    input_rate_ = input_rate;
    output_rate_ = output_rate;
    channels_ = channels;
    max_input_frames_ = max_input_frames;
    ring_capacity_ = replacement_capacity;
    if (!BuildCoefficients()) {
        ReleaseCoefficients();
        input_ring_ = AlignedPlanarBuffer{};
        input_rate_ = 0;
        output_rate_ = 0;
        channels_ = 0;
        max_input_frames_ = 0;
        ring_capacity_ = 0;
        return false;
    }
    Reset();
    return true;
}

std::size_t StreamingResampler::Process(
    const float *const *input,
    std::size_t input_frames,
    float *const *output,
    std::size_t output_capacity
) noexcept {
    if (!IsPrepared() || failed_ || input == nullptr || output == nullptr
        || input_frames > max_input_frames_) {
        failed_ = IsPrepared();
        return 0;
    }
    for (uint32_t channel = 0; channel < channels_; ++channel) {
        if (input[channel] == nullptr || output[channel] == nullptr) {
            failed_ = true;
            return 0;
        }
        for (std::size_t frame = 0; frame < input_frames; ++frame) {
            if (!std::isfinite(input[channel][frame])) {
                failed_ = true;
                return 0;
            }
        }
    }

    DiscardConsumedInput();
    if (input_frames > ring_capacity_ - ring_count_) {
        failed_ = true;
        return 0;
    }

    for (std::size_t frame = 0; frame < input_frames; ++frame) {
        const std::size_t slot = (ring_start_slot_ + ring_count_ + frame) % ring_capacity_;
        for (uint32_t channel = 0; channel < channels_; ++channel) {
            input_ring_.ChannelData(channel)[slot] = input[channel][frame];
        }
    }
    ring_count_ += input_frames;
    total_input_frames_ += input_frames;

    std::size_t produced = 0;
    while (produced < output_capacity) {
        const uint64_t center_index = next_source_numerator_ / output_rate_;
        if (center_index > std::numeric_limits<uint64_t>::max() - kRightTaps) break;
        const uint64_t rightmost_index = center_index + kRightTaps;
        if (rightmost_index >= total_input_frames_) break;

        const uint64_t remainder = next_source_numerator_ % output_rate_;
        const uint32_t phase = static_cast<uint32_t>(
            (remainder * kPhaseCount) / output_rate_
        );
        const float *phase_coefficients =
            coefficients_ + static_cast<std::size_t>(phase) * kTapCount;

        for (uint32_t channel = 0; channel < channels_; ++channel) {
            double sum = 0.0;
            for (uint32_t tap = 0; tap < kTapCount; ++tap) {
                const int64_t sample_index = static_cast<int64_t>(center_index)
                    + static_cast<int64_t>(tap) - kLeftTaps;
                sum += static_cast<double>(InputSample(channel, sample_index))
                    * phase_coefficients[tap];
            }
            float sample = static_cast<float>(sum);
            if (std::fpclassify(sample) == FP_SUBNORMAL) sample = 0.0F;
            output[channel][produced] = sample;
        }
        ++produced;
        if (next_source_numerator_ > std::numeric_limits<uint64_t>::max() - input_rate_) {
            failed_ = true;
            return produced;
        }
        next_source_numerator_ += input_rate_;
    }

    DiscardConsumedInput();
    return produced;
}

std::size_t StreamingResampler::MaxOutputFrames(std::size_t input_frames) const noexcept {
    if (input_rate_ == 0 || output_rate_ == 0) return 0;
    if (input_frames > std::numeric_limits<std::size_t>::max() - kTapCount * 2U) {
        return std::numeric_limits<std::size_t>::max();
    }
    const uint64_t available = static_cast<uint64_t>(input_frames + kTapCount * 2U);
    if (available > std::numeric_limits<uint64_t>::max() / output_rate_) {
        return std::numeric_limits<std::size_t>::max();
    }
    const uint64_t numerator = available * output_rate_;
    const uint64_t result = numerator / input_rate_ + 2U;
    return result > std::numeric_limits<std::size_t>::max()
        ? std::numeric_limits<std::size_t>::max()
        : static_cast<std::size_t>(result);
}

uint32_t StreamingResampler::LatencyInputFrames() const noexcept {
    return kRightTaps;
}

void StreamingResampler::Reset() noexcept {
    input_ring_.Clear();
    ring_start_slot_ = 0;
    ring_count_ = 0;
    ring_start_index_ = 0;
    total_input_frames_ = 0;
    next_source_numerator_ = 0;
    failed_ = false;
}

bool StreamingResampler::IsPrepared() const noexcept {
    return coefficients_ != nullptr && input_ring_.IsPrepared()
        && input_rate_ != 0 && output_rate_ != 0 && channels_ != 0;
}

bool StreamingResampler::BuildCoefficients() noexcept {
    if (coefficients_ == nullptr) return false;
    const double ratio = static_cast<double>(output_rate_) / input_rate_;
    const double cutoff = 0.475 * std::min(1.0, ratio);
    const double denominator = BesselI0(kKaiserBeta);

    for (uint32_t phase = 0; phase < kPhaseCount; ++phase) {
        const double fraction = static_cast<double>(phase) / kPhaseCount;
        double sum = 0.0;
        float *phase_coefficients =
            coefficients_ + static_cast<std::size_t>(phase) * kTapCount;
        for (uint32_t tap = 0; tap < kTapCount; ++tap) {
            const double window_position =
                (2.0 * tap) / static_cast<double>(kTapCount - 1) - 1.0;
            const double window = BesselI0(
                kKaiserBeta * std::sqrt(std::max(0.0, 1.0 - window_position * window_position))
            ) / denominator;
            const double distance = static_cast<double>(tap)
                - kLeftTaps - fraction;
            const double coefficient = 2.0 * cutoff
                * Sinc(2.0 * cutoff * distance) * window;
            phase_coefficients[tap] = static_cast<float>(coefficient);
            sum += coefficient;
        }
        if (!std::isfinite(sum) || std::fabs(sum) < 1.0e-12) return false;
        const float normalization = static_cast<float>(1.0 / sum);
        for (uint32_t tap = 0; tap < kTapCount; ++tap) {
            phase_coefficients[tap] *= normalization;
        }
    }
    return true;
}

void StreamingResampler::ReleaseCoefficients() noexcept {
    std::free(coefficients_);
    coefficients_ = nullptr;
}

void StreamingResampler::DiscardConsumedInput() noexcept {
    if (output_rate_ == 0 || ring_count_ == 0) return;
    const uint64_t next_center = next_source_numerator_ / output_rate_;
    const uint64_t keep_from = next_center > static_cast<uint64_t>(kLeftTaps + 1)
        ? next_center - static_cast<uint64_t>(kLeftTaps + 1)
        : 0;
    if (keep_from <= ring_start_index_) return;
    const uint64_t discard64 = std::min<uint64_t>(
        keep_from - ring_start_index_, ring_count_
    );
    const std::size_t discard = static_cast<std::size_t>(discard64);
    ring_start_slot_ = (ring_start_slot_ + discard) % ring_capacity_;
    ring_start_index_ += discard;
    ring_count_ -= discard;
}

float StreamingResampler::InputSample(
    uint32_t channel,
    int64_t absolute_index
) const noexcept {
    if (absolute_index < 0 || channel >= channels_) return 0.0F;
    const uint64_t index = static_cast<uint64_t>(absolute_index);
    if (index < ring_start_index_ || index >= ring_start_index_ + ring_count_) {
        return 0.0F;
    }
    const std::size_t offset = static_cast<std::size_t>(index - ring_start_index_);
    const std::size_t slot = (ring_start_slot_ + offset) % ring_capacity_;
    return input_ring_.ChannelData(channel)[slot];
}

} // namespace iem
