#include "AudioFormat.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

namespace {

bool Check(bool condition, const char *message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}

bool Near(float actual, float expected, float tolerance = 1.0e-6F) {
    return std::fabs(actual - expected) <= tolerance;
}

bool TestCapabilities() {
    using namespace viper::audio;
    if (!Check(!IsSupportedSampleRate(7999), "reject rate below 8 kHz")) return false;
    if (!Check(IsSupportedSampleRate(8000), "accept 8 kHz")) return false;
    if (!Check(IsSupportedSampleRate(44100), "accept 44.1 kHz")) return false;
    if (!Check(IsSupportedSampleRate(384000), "accept 384 kHz")) return false;
    if (!Check(!IsSupportedSampleRate(384001), "reject rate above 384 kHz")) return false;

    const std::array<audio_format_t, 5> formats{
        AUDIO_FORMAT_PCM_16_BIT,
        AUDIO_FORMAT_PCM_8_24_BIT,
        AUDIO_FORMAT_PCM_24_BIT_PACKED,
        AUDIO_FORMAT_PCM_32_BIT,
        AUDIO_FORMAT_PCM_FLOAT,
    };
    for (audio_format_t format : formats) {
        if (!Check(IsSupportedPcmFormat(format), "accept declared PCM format")) return false;
    }
    if (!Check(!IsSupportedPcmFormat(AUDIO_FORMAT_INVALID), "reject invalid format")) {
        return false;
    }
    if (!Check(BytesPerSample(AUDIO_FORMAT_PCM_16_BIT) == 2, "PCM16 byte width")) {
        return false;
    }
    if (!Check(BytesPerSample(AUDIO_FORMAT_PCM_24_BIT_PACKED) == 3, "PCM24 byte width")) {
        return false;
    }
    if (!Check(BytesPerSample(AUDIO_FORMAT_PCM_FLOAT) == 4, "Float32 byte width")) {
        return false;
    }
    return true;
}

bool TestIntegerInputConversion() {
    using namespace viper::audio;
    std::array<float, 4> converted{};

    const std::array<int16_t, 4> pcm16{
        std::numeric_limits<int16_t>::min(),
        -16384,
        0,
        std::numeric_limits<int16_t>::max(),
    };
    if (!Check(ToFloat(converted.data(), pcm16.data(), 2, AUDIO_FORMAT_PCM_16_BIT), "PCM16 decode")) {
        return false;
    }
    if (!Check(Near(converted[0], -1.0F), "PCM16 negative full scale")) return false;
    if (!Check(Near(converted[1], -0.5F), "PCM16 half scale")) return false;
    if (!Check(Near(converted[2], 0.0F), "PCM16 zero")) return false;
    if (!Check(Near(converted[3], 32767.0F / 32768.0F), "PCM16 positive full scale")) {
        return false;
    }

    const std::array<int32_t, 4> pcm8_24{
        std::numeric_limits<int32_t>::min(),
        -1073741824,
        0,
        2147483392,
    };
    if (!Check(ToFloat(converted.data(), pcm8_24.data(), 2, AUDIO_FORMAT_PCM_8_24_BIT), "PCM 8.24 decode")) {
        return false;
    }
    if (!Check(Near(converted[0], -1.0F), "PCM 8.24 negative full scale")) return false;
    if (!Check(Near(converted[1], -0.5F), "PCM 8.24 half scale")) return false;
    if (!Check(Near(converted[3], 2147483392.0F / 2147483648.0F), "PCM 8.24 positive full scale")) {
        return false;
    }

    const std::array<uint8_t, 12> pcm24{
        0x00, 0x00, 0x80,
        0x00, 0x00, 0xC0,
        0x00, 0x00, 0x00,
        0xFF, 0xFF, 0x7F,
    };
    if (!Check(ToFloat(converted.data(), pcm24.data(), 2, AUDIO_FORMAT_PCM_24_BIT_PACKED), "packed PCM24 decode")) {
        return false;
    }
    if (!Check(Near(converted[0], -1.0F), "packed PCM24 negative full scale")) return false;
    if (!Check(Near(converted[1], -0.5F), "packed PCM24 half scale")) return false;
    if (!Check(Near(converted[3], 8388607.0F / 8388608.0F), "packed PCM24 positive full scale")) {
        return false;
    }
    return true;
}

bool TestFloatInputSanitization() {
    using namespace viper::audio;
    const std::array<float, 4> input{
        std::numeric_limits<float>::quiet_NaN(),
        -2.0F,
        -0.25F,
        1.5F,
    };
    std::array<float, 4> converted{};
    if (!Check(ToFloat(converted.data(), input.data(), 2, AUDIO_FORMAT_PCM_FLOAT), "Float32 decode")) {
        return false;
    }
    return Check(Near(converted[0], 0.0F), "sanitize NaN")
        && Check(Near(converted[1], -1.0F), "clip negative float")
        && Check(Near(converted[2], -0.25F), "preserve finite float")
        && Check(Near(converted[3], 1.0F), "clip positive float");
}

bool TestOutputConversion() {
    using namespace viper::audio;
    const std::array<float, 4> input{-2.0F, -0.5F, 0.5F, 2.0F};
    std::array<int16_t, 4> pcm16{};
    if (!Check(FromFloat(pcm16.data(), input.data(), 2, AUDIO_FORMAT_PCM_16_BIT, false), "PCM16 encode")) {
        return false;
    }
    if (!Check(pcm16[0] == std::numeric_limits<int16_t>::min(), "PCM16 clamp minimum")) return false;
    if (!Check(pcm16[1] == -16384, "PCM16 encode negative half")) return false;
    if (!Check(pcm16[2] == 16384, "PCM16 encode positive half")) return false;
    if (!Check(pcm16[3] == std::numeric_limits<int16_t>::max(), "PCM16 clamp maximum")) return false;

    std::array<uint8_t, 12> pcm24{};
    if (!Check(FromFloat(pcm24.data(), input.data(), 2, AUDIO_FORMAT_PCM_24_BIT_PACKED, false), "packed PCM24 encode")) {
        return false;
    }
    const std::array<uint8_t, 12> expected{
        0x00, 0x00, 0x80,
        0x00, 0x00, 0xC0,
        0x00, 0x00, 0x40,
        0xFF, 0xFF, 0x7F,
    };
    if (!Check(pcm24 == expected, "packed PCM24 bytes")) return false;

    std::array<float, 4> accumulated{0.75F, -0.75F, 0.25F, -0.25F};
    const std::array<float, 4> addition{
        0.5F,
        -0.5F,
        std::numeric_limits<float>::quiet_NaN(),
        -0.5F,
    };
    if (!Check(FromFloat(accumulated.data(), addition.data(), 2, AUDIO_FORMAT_PCM_FLOAT, true), "Float32 accumulate")) {
        return false;
    }
    return Check(Near(accumulated[0], 1.0F), "accumulate positive clip")
        && Check(Near(accumulated[1], -1.0F), "accumulate negative clip")
        && Check(Near(accumulated[2], 0.25F), "ignore non-finite addition")
        && Check(Near(accumulated[3], -0.75F), "accumulate negative");
}

} // namespace

int main() {
    if (!TestCapabilities()) return 1;
    if (!TestIntegerInputConversion()) return 1;
    if (!TestFloatInputSanitization()) return 1;
    if (!TestOutputConversion()) return 1;
    std::puts("Audio format tests passed");
    return 0;
}
