#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace iem::tools {

using Sha256Digest = std::array<uint8_t, 32>;

Sha256Digest Sha256(const uint8_t *data, std::size_t size) noexcept;
std::string Sha256Hex(const uint8_t *data, std::size_t size);
std::string Sha256File(const std::filesystem::path &path);

} // namespace iem::tools
