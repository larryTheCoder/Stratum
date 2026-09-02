// Stratum — region rendering.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Turns a region file into a picture, for looking at terrain rather than
// reading coordinates out of a diff. A rendered image is not evidence of
// parity — that is what `stratum diff` is for — but it is how you notice
// that a whole biome moved.
//
// Rendering is deterministic: colours are derived from block and biome names
// by integer hashing, never from a random palette, so two runs of the same
// input produce identical images.

#pragma once

#include <stratum/image/png.hpp>
#include <stratum/region/region_file.hpp>

#include <string_view>

namespace stratum::render {

enum class Mode {
    /// Surface height, dark to light across the region's own range.
    Heightmap,
    /// The surface biome, one colour per biome id.
    Biome,
    /// The topmost non-air block, one colour per block id.
    Blocks,
};

[[nodiscard]] bool parseMode(std::string_view name, Mode& mode) noexcept;
[[nodiscard]] std::string_view modeName(Mode mode) noexcept;

/// Renders a whole region: 32 chunks square, one pixel per column, so 512 by
/// 512. Columns in chunks that are absent or undecodable are left at the
/// background colour rather than guessed at.
[[nodiscard]] image::Image renderRegion(const region::RegionFile& region, Mode mode);

} // namespace stratum::render
