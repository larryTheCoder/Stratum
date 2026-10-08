// Stratum — the aquifer's level rule where sea_level falls below the lava.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// `kLavaLevel = -54` is a compile-time constant; `lambdaLevel(seaLevel)` is
// not — it equals `sea_level` itself once `sea_level < -54`. At every
// sea_level this project had ever generated a world with, from 32 through
// 200, the two coincide, so every place in the level rule that should read
// `lambda` and instead reads a bare `-54` or `-62` is invisible. This is the
// one world shape that pulls them apart, and it settles two things this
// project carried as open.
//
// (a) THE ABORT THRESHOLD. `abortThreshold(seaLevel)` used to be a bare
// constant, `-62.0`, that "coincided with kLavaLevel - 8" — an identity every
// prior measurement (min_y in five values, sea_level in {40,100,128,200})
// left unproven, because none of them separate a bare constant from
// `lambdaLevel(seaLevel) - 8`. ELEVEN dimensions here bisect BOTH candidates
// at `sea_level` -70: every one from -55 up through -78.00 comes back
// BYTE-IDENTICAL to the others, and -78.01 alone flips — refuting the bare
// -62 outright (which sits nowhere near the true boundary here) and
// confirming `lambdaLevel(-70) - 8 = -78` exactly.
//
// (b) THE NEAR-SURFACE FLOOR. An aborting cell that fails the near-surface
// exemption used to floor to the literal `kLavaLevel`. The dimension first
// read for it, `b_floor` (psl -200), puts 251229 blocks the old reading calls
// wet up to y=-55 observed dry at every one — but every row it reads is above
// its own y_skip (-158), where the global picker answers and no lattice is
// consulted (vanilla_aquifer_yskip_test.cpp measures the cutoff). So that
// 0/251229 is the cutoff's, and it is kept as such. The floor itself is read
// on `a_lo` (psl -85, y_skip -50), where rows -70..-55 do reach the lattice.
//
// Below `sea_level` itself, none of this is observable: the global fluid
// picker overrides the aquifer lattice outright there (Q2.4), so every case
// here reads only y >= sea_level.
//
// The fixture is Mojang-derived and never committed (SPEC §12).
#include "support/probe_corpus.hpp"

#include <stratum/aquifer/lattice.hpp>
#include <stratum/aquifer/sampling.hpp>
#include <stratum/aquifer/selection.hpp>
#include <stratum/chunk/chunk.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

using stratum::aquifer::CellIndex;
using stratum::aquifer::CentreSource;
using stratum::aquifer::PslRead;

constexpr std::int32_t kSeaLevel = -70;
constexpr std::int32_t kChunks = 8;
// Blocks above sea_level worth reading. This used to say `lambda` was a hard
// floor on `level` in every branch the file exercises, citing the
// `max(lambda, ...)` in `ladderLevel`. That premise is FALSE now: the clamp
// is gone and a dry source reports `kNeverLevel`, so `level` can sit far
// below sea_level. The assertions are unaffected, and not by luck — they all
// read `y >= sea_level`, which here IS lambda (`sea_level` -70 < -54), and
// above lambda a floor at lambda and no floor at all give the identical
// `y < level`. So the window stands; only the reason does. The observed
// range never exceeded sixteen blocks above sea_level, and 120 matches the
// upper bound (absolute y 50) the probe was originally measured against.
constexpr std::int32_t kTopMargin = 120;

[[nodiscard]] std::filesystem::path fixtures() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11";
}

struct GroupAWorld {
    const char* dir;
    double psl;
};

/// The eleven bisection points. `floodedness` (set in the probe itself, 0.9)
/// is comfortably above BOTH of the level rule's gates, so the branch taken
/// depends on nothing but whether the scan aborted: not aborted -> the
/// boundary sits at `sea_level`; aborted -> the local bonus alone already
/// exceeds the ladder threshold, so it sits at the ladder — never near
/// `sea_level` for this geometry.
constexpr std::array<GroupAWorld, 11> kGroupA{{
    {"a_hi", -55.0},
    {"a_62m", -61.99},
    {"a_62", -62.00},
    {"a_62p", -62.01},
    {"a_65", -65.0},
    {"a_70", -70.0},
    {"a_75", -75.0},
    {"a_78m", -77.99},
    {"a_78", -78.00},
    {"a_78p", -78.01},
    {"a_lo", -85.0},
}};

