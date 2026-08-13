#include "viper/utils/WavReader.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

void AppendU16(std::vector<uint8_t> &bytes, uint16_t value) {
    bytes.push_back(static_cast<uint8_t>(value));
    bytes.push_back(static_cast<uint8_t>(value >> 8));
}

void AppendU32(std::vector<uint8_t> &bytes, uint32_t value) {
    bytes.push_back(static_cast<uint8_t>(value));
    bytes.push_back(static_cast<uint8_t>(value >> 8));
    bytes.push_back(static_cast<uint8_t>(value >> 16));
    bytes.push_back(static_cast<uint8_t>(value >> 24));
}

void AppendTag(std::vector<uint8_t> &bytes, const char tag[4]) {
    bytes.insert(bytes.end(), tag, tag + 4);
}

std::vector<uint8_t> ExtensibleFloatWav() {
    constexpr uint32_t channels = 4;
    constexpr uint32_t frames = 16;
    constexpr uint32_t data_size = channels * frames * sizeof(float);
    std::vector<uint8_t> bytes;
    AppendTag(bytes, "RIFF");
    AppendU32(bytes, 4 + 8 + 40 + 8 + data_size);
    AppendTag(bytes, "WAVE");
    AppendTag(bytes, "fmt ");
    AppendU32(bytes, 40);
    AppendU16(bytes, 0xFFFE);
    AppendU16(bytes, channels);
    AppendU32(bytes, 48000);
    AppendU32(bytes, 48000 * channels * sizeof(float));
    AppendU16(bytes, channels * sizeof(float));
    AppendU16(bytes, 32);
    AppendU16(bytes, 22);
    AppendU16(bytes, 32);
    AppendU32(bytes, 0);
    AppendU32(bytes, 3);
    AppendU16(bytes, 0);
    AppendU16(bytes, 0x0010);
    const uint8_t guid_tail[8]{0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
    bytes.insert(bytes.end(), guid_tail, guid_tail + 8);
    AppendTag(bytes, "data");
    AppendU32(bytes, data_size);
    for (uint32_t frame = 0; frame < frames; ++frame) {
        for (uint32_t channel = 0; channel < channels; ++channel) {
            const float sample = static_cast<float>(channel + 1) / 4.0F;
            const auto *raw = reinterpret_cast<const uint8_t *>(&sample);
            bytes.insert(bytes.end(), raw, raw + sizeof(float));
        }
    }
    return bytes;
}

std::vector<uint8_t> ThreeChannelPcm16Wav() {
    constexpr uint32_t channels = 3;
    constexpr uint32_t frames = 16;
    constexpr uint32_t data_size = channels * frames * sizeof(int16_t);
    std::vector<uint8_t> bytes;
    AppendTag(bytes, "RIFF");
    AppendU32(bytes, 4 + 8 + 16 + 8 + data_size);
    AppendTag(bytes, "WAVE");
    AppendTag(bytes, "fmt ");
    AppendU32(bytes, 16);
    AppendU16(bytes, 1);
    AppendU16(bytes, channels);
    AppendU32(bytes, 48000);
    AppendU32(bytes, 48000 * channels * sizeof(int16_t));
    AppendU16(bytes, channels * sizeof(int16_t));
    AppendU16(bytes, 16);
    AppendTag(bytes, "data");
    AppendU32(bytes, data_size);
    bytes.resize(bytes.size() + data_size, 0);
    return bytes;
}

bool WriteFixture(const char *path, const std::vector<uint8_t> &bytes) {
    FILE *file = std::fopen(path, "wb");
    if (file == nullptr) return false;
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    std::fclose(file);
    return ok;
}

bool Check(bool condition, const char *message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}

bool TestExtensibleFourChannelFloat() {
    const char *path = "wav_reader_extensible_fixture.wav";
    if (!WriteFixture(path, ExtensibleFloatWav())) return false;
    WavData wav{};
    const bool decoded = ReadWavFile(path, &wav);
    std::remove(path);
    if (!Check(decoded, "decode extensible four-channel float WAV")) return false;
    const bool valid = Check(wav.channels == 4, "preserve four channels")
        && Check(wav.frame_count == 16, "preserve frame count")
        && Check(wav.sample_rate == 48000, "preserve sample rate")
        && Check(std::fabs(wav.samples[0] - 0.25F) < 1.0e-6F, "channel one sample")
        && Check(std::fabs(wav.samples[3] - 1.0F) < 1.0e-6F, "channel four sample");
    delete[] wav.samples;
    return valid;
}

bool TestRejectsUnsupportedChannelCount() {
    const char *path = "wav_reader_unsupported_fixture.wav";
    if (!WriteFixture(path, ThreeChannelPcm16Wav())) return false;
    WavData wav{};
    const bool decoded = ReadWavFile(path, &wav);
    std::remove(path);
    delete[] wav.samples;
    return Check(!decoded, "reject three-channel WAV");
}

} // namespace

int main() {
    if (!TestExtensibleFourChannelFloat()) return 1;
    if (!TestRejectsUnsupportedChannelCount()) return 1;
    std::puts("WAV reader tests passed");
    return 0;
}
