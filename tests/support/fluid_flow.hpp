// Stratum — telling an aquifer that decided wrong from fluid that moved.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// A golden region is what the server SAVED, and by then some of its fluids
// have moved: spec Q8.1's post-processing flag schedules a fluid tick for
// every flagged position when a chunk is loaded, and water and lava flow
// before the region reaches disk. Freezing the world first (fetch-vanilla
// does, and so do the probe harnesses: support/probe_corpus.hpp) stops the
// ticks but not all of the movement — a small remnant, different from one
// frozen run to the next, survives. Stratum carries the flag but runs no
// fluid ticks, so a first pass that is exactly right still disagrees with a
// saved region wherever fluid moved — and the bare category counts these
// files used to pin could not tell that from an aquifer decision going wrong.
//
// `explainedByFlow` attributes one raw-category disagreement to the five
// shapes fluid movement leaves, and to nothing else. Measured, not chosen: on
// all eight golden overworld regions and the aquifer-on probe's 64-chunk
// sweep it accounts for every disagreement the raw first pass leaves (SPEC
// §11, "The deep-dark override"), while each wrong reading of Q5.9 tried
// there left disagreements it does not account for — which is what makes it
// a test rather than an excuse.
#pragma once

#include <stratum/chunk/chunk.hpp>
#include <stratum/javamath.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace stratum::test {

enum class Category : std::uint8_t { Air, Water, Lava, Solid };

/// By the block's NAME, exactly — never a substring of its state string,
/// where `waterlogged=false` reads as water and every `*_stairs` as air.
/// `void_air` is air (the world's own below-floor air); a `bubble_column`
/// is water that moved, which only fluid ticks make, so it stays Solid here
/// and `explainedByFlow` does not cover it — none appears in any golden.
[[nodiscard]] inline Category categoryOf(const std::string& name) {
    if (name == "minecraft:air" || name == "minecraft:cave_air" || name == "minecraft:void_air") {
        return Category::Air;
    }
    if (name == "minecraft:water") {
        return Category::Water;
    }
    if (name == "minecraft:lava") {
        return Category::Lava;
    }
    return Category::Solid;
}

[[nodiscard]] inline bool isFluid(Category category) {
    return category == Category::Water || category == Category::Lava;
}

/// One region file, read lazily a chunk at a time, so that a block's
/// neighbours can be looked at across a chunk edge. Null outside it.
class GoldenRegion {
public:
    explicit GoldenRegion(const std::filesystem::path& path)
        : file_(region::RegionFile::open(path)) {}

    [[nodiscard]] const chunk::Chunk& chunk(std::int32_t cx, std::int32_t cz) {
        const auto key = std::make_pair(cx, cz);
        auto found = chunks_.find(key);
        if (found == chunks_.end()) {
            found =
                chunks_.emplace(key, chunk::Chunk::decode(nbt::read(file_.readChunk(cx, cz)).root))
                    .first;
        }
        return found->second;
    }

    [[nodiscard]] const chunk::BlockState* blockAt(std::int32_t x, std::int32_t y, std::int32_t z) {
        const std::int32_t cx = javamath::floorDiv(x, 16);
        const std::int32_t cz = javamath::floorDiv(z, 16);
        if (cx < 0 || cz < 0 || cx >= 32 || cz >= 32 || !file_.hasChunk(cx, cz)) {
            return nullptr;
        }
        return chunk(cx, cz).blockAt(javamath::floorMod(x, 16), y, javamath::floorMod(z, 16));
    }

    [[nodiscard]] bool hasChunk(std::int32_t cx, std::int32_t cz) const {
        return file_.hasChunk(cx, cz);
    }

private:
    region::RegionFile file_;
    std::map<std::pair<std::int32_t, std::int32_t>, chunk::Chunk> chunks_;
};

/// A fluid's `level` property: 0 for a source, 1-15 for flowing or falling,
/// -1 for a block that has none.
[[nodiscard]] inline int fluidLevel(const chunk::BlockState* block) {
    if (block == nullptr) {
        return -1;
    }
    for (const auto& [key, value] : block->properties) {
        if (key == "level") {
            return std::stoi(value);
        }
    }
    return -1;
}

