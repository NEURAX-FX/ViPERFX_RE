#ifndef VIPER_AUDIO_FORMAT_H
#define VIPER_AUDIO_FORMAT_H

#include "include/essential.h"
#include <cstddef>
#include <cstdint>

namespace viper::audio {

constexpr uint32_t kMinSampleRate = 8000;
constexpr uint32_t kMaxSampleRate = 384000;
constexpr size_t kChannelCount = 2;
constexpr size_t kMaxBlockFrames = 8192;

bool IsSupportedSampleRate(uint32_t sample_rate);
bool IsSupportedPcmFormat(audio_format_t format);
size_t BytesPerSample(audio_format_t format);

bool ToFloat(
    float *destination,
    const void *source,
    size_t frame_count,
    audio_format_t format
);

bool FromFloat(
    void *destination,
    const float *source,
    size_t frame_count,
    audio_format_t format,
    bool accumulate
);

} // namespace viper::audio

#endif
