// Stratum — MD5.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Present for one reason: vanilla salts its random sources with the MD5 of a
// name — "octave_-7", and later resource locations — and takes the digest as
// two 64-bit halves. Getting that wrong shifts every derived generator, so
// it is implemented here rather than approximated.
//
// Implemented from RFC 1321 and verified against md5sum, which is a genuinely
// independent oracle. This is not a security primitive and must not be used
// as one: MD5 is broken for every purpose that depends on collision
// resistance. It is used here only to reproduce vanilla's seed derivation.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace stratum::hash {

using Md5Digest = std::array<std::uint8_t, 16>;

[[nodiscard]] Md5Digest md5(std::span<const std::byte> data) noexcept;

[[nodiscard]] Md5Digest md5(std::string_view text) noexcept;

/// Lowercase hexadecimal, as md5sum prints it.
[[nodiscard]] std::string toHex(const Md5Digest& digest);

} // namespace stratum::hash
