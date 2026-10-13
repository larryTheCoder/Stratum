// Stratum — a dimension's biome source: which biome sits at a quart.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// A dimension's generator declares a `biome_source` beside its noise
// settings, and that, not the settings, decides how a position becomes a
// biome. Two of vanilla's four types are implemented here, and the other two
// (`minecraft:fixed`, `minecraft:checkerboard`) are refused by name where a
// caller names one (world::BiomeSource::parse), never approximated:
//
//   * `minecraft:multi_noise`: six climate entries of the noise router, read
//     at the quart's own lowest corner and searched in a ParameterList. The
//     overworld's and the Nether's (SPEC §11's biome entries).
//   * `minecraft:the_end`: no fields at all. One biome per CHUNK, every quart
//     column of it alike, at every y the router agrees on.
//
// THE END'S RULE (SPEC §11, "The End's biome source"). For the quart at quart
// coordinates (qx, qy, qz):
//
//   1. its chunk is (floorDiv(qx, 4), floorDiv(qz, 4)) — floorDiv, which the
//      server's ring chunk (-64, 1) settles: truncation would read quart
//      columns 1..3 of it in chunk -63, inside the ring;
//   2. if chunkX^2 + chunkZ^2 <= 4096, in 64-bit integers, the biome is
//      `minecraft:the_end`, whatever the router says;
//   3. otherwise the router's `erosion` is read at block (16 chunkX + 8,
//      4 qy, 16 chunkZ + 8) and compared in double: above 0.25
//      `end_highlands`; else at or above -0.0625 `end_midlands`; else below
//      -0.21875 `small_end_islands`; else `end_barrens`.
//
// PROVENANCE. Adapted from cubiomes (MIT, Cubitect/cubiomes, biomenoise.c:
// mapEnd, mapEndBiome and getEndBiome), read as text and recorded in NOTICE:
// the chunk taken from the quart by a floor shift; the central test,
// inclusive at 4096 in 64-bit chunk coordinates; one sample per chunk, in
// the island-field cell that holds the chunk's centre; and the three edges
// with their inclusivity — cubiomes' h < 3600, <= 10000 and <= 14400 on its
// squared island distance are erosion > 0.25, >= -0.0625 and >= -0.21875.
//
// This build's own, and NOT cubiomes': the order of the last two tests.
// cubiomes tests barrens before falling through to small islands, which in
// erosion terms sends a NaN to `small_end_islands`; this tests small islands
// first, so a NaN falls through to `end_barrens`. Why that biome: cubiomes
// models the server's empty outer rings with one test, `(int)rsq < 0` on the
// square of the island-field cell coordinate (cells of 8 blocks; first true
// past 46340 on an axis), made in two places — sampleNoiseColumnEnd makes
// the End's density column NaN there, so it holds no block, and mapEndBiome
// makes the biome `end_barrens`. Taken together, cubiomes' two checks model
// a NaN island field reading `end_barrens`, which is what this order gives
// a NaN erosion. A reading of cubiomes, not its order of the tests, and not
// a measurement: see below.
//
// What cubiomes does not say, the server does, and where the two part the
// server wins: cubiomes evaluates a hard-wired copy of the island field, and
// the vanilla server reads the dimension's own router. Measured by
// tools/analysis/end-biome-router-probe.sh (22 datapack dimensions with
// this source and the End's settings: a control with nothing changed, 19
// that change erosion alone, one final_density alone, and one that sets the
// five other climate entries): every erosion constant turns all 725 outer
// chunks of r.-2.-2 into one biome while the 299 inside the ring stay
// `the_end`; the other climate entries change nothing, nor does a constant
// final_density; the edges are held to 1e-10 (so double, not float), with
// 0.25 itself midlands, -0.0625 midlands and -0.21875 barrens; and a
// y-varying erosion is read at block y = 4 * qy. Its `--set sample` routes
// a noise that differs from block to block there, and of a chunk's 256
// columns only (16c + 8, 16c + 8) reads all 725 outer chunks (the next
// best, 228). Over every quart of every End region on
// disk — eight golden r.0.0, end-islands' four, end-biomes' eight at four
// seeds (two of them End worlds the goldens do not hold, so eight distinct
// End worlds in all), and the two probes' 25 — the rule is exact, 47185920
// of 47185920, through fillChunk (by way of CompiledDimension::fillBiomes)
// and through `at` at each quart's own corner alike: see
// tests/conformance/vanilla_end_biome_source_test.cpp.
//
// WHAT THE PROBES CANNOT SAY:
//
//   * NaN. Nothing on disk produces one, nor can vanilla's erosion as this
//     build computes it (noise::EndIslands takes square roots of 64-bit
//     sums, never of a negative). The rings past 46340 island-field cells
//     (about 370 000 blocks), where cubiomes' column is NaN, are where one
//     would arise if the server's field is NaN there; no probe reaches
//     them. Where one goes is the order of the tests above — this build's
//     choice, unmeasured;
//   * the central test past 46340 chunks, where a 32-bit square would wrap.
//     64-bit is cubiomes' reading; no probe reaches it;
//   * an erosion strictly between an edge and 1e-10 past it. The probes pin
//     each edge's own value, on its own side, and the values 1e-10 and 1e-5
//     past it, on the far side; the rule puts everything strictly past an
//     edge on the far side, as cubiomes' thresholds do, and no value on
//     disk falls in between.
//
// Nor does this source settle the island field it reads past about 38 000
// blocks, where no probe goes; noise::EndIslands owns that (SPEC §11).
#pragma once

