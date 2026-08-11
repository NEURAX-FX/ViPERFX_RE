#pragma once

#include <filesystem>
#include <string>

namespace iem::tools {

struct CompileResult {
    bool success = false;
    std::string error;
};

CompileResult CompilePinnedAssets(
    const std::filesystem::path &source_directory,
    const std::filesystem::path &output_directory
);

} // namespace iem::tools
