#include "IemAssetCompiler.h"
#include "Sha256.h"

#include "iem/StreamingResampler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace iem::tools {
namespace {

constexpr uint32_t kOutputRate = 96000;

struct SourceSpec {
    const char *path;
    const char *display_name;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t frames;
    int order;
    int eq_id;
};

constexpr std::array<SourceSpec, 26> kSources{{
    {"IRs/irsOrd1.wav", "KU100 Ord1", 44100, 4, 236, 1, -1},
    {"IRs/irsOrd2.wav", "KU100 Ord2", 44100, 9, 236, 2, -1},
    {"IRs/irsOrd3.wav", "KU100 Ord3", 44100, 16, 236, 3, -1},
    {"EQ/AKG-K1000-Closed.wav", "AKG K1000 Closed", 48000, 2, 2048, 0, 0},
    {"EQ/AKG-K1000-Open.wav", "AKG K1000 Open", 48000, 2, 2048, 0, 1},
    {"EQ/AKG-K141MK2.wav", "AKG K141 MK2", 48000, 2, 2048, 0, 2},
    {"EQ/AKG-K240DF.wav", "AKG K240 DF", 48000, 2, 2048, 0, 3},
    {"EQ/AKG-K240MK2.wav", "AKG K240 MK2", 48000, 2, 2048, 0, 4},
    {"EQ/AKG-K271MK2.wav", "AKG K271 MK2", 48000, 2, 2048, 0, 5},
    {"EQ/AKG-K271STUDIO.wav", "AKG K271 Studio", 48000, 2, 2048, 0, 6},
    {"EQ/AKG-K601.wav", "AKG K601", 48000, 2, 2048, 0, 7},
    {"EQ/AKG-K701.wav", "AKG K701", 48000, 2, 2048, 0, 8},
    {"EQ/AKG-K702.wav", "AKG K702", 48000, 2, 2048, 0, 9},
    {"EQ/AudioTechnica-ATH-M50.wav", "Audio-Technica ATH-M50", 48000, 2, 2048, 0, 10},
    {"EQ/Beyerdynamic-DT250.wav", "Beyerdynamic DT250", 48000, 2, 2048, 0, 11},
    {"EQ/Beyerdynamic-DT770PRO-250Ohms.wav", "Beyerdynamic DT770 Pro 250 Ohms", 48000, 2, 2048, 0, 12},
    {"EQ/Beyerdynamic-DT880.wav", "Beyerdynamic DT880", 48000, 2, 2048, 0, 13},
    {"EQ/Beyerdynamic-DT990PRO.wav", "Beyerdynamic DT990 Pro", 48000, 2, 2048, 0, 14},
    {"EQ/Presonus-HD7.wav", "Presonus HD7", 48000, 2, 2048, 0, 15},
    {"EQ/Sennheiser-HD430.wav", "Sennheiser HD430", 48000, 2, 2048, 0, 16},
    {"EQ/Sennheiser-HD480.wav", "Sennheiser HD480", 48000, 2, 2048, 0, 17},
    {"EQ/Sennheiser-HD560ovationII.wav", "Sennheiser HD560 Ovation II", 48000, 2, 2048, 0, 18},
    {"EQ/Sennheiser-HD565ovation.wav", "Sennheiser HD565 Ovation", 48000, 2, 2048, 0, 19},
    {"EQ/Sennheiser-HD600.wav", "Sennheiser HD600", 48000, 2, 2048, 0, 20},
    {"EQ/Sennheiser-HD650.wav", "Sennheiser HD650", 48000, 2, 2048, 0, 21},
    {"EQ/SHURE-SRH940.wav", "Shure SRH940", 48000, 2, 2048, 0, 22},
}};

struct WavData {
    uint32_t sample_rate = 0;
    uint16_t channels = 0;
    uint32_t frames = 0;
    std::vector<float> interleaved;
};

struct GeneratedResource {
    const SourceSpec *spec = nullptr;
    std::string source_sha256;
    uint32_t channels = 0;
    uint32_t frames = 0;
    std::vector<float> samples;
    std::string output_sha256;
};

uint16_t ReadU16(const uint8_t *data) {
    return static_cast<uint16_t>(data[0]) | static_cast<uint16_t>(data[1]) << 8U;
}

uint32_t ReadU32(const uint8_t *data) {
    return static_cast<uint32_t>(data[0]) | static_cast<uint32_t>(data[1]) << 8U
        | static_cast<uint32_t>(data[2]) << 16U
        | static_cast<uint32_t>(data[3]) << 24U;
}

bool LoadPinnedHashes(const std::filesystem::path &source_directory,
    std::map<std::string, std::string> &hashes, std::string &error) {
    std::ifstream input(source_directory / "SHA256SUMS");
    if (!input) { error = "cannot open SHA256SUMS"; return false; }
    std::string hash;
    std::string path;
    while (input >> hash >> path) {
        if (hash.size() != 64 || path.empty() || !hashes.emplace(path, hash).second) {
            error = "invalid SHA256SUMS entry"; return false;
        }
    }
    if (hashes.size() != kSources.size()) {
        error = "SHA256SUMS must contain exactly 26 entries"; return false;
    }
    return true;
}

bool ReadWav(const std::filesystem::path &path, WavData &wav, std::string &error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) { error = "cannot open " + path.string(); return false; }
    input.seekg(0, std::ios::end);
    const std::streamoff length = input.tellg();
    if (length < 44) { error = "truncated WAV " + path.string(); return false; }
    input.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<std::size_t>(length));
    input.read(reinterpret_cast<char *>(bytes.data()), length);
    if (!input || std::memcmp(bytes.data(), "RIFF", 4) != 0
        || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        error = "invalid RIFF/WAVE " + path.string(); return false;
    }
    const uint8_t *format = nullptr;
    std::size_t format_size = 0;
    const uint8_t *pcm = nullptr;
    std::size_t pcm_size = 0;
    std::size_t offset = 12;
    while (offset + 8U <= bytes.size()) {
        const uint32_t chunk_size = ReadU32(bytes.data() + offset + 4U);
        const std::size_t data_offset = offset + 8U;
        if (data_offset + chunk_size > bytes.size()) {
            error = "truncated WAV chunk " + path.string(); return false;
        }
        if (std::memcmp(bytes.data() + offset, "fmt ", 4) == 0) {
            format = bytes.data() + data_offset;
            format_size = chunk_size;
        } else if (std::memcmp(bytes.data() + offset, "data", 4) == 0) {
            pcm = bytes.data() + data_offset;
            pcm_size = chunk_size;
        }
        offset = data_offset + chunk_size + (chunk_size & 1U);
    }
    if (format == nullptr || format_size < 16 || pcm == nullptr) {
        error = "missing WAV format/data " + path.string(); return false;
    }
    const uint16_t format_code = ReadU16(format);
    const uint16_t bits_per_sample = ReadU16(format + 14);
    const uint16_t bytes_per_sample = bits_per_sample / 8U;
    if (!((format_code == 1 && bits_per_sample == 16)
            || (format_code == 3 && bits_per_sample == 32))) {
        error = "WAV must be PCM16 or float32 " + path.string(); return false;
    }
    wav.channels = ReadU16(format + 2);
    wav.sample_rate = ReadU32(format + 4);
    const uint16_t block_align = ReadU16(format + 12);
    if (wav.channels == 0 || block_align != wav.channels * bytes_per_sample
        || pcm_size % block_align != 0) {
        error = "invalid WAV alignment " + path.string(); return false;
    }
    wav.frames = static_cast<uint32_t>(pcm_size / block_align);
    wav.interleaved.resize(static_cast<std::size_t>(wav.frames) * wav.channels);
    for (std::size_t index = 0; index < wav.interleaved.size(); ++index) {
        if (format_code == 1) {
            const int16_t sample = static_cast<int16_t>(ReadU16(pcm + index * 2U));
            wav.interleaved[index] = static_cast<float>(sample) / 32768.0F;
        } else {
            const uint32_t bits = ReadU32(pcm + index * 4U);
            std::memcpy(&wav.interleaved[index], &bits, sizeof(bits));
            if (!std::isfinite(wav.interleaved[index])) {
                error = "non-finite float WAV " + path.string(); return false;
            }
        }
    }
    return true;
}

