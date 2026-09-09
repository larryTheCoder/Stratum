// Stratum — the whole terrain chain against vanilla's own recorded heights.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// With old_blended_noise and weird_scaled_sampler settled, the overworld's
// `final_density` evaluates end to end for the first time — climate, splines,
// the blended noise, the cell lattice, and the caves that reach terrain
// through `min`s nested deep inside it. This is the test that says how close
// that is to vanilla, and the answer is now CLOSE, AND RIGHT EXCEPT WHERE A
// WHOLE UNIMPLEMENTED FEATURE SAYS SO.
//
// A chunk's stored OCEAN_FLOOR heightmap is the highest block that is neither
// air nor fluid. That is NOT the same as where `final_density` last crossed
// zero, and the difference used to be the whole story of this test — until
// its two components resolved to two very different things.
//
// **The aquifer fill decision.** Vanilla's overworld runs aquifers, and an
// aquifer places a stone BARRIER between two bodies of water at different
// levels — solid blocks no density function produced. Where that happens,
// OCEAN_FLOOR reports the barrier, high above the terrain, and a comparison
// against `final_density` reads it as the terrain being wrong. This build
// does not implement the aquifer fill decision at all (SPEC §10, milestone
// MA) — this gap is real, and it is the whole remaining reason this test's
// seed -1 section does not read 256 of 256.
//
// **The density chain's own residual — RESOLVED.** golden_terrain_no_aquifer_
// test.cpp isolates the density chain from the aquifer gap above, and it used
// to find something real but small there too: 1.7% of columns off by exactly
// one block. That traced, eventually, to one missing epsilon in
// `PerlinNoise::sample`'s fold — `old_blended_noise`'s Modern reading,
// confirmed via a clean-room spec and direct server bisection (SPEC §11,
// `spec/blended-noise-spec.md`, `tools/analysis/final-density-probe.sh`).
// Fixed: this build's own arithmetic now agrees with the server exactly,
// everywhere that test looks.
//
// So what the two sections below measure, now, is ONLY the aquifer gap:
//
//   seed 42: 256 of 256 columns exact — no aquifer barriers on this seed
//   seed -1: 252 of 256 exact, worst 28 blocks — every remaining miss is a
//            barrier OCEAN_FLOOR reports that no density function placed
//
// The worst column, (104, 112), is the aquifer mechanism in miniature: gravel
// over stone at y = 27-28 floating in water with aquifers on, water all the
// way down with them off, and OCEAN_FLOOR moving 28 -> 9 — where this build's
// density does turn positive, correctly, once the aquifer is out of the way.
//
// Do not read a 256-of-256 seed as "terrain works end to end" — it means this
// particular seed has no aquifer barriers to expose the gap that still
// exists. golden_terrain_no_aquifer_test.cpp is what actually certifies the
// density chain, and MA's own tests are what will certify the aquifer once
// it exists.
//
// Sampling: every eighth column over 8x8 chunks, so 256 columns spread across
// 128x128 blocks rather than packed into one chunk. That matters — a single
// chunk of seed 42 comes out 256 of 256 and would have hidden the residual
// entirely. Full coverage costs six minutes a seed, which is why this samples;
// the wider runs are in SPEC.
#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/settings/noise_settings.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

constexpr int kChunks = 8;
constexpr int kStride = 8;

struct Comparison {
    std::size_t columns = 0;
    std::size_t exact = 0;
    std::size_t withinOne = 0;
    long long worst = 0;
    /// The smallest distance from zero of either density that decides a
    /// column's height. This is what makes the counts above safe to pin: a
    /// column only changes answer on a last-bit difference if it sits within
    /// a few ULP of a tie, and none of them do.
    double margin = 1.0e30;
};

[[nodiscard]] std::filesystem::path fixtures() {
    return std::filesystem::path(STRATUM_FIXTURES_DIR) / "1.21.11";
}

