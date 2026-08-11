#include "Sha256.h"

#include <array>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace iem::tools {
namespace {

constexpr std::array<uint32_t, 64> kRound{
    0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U, 0x3956C25BU,
    0x59F111F1U, 0x923F82A4U, 0xAB1C5ED5U, 0xD807AA98U, 0x12835B01U,
    0x243185BEU, 0x550C7DC3U, 0x72BE5D74U, 0x80DEB1FEU, 0x9BDC06A7U,
    0xC19BF174U, 0xE49B69C1U, 0xEFBE4786U, 0x0FC19DC6U, 0x240CA1CCU,
    0x2DE92C6FU, 0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU, 0x983E5152U,
    0xA831C66DU, 0xB00327C8U, 0xBF597FC7U, 0xC6E00BF3U, 0xD5A79147U,
    0x06CA6351U, 0x14292967U, 0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU,
    0x53380D13U, 0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U,
    0xA2BFE8A1U, 0xA81A664BU, 0xC24B8B70U, 0xC76C51A3U, 0xD192E819U,
    0xD6990624U, 0xF40E3585U, 0x106AA070U, 0x19A4C116U, 0x1E376C08U,
    0x2748774CU, 0x34B0BCB5U, 0x391C0CB3U, 0x4ED8AA4AU, 0x5B9CCA4FU,
    0x682E6FF3U, 0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U,
    0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U, 0xC67178F2U,
};

uint32_t RotateRight(uint32_t value, uint32_t amount) noexcept {
    return (value >> amount) | (value << (32U - amount));
}

void Transform(const uint8_t block[64], std::array<uint32_t, 8> &state) noexcept {
    std::array<uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16; ++index) {
        words[index] = static_cast<uint32_t>(block[index * 4U]) << 24U
            | static_cast<uint32_t>(block[index * 4U + 1U]) << 16U
            | static_cast<uint32_t>(block[index * 4U + 2U]) << 8U
            | static_cast<uint32_t>(block[index * 4U + 3U]);
    }
    for (std::size_t index = 16; index < words.size(); ++index) {
        const uint32_t s0 = RotateRight(words[index - 15U], 7U)
            ^ RotateRight(words[index - 15U], 18U) ^ (words[index - 15U] >> 3U);
        const uint32_t s1 = RotateRight(words[index - 2U], 17U)
            ^ RotateRight(words[index - 2U], 19U) ^ (words[index - 2U] >> 10U);
        words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (std::size_t index = 0; index < words.size(); ++index) {
        const uint32_t sum1 = RotateRight(e, 6U) ^ RotateRight(e, 11U)
            ^ RotateRight(e, 25U);
        const uint32_t choice = (e & f) ^ (~e & g);
        const uint32_t temporary1 = h + sum1 + choice + kRound[index] + words[index];
        const uint32_t sum0 = RotateRight(a, 2U) ^ RotateRight(a, 13U)
            ^ RotateRight(a, 22U);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temporary2 = sum0 + majority;
        h = g; g = f; f = e; e = d + temporary1;
        d = c; c = b; b = a; a = temporary1 + temporary2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

} // namespace

Sha256Digest Sha256(const uint8_t *data, std::size_t size) noexcept {
    std::array<uint32_t, 8> state{
        0x6A09E667U, 0xBB67AE85U, 0x3C6EF372U, 0xA54FF53AU,
        0x510E527FU, 0x9B05688CU, 0x1F83D9ABU, 0x5BE0CD19U,
    };
    std::size_t offset = 0;
    while (size - offset >= 64U) {
        Transform(data + offset, state);
        offset += 64U;
    }
    std::array<uint8_t, 128> tail{};
    const std::size_t remaining = size - offset;
    for (std::size_t index = 0; index < remaining; ++index) tail[index] = data[offset + index];
    tail[remaining] = 0x80U;
    const std::size_t padded = remaining < 56U ? 64U : 128U;
    const uint64_t bit_length = static_cast<uint64_t>(size) * 8U;
    for (int index = 0; index < 8; ++index) {
        tail[padded - 1U - static_cast<std::size_t>(index)]
            = static_cast<uint8_t>(bit_length >> (index * 8));
    }
    Transform(tail.data(), state);
    if (padded == 128U) Transform(tail.data() + 64U, state);

    Sha256Digest digest{};
    for (std::size_t index = 0; index < state.size(); ++index) {
        digest[index * 4U] = static_cast<uint8_t>(state[index] >> 24U);
        digest[index * 4U + 1U] = static_cast<uint8_t>(state[index] >> 16U);
        digest[index * 4U + 2U] = static_cast<uint8_t>(state[index] >> 8U);
        digest[index * 4U + 3U] = static_cast<uint8_t>(state[index]);
    }
    return digest;
}

std::string Sha256Hex(const uint8_t *data, std::size_t size) {
    const Sha256Digest digest = Sha256(data, size);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (uint8_t byte : digest) output << std::setw(2) << static_cast<unsigned>(byte);
    return output.str();
}

std::string Sha256File(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    input.seekg(0, std::ios::end);
    const std::streamoff length = input.tellg();
    if (length < 0) return {};
    input.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<std::size_t>(length));
    if (!bytes.empty()) input.read(reinterpret_cast<char *>(bytes.data()), length);
    if (!input && !bytes.empty()) return {};
    return Sha256Hex(bytes.data(), bytes.size());
}

} // namespace iem::tools
