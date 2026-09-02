// Stratum — NBT byte-stream builder for tests.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Tests assemble NBT byte by byte so each one states exactly what the reader
// is being handed, rather than round-tripping through a writer whose bugs
// would cancel the reader's out.

#pragma once

#include <stratum/nbt/tag.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace stratum::test {

class NbtWriter {
public:
    NbtWriter& u8(std::uint8_t value) {
        bytes_.push_back(static_cast<std::byte>(value));
        return *this;
    }

    NbtWriter& type(nbt::TagType value) { return u8(static_cast<std::uint8_t>(value)); }

    NbtWriter& u16(std::uint16_t value) {
        return u8(static_cast<std::uint8_t>(value >> 8U)).u8(static_cast<std::uint8_t>(value));
    }

    NbtWriter& u32(std::uint32_t value) {
        return u8(static_cast<std::uint8_t>(value >> 24U))
            .u8(static_cast<std::uint8_t>(value >> 16U))
            .u8(static_cast<std::uint8_t>(value >> 8U))
            .u8(static_cast<std::uint8_t>(value));
    }

    NbtWriter& u64(std::uint64_t value) {
        return u32(static_cast<std::uint32_t>(value >> 32U)).u32(static_cast<std::uint32_t>(value));
    }

    NbtWriter& str(std::string_view value) {
        u16(static_cast<std::uint16_t>(value.size()));
        for (const char character : value) {
            u8(static_cast<std::uint8_t>(character));
        }
        return *this;
    }

    /// A named tag header: type byte followed by the name.
    NbtWriter& named(nbt::TagType value, std::string_view name) { return type(value).str(name); }

    NbtWriter& end() { return type(nbt::TagType::End); }

    [[nodiscard]] std::span<const std::byte> span() const noexcept { return bytes_; }

    [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept { return bytes_; }

    [[nodiscard]] std::vector<std::byte>& bytes() noexcept { return bytes_; }

private:
    std::vector<std::byte> bytes_;
};

} // namespace stratum::test
