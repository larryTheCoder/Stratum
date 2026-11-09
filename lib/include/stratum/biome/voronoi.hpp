// Stratum — the block-level biome "zoom": which quart a block's biome is
// read from.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// A biome source answers per quart (a 4x4x4 cell). A block does not simply
// read its own quart: each lattice point of the quart grid is jittered by a
// hash of the world seed, and a block reads the quart whose jittered point is
// nearest to it. So near a biome border the block-level biome is a ragged
// voronoi edge rather than the quart grid's straight one. The surface rules
// read this block-level biome (SPEC §11, "The surface rules read the zoomed
// biome"; vanilla_legacy_nether_surface_test.cpp).
//
// ADAPTED (CLAUDE.md, NOTICE) from cubiomes (https://github.com/Cubitect/
// cubiomes, MIT License, Copyright (c) 2020 Cubitect), at
// e61f90580cbdd883214a8054670dacae655e59c0: layers.c `getVoronoiSHA`,
// `getVoronoiCell` and `voronoiAccess3D`, with rng.h `mcStepSeed`. Rewritten
// to this project's determinism rules: every shift and mask of a possibly
// negative coordinate is javamath::floorDiv / floorMod, the step is explicit
// 64-bit wrapping arithmetic, and the squared distances are int64.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace stratum::biome {

/// A quart's coordinates: block coordinates divided by four, floored.
struct QuartPos {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;

    [[nodiscard]] bool operator==(const QuartPos&) const = default;
};

class VoronoiZoom {
public:
    /// The obfuscated seed the zoom is keyed by: the first eight bytes of the
    /// SHA-256 of @p worldSeed's eight little-endian bytes, read as a
    /// little-endian integer. All 64 bits of the world seed reach it, unlike
    /// anything seeded through java.util.Random.
    [[nodiscard]] static std::uint64_t obfuscatedSeed(std::int64_t worldSeed) noexcept;

    explicit VoronoiZoom(std::int64_t worldSeed) noexcept : seed_(obfuscatedSeed(worldSeed)) {}

    /// The jitter of the quart-grid lattice point (@p x, @p y, @p z), in
    /// 1/10240ths of a quart, each component in [-18432, 18396].
    [[nodiscard]] std::array<std::int32_t, 3> jitter(std::int32_t x, std::int32_t y,
                                                     std::int32_t z) const noexcept;

    /// The quart whose biome the block (@p x, @p y, @p z) reads. Of the
    /// eight lattice points around the block (offset by two blocks), the
    /// nearest after jitter; on a tie, the first in the order
    /// (0,0,0), (0,0,1), (0,1,0), ... (1,1,1) in (x, y, z).
    [[nodiscard]] QuartPos zoom(std::int32_t x, std::int32_t y, std::int32_t z) const noexcept;

    [[nodiscard]] std::uint64_t seed() const noexcept { return seed_; }

private:
    std::uint64_t seed_;
};

/// VoronoiZoom::zoom over a box of blocks, with the work it repeats done
/// once: every lattice point's jitter the box can reach is computed up front,
/// and the horizontal part of the eight distances is kept while successive
/// blocks share a column and a lattice layer, which a column walked downward
/// does four blocks at a time. Per-task scratch (SPEC §4.1): built per fill,
/// never shared. Its answers are VoronoiZoom::zoom's exactly, and a block
/// outside the box is answered by VoronoiZoom::zoom itself.
class VoronoiWindow {
public:
    /// The box is [@p minX, @p maxX] x [@p minY, @p maxY] x [@p minZ, @p maxZ],
    /// in blocks, inclusive.
    VoronoiWindow(const VoronoiZoom& zoom, std::int32_t minX, std::int32_t minY, std::int32_t minZ,
                  std::int32_t maxX, std::int32_t maxY, std::int32_t maxZ);

    [[nodiscard]] QuartPos zoom(std::int32_t x, std::int32_t y, std::int32_t z) noexcept;

private:
    const VoronoiZoom* zoom_;
    std::int32_t minX_, minY_, minZ_, maxX_, maxY_, maxZ_;
    /// Lattice origin and extent: (block - 2) floor-divided by four, over the
    /// box, plus one on the high side for each cell's far corners.
    std::int32_t latticeX_, latticeY_, latticeZ_;
    std::size_t spanX_, spanY_, spanZ_;
    std::vector<std::array<std::int32_t, 3>> jitter_;

    // The horizontal parts kept for the last (x, z, lattice layer) asked.
    bool horizontalKnown_ = false;
    std::int32_t lastX_ = 0, lastZ_ = 0, lastLayer_ = 0;
    std::array<std::int64_t, 8> horizontal_{};
    std::array<std::int32_t, 8> jitterY_{};

    [[nodiscard]] const std::array<std::int32_t, 3>& at(std::int64_t lx, std::int64_t ly,
                                                        std::int64_t lz) const noexcept;
};

} // namespace stratum::biome
