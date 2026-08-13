#include "IemAssetCompiler.h"
#include "IemResourceManifest.h"
#include "Sha256.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef IEM_ASSET_SOURCE_DIR
#error IEM_ASSET_SOURCE_DIR must be defined
#endif
#ifndef IEM_GENERATED_RESOURCE_DIR
#error IEM_GENERATED_RESOURCE_DIR must be defined
#endif

namespace {

bool Check(bool condition, const char *message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}

std::vector<uint8_t> ReadBytes(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    input.seekg(0, std::ios::end);
    const std::streamoff length = input.tellg();
    input.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(length > 0 ? static_cast<std::size_t>(length) : 0U);
    if (!bytes.empty()) input.read(reinterpret_cast<char *>(bytes.data()), length);
    return bytes;
}

bool TestSha256Vectors() {
    const std::array<uint8_t, 1> empty{};
    const std::array<uint8_t, 3> abc{'a', 'b', 'c'};
    return Check(iem::tools::Sha256Hex(empty.data(), 0)
            == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
            "SHA-256 empty vector")
        && Check(iem::tools::Sha256Hex(abc.data(), abc.size())
            == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "SHA-256 abc vector");
}

bool TestReproducibleCompiler() {
    const std::filesystem::path root = std::filesystem::temp_directory_path()
        / "iem-asset-compiler-test";
    const std::filesystem::path first = root / "first";
    const std::filesystem::path second = root / "second";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    const auto first_result = iem::tools::CompilePinnedAssets(IEM_ASSET_SOURCE_DIR, first);
    const auto second_result = iem::tools::CompilePinnedAssets(IEM_ASSET_SOURCE_DIR, second);
    if (!Check(first_result.success && second_result.success,
            "compile pinned assets twice")) return false;
    for (const char *file : {"IemResourceManifest.h", "IemResources.cpp"}) {
        const auto first_bytes = ReadBytes(first / file);
        if (!Check(first_bytes == ReadBytes(second / file),
                "two generated outputs are identical")) return false;
        if (!Check(first_bytes == ReadBytes(std::filesystem::path(IEM_GENERATED_RESOURCE_DIR) / file),
                "committed generated output is reproducible")) return false;
    }
    std::filesystem::remove_all(root, error);
    return true;
}

bool TestManifest() {
    if (!Check(std::string(iem::resources::kUpstreamCommit)
            == "39de1dd5883f1bd8d65fe1662487f2470a1d7b55",
            "pinned upstream commit")) return false;
    constexpr std::array<uint32_t, 3> inputs{4, 9, 16};
    for (uint32_t order = 1; order <= 3; ++order) {
        const auto *resource = iem::resources::FindKu100(order);
        if (!Check(resource != nullptr && resource->input_channels == inputs[order - 1U]
                && resource->frames == 514, "KU100 generated metadata")) return false;
        for (uint32_t channel = 0; channel < resource->input_channels; ++channel) {
            const uint32_t degree = static_cast<uint32_t>(std::floor(std::sqrt(channel)));
            const int m = static_cast<int>(channel)
                - static_cast<int>(degree * (degree + 1U));
            for (uint32_t frame = 0; frame < resource->frames; ++frame) {
                const float left = resource->ir[
                    static_cast<std::size_t>(channel) * resource->frames + frame];
                const float right = resource->ir[
                    (static_cast<std::size_t>(resource->input_channels) + channel)
                        * resource->frames + frame];
                if (!Check(right == (m < 0 ? -left : left),
                        "KU100 Mid/Side sign mapping")) return false;
            }
        }
    }
    if (!Check(iem::resources::FindKu100(0) == nullptr
            && iem::resources::FindKu100(4) == nullptr, "reject invalid KU100 order")) {
        return false;
    }
    for (int id = 0; id < 23; ++id) {
        const auto *resource = iem::resources::FindHeadphoneEq(id);
        if (!Check(resource != nullptr && resource->id == id
                && resource->channels == 2 && resource->frames == 4096,
                "headphone EQ generated metadata")) return false;
    }
    return Check(std::string(iem::resources::FindHeadphoneEq(0)->display_name)
            == "AKG K1000 Closed", "first EQ model")
        && Check(std::string(iem::resources::FindHeadphoneEq(22)->display_name)
            == "Shure SRH940", "last EQ model")
        && Check(iem::resources::FindHeadphoneEq(-1) == nullptr
            && iem::resources::FindHeadphoneEq(23) == nullptr, "reject invalid EQ model");
}

bool TestDialogNetResource() {
    const std::filesystem::path path =
        std::filesystem::path(IEM_ASSET_SOURCE_DIR) / "halo/dialog.net";
    if (!Check(iem::tools::Sha256File(path)
            == "652cbd597b9afbd82eb9b39fe80e3e825a381e448c3c2a269c07842f88eb5b72",
            "dialog.net source hash")) return false;
    const auto &net = iem::resources::DialogNet();
    return Check(net.connection_count == 391, "dialog.net connection count")
        && Check(net.weights != nullptr, "dialog.net weights are present")
        && Check(std::string(net.source_sha256)
            == "652cbd597b9afbd82eb9b39fe80e3e825a381e448c3c2a269c07842f88eb5b72",
            "dialog.net embedded hash");
}

bool TestDialogNetHashFailure() {
    const std::filesystem::path root = std::filesystem::temp_directory_path()
        / "iem-dialog-net-corruption-test";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::copy(IEM_ASSET_SOURCE_DIR, root,
        std::filesystem::copy_options::recursive, error);
    if (!Check(!error, "copy source fixture for dialog.net")) return false;
    std::fstream file(root / "halo/dialog.net",
        std::ios::in | std::ios::out | std::ios::binary);
    file.seekp(0);
    const char corrupt = 'X';
    file.write(&corrupt, 1);
    file.close();
    const auto result = iem::tools::CompilePinnedAssets(root, root / "generated");
    const bool generated = std::filesystem::exists(root / "generated" / "IemResources.cpp");
    std::filesystem::remove_all(root, error);
    return Check(!result.success && result.error.find("SHA-256") != std::string::npos,
            "corrupted dialog.net is rejected by hash")
        && Check(!generated, "failed compile writes no generated files");
}

bool TestHashFailure() {
    const std::filesystem::path root = std::filesystem::temp_directory_path()
        / "iem-asset-corruption-test";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::copy(IEM_ASSET_SOURCE_DIR, root,
        std::filesystem::copy_options::recursive, error);
    if (!Check(!error, "copy source fixture")) return false;
    std::fstream file(root / "IRs/irsOrd1.wav",
        std::ios::in | std::ios::out | std::ios::binary);
    file.seekp(44);
    const char corrupt = 0x5A;
    file.write(&corrupt, 1);
    file.close();
    const auto result = iem::tools::CompilePinnedAssets(root, root / "generated");
    std::filesystem::remove_all(root, error);
    return Check(!result.success && result.error.find("SHA-256 mismatch") != std::string::npos,
        "corrupted source is rejected by hash");
}

} // namespace

int main() {
    if (!TestSha256Vectors()) return 1;
    if (!TestReproducibleCompiler()) return 1;
    if (!TestManifest()) return 1;
    if (!TestHashFailure()) return 1;
    if (!TestDialogNetResource()) return 1;
    if (!TestDialogNetHashFailure()) return 1;
    std::puts("IEM asset compiler tests passed");
    return 0;
}