[[nodiscard]] inline bool named(const chunk::BlockState* block, std::string_view name) {
    return block != nullptr && block->name == name;
}

/// What water meeting lava leaves behind.
[[nodiscard]] inline bool fluidContactBlock(const chunk::BlockState* block) {
    return named(block, "minecraft:obsidian") || named(block, "minecraft:cobblestone");
}

/// Whether a column's SURFACE — the first block from the sky that is neither
/// air nor fluid, which several golden cases count over — is one that fluid
/// flow made after generation instead of one the generator placed: what
/// water meeting lava leaves, or stone with flowing lava directly on it
/// (lava that fell onto water). Those cases pin their counts exactly because
/// flow never reaches a surface: on both golden region sets generated
/// (SPEC §7, §11) not one of 2097152 overworld column surfaces is flow-made.
/// They check that here rather than assume it, so a regeneration whose
/// remnant ever does reach one fails by that name instead of moving a count.
[[nodiscard]] inline bool flowMadeSurface(const chunk::BlockState* surface,
                                          const chunk::BlockState* above) {
    return fluidContactBlock(surface) || (named(surface, "minecraft:stone") &&
                                          named(above, "minecraft:lava") && fluidLevel(above) > 0);
}

/// Whether a raw-category disagreement at (x, y, z) is the server's fluid
/// moving AFTER generation rather than the aquifer deciding differently.
/// Five shapes, and nothing else is let through:
///
///   - golden fluid FLOWING (level > 0) where the first pass has air or the
///     other fluid — the aquifer places sources only, so a level above 0 is
///     movement whatever stood there first (water falling through the lava
///     sea's top row, on the level probe's water twin);
///   - golden water SOURCE where the first pass has air, between at least two
///     horizontal water sources — flow the infinite-source rule converted
///     back into a source;
///   - golden obsidian or cobblestone where the first pass has fluid or air —
///     water met lava, where one of them was or had flowed (stone is let
///     through only in the last shape, below);
///   - golden water where the first pass has lava, beside such a block;
///   - golden STONE where the first pass has air or fluid, with flowing lava
///     directly above it and water's meeting with lava beside it (obsidian,
///     cobblestone or flowing water) — lava fell onto water there. Stone
///     alone is never enough: it is also what a missed barrier looks like.
[[nodiscard]] inline bool explainedByFlow(GoldenRegion& golden, std::int32_t x, std::int32_t y,
                                          std::int32_t z, Category goldenCategory,
                                          Category rawCategory) {
    const chunk::BlockState* here = golden.blockAt(x, y, z);
    constexpr std::array<std::array<std::int32_t, 3>, 6> kNeighbours{
        {{1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}, {0, 1, 0}, {0, -1, 0}}};
    int horizontalWaterSources = 0;
    bool besideContact = false;
    bool besideFlowingWater = false;
    for (const auto& [dx, dy, dz] : kNeighbours) {
        const chunk::BlockState* next = golden.blockAt(x + dx, y + dy, z + dz);
        if (dy == 0 && named(next, "minecraft:water") && fluidLevel(next) == 0) {
            ++horizontalWaterSources;
        }
        besideContact = besideContact || fluidContactBlock(next);
        besideFlowingWater =
            besideFlowingWater || (named(next, "minecraft:water") && fluidLevel(next) > 0);
    }
    if (isFluid(goldenCategory) && rawCategory == Category::Air) {
        return fluidLevel(here) > 0 || (goldenCategory == Category::Water &&
                                        fluidLevel(here) == 0 && horizontalWaterSources >= 2);
    }
    if (isFluid(goldenCategory) && isFluid(rawCategory) && fluidLevel(here) > 0) {
        return true;
    }
    if (goldenCategory == Category::Solid &&
        (isFluid(rawCategory) || rawCategory == Category::Air)) {
        if (fluidContactBlock(here)) {
            return true;
        }
        const chunk::BlockState* above = golden.blockAt(x, y + 1, z);
        return named(here, "minecraft:stone") && named(above, "minecraft:lava") &&
               fluidLevel(above) > 0 && (besideContact || besideFlowingWater);
    }
    if (goldenCategory == Category::Water && rawCategory == Category::Lava) {
        return besideContact;
    }
    return false;
}

} // namespace stratum::test
