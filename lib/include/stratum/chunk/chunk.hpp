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
#include <string_view>
#include <utility>
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
inline constexpr std::size_t kBlocksPerSection = std::size_t{16} * 16 * 16;
inline constexpr std::size_t kBiomesPerSection = std::size_t{4} * 4 * 4;

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

/// The heightmaps vanilla stores beside a chunk's blocks. Each is 256
/// entries — one per column, in ZX order — packed nine bits at a time.
///
/// They matter beyond bookkeeping: they are the only thing in a golden
/// region that states a *surface height* directly, which makes them the
/// oracle a generated terrain column can be compared against without first
/// getting aquifers, surface rules and block mapping right (SPEC §7).
enum class Heightmap : std::uint8_t {
    /// Highest block that is not air.
    WorldSurface,
    /// Highest block that is neither air nor fluid — the terrain surface
    /// under an ocean, which is the one a density function speaks to.
    OceanFloor,
    /// Highest block that blocks motion, fluids included.
    MotionBlocking,
    /// The same, ignoring leaves.
    MotionBlockingNoLeaves,
};

/// "OCEAN_FLOOR" — the name vanilla stores it under.
[[nodiscard]] std::string_view heightmapName(Heightmap kind) noexcept;

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

    /// The stored heightmap of this kind, as world y per column in ZX order,
    /// or nothing when the chunk does not carry it. A column with no
    /// qualifying block at all is nullopt within the array.
    ///
    /// Vanilla stores `y + 1 - minY` rather than y, so that zero can mean
    /// "nothing here" — measured, not assumed: across all 16384 columns of
    /// one golden region the stored WORLD_SURFACE value was exactly 65 more
    /// than this decoder's own highestNonAir, and 65 is `1 - minY` for a
    /// world whose yPos is -4. The Nether and the End, whose yPos is 0,
    /// offset by 1.
    [[nodiscard]] std::optional<std::vector<std::optional<int>>> heightmap(Heightmap kind) const;

    /// The lowest section index the chunk declares, from `yPos`. This is
    /// what the heightmaps are measured from, so it is kept rather than
    /// inferred from the sections present — a padding section would move an
    /// inferred one and shift every height by sixteen.
    [[nodiscard]] std::int32_t lowestSection() const noexcept { return lowestSection_; }

private:
    std::int32_t x_ = 0;
    std::int32_t z_ = 0;
    std::int32_t dataVersion_ = 0;
    std::int32_t lowestSection_ = 0;
    std::string status_;
    std::vector<Section> sections_;
    /// Kept as stored, decoded on demand: most callers want none of them.
    std::vector<std::pair<Heightmap, std::vector<std::int64_t>>> heightmaps_;
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
