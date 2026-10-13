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
// PROVENANCE. The structure — the inclusive 4096 in chunk coordinates, one
// sample per chunk at its centre, the three edges, and the order of the tests
// that sends a NaN to `end_barrens` — is adapted from cubiomes (MIT,
// Cubitect/cubiomes, biomenoise.c: mapEndBiome and getEndBiome), read as
// text. What cubiomes does NOT say, the server does, and where the two part
// the server wins: cubiomes evaluates a hard-wired copy of the island field,
// and the vanilla server reads the dimension's own router. Measured by
// tools/analysis/end-biome-router-probe.sh (22 datapack dimensions with this
// source and the End's settings, one router entry changed in each): every
// erosion constant turns all 725 outer chunks of r.-2.-2 into one biome while
// the 299 inside the ring stay `the_end`; constant temperature, vegetation,
// continents, depth and ridges change nothing, nor does a constant
// final_density; the edges hold to 1e-10 (so double, not float), and a
// y-varying erosion is read at block y = 4 * qy. Its `--set sample` routes a
// noise that differs from block to block there, and of a chunk's 256 columns
// only (16c + 8, 16c + 8) reads all 725 outer chunks (the next best, 228).
// Over every quart of every End region on disk — eight golden r.0.0,
// end-islands' four, end-biomes' eight at four more worlds, and the two
// probes' 25 — the rule is exact, 47185920 of 47185920
// (tests/conformance/vanilla_end_biome_source_test.cpp).
//
// WHAT THE PROBES CANNOT SAY, and is therefore cubiomes' reading:
//
//   * NaN. Nothing on disk produces one. The order of the tests is cubiomes',
//     whose "outer rings" (an int overflow in the island field past about
//     370 000 blocks) come out `end_barrens`;
//   * the central test past 46340 chunks, where a 32-bit square would wrap.
//     64-bit is cubiomes' reading; no probe reaches it.
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
    /// `end_barrens` (the order of the tests, see the file comment).
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
