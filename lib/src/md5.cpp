// Stratum — MD5.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// From RFC 1321. The sine table below is the RFC's K[i] = floor(2^32 *
// |sin(i + 1)|), generated rather than transcribed; a wrong entry would be
// caught by the md5sum vectors either way.

#include <stratum/hash/md5.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace stratum::hash {

namespace {

constexpr std::array<std::uint32_t, 64> kSine = {
    UINT32_C(0xD76AA478), UINT32_C(0xE8C7B756), UINT32_C(0x242070DB), UINT32_C(0xC1BDCEEE),
    UINT32_C(0xF57C0FAF), UINT32_C(0x4787C62A), UINT32_C(0xA8304613), UINT32_C(0xFD469501),
    UINT32_C(0x698098D8), UINT32_C(0x8B44F7AF), UINT32_C(0xFFFF5BB1), UINT32_C(0x895CD7BE),
    UINT32_C(0x6B901122), UINT32_C(0xFD987193), UINT32_C(0xA679438E), UINT32_C(0x49B40821),
    UINT32_C(0xF61E2562), UINT32_C(0xC040B340), UINT32_C(0x265E5A51), UINT32_C(0xE9B6C7AA),
    UINT32_C(0xD62F105D), UINT32_C(0x02441453), UINT32_C(0xD8A1E681), UINT32_C(0xE7D3FBC8),
    UINT32_C(0x21E1CDE6), UINT32_C(0xC33707D6), UINT32_C(0xF4D50D87), UINT32_C(0x455A14ED),
    UINT32_C(0xA9E3E905), UINT32_C(0xFCEFA3F8), UINT32_C(0x676F02D9), UINT32_C(0x8D2A4C8A),
    UINT32_C(0xFFFA3942), UINT32_C(0x8771F681), UINT32_C(0x6D9D6122), UINT32_C(0xFDE5380C),
    UINT32_C(0xA4BEEA44), UINT32_C(0x4BDECFA9), UINT32_C(0xF6BB4B60), UINT32_C(0xBEBFBC70),
    UINT32_C(0x289B7EC6), UINT32_C(0xEAA127FA), UINT32_C(0xD4EF3085), UINT32_C(0x04881D05),
    UINT32_C(0xD9D4D039), UINT32_C(0xE6DB99E5), UINT32_C(0x1FA27CF8), UINT32_C(0xC4AC5665),
    UINT32_C(0xF4292244), UINT32_C(0x432AFF97), UINT32_C(0xAB9423A7), UINT32_C(0xFC93A039),
    UINT32_C(0x655B59C3), UINT32_C(0x8F0CCC92), UINT32_C(0xFFEFF47D), UINT32_C(0x85845DD1),
    UINT32_C(0x6FA87E4F), UINT32_C(0xFE2CE6E0), UINT32_C(0xA3014314), UINT32_C(0x4E0811A1),
    UINT32_C(0xF7537E82), UINT32_C(0xBD3AF235), UINT32_C(0x2AD7D2BB), UINT32_C(0xEB86D391),
};

/// Per-round left-rotation amounts, RFC 1321 section 3.4.
constexpr std::array<unsigned, 64> kShifts = {
    7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22, 5,  9,  14, 20, 5,  9,
    14, 20, 5,  9,  14, 20, 5,  9,  14, 20, 4,  11, 16, 23, 4,  11, 16, 23, 4,  11, 16, 23,
    4,  11, 16, 23, 6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21,
};

/// libstdc++ and libc++ disagree about whether std::rotl's shift parameter is
/// int or unsigned, so a runtime shift trips -Wsign-conversion on one of
/// them. The rotation is spelled out instead. The mask keeps a shift of 0
/// well defined, though RFC 1321 never uses one.
[[nodiscard]] constexpr std::uint32_t rotateLeft(std::uint32_t value, unsigned shift) noexcept {
    return (value << shift) | (value >> ((32U - shift) & 31U));
}

[[nodiscard]] std::uint32_t readLittleEndian32(const std::uint8_t* data) noexcept {
    return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8U) |
           (static_cast<std::uint32_t>(data[2]) << 16U) |
           (static_cast<std::uint32_t>(data[3]) << 24U);
}