/// The fluid/air transition height per sampled column, as an ordered
/// histogram: `{level, column count}` pairs. NOT a single number — the "not
/// aborted" branch is not uniform across a dimension (the trailing guard and
/// barrier stone both make it vary with which cell governs a given column;
/// see `lattice.hpp`'s own account of the trailing guard), so the only
/// robust readout is the whole distribution, compared whole against whole.
[[nodiscard]] std::vector<std::pair<int, long>>
transitionHistogram(const std::filesystem::path& region) {
    const auto file = stratum::region::RegionFile::open(region);
    std::map<int, long> counts;
    for (std::int32_t cz = 0; cz < kChunks; ++cz) {
        for (std::int32_t cx = 0; cx < kChunks; ++cx) {
            // A probe region holds every chunk of its window: a missing one is
            // a broken corpus, not a smaller sample.
            REQUIRE(file.hasChunk(cx, cz));
            const auto chunk =
                stratum::chunk::Chunk::decode(stratum::nbt::read(file.readChunk(cx, cz)).root);
            for (std::int32_t lz = 0; lz < 16; ++lz) {
                for (std::int32_t lx = 0; lx < 16; ++lx) {
                    int top = -1000;
                    for (std::int32_t y = kSeaLevel - 1; y < kSeaLevel + kTopMargin; ++y) {
                        const auto* block = chunk.blockAt(lx, y, lz);
                        if (block != nullptr &&
                            (block->name == "minecraft:water" || block->name == "minecraft:lava")) {
                            top = y;
                        }
                    }
                    ++counts[top + 1];
                }
            }
        }
    }
    std::vector<std::pair<int, long>> out(counts.begin(), counts.end());
    return out;
}

} // namespace

TEST_CASE("the abort threshold is lambda(sea_level) - 8, not a bare -62",
          "[conformance][aquifer]") {
    // `aborted` is the ONLY psl-dependent quantity this branch reads (the
    // off-near-surface, off-depth-path `!aborted && floodedness > 0.8 -> sea`
    // shape never touches gate/cap/anchor's actual value) — so every world
    // that does not abort must produce a BYTE-IDENTICAL world to every other
    // one that does not, regardless of how far apart their psl values sit.
    // That is the whole test: no model of the trailing guard or the barrier
    // is needed, because whatever they do, they do it identically across the
    // whole "not aborted" population.
    const std::filesystem::path reference =
        fixtures() / "probes" / "lowsea" / kGroupA.front().dir / "r.0.0.mca";
    if (!std::filesystem::is_regular_file(reference)) {
        SKIP("no low-sea aquifer probe at "
             << reference << "; generate it with tools/analysis/aquifer-lowsea-probe.sh");
    }
    stratum::test::requireFrozen(fixtures() / "probes" / "lowsea",
                                 "tools/analysis/aquifer-lowsea-probe.sh");
    stratum::test::requireSeed(fixtures() / "probes" / "lowsea", 42);
    const auto referenceHistogram = transitionHistogram(reference);
    REQUIRE(referenceHistogram.size() > 1); // the reference itself must show variation

    for (const GroupAWorld& world : kGroupA) {
        const std::filesystem::path region =
            fixtures() / "probes" / "lowsea" / world.dir / "r.0.0.mca";
        if (!std::filesystem::is_regular_file(region)) {
            SKIP("no low-sea aquifer probe at "
                 << region << "; generate it with tools/analysis/aquifer-lowsea-probe.sh");
        }

        const bool matchesReference = transitionHistogram(region) == referenceHistogram;
        const bool predictedAborted = world.psl < stratum::aquifer::abortThreshold(kSeaLevel);
        INFO("psl " << world.psl << " (" << world.dir
                    << "): " << (matchesReference ? "matches" : "differs from")
                    << " the a_hi reference; abortThreshold predicts "
                    << (predictedAborted ? "aborted" : "not aborted"));
        CHECK(matchesReference == !predictedAborted);
    }
}

