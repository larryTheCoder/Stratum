// Stratum — chunk decoding, from NBT to blocks.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Turns the NBT of one chunk into addressable block states, in Java block
// space — which is where Tier-A parity is defined (SPEC §7). Nothing here
// touches Bedrock mapping; that happens strictly downstream (SPEC §9).
//
// Format reference: the Anvil chunk format as documented on minecraft.wiki.
// No Mojang code was consulted.

#pragma once

#include <stratum/nbt/tag.hpp>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace stratum::chunk {

/// Raised when a chunk's NBT does not describe a chunk we can read. Always
/// names the chunk and what was wrong: a diff that quietly treated an
/// undecodable chunk as empty would report parity it never established.
class FormatError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

inline constexpr int kSectionSize = 16;
inline constexpr std::size_t kBlocksPerSection = 16 * 16 * 16;
inline constexpr std::size_t kBiomesPerSection = 4 * 4 * 4;

/// A block state: its identifier plus its properties, kept as written.
/// Properties are sorted by key so that two states compare equal when they
/// mean the same thing, whatever order they were serialised in.
struct BlockState {
    std::string name;
    std::vector<std::pair<std::string, std::string>> properties;

    [[nodiscard]] bool operator==(const BlockState& other) const = default;

    /// "minecraft:oak_stairs[facing=north,half=top]" — the spelling used in
    /// diff output, so a divergence is greppable.
    [[nodiscard]] std::string toString() const;
};

/// One 16x16x16 section, already unpacked: `blocks` holds a palette index
/// per position, in YZX order.
struct Section {
    int y = 0;
    std::vector<BlockState> palette;
    std::vector<std::uint16_t> blocks;
    std::vector<std::string> biomePalette;
    std::vector<std::uint16_t> biomes;

    [[nodiscard]] const BlockState& blockAt(int x, int y, int z) const;
};

class Chunk {
public:
    /// Decodes one chunk from the root tag of its NBT.
    [[nodiscard]] static Chunk decode(const nbt::Tag& root);

    [[nodiscard]] std::int32_t x() const noexcept { return x_; }

    [[nodiscard]] std::int32_t z() const noexcept { return z_; }

    [[nodiscard]] std::int32_t dataVersion() const noexcept { return dataVersion_; }

    [[nodiscard]] const std::string& status() const noexcept { return status_; }

    [[nodiscard]] const std::vector<Section>& sections() const noexcept { return sections_; }

    /// Lowest and highest block y this chunk stores, inclusive.
    [[nodiscard]] int minY() const noexcept;
    [[nodiscard]] int maxY() const noexcept;

    /// The block at chunk-local x/z (0..15) and world y. Returns nullptr
    /// when y falls outside the sections this chunk stores.
    [[nodiscard]] const BlockState* blockAt(int localX, int y, int localZ) const;

    /// Highest y at which the column holds something other than air, or
    /// nullopt for an entirely empty column.
    [[nodiscard]] std::optional<int> highestNonAir(int localX, int localZ) const;

private:
    std::int32_t x_ = 0;
    std::int32_t z_ = 0;
    std::int32_t dataVersion_ = 0;
    std::string status_;
    std::vector<Section> sections_;
};

/// Unpacks vanilla's packed index array. Entries are @p bitsPerEntry wide
/// and never straddle a long: any bits left at the top of a long are
/// padding, which is the rule that changed in 1.16 and silently shifts every
/// block if it is got wrong.
[[nodiscard]] std::vector<std::uint16_t> unpackIndices(const std::vector<std::int64_t>& packed,
                                                       int bitsPerEntry, std::size_t count);

/// Bits per entry for a palette of @p paletteSize, given a floor. Blocks use
/// a floor of 4, biomes 1.
[[nodiscard]] int bitsPerEntryFor(std::size_t paletteSize, int floorBits) noexcept;

} // namespace stratum::chunk
