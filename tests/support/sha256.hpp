// Stratum — SHA-256 for tests (FIPS 180-4), to check digests a spec records.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace stratum::test {

/// The SHA-256 of @p bytes, as lowercase hex.
[[nodiscard]] inline std::string sha256Hex(std::span<const std::uint8_t> bytes) {
    constexpr std::array<std::uint32_t, 64> kK = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    std::array<std::uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    std::vector<std::uint8_t> message(bytes.begin(), bytes.end());
    const std::uint64_t bitLength = static_cast<std::uint64_t>(bytes.size()) * 8U;
    message.push_back(0x80);
    while (message.size() % 64U != 56U) {
        message.push_back(0x00);
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        message.push_back(static_cast<std::uint8_t>(bitLength >> static_cast<unsigned>(shift)));
    }

    for (std::size_t block = 0; block < message.size(); block += 64U) {
        std::array<std::uint32_t, 64> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(message[block + (4 * i)]) << 24U) |
                   (static_cast<std::uint32_t>(message[block + (4 * i) + 1]) << 16U) |
                   (static_cast<std::uint32_t>(message[block + (4 * i) + 2]) << 8U) |
                   static_cast<std::uint32_t>(message[block + (4 * i) + 3]);
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 =
                std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3U);
            const std::uint32_t s1 =
                std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10U);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::array<std::uint32_t, 8> s = h;
        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t t1 =
                s[7] + (std::rotr(s[4], 6) ^ std::rotr(s[4], 11) ^ std::rotr(s[4], 25)) +
                ((s[4] & s[5]) ^ (~s[4] & s[6])) + kK[i] + w[i];
            const std::uint32_t t2 =
                (std::rotr(s[0], 2) ^ std::rotr(s[0], 13) ^ std::rotr(s[0], 22)) +
                ((s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]));
            s[7] = s[6];
            s[6] = s[5];
            s[5] = s[4];
            s[4] = s[3] + t1;
            s[3] = s[2];
            s[2] = s[1];
            s[1] = s[0];
            s[0] = t1 + t2;
        }
        for (std::size_t i = 0; i < 8; ++i) {
            h[i] += s[i];
        }
    }

    constexpr const char* kHex = "0123456789abcdef";
    std::string out;
    for (const std::uint32_t word : h) {
        for (int shift = 28; shift >= 0; shift -= 4) {
            out.push_back(kHex[(word >> static_cast<unsigned>(shift)) & 0xFU]);
        }
    }
    return out;
}

} // namespace stratum::test