TEST_CASE("a flat surface of -200 at sea_level -70 is the global picker's on every row read",
          "[conformance][aquifer]") {
    // Every row read here is above this dimension's y_skip (-158), so the
    // global picker answers it: air from sea_level up, whatever the lattice
    // would say. The level rule below agrees with that on every block, and
    // so — this case's original purpose — does lambda's floor, where
    // kLavaLevel's does not; but at these rows the lattice is not consulted,
    // so the floor is measured on a_lo instead (the next case).
    REQUIRE(stratum::aquifer::chunkYSkip(
                [](std::int32_t, std::int32_t, std::int32_t) { return -200.0; }, 0, 0) < kSeaLevel);
    const std::filesystem::path region = fixtures() / "probes" / "lowsea" / "b_floor" / "r.0.0.mca";
    if (!std::filesystem::is_regular_file(region)) {
        SKIP("no low-sea aquifer probe at "
             << region << "; generate it with tools/analysis/aquifer-lowsea-probe.sh");
    }
    stratum::test::requireFrozen(fixtures() / "probes" / "lowsea",
                                 "tools/analysis/aquifer-lowsea-probe.sh");
    stratum::test::requireSeed(fixtures() / "probes" / "lowsea", 42);

    const stratum::aquifer::CentreSource centres{42};
    const auto file = stratum::region::RegionFile::open(region);
    const auto sampler = [](std::int32_t, std::int32_t, std::int32_t) { return -200.0; };

    long blocks = 0;
    long agree = 0;
    for (std::int32_t cz = 0; cz < kChunks; ++cz) {
        for (std::int32_t cx = 0; cx < kChunks; ++cx) {
            // A probe region holds every chunk of its window: a missing one is
            // a broken corpus, not a smaller sample.
            REQUIRE(file.hasChunk(cx, cz));
            const auto chunk =
                stratum::chunk::Chunk::decode(stratum::nbt::read(file.readChunk(cx, cz)).root);
            for (std::int32_t lz = 0; lz < 16; ++lz) {
                for (std::int32_t lx = 0; lx < 16; ++lx) {
                    const std::int32_t x = (cx * 16) + lx;
                    const std::int32_t z = (cz * 16) + lz;
                    // Only y >= sea_level: below it the global picker
                    // overrides the aquifer lattice outright (Q2.4), and
                    // nothing there is this branch's to predict.
                    for (std::int32_t y = kSeaLevel; y < kSeaLevel + kTopMargin; ++y) {
                        const auto* block = chunk.blockAt(lx, y, lz);
                        if (block == nullptr) {
                            continue;
                        }
                        const bool fluid =
                            block->name == "minecraft:water" || block->name == "minecraft:lava";
                        if (!fluid && block->name != "minecraft:air") {
                            continue; // barrier stone
                        }
                        const CellIndex centre =
                            stratum::aquifer::selectSources(centres, x, y, z).nearest().centre;
                        const PslRead surface =
                            stratum::aquifer::readPreliminarySurface(sampler, centre, kSeaLevel);
                        const std::int32_t level = stratum::aquifer::cellFluidLevel(
                            stratum::aquifer::CellFluid{.centreY = centre.y,
                                                        .surface = surface,
                                                        .seaLevel = kSeaLevel,
                                                        .floodedness = 0.5,
                                                        .spread = 0.0});
                        ++blocks;
                        agree += static_cast<int>((y < level) == fluid);
                    }
                }
            }
        }
    }

    REQUIRE(blocks > 1000000);
    INFO("blocks " << blocks << ", agree " << agree);
    // Exact when this was written: 1966080/1966080. The old bare-kLavaLevel
    // reading scored 0.87 here (0/251229 on the disagreeing population,
    // matching the rest by coincidence) — against the global picker's air.
    CHECK(agree == blocks);
}