struct State {
    std::uint32_t a = UINT32_C(0x67452301);
    std::uint32_t b = UINT32_C(0xEFCDAB89);
    std::uint32_t c = UINT32_C(0x98BADCFE);
    std::uint32_t d = UINT32_C(0x10325476);
};

void processBlock(State& state, const std::uint8_t* block) noexcept {
    std::array<std::uint32_t, 16> words{};
    for (std::size_t i = 0; i < 16; ++i) {
        words[i] = readLittleEndian32(block + (i * 4U));
    }

    std::uint32_t a = state.a;
    std::uint32_t b = state.b;
    std::uint32_t c = state.c;
    std::uint32_t d = state.d;

    for (std::size_t i = 0; i < 64; ++i) {
        std::uint32_t mixed = 0;
        std::size_t wordIndex = 0;

        if (i < 16) {
            mixed = (b & c) | (~b & d);
            wordIndex = i;
        } else if (i < 32) {
            mixed = (d & b) | (~d & c);
            wordIndex = ((5U * i) + 1U) % 16U;
        } else if (i < 48) {
            mixed = b ^ c ^ d;
            wordIndex = ((3U * i) + 5U) % 16U;
        } else {
            mixed = c ^ (b | ~d);
            wordIndex = (7U * i) % 16U;
        }

        const std::uint32_t rotated = a + mixed + kSine[i] + words[wordIndex];
        a = d;
        d = c;
        c = b;
        b += rotateLeft(rotated, kShifts[i]);
    }

    state.a += a;
    state.b += b;
    state.c += c;
    state.d += d;
}

void appendLittleEndian32(Md5Digest& digest, std::size_t at, std::uint32_t value) noexcept {
    digest[at] = static_cast<std::uint8_t>(value & 0xFFU);
    digest[at + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
    digest[at + 2U] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
    digest[at + 3U] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
}

} // namespace

Md5Digest md5(std::span<const std::byte> data) noexcept {
    State state;

    const auto* bytes = reinterpret_cast<const std::uint8_t*>(data.data());
    const std::size_t length = data.size();

    std::size_t offset = 0;
    for (; offset + 64U <= length; offset += 64U) {
        processBlock(state, bytes + offset);
    }

    // Padding: 0x80, then zeroes, then the length in bits as 64 little-endian
    // bits. Two blocks are always enough for whatever tail is left.
    std::array<std::uint8_t, 128> tail{};
    const std::size_t remaining = length - offset;
    if (remaining > 0) {
        std::memcpy(tail.data(), bytes + offset, remaining);
    }
    tail[remaining] = 0x80U;

    const std::size_t tailBlocks = (remaining + 1U + 8U > 64U) ? 2U : 1U;
    const std::uint64_t bitLength = static_cast<std::uint64_t>(length) * 8U;
    for (std::size_t i = 0; i < 8; ++i) {
        tail[(tailBlocks * 64U) - 8U + i] =
            static_cast<std::uint8_t>((bitLength >> (8U * i)) & 0xFFU);
    }
    for (std::size_t block = 0; block < tailBlocks; ++block) {
        processBlock(state, tail.data() + (block * 64U));
    }

    Md5Digest digest{};
    appendLittleEndian32(digest, 0, state.a);
    appendLittleEndian32(digest, 4, state.b);
    appendLittleEndian32(digest, 8, state.c);
    appendLittleEndian32(digest, 12, state.d);
    return digest;
}

Md5Digest md5(std::string_view text) noexcept {
    return md5(std::as_bytes(std::span{text.data(), text.size()}));
}

std::string toHex(const Md5Digest& digest) {
    static constexpr std::string_view kHexDigits = "0123456789abcdef";
    std::string text;
    text.reserve(32);
    for (const std::uint8_t byte : digest) {
        text += kHexDigits[byte >> 4U];
        text += kHexDigits[byte & 0x0FU];
    }
    return text;
}

} // namespace stratum::hash
