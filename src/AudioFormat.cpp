#include "AudioFormat.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace viper::audio {
namespace {

constexpr float kInt16Scale = 32768.0F;
constexpr float kInt24Scale = 8388608.0F;
constexpr float kInt32Scale = 2147483648.0F;

float SanitizeAndClamp(float value) {
    if (!std::isfinite(value)) return 0.0F;
    return std::clamp(value, -1.0F, 1.0F);
}

int32_t DecodePacked24(const uint8_t *source) {
    uint32_t value = static_cast<uint32_t>(source[0])
        | (static_cast<uint32_t>(source[1]) << 8U)
        | (static_cast<uint32_t>(source[2]) << 16U);
    if ((value & 0x00800000U) != 0) value |= 0xFF000000U;
    return static_cast<int32_t>(value);
}

void EncodePacked24(uint8_t *destination, int32_t value) {
    const uint32_t bits = static_cast<uint32_t>(value) & 0x00FFFFFFU;
    destination[0] = static_cast<uint8_t>(bits);
    destination[1] = static_cast<uint8_t>(bits >> 8U);
    destination[2] = static_cast<uint8_t>(bits >> 16U);
}

int64_t QuantizeSigned(float value, unsigned bits) {
    const int64_t scale = int64_t{1} << (bits - 1U);
    const int64_t minimum = -scale;
    const int64_t maximum = scale - 1;
    const float normalized = SanitizeAndClamp(value);
    if (normalized <= -1.0F) return minimum;
    if (normalized >= 1.0F) return maximum;
    return std::clamp(
        static_cast<int64_t>(std::llround(static_cast<double>(normalized) * scale)),
        minimum,
        maximum
    );
}

bool ValidPointers(const void *destination, const void *source, size_t frame_count) {
    if (destination == nullptr || source == nullptr) return false;
    return frame_count <= std::numeric_limits<size_t>::max() / kChannelCount;
}

} // namespace

bool IsSupportedSampleRate(uint32_t sample_rate) {
    return sample_rate >= kMinSampleRate && sample_rate <= kMaxSampleRate;
}

bool IsSupportedPcmFormat(audio_format_t format) {
    switch (format) {
        case AUDIO_FORMAT_PCM_16_BIT:
        case AUDIO_FORMAT_PCM_8_24_BIT:
        case AUDIO_FORMAT_PCM_24_BIT_PACKED:
        case AUDIO_FORMAT_PCM_32_BIT:
        case AUDIO_FORMAT_PCM_FLOAT:
            return true;
        default:
            return false;
    }
}

size_t BytesPerSample(audio_format_t format) {
    switch (format) {
        case AUDIO_FORMAT_PCM_16_BIT:
            return sizeof(int16_t);
        case AUDIO_FORMAT_PCM_24_BIT_PACKED:
            return 3;
        case AUDIO_FORMAT_PCM_8_24_BIT:
        case AUDIO_FORMAT_PCM_32_BIT:
        case AUDIO_FORMAT_PCM_FLOAT:
            return sizeof(int32_t);
        default:
            return 0;
    }
}

bool ToFloat(
    float *destination,
    const void *source,
    size_t frame_count,
    audio_format_t format
) {
    if (!ValidPointers(destination, source, frame_count) || !IsSupportedPcmFormat(format)) {
        return false;
    }
    const size_t sample_count = frame_count * kChannelCount;
    switch (format) {
        case AUDIO_FORMAT_PCM_16_BIT: {
            const auto *input = static_cast<const int16_t *>(source);
            for (size_t i = 0; i < sample_count; ++i) {
                destination[i] = static_cast<float>(input[i]) / kInt16Scale;
            }
            return true;
        }
        case AUDIO_FORMAT_PCM_8_24_BIT: {
            const auto *input = static_cast<const int32_t *>(source);
            for (size_t i = 0; i < sample_count; ++i) {
                destination[i] = static_cast<float>(input[i]) / kInt32Scale;
            }
            return true;
        }
        case AUDIO_FORMAT_PCM_24_BIT_PACKED: {
            const auto *input = static_cast<const uint8_t *>(source);
            for (size_t i = 0; i < sample_count; ++i) {
                destination[i] =
                    static_cast<float>(DecodePacked24(input + i * 3)) / kInt24Scale;
            }
            return true;
        }
        case AUDIO_FORMAT_PCM_32_BIT: {
            const auto *input = static_cast<const int32_t *>(source);
            for (size_t i = 0; i < sample_count; ++i) {
                destination[i] = static_cast<float>(input[i]) / kInt32Scale;
            }
            return true;
        }
        case AUDIO_FORMAT_PCM_FLOAT: {
            const auto *input = static_cast<const float *>(source);
            for (size_t i = 0; i < sample_count; ++i) {
                destination[i] = SanitizeAndClamp(input[i]);
            }
            return true;
        }
        default:
            return false;
    }
}

bool FromFloat(
    void *destination,
    const float *source,
    size_t frame_count,
    audio_format_t format,
    bool accumulate
) {
    if (!ValidPointers(destination, source, frame_count) || !IsSupportedPcmFormat(format)) {
        return false;
    }
    const size_t sample_count = frame_count * kChannelCount;
    switch (format) {
        case AUDIO_FORMAT_PCM_16_BIT: {
            auto *output = static_cast<int16_t *>(destination);
            for (size_t i = 0; i < sample_count; ++i) {
                float value = SanitizeAndClamp(source[i]);
                if (accumulate) value += static_cast<float>(output[i]) / kInt16Scale;
                output[i] = static_cast<int16_t>(QuantizeSigned(value, 16));
            }
            return true;
        }
        case AUDIO_FORMAT_PCM_8_24_BIT: {
            auto *output = static_cast<int32_t *>(destination);
            for (size_t i = 0; i < sample_count; ++i) {
                float value = SanitizeAndClamp(source[i]);
                if (accumulate) value += static_cast<float>(output[i]) / kInt32Scale;
                const int64_t quantized = QuantizeSigned(value, 24);
                output[i] = static_cast<int32_t>(quantized * 256);
            }
            return true;
        }
        case AUDIO_FORMAT_PCM_24_BIT_PACKED: {
            auto *output = static_cast<uint8_t *>(destination);
            for (size_t i = 0; i < sample_count; ++i) {
                float value = SanitizeAndClamp(source[i]);
                if (accumulate) {
                    value += static_cast<float>(DecodePacked24(output + i * 3)) / kInt24Scale;
                }
                EncodePacked24(output + i * 3, static_cast<int32_t>(QuantizeSigned(value, 24)));
            }
            return true;
        }
        case AUDIO_FORMAT_PCM_32_BIT: {
            auto *output = static_cast<int32_t *>(destination);
            for (size_t i = 0; i < sample_count; ++i) {
                float value = SanitizeAndClamp(source[i]);
                if (accumulate) value += static_cast<float>(output[i]) / kInt32Scale;
                output[i] = static_cast<int32_t>(QuantizeSigned(value, 32));
            }
            return true;
        }
        case AUDIO_FORMAT_PCM_FLOAT: {
            auto *output = static_cast<float *>(destination);
            for (size_t i = 0; i < sample_count; ++i) {
                const float existing = accumulate ? SanitizeAndClamp(output[i]) : 0.0F;
                output[i] = SanitizeAndClamp(existing + SanitizeAndClamp(source[i]));
            }
            return true;
        }
        default:
            return false;
    }
}

} // namespace viper::audio