#include <stratum/biome/parameter_list.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/settings/noise_settings.hpp>

#include <cstdint>
#include <span>

namespace stratum::biome {

/// `minecraft:the_end`'s rule, in the pieces a test can reach on their own.
struct EndRule {
    /// A chunk is the central island's, `minecraft:the_end`, when its squared
    /// distance from chunk (0, 0), in chunk coordinates, is at most this.
    /// Inclusive: chunks (-64, 0) and (0, -64) are `the_end` on the server,
    /// (-64, 1) and (1, -64) are not.
    static constexpr std::int64_t kCentralRadiusSquared = 4096;

    /// The three erosion edges, all exact binary fractions. Above
    /// kHighlandsAbove is highlands; from kMidlandsFrom up to it midlands;
    /// below kSmallIslandsBelow small islands; barrens between.
    static constexpr double kHighlandsAbove = 0.25;
    static constexpr double kMidlandsFrom = -0.0625;
    static constexpr double kSmallIslandsBelow = -0.21875;

    /// Whether chunk (@p chunkX, @p chunkZ) is inside the ring.
    [[nodiscard]] static bool isCentral(std::int32_t chunkX, std::int32_t chunkZ) noexcept {
        const auto x = static_cast<std::int64_t>(chunkX);
        const auto z = static_cast<std::int64_t>(chunkZ);
        return (x * x) + (z * z) <= kCentralRadiusSquared;
    }

    /// Where a chunk outside the ring reads `erosion`, for the quart layer
    /// whose lowest block is @p blockY: the chunk's centre, (16 chunkX + 8,
    /// blockY, 16 chunkZ + 8). Computed in 64 bits and narrowed, which
    /// wraps as Java's int does (C++20 defines the narrowing); no chunk
    /// inside the world border comes near it.
    [[nodiscard]] static density::Point samplePoint(std::int32_t chunkX, std::int32_t blockY,
                                                    std::int32_t chunkZ) noexcept {
        return density::Point{.x = static_cast<std::int32_t>((std::int64_t{chunkX} * 16) + 8),
                              .y = blockY,
                              .z = static_cast<std::int32_t>((std::int64_t{chunkZ} * 16) + 8)};
    }

    /// The central island's biome.
    [[nodiscard]] static const data::ResourceLocation& central();

    /// The biome a chunk outside the ring gets for @p erosion. A NaN is
    /// `end_barrens`, by this build's own order of the tests — unmeasured,
    /// and not cubiomes' order (the file comment says why).
    [[nodiscard]] static const data::ResourceLocation& outer(double erosion);
};

/// One dimension's biome source, bound to whatever it reads. A small value:
/// a multi-noise source holds a pointer to its ParameterList, which must
/// outlive it, and the End's holds nothing.
class Source {
public:
    enum class Type : std::uint8_t {
        /// `minecraft:multi_noise`.
        MultiNoise,
        /// `minecraft:the_end`.
        TheEnd,
    };

    /// `minecraft:multi_noise`, searching @p parameters.
    [[nodiscard]] static Source multiNoise(const ParameterList& parameters) noexcept {
        return {Type::MultiNoise, &parameters};
    }

    /// `minecraft:the_end`.
    [[nodiscard]] static Source theEnd() noexcept { return {Type::TheEnd, nullptr}; }

    [[nodiscard]] Type type() const noexcept { return type_; }

    /// The router entries this source reads, and nothing else of the router:
    /// the six climate entries for a multi-noise source, `erosion` alone for
    /// the End's. A caller checks these are evaluable before it generates.
    [[nodiscard]] std::span<const settings::RouterEntry> routerEntries() const noexcept;

    /// The biome of one quart. @p quartCorner is that quart's lowest block,
    /// every coordinate a multiple of 4 — where a multi-noise source reads
    /// its climate. The reference stays valid as long as this source's
    /// ParameterList does (the End's biomes live forever).
    [[nodiscard]] const data::ResourceLocation& at(const density::Interpreter& interpreter,
                                                   const settings::NoiseRouter& router,
                                                   density::Point quartCorner,
                                                   density::Interpreter::CornerCache& cache) const;

    /// The biome of every quart of chunk (@p chunkX, @p chunkZ), from quart
    /// layer @p minQuartY up, into `into[(qy * 4 + qz) * 4 + qx]` — vanilla's
    /// storage order, `into.size() / 16` layers. The same answers `at` gives
    /// quart by quart; the End's are read once a layer, its sixteen columns
    /// sharing one sample.
    void fillChunk(const density::Interpreter& interpreter, const settings::NoiseRouter& router,
                   std::int32_t chunkX, std::int32_t chunkZ, std::int32_t minQuartY,
                   std::span<const data::ResourceLocation*> into,
                   density::Interpreter::CornerCache& cache) const;

private:
    Source(const Type type, const ParameterList* parameters) noexcept
        : type_(type), parameters_(parameters) {}

    /// fillChunk for `minecraft:the_end`.
    static void fillEndChunk(const density::Interpreter& interpreter,
                             const settings::NoiseRouter& router, std::int32_t chunkX,
                             std::int32_t chunkZ, std::int32_t minQuartY,
                             std::span<const data::ResourceLocation*> into,
                             density::Interpreter::CornerCache& cache);

    Type type_;
    const ParameterList* parameters_;
};

} // namespace stratum::biome