bool Resample(const WavData &wav, uint32_t selected_channels,
    std::vector<std::vector<float>> &output, std::string &error) {
    const uint32_t output_frames = static_cast<uint32_t>(std::llround(
        static_cast<double>(wav.frames) * kOutputRate / wav.sample_rate));
    const std::size_t padded_frames = wav.frames + StreamingResampler::kTapCount;
    std::vector<std::vector<float>> source(selected_channels,
        std::vector<float>(padded_frames, 0.0F));
    for (uint32_t channel = 0; channel < selected_channels; ++channel) {
        for (uint32_t frame = 0; frame < wav.frames; ++frame) {
            source[channel][frame] = wav.interleaved[
                static_cast<std::size_t>(frame) * wav.channels + channel];
        }
    }
    output.assign(selected_channels, std::vector<float>(output_frames));
    std::vector<const float *> input_pointers(selected_channels);
    std::vector<float *> output_pointers(selected_channels);
    for (uint32_t channel = 0; channel < selected_channels; ++channel) {
        input_pointers[channel] = source[channel].data();
        output_pointers[channel] = output[channel].data();
    }
    StreamingResampler resampler;
    if (!resampler.Prepare(wav.sample_rate, kOutputRate, selected_channels,
            padded_frames)) {
        error = "resampler prepare failed"; return false;
    }
    const std::size_t produced = resampler.Process(input_pointers.data(), padded_frames,
        output_pointers.data(), output_frames);
    if (produced != output_frames || resampler.Failed()) {
        error = "resampler output length mismatch"; return false;
    }
    return true;
}

