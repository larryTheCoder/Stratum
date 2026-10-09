// Stratum — what the surface pass reads off one column of the first pass.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The surface pass does not read blocks to decide a condition; it reads what
// the first pass's blocks imply about their column, and only through each
// block's CATEGORY: the stone-depth run counted down from the world's top
// (`stone_depth` floor), the one counted up from its floor (`stone_depth`
// ceiling), the latched water height (`water`), and the topmost non-air
// block, where the scan starts. This header is that category and that
// counting, out of terrain::ChunkFiller's own source so that a second reader
// can call the same code instead of restating it.
//
// The second reader is tools/analysis/legacy-goldens-surface-decoder.hpp,
// which reconstructs these reads out of a golden region to invert the
// surface rules. It kept its own copy, and the copy fell behind when the
// filler changed (pipeline engine v7, SPEC §11 "Lava in the surface pass's
// runs"): lava stayed Solid in the overworld and fluid still held the
// bottom-up run. Calling this is what keeps the two from parting again.
//
// MEASURED, every rule here (SPEC §11): fluid is `default_fluid` and the
// aquifer's literal lava alike, whichever dimension; top down, air resets the
// run, solid counts, fluid neither counts nor breaks it; bottom up, ANY
// non-solid block resets it, water as much as air; the water height is one
// above the first fluid block met descending, latched.
#pragma once

#include <stratum/settings/noise_settings.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace stratum::terrain {

/// Solid, fluid or air — what the FIRST pass decided, read back for the
/// second. Not stored anywhere: rederived from the block a position already
/// holds. Fluid is `default_fluid` AND the aquifer's literal lava
/// (`minecraft:lava`, level 0), the only two fluids the first pass writes;
/// the ore veins' blocks are Solid. Compared as whole states, so a fluid at
/// any other level — which the first pass never writes — is Solid here; a
/// caller reading a saved world, where ticks have moved fluid, has to undo
/// that itself before it asks (the golden decoder does, and says how).
enum class Category : std::uint8_t { Air, Fluid, Solid };

[[nodiscard]] Category categorize(const settings::BlockState& block,
                                  const settings::NoiseSettings& settings);

/// One column's surface-pass reads, from its categories.
///
/// Reusable: `read` overwrites everything, so a caller walking many columns
/// keeps one of these and its storage rather than allocating per column.
class SurfaceColumn {
public:
    /// Reads a column whose @p categories run bottom to top: entry i is
    /// y = @p minY + i.
    void read(std::span<const Category> categories, std::int32_t minY);

    /// The run counted from the top, at @p y (`stone_depth` floor). Throws
    /// std::out_of_range for a y outside the column read.
    [[nodiscard]] std::int32_t stoneDepthAbove(std::int32_t y) const;

    /// The run counted from the floor, at @p y (`stone_depth` ceiling).
    /// Throws std::out_of_range for a y outside the column read.
    [[nodiscard]] std::int32_t stoneDepthBelow(std::int32_t y) const;

    /// One above the topmost fluid block, or absent for a column with none.
    [[nodiscard]] std::optional<std::int32_t> waterHeight() const noexcept { return waterHeight_; }

    /// The topmost non-air y, or minY - 1 for a column of nothing but air.
    [[nodiscard]] std::int32_t top() const noexcept { return top_; }

private:
    std::int32_t minY_ = 0;
    std::vector<std::int32_t> above_;
    std::vector<std::int32_t> below_;
    std::optional<std::int32_t> waterHeight_;
    std::int32_t top_ = 0;
};

} // namespace stratum::terrain
