// Stratum — the End's island field.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// `minecraft:end_islands` is the one density function type vanilla ships that
// carries no parameters at all and is nevertheless seed-dependent: a 2D
// simplex field decides where the outer islands sit, and a fixed radial term
// draws the central one.
//
// PROVENANCE. The shape below is the community-documented End island field,
// implemented from that description and then SETTLED against the vanilla
// server's own output: eight golden End regions and two probe regions block by
// block (tests/conformance/golden_end_test.cpp), and the field itself read out
// of a probe dimension column by column
// (tools/analysis/end-islands-field-probe.sh). Nothing here is transcribed
// from Mojang code (CLAUDE.md's provenance rule) — and the one part that was
// taken on reasoning rather than measurement, the simplex seeding, was WRONG
// and the measurement is what caught it.
//
// WHAT THE GOLDEN REGIONS ALONE REACH, and what they do not. Region r.0.0 of
// a world covers blocks 0..511 on both axes, and that leaves TWO holes:
//
//   * THE OUTER ISLANDS. Their term is gated on
//     `cellX*cellX + cellZ*cellZ > 4096` with the cell coordinate being
//     `blockX / 16`, so it cannot fire below 1024 blocks from the origin even
//     once the +/-12 cell neighbourhood is allowed for (43*43 * 2 = 3698 <
//     4096). Everything about that term — the simplex gate, the steepness
//     hash, AND THE SEEDING — is unreached by r.0.0.
//   * NEGATIVE COORDINATES. `blockX / 8`, and the `/2` and `%2` beneath it,
//     are floorDiv/floorMod here, per CLAUDE.md's determinism rule. Java's
//     own `/` and `%` truncate toward zero, and the two readings disagree at
//     every negative coordinate — for the CENTRAL island too, since
//     `floorDiv(-1, 8)` is -1 where truncation gives 0. r.0.0 is entirely
//     non-negative and cannot separate them.
//
// tools/analysis/end-islands-probe.sh generates exactly the two regions that
// close those holes — r.64.0 (blocks 32768..33279, far outside the gate) and
// r.-1.-1 (blocks -512..-1, whose HIGH corner is where the central island
// reaches) — and golden_end_test.cpp scores against them when they are present
// and says so when they are not. tools/analysis/end-islands-field-probe.sh
// goes further and reads the field itself, unburied by terrain;
// `sampleSimplexSeeding` is exposed for it, so the seeding has one call site
// to point a probe at rather than a re-derivation.
//
// All of the arithmetic below is `float`, which is not a detail: the field is
// compared against a `float` zero crossing after being widened, and doing it
// in double moves the island edge.

#pragma once

#include <stratum/noise/perlin.hpp>

#include <cstdint>

namespace stratum::noise {

/// The End's island height field, and the density function built on it.
class EndIslands {
public:
    /// The simplex source: the world seed into `java.util.Random`, then
    /// **17292 LCG steps discarded**, then the ordinary three-doubles-plus-
    /// permutation construction.
    ///
    /// MEASURED, on the server's own field rather than on terrain — see
    /// tools/analysis/end-islands-field-probe.sh, which reads `end_islands`
    /// out of a probe dimension directly, and end-islands-analyze.cpp, which
    /// scores candidates against it. Without the skip the island positions
    /// are uncorrelated with vanilla's; with it the field agrees on every
    /// column the readback can resolve.
    ///
    /// THE SKIP IS NOT DERIVED FROM ANYTHING. It is a literal, and the only
    /// reason to believe it is that it was measured; nothing else about the
    /// field predicts it. The whole range [0, 65536] was rescored at two
    /// seeds: exactly one skip reaches full agreement in each, and it is the
    /// same one; the next best is 920 of 1024 at seed 0 and 908 at seed 42.
    ///
    /// It does NOT go through the dimension's declared random source, which
    /// is itself measured: a `legacy_random_source` probe dimension and its
    /// flag-off control read the same field on every shared column. So this
    /// is not the `BlendedNoise::legacyFromWorldSeed` rule next door applied
    /// again — it is its own rule, and assuming the analogy was exactly the
    /// error the probe caught.
    [[nodiscard]] static EndIslands fromWorldSeed(std::int64_t worldSeed);

    explicit EndIslands(PerlinNoise source) noexcept : source_(source) {}

    /// The density function's own value at a block position: the height
    /// field sampled on the `/8` grid, shifted by 8 and divided by 128.
    /// y is not read — the field is column-invariant.
    [[nodiscard]] double sample(std::int32_t blockX, std::int32_t blockZ) const noexcept;

    /// The raw height field, in blocks, on the `/8` grid. Exposed because it
    /// is what a probe reads back and what the goldens pin.
    [[nodiscard]] float heightAt(std::int32_t gridX, std::int32_t gridZ) const noexcept;

    /// The central island alone — the seed-independent radial term. Exposed
    /// so a test can say which half of the field it is measuring.
    [[nodiscard]] static float centralIslandHeight(std::int32_t gridX, std::int32_t gridZ) noexcept;

    /// The simplex field the outer islands are gated on, at one cell. The
    /// seeding's single call site; see the file comment.
    [[nodiscard]] double sampleSimplexSeeding(std::int32_t cellX,
                                              std::int32_t cellZ) const noexcept;

private:
    PerlinNoise source_;
};

} // namespace stratum::noise