TEST_CASE("at sea_level -70 the aborting near-surface floor is lava to -55, not lambda",
          "[conformance][aquifer]") {
    // a_lo: psl -85, so every scan aborts and y_skip is -50 — rows -70..-50
    // are decided by the lattice (vanilla_aquifer_yskip_test.cpp measures the
    // cutoff). A near-surface cell that fails the exemption (centred at or
    // below cap + 20 = -65, or below lambda) takes the aborting floor. Built,
    // that is lambda (-70): dry everywhere above the lava sea. The server
    // disagrees on every block such a cell is nearest to: lava up to y = -55
    // and barrier stone, not one block of air. So the floor is wet to -54
    // and LAVA — spec Q1.1's A_lava = (-54, lava), the status Q5.3(b) hands
    // an aborted scan, whose level is the literal -54 rather than lambda.
    // Open (SPEC §11, "y_skip against the server"): the build keeps lambda
    // until the floor's status is changed with its own measurements, since
    // its type also reaches the fluid-update flag at every sea_level.
    //
    // The exemption itself holds as built: a cell centred above -65 takes
    // the sea (-70) and is dry here, where kLavaLevel's comparand would have
    // floored every cell centred below -54.
    const std::filesystem::path region = fixtures() / "probes" / "lowsea" / "a_lo" / "r.0.0.mca";
    if (!std::filesystem::is_regular_file(region)) {
        SKIP("no low-sea aquifer probe at "
             << region << "; generate it with tools/analysis/aquifer-lowsea-probe.sh");
    }
    stratum::test::requireFrozen(fixtures() / "probes" / "lowsea",
                                 "tools/analysis/aquifer-lowsea-probe.sh");
    stratum::test::requireSeed(fixtures() / "probes" / "lowsea", 42);

    const auto sampler = [](std::int32_t, std::int32_t, std::int32_t) { return -85.0; };
    const std::int32_t lambda = stratum::aquifer::lambdaLevel(kSeaLevel);
    const std::int32_t ySkipLevel = stratum::aquifer::chunkYSkip(sampler, 0, 0);
    REQUIRE(ySkipLevel == -50);
    const stratum::aquifer::CentreSource centres{42};
    const auto file = stratum::region::RegionFile::open(region);

    long floorBlocks = 0; ///< rows lambda..-55 whose nearest source takes the floor
    long floorAir = 0;
    long floorLava = 0;
    long floorStone = 0;
    long floorLavaAbove = 0; ///< lava on rows -54..y_skip whose nearest takes the floor
    long exemptBlocks = 0;   ///< rows lambda..-55 whose nearest takes the sea
    long exemptAir = 0;
    long builtLambda = 0; ///< floor blocks whose nearest the build puts at lambda
    for (std::int32_t cz = 0; cz < kChunks; ++cz) {
        for (std::int32_t cx = 0; cx < kChunks; ++cx) {
            REQUIRE(file.hasChunk(cx, cz));
            const auto chunk =
                stratum::chunk::Chunk::decode(stratum::nbt::read(file.readChunk(cx, cz)).root);
            for (std::int32_t lz = 0; lz < 16; ++lz) {
                for (std::int32_t lx = 0; lx < 16; ++lx) {
                    const std::int32_t x = (cx * 16) + lx;
                    const std::int32_t z = (cz * 16) + lz;
                    for (std::int32_t y = lambda; y <= ySkipLevel; ++y) {
                        const CellIndex centre =
                            stratum::aquifer::selectSources(centres, x, y, z).nearest().centre;
                        const PslRead surface =
                            stratum::aquifer::readPreliminarySurface(sampler, centre, kSeaLevel);
                        if (surface.gate - centre.y >= stratum::aquifer::kNearSurfaceDepth) {
                            continue; // off the near-surface path
                        }
                        const bool exempt =
                            centre.y >= lambda &&
                            centre.y > surface.cap + stratum::aquifer::kNearSurfaceFloorOffset;
                        const auto* block = chunk.blockAt(lx, y, lz);
                        const std::string name = block != nullptr ? block->name : "";
                        if (y >= stratum::aquifer::kLavaLevel) {
                            floorLavaAbove +=
                                static_cast<long>(!exempt && name == "minecraft:lava");
                            continue;
                        }
                        if (exempt) {
                            ++exemptBlocks;
                            exemptAir += static_cast<long>(name == "minecraft:air");
                            continue;
                        }
                        ++floorBlocks;
                        floorAir += static_cast<long>(name == "minecraft:air");
                        floorLava += static_cast<long>(name == "minecraft:lava");
                        floorStone += static_cast<long>(name == "minecraft:stone");
                        builtLambda += static_cast<long>(
                            stratum::aquifer::cellFluidLevel(
                                stratum::aquifer::CellFluid{.centreY = centre.y,
                                                            .surface = surface,
                                                            .seaLevel = kSeaLevel,
                                                            .floodedness = 0.9,
                                                            .spread = 0.0}) == lambda);
                    }
                }
            }
        }
    }
    INFO("floor blocks " << floorBlocks << ": air " << floorAir << ", lava " << floorLava
                         << ", stone " << floorStone << "; lava above -55 " << floorLavaAbove
                         << "; the build puts " << builtLambda << " at lambda (dry); exempt "
                         << exemptBlocks << ", air " << exemptAir);
    REQUIRE(floorBlocks > 100000);
    // Wet to -54 and lava, every one: the barrier's stone and A_lava's lava.
    CHECK(floorAir == 0);
    CHECK(floorLava + floorStone == floorBlocks);
    CHECK(floorLavaAbove == 0);
    // The exempt cells take the sea, dry here — most of their blocks air,
    // the rest the barrier between them and the floor's lava.
    CHECK(exemptAir * 4 > exemptBlocks * 3);
}