[[nodiscard]] Comparison compare(std::int64_t seed) {
    using namespace stratum;
    const auto pack = data::Pack::open(fixtures() / "worldgen");
    const auto loaded = settings::loadAll(pack);
    const auto& overworld =
        loaded.settings.at(data::ResourceLocation::parse("minecraft:overworld"));

    const auto noises = density::NoiseRegistry::create(pack, loaded.graph.referencedNoises(), seed,
                                                       density::RandomSource::Xoroshiro);
    const density::Interpreter interpreter(
        loaded.graph, noises,
        density::CellGeometry{.width = overworld.geometry.cellWidth(),
                              .height = overworld.geometry.cellHeight()});
    const density::NodeIndex root = overworld.router.at(settings::RouterEntry::FinalDensity);
    interpreter.requireEvaluable(root);

    const auto region = region::RegionFile::open(
        fixtures() / "regions" / ("seed-" + std::to_string(seed)) / "overworld" / "r.0.0.mca");
    const int minY = overworld.geometry.minY;
    const int height = overworld.geometry.height;

    Comparison result;
    for (std::int32_t chunkZ = 0; chunkZ < kChunks; ++chunkZ) {
        for (std::int32_t chunkX = 0; chunkX < kChunks; ++chunkX) {
            if (!region.hasChunk(chunkX, chunkZ)) {
                continue;
            }
            const chunk::Chunk decoded =
                chunk::Chunk::decode(nbt::read(region.readChunk(chunkX, chunkZ)).root);
            const auto floor = decoded.heightmap(chunk::Heightmap::OceanFloor);
            if (!floor.has_value()) {
                continue;
            }
            for (int localZ = 0; localZ < 16; localZ += kStride) {
                for (int localX = 0; localX < 16; localX += kStride) {
                    const auto stored = (*floor)[static_cast<std::size_t>((localZ * 16) + localX)];
                    if (!stored.has_value()) {
                        continue;
                    }
                    const std::int32_t x = (chunkX * 16) + localX;
                    const std::int32_t z = (chunkZ * 16) + localZ;
                    int ours = minY - 1;
                    for (int y = minY + height - 1; y >= minY; --y) {
                        if (interpreter.evaluate(root, density::Point{.x = x, .y = y, .z = z}) >
                            0.0) {
                            ours = y;
                            break;
                        }
                    }
                    // The two values the answer turns on: the surface block
                    // must be positive and the block above it must not be.
                    const double below =
                        interpreter.evaluate(root, density::Point{.x = x, .y = ours, .z = z});
                    const double above =
                        interpreter.evaluate(root, density::Point{.x = x, .y = ours + 1, .z = z});
                    result.margin =
                        std::min(result.margin, std::min(std::abs(below), std::abs(above)));

                    ++result.columns;
                    const long long diff = ours - *stored;
                    if (diff == 0) {
                        ++result.exact;
                    }
                    if (std::llabs(diff) <= 1) {
                        ++result.withinOne;
                    }
                    result.worst = std::max(result.worst, std::llabs(diff));
                }
            }
        }
    }
    return result;
}

} // namespace

TEST_CASE("the terrain chain runs end to end, and is close but not right",
          "[conformance][terrain]") {
    if (!std::filesystem::is_directory(fixtures() / "worldgen") ||
        !std::filesystem::is_directory(fixtures() / "regions")) {
        SKIP("no extracted worldgen tree or golden regions under " << STRATUM_FIXTURES_DIR);
    }

    SECTION("seed 42") {
        const Comparison result = compare(42);
        REQUIRE(result.columns == 256U);
        // Pinning exact counts is only sound if no column is near a tie.
        // Measured, the closest is 1.4e-05 from zero — six orders of
        // magnitude above double rounding — so an x86-64/ARM64 contraction
        // difference cannot move these numbers. If that ever stops being
        // true, this fails instead of the counts going quietly flaky.
        CHECK(result.margin > 1.0e-9);
        // Every column exact — this seed has no aquifer barriers, and the
        // density chain's own residual is closed (SPEC §11's fold epsilon).
        // Before that fix: 254 of 256, worst 1.
        CHECK(result.exact == 256U);
        CHECK(result.withinOne == 256U);
        CHECK(result.worst == 0);
    }

    SECTION("seed -1") {
        const std::filesystem::path region = fixtures() / "regions" / "seed--1";
        if (!std::filesystem::is_directory(region)) {
            SKIP("no golden region for seed -1");
        }
        const Comparison result = compare(-1);
        REQUIRE(result.columns == 256U);
        CHECK(result.margin > 1.0e-9);
        // What is left, now, is ENTIRELY the aquifer gap: this seed has
        // barriers, and 28 blocks is a column whose terrain this build gets
        // wrong specifically because it does not place them (SPEC §10,
        // milestone MA) — not a density error. Before the fold epsilon:
        // 249 of 256, 252 within one; the four newly-exact columns were
        // ordinary density misses, distinct from the barrier gap below.
        CHECK(result.exact == 252U);
        CHECK(result.withinOne == 252U);
        CHECK(result.worst == 28);
    }
}