bool Generate(const SourceSpec &spec, const std::string &source_hash,
    const WavData &wav, GeneratedResource &resource, std::string &error) {
    if (wav.sample_rate != spec.sample_rate || wav.channels != spec.channels
        || wav.frames != spec.frames) {
        error = "metadata mismatch for " + std::string(spec.path); return false;
    }
    resource.spec = &spec;
    resource.source_sha256 = source_hash;
    if (spec.order > 0) {
        const uint32_t inputs = static_cast<uint32_t>((spec.order + 1) * (spec.order + 1));
        std::vector<std::vector<float>> filters;
        if (!Resample(wav, inputs, filters, error)) return false;
        resource.channels = inputs;
        resource.frames = static_cast<uint32_t>(filters[0].size());
        resource.samples.resize(static_cast<std::size_t>(2U) * inputs * resource.frames);
        for (uint32_t channel = 0; channel < inputs; ++channel) {
            const uint32_t degree = static_cast<uint32_t>(std::floor(std::sqrt(channel)));
            const int m = static_cast<int>(channel)
                - static_cast<int>(degree * (degree + 1U));
            const float scale = 0.3F * std::sqrt(static_cast<float>(2U * degree + 1U))
                * (44100.0F / 96000.0F);
            for (uint32_t frame = 0; frame < resource.frames; ++frame) {
                const float value = filters[channel][frame] * scale;
                resource.samples[static_cast<std::size_t>(channel) * resource.frames + frame]
                    = value;
                resource.samples[(static_cast<std::size_t>(inputs) + channel)
                    * resource.frames + frame] = m < 0 ? -value : value;
            }
        }
    } else {
        std::vector<std::vector<float>> filters;
        if (!Resample(wav, 2, filters, error)) return false;
        resource.channels = 2;
        resource.frames = static_cast<uint32_t>(filters[0].size());
        resource.samples.resize(static_cast<std::size_t>(2U) * resource.frames);
        std::copy(filters[0].begin(), filters[0].end(), resource.samples.begin());
        std::copy(filters[1].begin(), filters[1].end(),
            resource.samples.begin() + resource.frames);
    }
    resource.output_sha256 = Sha256Hex(
        reinterpret_cast<const uint8_t *>(resource.samples.data()),
        resource.samples.size() * sizeof(float));
    return true;
}

std::string SymbolName(const GeneratedResource &resource) {
    return resource.spec->order > 0
        ? "kKu100Order" + std::to_string(resource.spec->order)
        : "kHeadphoneEq" + std::to_string(resource.spec->eq_id);
}

void EmitFloatArray(std::ostream &output, const std::string &name,
    const std::vector<float> &samples) {
    output << "alignas(64) const float " << name << "[] = {\n";
    output << std::hexfloat << std::setprecision(std::numeric_limits<float>::max_digits10);
    for (std::size_t index = 0; index < samples.size(); ++index) {
        if (index % 6U == 0) output << "    ";
        output << samples[index] << "F";
        if (index + 1U != samples.size()) output << ",";
        if (index % 6U == 5U || index + 1U == samples.size()) {
            output << "\n";
        } else {
            output << " ";
        }
    }
    output << std::defaultfloat << "};\n\n";
}

bool WriteGenerated(const std::filesystem::path &directory,
    const std::vector<GeneratedResource> &resources, std::string &error) {
    std::error_code filesystem_error;
    std::filesystem::create_directories(directory, filesystem_error);
    if (filesystem_error) { error = filesystem_error.message(); return false; }
    std::ofstream header(directory / "IemResourceManifest.h", std::ios::binary);
    std::ofstream source(directory / "IemResources.cpp", std::ios::binary);
    if (!header || !source) { error = "cannot create generated outputs"; return false; }

    header << "#pragma once\n\n#include <cstdint>\n\nnamespace iem::resources {\n\n"
        << "struct Ku100Resource { uint32_t order; uint32_t input_channels; uint32_t frames; "
        << "const float *ir; const char *source_sha256; const char *output_sha256; };\n"
        << "struct HeadphoneEqResource { int32_t id; const char *display_name; uint32_t channels; uint32_t frames; "
        << "const float *impulse; const char *source_sha256; const char *output_sha256; };\n\n"
        << "constexpr uint32_t kKu100ResourceCount = 3;\n"
        << "constexpr uint32_t kHeadphoneEqResourceCount = 23;\n"
        << "extern const char kUpstreamRepository[];\nextern const char kUpstreamCommit[];\n"
        << "extern const char kKu100Attribution[];\nextern const char kRenderingAttribution[];\n"
        << "const Ku100Resource *FindKu100(uint32_t order) noexcept;\n"
        << "const HeadphoneEqResource *FindHeadphoneEq(int32_t id) noexcept;\n\n"
        << "} // namespace iem::resources\n";

    source << "#include \"IemResourceManifest.h\"\n\nnamespace iem::resources {\nnamespace {\n\n";
    for (const GeneratedResource &resource : resources) {
        EmitFloatArray(source, SymbolName(resource), resource.samples);
    }
    source << "const Ku100Resource kKu100[] = {\n";
    for (const GeneratedResource &resource : resources) {
        if (resource.spec->order <= 0) continue;
        source << "    {" << resource.spec->order << ", " << resource.channels << ", "
            << resource.frames << ", " << SymbolName(resource) << ", \""
            << resource.source_sha256 << "\", \"" << resource.output_sha256 << "\"},\n";
    }
    source << "};\n\nconst HeadphoneEqResource kHeadphoneEq[] = {\n";
    for (const GeneratedResource &resource : resources) {
        if (resource.spec->eq_id < 0) continue;
        source << "    {" << resource.spec->eq_id << ", \"" << resource.spec->display_name
            << "\", " << resource.channels << ", " << resource.frames << ", "
            << SymbolName(resource) << ", \""
            << resource.source_sha256 << "\", \"" << resource.output_sha256 << "\"},\n";
    }
    source << "};\n\n} // namespace\n\n"
        << "const char kUpstreamRepository[] = \"https://git.iem.at/audioplugins/IEMPluginSuite.git\";\n"
        << "const char kUpstreamCommit[] = \"39de1dd5883f1bd8d65fe1662487f2470a1d7b55\";\n"
        << "const char kKu100Attribution[] = \"Neumann KU100 far-field HRIR/HRTF compilation by Benjamin Bernschuetz\";\n"
        << "const char kRenderingAttribution[] = \"Magnitude-least-squares rendering by Schoerkhuber, Zaunschirm, and Hoeldrich\";\n\n"
        << "const Ku100Resource *FindKu100(uint32_t order) noexcept {\n"
        << "    return order >= 1 && order <= 3 ? &kKu100[order - 1] : nullptr;\n}\n\n"
        << "const HeadphoneEqResource *FindHeadphoneEq(int32_t id) noexcept {\n"
        << "    return id >= 0 && id < 23 ? &kHeadphoneEq[id] : nullptr;\n}\n\n"
        << "} // namespace iem::resources\n";
    return static_cast<bool>(header) && static_cast<bool>(source);
}

} // namespace

CompileResult CompilePinnedAssets(const std::filesystem::path &source_directory,
    const std::filesystem::path &output_directory) {
    std::map<std::string, std::string> hashes;
    std::string error;
    if (!LoadPinnedHashes(source_directory, hashes, error)) return {false, error};
    std::vector<GeneratedResource> resources;
    resources.reserve(kSources.size());
    for (const SourceSpec &spec : kSources) {
        const auto hash = hashes.find(spec.path);
        if (hash == hashes.end()) return {false, "missing hash for " + std::string(spec.path)};
        const std::filesystem::path path = source_directory / spec.path;
        if (Sha256File(path) != hash->second) {
            return {false, "SHA-256 mismatch for " + path.string()};
        }
        WavData wav;
        if (!ReadWav(path, wav, error)) return {false, error};
        GeneratedResource generated;
        if (!Generate(spec, hash->second, wav, generated, error)) return {false, error};
        resources.push_back(std::move(generated));
    }
    if (!WriteGenerated(output_directory, resources, error)) return {false, error};
    return {true, {}};
}

} // namespace iem::tools

#ifndef IEM_ASSET_COMPILER_NO_MAIN
#include <cstdio>
int main(int argc, char **argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: iem_asset_compiler SOURCE_DIR OUTPUT_DIR\n");
        return 2;
    }
    const iem::tools::CompileResult result = iem::tools::CompilePinnedAssets(argv[1], argv[2]);
    if (!result.success) {
        std::fprintf(stderr, "%s\n", result.error.c_str());
        return 1;
    }
    return 0;
}
#endif
