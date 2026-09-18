// Stratum — above_preliminary_surface's boundary, read off the vanilla server.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// This project carried the condition for two milestones as "the documented
// reading, `y >= preliminary_surface_level`, strictness unmeasured", on the
// strength of one probe that was said to have found it true everywhere. That
// probe's own region file says otherwise, and it was on disk the whole time:
// re-read at the BOTTOM edge of the band it paints rather than at the terrain
// top, every one of its 36864 columns carries a clean stone/marker step, two
// to eight blocks BELOW the level the condition is named after. The earlier
// analysis read the top block of the terrain, where a condition of this shape
// is true by construction.
//
// WHAT THE PROBE IS. A dimension whose `raw_final_density` is the constant 1
// — solid from the world floor to the top, so no terrain height, no air, no
// fluid, no aquifer, and the marker band's LOWER EDGE is the condition's own
// boundary at single-block resolution — and whose entire surface rule is
//
//     { condition: above_preliminary_surface, then_run: diamond_block }
//
// with `preliminary_surface_level` overridden to a constant of the sweep's
// choosing. tools/analysis/aps-boundary-probe.sh builds it.
//
// WHAT IT MEASURES, each of the three a separate case below:
//   * the boundary is `floor(psl) + surfaceDepth(x, z) - 8`, so `psl` enters
//     as a pure per-block offset and the column's own surface depth SUBTRACTS
//     from it. Nineteen constants from -60 to +200 pin the offset.
//   * the double -> y conversion is FLOOR, not truncation toward zero. Only
//     a negative fraction separates them, which vanilla's own integer-valued
//     `find_top_surface` never produces — so it is a datapack-only
//     difference, and it is still measured rather than assumed.
//   * the 8 is a LITERAL. Twelve geometry variants — cell heights 4/8/16,
//     cell widths 4/8/16, three floors, three heights, three sea levels —
//     leave it at 8. Those twelve re-score ONE seed's same 36864 columns
//     twelve times over; the column evidence underneath the boundary itself
//     is 36864 columns at each of two seeds, and the two numbers are not
//     interchangeable.
//
// AND WHAT KEEPS EACH OF THOSE HONEST, which is the other half of the file:
//   * the control that this is the CONDITION and not the surface pass. A
//     probe that paints where a condition holds cannot tell "false below y"
//     from "the pass stopped at y". `probes/surf`'s `bandlands` and `steep`
//     entries — same world, same pinned psl, rules that do not read y — paint
//     to the world floor where `aps` stops at -8..-2.
//   * both DIRECTIONS on real overworlds. Counting only the blocks the old
//     reading cannot place rewards a boundary for reaching further down, so
//     the band the new reading opens is read at the column's own surface too.
//   * the census that says what is still open and why: `8 - surfaceDepth`
//     against `8 - max(0, surfaceDepth)` is not separated by any vanilla
//     world, because the returned depth is never negative in 2097152 golden
//     columns.
//   * the varying-psl counts, so the numbers SPEC §11 and PROGRESS.md quote
//     for the still-open sampling question cannot drift from the fixture.
//
// The fixtures are Mojang-derived and never committed (SPEC §12).
#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/javamath.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/executor.hpp>
#include <stratum/surface/rule_graph.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using stratum::data::ResourceLocation;

[[nodiscard]] std::filesystem::path fixtures() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11";
}

/// Everything a scoring pass needs that depends only on the world seed: the
/// vanilla pack, an interpreter over its density graph, and a compiled surface
/// Executor whose `surfaceDepth` is the one the engine itself runs.
class World {
public:
    explicit World(std::int64_t seed)
        : pack_(stratum::data::Pack::open(fixtures() / "worldgen")),
          loaded_(stratum::settings::loadAll(pack_)),
          overworld_(loaded_.settings.at(ResourceLocation::parse("minecraft:overworld"))),
          rules_(stratum::surface::RuleGraph::resolve(
              overworld_.surfaceRule, ResourceLocation::parse("minecraft:overworld"))),
          noises_(stratum::density::NoiseRegistry::create(
              pack_, wanted(), seed, stratum::density::RandomSource::Xoroshiro)),
          executor_(stratum::surface::Executor::compile(rules_, seed, overworld_.geometry, &noises_,
                                                        overworld_.seaLevel)),
          interpreter_(loaded_.graph, noises_,
                       stratum::density::CellGeometry{.width = overworld_.geometry.cellWidth(),
                                                      .height = overworld_.geometry.cellHeight()}) {
    }

    [[nodiscard]] std::int32_t surfaceDepth(std::int32_t x, std::int32_t z) const {
        return executor_.surfaceDepth(x, z);
    }

    /// The same before the cast — what the clamp question below is actually
    /// about.
    [[nodiscard]] double surfaceDepthRaw(std::int32_t x, std::int32_t z) const {
        return executor_.surfaceDepthRaw(x, z);
    }

    /// The overworld's own `preliminary_surface_level`, floored the way the
    /// filler floors it.
    [[nodiscard]] std::int32_t preliminarySurface(std::int32_t x, std::int32_t z) const {
        return static_cast<std::int32_t>(std::floor(interpreter_.evaluate(
            overworld_.router.at(stratum::settings::RouterEntry::PreliminarySurfaceLevel),
            stratum::density::Point{.x = x, .y = 0, .z = z})));
    }

    [[nodiscard]] const stratum::settings::NoiseSettings& overworld() const { return overworld_; }

private:
    [[nodiscard]] std::vector<ResourceLocation> wanted() const {
        auto names = loaded_.graph.referencedNoises();
        const auto surface = rules_.referencedNoises();
        names.insert(names.end(), surface.begin(), surface.end());
        names.push_back(ResourceLocation::parse("minecraft:surface"));
        names.push_back(ResourceLocation::parse("minecraft:surface_secondary"));
        names.push_back(ResourceLocation::parse("minecraft:clay_bands_offset"));
        return names;
    }

    stratum::data::Pack pack_;
    stratum::settings::LoadedSettings loaded_;
    stratum::settings::NoiseSettings overworld_;
    stratum::surface::RuleGraph rules_;
    stratum::density::NoiseRegistry noises_;
    stratum::surface::Executor executor_;
    stratum::density::Interpreter interpreter_;
};

struct Boundaries {
    long long columns = 0;
    long long right = 0;     // boundary == floor(psl) + surfaceDepth - 8
    long long atPsl = 0;     // boundary == floor(psl), the reading this replaced
    long long truncated = 0; // boundary == trunc(psl) + surfaceDepth - 8
    long long broken = 0;    // the painted band was not one contiguous run
};

/// Every column of one probe dimension: the LOWEST y carrying the marker,
/// against each candidate boundary. A column whose marker run is interrupted
/// is counted separately rather than scored, since the readout assumes the
/// step the probe's own design produces.
[[nodiscard]] Boundaries scoreProbe(const World& world, const std::filesystem::path& region,
                                    double psl, std::int32_t minY, std::int32_t height) {
    const auto file = stratum::region::RegionFile::open(region);
    const auto floored = static_cast<std::int32_t>(std::floor(psl));
    const auto truncated = static_cast<std::int32_t>(psl);
    Boundaries score;
    for (std::int32_t chunkZ = 0; chunkZ < 12; ++chunkZ) {
        for (std::int32_t chunkX = 0; chunkX < 12; ++chunkX) {
            if (!file.hasChunk(chunkX, chunkZ)) {
                continue;
            }
            const auto chunk = stratum::chunk::Chunk::decode(
                stratum::nbt::read(file.readChunk(chunkX, chunkZ)).root);
            for (int localZ = 0; localZ < 16; ++localZ) {
                for (int localX = 0; localX < 16; ++localX) {
                    std::int32_t lowest = 0;
                    std::int32_t highest = 0;
                    long long marked = 0;
                    for (std::int32_t y = minY; y < minY + height; ++y) {
                        const auto* found = chunk.blockAt(localX, y, localZ);
                        if (found == nullptr || found->name != "minecraft:diamond_block") {
                            continue;
                        }
                        if (marked == 0) {
                            lowest = y;
                        }
                        highest = y;
                        ++marked;
                    }
                    // A chunk that never reached the surface stage carries no
                    // marker at all — the region holds many more chunks than
                    // the probe forceloaded, and scoring those would average
                    // in untouched terrain.
                    if (marked == 0) {
                        continue;
                    }
                    ++score.columns;
                    if (highest - lowest + 1 != marked) {
                        ++score.broken;
                        continue;
                    }
                    const std::int32_t x = (chunkX * 16) + localX;
                    const std::int32_t z = (chunkZ * 16) + localZ;
                    const std::int32_t depth = world.surfaceDepth(x, z);
                    score.right += static_cast<long long>(lowest == floored + depth - 8);
                    score.atPsl += static_cast<long long>(lowest == floored);
                    score.truncated += static_cast<long long>(lowest == truncated + depth - 8);
                }
            }
        }
    }
    return score;
}

struct Case {
    const char* entry;
    double psl;
};

/// Every world seed `tools/fetch-vanilla` generated a golden overworld region
/// for. Two million columns between them, which is what makes a statement
/// about the surface depth's TAIL worth making at all.
[[nodiscard]] std::vector<std::int64_t> goldenSeeds() {
    return {std::int64_t{0},
            std::int64_t{-1},
            std::int64_t{1},
            std::int64_t{42},
            std::int64_t{2891948927356891},
            std::int64_t{-4172144997902289642},
            std::int64_t{9223372036854775807},
            std::numeric_limits<std::int64_t>::min()};
}

[[nodiscard]] std::filesystem::path overworldRegion(std::int64_t seed) {
    return fixtures() / "regions" / ("seed-" + std::to_string(seed)) / "overworld" / "r.0.0.mca";
}

/// Anything a probe's surface rule paints: the `diamond_block` the marker
/// rules place and the clay bands `bandlands` places. Everything else in a
/// probe column is the terrain filler's own stone.
[[nodiscard]] bool painted(const std::string& name) {
    return name == "minecraft:diamond_block" || name.find("terracotta") != std::string::npos;
}

/// The lowest y a probe entry's surface rule painted, per column, over one
/// region — and how many of those reached the world floor.
/// One probe entry's band, read from below. `lowestEdge` and `highestEdge`
/// are the extremes OF THE PER-COLUMN LOWER EDGE — the deepest and shallowest
/// y at which any column's painted run begins — not of the run itself.
struct Painted {
    long long columns = 0;
    long long toFloor = 0;
    std::int32_t lowestEdge = 0;
    std::int32_t highestEdge = 0;
};

[[nodiscard]] Painted scorePainted(const std::filesystem::path& region, std::int32_t minY,
                                   std::int32_t height) {
    const auto file = stratum::region::RegionFile::open(region);
    Painted score;
    score.lowestEdge = minY + height;
    score.highestEdge = minY - 1;
    for (std::int32_t chunkZ = 0; chunkZ < 12; ++chunkZ) {
        for (std::int32_t chunkX = 0; chunkX < 12; ++chunkX) {
            if (!file.hasChunk(chunkX, chunkZ)) {
                continue;
            }
            const auto chunk = stratum::chunk::Chunk::decode(
                stratum::nbt::read(file.readChunk(chunkX, chunkZ)).root);
            for (int localZ = 0; localZ < 16; ++localZ) {
                for (int localX = 0; localX < 16; ++localX) {
                    for (std::int32_t y = minY; y < minY + height; ++y) {
                        const auto* block = chunk.blockAt(localX, y, localZ);
                        if (block == nullptr || !painted(block->name)) {
                            continue;
                        }
                        ++score.columns;
                        score.toFloor += static_cast<long long>(y == minY);
                        score.lowestEdge = std::min(score.lowestEdge, y);
                        score.highestEdge = std::max(score.highestEdge, y);
                        break;
                    }
                }
            }
        }
    }
    return score;
}

/// The blocks that appear inside `surface_rule.sequence[1]` — the subtree
/// `above_preliminary_surface` gates — and nowhere else in the overworld's
/// tree, less the three the terrain filler itself writes and so which cannot
/// be attributed to the subtree: `air`, `water` and `stone`.
///
/// This is a block the gated subtree CAN place and the terrain filler cannot.
/// It is NOT a block only that subtree can place, and the difference matters
/// to what the count below means: the derivation behind this list is "in the
/// gated subtree and nowhere else in the surface-rule TREE", which says
/// nothing about FEATURES, and features run after the surface pass. Vanilla
/// places gravel from `disk_gravel`/`ore_gravel`, sand from `disk_sand`, dirt
/// from `ore_dirt` and some forty-five tree features, podzol from the mega
/// spruce/pine and jungle-grass patches, and coarse_dirt, snow_block and
/// packed_ice from the ice patches and icebergs. So a count of these is an
/// UPPER BOUND on positions the gated subtree ran at, in the same way the
/// remainder below is a bound rather than a verdict.
[[nodiscard]] bool gatedOnlyMaterial(const std::string& name) {
    static const std::vector<std::string> kMaterials{"minecraft:calcite",
                                                     "minecraft:coarse_dirt",
                                                     "minecraft:dirt",
                                                     "minecraft:grass_block",
                                                     "minecraft:gravel",
                                                     "minecraft:ice",
                                                     "minecraft:mud",
                                                     "minecraft:mycelium",
                                                     "minecraft:orange_terracotta",
                                                     "minecraft:packed_ice",
                                                     "minecraft:podzol",
                                                     "minecraft:powder_snow",
                                                     "minecraft:red_sand",
                                                     "minecraft:red_sandstone",
                                                     "minecraft:sand",
                                                     "minecraft:sandstone",
                                                     "minecraft:snow_block",
                                                     "minecraft:terracotta",
                                                     "minecraft:white_terracotta"};
    return std::find(kMaterials.begin(), kMaterials.end(), name) != kMaterials.end();
}

/// Scores one family of probe dimensions, all in the same world and so all at
/// the same seed and geometry. Returns the total, and asserts per entry so a
/// failure names the dimension that moved.
[[nodiscard]] Boundaries scoreFamily(const World& world, const std::string& spec,
                                     const std::vector<Case>& cases, std::int32_t minY = -64,
                                     std::int32_t height = 384) {
    Boundaries total;
    for (const Case& probe : cases) {
        const std::filesystem::path region =
            fixtures() / "probes" / spec / probe.entry / "r.0.0.mca";
        if (!std::filesystem::is_regular_file(region)) {
            continue;
        }
        const Boundaries score = scoreProbe(world, region, probe.psl, minY, height);
        INFO(spec << "/" << probe.entry << " (psl " << probe.psl << "): " << score.right << " of "
                  << score.columns << " columns, " << score.broken << " with a broken band");
        CHECK(score.columns > 30000);
        CHECK(score.broken == 0);
        CHECK(score.right == score.columns);
        total.columns += score.columns;
        total.right += score.right;
        total.atPsl += score.atPsl;
        total.truncated += score.truncated;
        total.broken += score.broken;
    }
    return total;
}

} // namespace

TEST_CASE("above_preliminary_surface's boundary carries the column's surface depth",
          "[conformance][surface]") {
    const std::filesystem::path root = fixtures() / "probes" / "apsb";
    if (!std::filesystem::is_directory(fixtures() / "worldgen") ||
        !std::filesystem::is_directory(root)) {
        SKIP("no above_preliminary_surface sweep at "
             << root << "; generate it with tools/analysis/aps-boundary-probe.sh --accept-eula");
    }

    const World world{42};
    // Nineteen constants, on and off every lattice the world has: multiples
    // of 8, of 4, of 2 and of none, so a quantisation of `psl` onto a cell
    // lattice would show up as a staircase rather than as a line. -60 is
    // deliberately low enough that the boundary falls through the world floor
    // and the band is clipped, so it is excluded from the exact score and
    // checked on its own below.
    const Boundaries constants = scoreFamily(world, "apsb",
                                             {{"k_m40", -40},
                                              {"k_m33", -33},
                                              {"k_m20", -20},
                                              {"k_m9", -9},
                                              {"k_m8", -8},
                                              {"k_m7", -7},
                                              {"k_m4", -4},
                                              {"k_m1", -1},
                                              {"k_p0", 0},
                                              {"k_p1", 1},
                                              {"k_p3", 3},
                                              {"k_p4", 4},
                                              {"k_p7", 7},
                                              {"k_p16", 16},
                                              {"k_p33", 33},
                                              {"k_p64", 64},
                                              {"k_p100", 100},
                                              {"k_p200", 200}});
    INFO("constants: " << constants.right << " of " << constants.columns);
    REQUIRE(constants.columns > 600000);
    CHECK(constants.right == constants.columns);
    // The reading this replaced. It is not merely worse — it is wrong on
    // every column, which is why a probe that looked at the terrain top
    // instead of the band's lower edge could believe it for two milestones.
    CHECK(constants.atPsl == 0);

    // Terrain instead of a solid column, same constants: the boundary must
    // not move, since nothing in it reads the terrain's own height.
    const Boundaries terrain =
        scoreFamily(world, "apsb", {{"t_m20", -20}, {"t_p0", 0}, {"t_p40", 40}});
    INFO("over terrain: " << terrain.right << " of " << terrain.columns);
    REQUIRE(terrain.columns > 100000);
    CHECK(terrain.right == terrain.columns);
}

TEST_CASE("the preliminary surface level reaches the condition FLOORED, not truncated",
          "[conformance][surface]") {
    const std::filesystem::path root = fixtures() / "probes" / "apsb";
    if (!std::filesystem::is_directory(fixtures() / "worldgen") ||
        !std::filesystem::is_directory(root)) {
        SKIP("no above_preliminary_surface sweep at "
             << root << "; generate it with tools/analysis/aps-boundary-probe.sh --accept-eula");
    }

    const World world{42};
    // Only the negative fractions separate the two. +0.5 and +3.9999 are in
    // the sweep as controls: both conversions agree there, so a harness that
    // scored them alone would report a clean pass for either.
    const Boundaries fractions = scoreFamily(world, "apsb",
                                             {{"r_p0_5", 0.5},
                                              {"r_m0_5", -0.5},
                                              {"r_p3_5", 3.5},
                                              {"r_m3_5", -3.5},
                                              {"r_p3_9999", 3.9999},
                                              {"r_m3_9999", -3.9999},
                                              {"r_m0_0001", -0.0001}});
    INFO("fractional levels: " << fractions.right << " of " << fractions.columns << ", truncating "
                               << fractions.truncated);
    REQUIRE(fractions.columns > 200000);
    CHECK(fractions.right == fractions.columns);
    // Truncation toward zero — what this build did before the sweep — is
    // wrong on every column of the four negative entries and right on the
    // three positive ones, so it lands at exactly three sevenths.
    CHECK(fractions.truncated == (fractions.columns / 7) * 3);
}

TEST_CASE("the 8 in above_preliminary_surface's boundary is a literal, not geometry",
          "[conformance][surface]") {
    const std::filesystem::path root = fixtures() / "probes" / "apsb3";
    if (!std::filesystem::is_directory(fixtures() / "worldgen") ||
        !std::filesystem::is_directory(root)) {
        SKIP("no geometry sweep at "
             << root << "; generate it with tools/analysis/aps-boundary-probe.sh --accept-eula");
    }

    const World world{42};
    // Cell height (size_vertical 1/2/4), cell width (size_horizontal 1/2/4)
    // and sea level, all in the default -64..320 world.
    const Boundaries cells = scoreFamily(world, "apsb3",
                                         {{"g_base", 100},
                                          {"g_sv2", 100},
                                          {"g_sv4", 100},
                                          {"g_sv4b", 200},
                                          {"g_sh2", 100},
                                          {"g_sh4", 100},
                                          {"g_sv2_sh2", 100},
                                          {"g_sea63", 100},
                                          {"g_sea200", 100}});
    INFO("cell shape and sea level: " << cells.right << " of " << cells.columns);
    REQUIRE(cells.columns > 300000);
    CHECK(cells.right == cells.columns);

    // The three that move the world's own floor and roof, each scored over
    // its own vertical extent.
    for (const auto& [entry, minY, height] :
         {std::tuple{"g_miny_m32", -32, 256}, std::tuple{"g_miny_p0", 0, 256},
          std::tuple{"g_h128", -64, 192}}) {
        const Boundaries score = scoreFamily(world, "apsb3", {{entry, 100}}, minY, height);
        INFO(entry << ": " << score.right << " of " << score.columns);
        CHECK(score.columns > 30000);
        CHECK(score.right == score.columns);
    }
}

TEST_CASE("the boundary holds at a second seed", "[conformance][surface]") {
    const std::filesystem::path root = fixtures() / "probes" / "apsb2";
    if (!std::filesystem::is_directory(fixtures() / "worldgen") ||
        !std::filesystem::is_directory(root)) {
        SKIP("no second-seed sweep at "
             << root << "; generate it with tools/analysis/aps-boundary-probe.sh --accept-eula");
    }

    // A per-column field fitted at one seed is exactly the kind of thing that
    // matches by luck, and this project has been burned by it before.
    const World world{31337};
    const Boundaries score = scoreFamily(world, "apsb2",
                                         {{"k_m40", -40},
                                          {"k_m8", -8},
                                          {"k_p0", 0},
                                          {"k_p1", 1},
                                          {"k_p7", 7},
                                          {"k_p64", 64},
                                          {"r_p0_5", 0.5},
                                          {"r_m0_5", -0.5},
                                          {"t_p0", 0}});
    INFO("seed 31337: " << score.right << " of " << score.columns);
    REQUIRE(score.columns > 300000);
    CHECK(score.right == score.columns);
    CHECK(score.atPsl == 0);
}

TEST_CASE("real overworlds, both directions of the boundary at once", "[conformance][surface]") {
    // The probes settle the boundary; this is what it costs in a world
    // someone would actually play. A `grass_block` is placed by the
    // surface-materials subtree, and that subtree is what
    // `above_preliminary_surface` gates — it is `surface_rule.sequence[1]`
    // in the overworld, the amplified and the large-biomes settings, and the
    // condition appears nowhere else in any of vanilla's seven dimensions.
    // So a grass_block BELOW `preliminary_surface_level` is a block the old
    // reading cannot place at all, and the measured band has to account for
    // it.
    //
    // THAT DIRECTION ALONE PROVES TOO LITTLE, which is why both are here.
    // Counting only what the old reading cannot explain rewards a boundary
    // for reaching further down; a boundary that reached to the world floor
    // would score perfectly on it. The other direction is the band the new
    // reading OPENS and the old one leaves shut — `[psl + depth - 8, psl)` —
    // read at the one position in it where vanilla's own behaviour is
    // pinned: the column's SURFACE, the first block reached from the sky that
    // is neither air nor fluid. There the materials subtree is what decides
    // the block. If the new reading over-reaches, that is where it shows.
    //
    // Measured, 8 regions: 11953 columns have their own surface inside the
    // band, and 10676 of them carry a block only the gated subtree can place.
    // The other 1277 do NOT refute the reading and are not counted as if they
    // did: 1265 are `minecraft:stone`, which that subtree can itself place
    // (so the block is silent about whether the subtree ran), and 12 are ore
    // and granite, written by features after the surface pass. Absence of a
    // material is never a refutation here — the subtree is a `sequence` whose
    // own inner rules may decline at a position its gate opened — so this
    // measures a bound, and says so.
    const std::filesystem::path regions = fixtures() / "regions";
    if (!std::filesystem::is_directory(fixtures() / "worldgen") ||
        !std::filesystem::is_directory(regions)) {
        SKIP("no golden regions at " << regions << "; generate them with tools/fetch-vanilla");
    }

    long long grass = 0;
    long long belowOld = 0;
    long long insideBand = 0;
    long long surfaceInBand = 0;
    long long surfaceMaterial = 0;
    long long seedsWithGrass = 0;
    std::map<std::string, long long> surfaceBlocks;
    int scored = 0;

    for (const std::int64_t seed : goldenSeeds()) {
        const std::filesystem::path region = overworldRegion(seed);
        if (!std::filesystem::is_regular_file(region)) {
            continue;
        }
        const World world{seed};
        const auto file = stratum::region::RegionFile::open(region);
        const std::int32_t minY = world.overworld().geometry.minY;
        const std::int32_t topY = minY + world.overworld().geometry.height;
        long long seedBelowOld = 0;
        long long seedSurfaceInBand = 0;
        for (std::int32_t chunkZ = 0; chunkZ < 32; ++chunkZ) {
            for (std::int32_t chunkX = 0; chunkX < 32; ++chunkX) {
                if (!file.hasChunk(chunkX, chunkZ)) {
                    continue;
                }
                const auto chunk = stratum::chunk::Chunk::decode(
                    stratum::nbt::read(file.readChunk(chunkX, chunkZ)).root);
                for (int localZ = 0; localZ < 16; ++localZ) {
                    for (int localX = 0; localX < 16; ++localX) {
                        const std::int32_t x = (chunkX * 16) + localX;
                        const std::int32_t z = (chunkZ * 16) + localZ;
                        const std::int32_t psl = world.preliminarySurface(x, z);
                        const std::int32_t depth = world.surfaceDepth(x, z);

                        // Forward: blocks the OLD reading cannot place.
                        for (std::int32_t y = minY; y < topY; ++y) {
                            const auto* found = chunk.blockAt(localX, y, localZ);
                            if (found == nullptr || found->name != "minecraft:grass_block") {
                                continue;
                            }
                            ++grass;
                            if (y >= psl) {
                                continue;
                            }
                            ++belowOld;
                            ++seedBelowOld;
                            insideBand += static_cast<long long>(y >= psl + depth - 8);
                        }

                        // Reverse: the column's own surface, when it falls in
                        // the band the new reading opens.
                        for (std::int32_t y = topY - 1; y >= minY; --y) {
                            const auto* found = chunk.blockAt(localX, y, localZ);
                            if (found == nullptr) {
                                continue;
                            }
                            const std::string& name = found->name;
                            if (name == "minecraft:air" || name == "minecraft:cave_air" ||
                                name == "minecraft:water" || name == "minecraft:lava") {
                                continue;
                            }
                            if (y >= psl + depth - 8 && y < psl) {
                                ++surfaceInBand;
                                ++seedSurfaceInBand;
                                ++surfaceBlocks[name];
                                surfaceMaterial += static_cast<long long>(gatedOnlyMaterial(name));
                            }
                            break;
                        }
                    }
                }
            }
        }
        INFO("seed " << seed << ": " << seedBelowOld << " grass_blocks below the old boundary, "
                     << seedSurfaceInBand << " column surfaces inside the band");
        seedsWithGrass += static_cast<long long>(seedBelowOld > 0);
        CHECK(seedSurfaceInBand > 0);
        ++scored;
    }

    REQUIRE(scored == 8);
    // Seed 2891948927356891's region is desert and ocean end to end and holds
    // no grass_block at all, so the forward direction is silent there. It is
    // not silent in the reverse one — 1006 column surfaces inside the band,
    // 987 of them sand or sandstone — which is part of why both are here.
    CHECK(seedsWithGrass == 7);

    INFO(grass << " grass_blocks, " << belowOld << " of them below the old boundary, " << insideBand
               << " inside the measured band");
    // The old reading switches the whole subtree off below `psl`, so each of
    // these is a block it cannot explain.
    CHECK(belowOld == 14008);
    // Most of them land inside the band, per column. The residual is not
    // noise and is not waved away: a spatially varying `preliminary_surface_
    // level` reaches the condition SAMPLED AND INTERPOLATED rather than read
    // per column (SPEC §11, PROGRESS.md's M4 entry), which moves the
    // boundary by a few blocks on steep ground. That is measured, open, and
    // deliberately not implemented here.
    CHECK(insideBand == 11579);

    INFO(surfaceInBand << " column surfaces inside the band, " << surfaceMaterial
                       << " of them a block only the gated subtree places");
    CHECK(surfaceInBand == 11953);
    CHECK(surfaceMaterial == 10676);
    // Named rather than lumped into a residual: every one of the 1277 that is
    // not a gated-only material is either `stone`, which the subtree itself
    // places, or a feature's own block written after the surface pass. None
    // of them is a block the subtree could not have produced, so the reverse
    // direction produces no counter-example — and no confirmation of those
    // 1277 either.
    long long unattributable = 0;
    for (const auto& [name, count] : surfaceBlocks) {
        if (gatedOnlyMaterial(name)) {
            continue;
        }
        INFO("not a gated-only material: " << name << " x" << count);
        CHECK((name == "minecraft:stone" || name == "minecraft:granite" ||
               name == "minecraft:copper_ore"));
        unattributable += count;
    }
    CHECK(unattributable == 1277);
}

TEST_CASE("the surface depth never reaches a negative integer in the eight golden regions",
          "[conformance][surface]") {
    // `8 - surfaceDepth` and `8 - max(0, surfaceDepth)` are the same boundary
    // on every column whose depth is >= 0, so the clamp question is a question
    // about the depth's LOWER TAIL and nothing else. This is that tail,
    // measured over every column of every golden overworld region rather than
    // asserted: 2097152 columns at eight seeds.
    //
    // Inside those regions vanilla's own `minecraft:surface` — three octaves
    // of amplitude 1 from first octave -6 — does not get there. The RAW value
    // does go below zero (207 columns, all of them at seed 0), but the cast
    // truncates toward zero, so a raw of -0.45 is a depth of 0, which is
    // exactly what a clamp would have produced.
    //
    // READ THE SCOPE OF THIS CASE CAREFULLY. It says the eight golden regions
    // contain no such column. It does NOT say vanilla cannot produce one, and
    // an earlier version of this case was named as though it did. The case
    // below — "vanilla does reach a negative surface depth" — finds four, and
    // the two together are what the clamp question actually rests on: the
    // event is real and is about one column in 134 million, which is why a
    // two-million-column census sees none of it.
    if (!std::filesystem::is_directory(fixtures() / "worldgen") ||
        !std::filesystem::is_directory(fixtures() / "regions")) {
        SKIP("no golden regions at " << (fixtures() / "regions")
                                     << "; generate them with tools/fetch-vanilla");
    }

    long long columns = 0;
    long long negative = 0;
    long long rawNegative = 0;
    long long zero = 0;
    long long castDisagreed = 0;
    double lowestRaw = std::numeric_limits<double>::infinity();
    int scored = 0;

    for (const std::int64_t seed : goldenSeeds()) {
        const std::filesystem::path region = overworldRegion(seed);
        if (!std::filesystem::is_regular_file(region)) {
            continue;
        }
        const World world{seed};
        const auto file = stratum::region::RegionFile::open(region);
        long long seedColumns = 0;
        double seedLowest = std::numeric_limits<double>::infinity();
        for (std::int32_t chunkZ = 0; chunkZ < 32; ++chunkZ) {
            for (std::int32_t chunkX = 0; chunkX < 32; ++chunkX) {
                if (!file.hasChunk(chunkX, chunkZ)) {
                    continue;
                }
                for (int localZ = 0; localZ < 16; ++localZ) {
                    for (int localX = 0; localX < 16; ++localX) {
                        const std::int32_t x = (chunkX * 16) + localX;
                        const std::int32_t z = (chunkZ * 16) + localZ;
                        const double raw = world.surfaceDepthRaw(x, z);
                        const std::int32_t depth = world.surfaceDepth(x, z);
                        ++seedColumns;
                        seedLowest = std::min(seedLowest, raw);
                        negative += static_cast<long long>(depth < 0);
                        zero += static_cast<long long>(depth == 0);
                        rawNegative += static_cast<long long>(raw < 0.0);
                        // The cast is truncation toward zero, which is what
                        // makes the two questions different: a raw in (-1, 0)
                        // is already a depth of 0.
                        castDisagreed +=
                            static_cast<long long>(depth != static_cast<std::int32_t>(raw));
                    }
                }
            }
        }
        columns += seedColumns;
        lowestRaw = std::min(lowestRaw, seedLowest);
        INFO("seed " << seed << ": " << seedColumns << " columns, lowest raw depth " << seedLowest);
        CHECK(seedColumns == 262144);
        ++scored;
    }

    REQUIRE(scored == 8);
    INFO(columns << " columns, " << negative << " with a negative depth, " << rawNegative
                 << " with a negative RAW depth, " << zero << " at exactly 0, lowest raw "
                 << lowestRaw);
    CHECK(columns == 2097152);
    // A structural guard, not a measurement: `surfaceDepth` IS `(int)` of
    // `surfaceDepthRaw`, so this compares a function with itself and cannot
    // fail today. It is here to catch a future divergence between them, and
    // it establishes nothing about the cast on its own.
    CHECK(castDisagreed == 0);
    // Absent from these regions — which is not the same as absent from
    // vanilla; see the case below.
    CHECK(negative == 0);
    // ... and not marginally absent HERE: within the golden regions the raw
    // value never came within half a block of the -1 it would take.
    CHECK(lowestRaw > -1.0);
    // It does go negative, though, which is what the truncation-versus-floor
    // measurement lives on — so the two questions are not the same question.
    CHECK(lowestRaw < 0.0);
    CHECK(rawNegative == 207);
    // What IS common at the bottom of the range is 0, which matters on its
    // own account because `hole` is exactly `depth <= 0`.
    CHECK(zero == 6745);
}

TEST_CASE("vanilla does reach a negative surface depth, four columns in half a billion",
          "[conformance][surface]") {
    // The counter-example to the case above, and the reason its name is
    // scoped to the golden regions. The clamp question — `8 - surfaceDepth`
    // against `8 - max(0, surfaceDepth)` — was twice written up in this
    // project as closed: first as "the depth reaches -1 on about one column in
    // 22000" (far too common), then as "vanilla's amplitudes do not produce
    // one at all" (false). Both were wrong, in opposite directions, and both
    // were stated from a sample that could not support either.
    //
    // WHAT FOUND THESE. The same eight golden seeds, swept over x, z in
    // [-4096, 4096) — 67108864 columns each, 536870912 in all, versus the
    // 2097152 of the r.0.0 regions. Four columns come back at depth -1, all
    // at one seed. That is about one in 134 million: the golden census is
    // roughly 256x too small to expect even one, so finding none there was
    // never evidence of anything. The sweep is not re-run here — it is ~9
    // minutes of pure noise evaluation — but its four answers are pinned, and
    // any change to the derivation moves them.
    //
    // WHY IT MATTERS RATHER THAN BEING A CURIOSITY. At depth -1 the two
    // candidates differ over exactly one block: the unclamped reading opens
    // the gated subtree at `y = psl - 9`, the clamped one at `y = psl - 8`.
    // These four columns sit in chunk (142, 117) — region r.4.3.mca, which is
    // not in the golden set — so generating that one region and reading that
    // one block separates the two readings directly. No data pack is needed,
    // which is what the previous write-up concluded was the only route.
    if (!std::filesystem::is_directory(fixtures() / "worldgen")) {
        SKIP("no vanilla pack at " << (fixtures() / "worldgen")
                                   << "; generate it with tools/fetch-vanilla");
    }

    const World world{std::int64_t{-4172144997902289642}};

    struct Column {
        std::int32_t x;
        std::int32_t z;
        double raw;
    };

    // Every column vanilla is known to reach a negative depth on.
    static constexpr std::array<Column, 4> kNegative{{
        {2282, 1879, -1.033659749},
        {2282, 1880, -1.048435300},
        {2283, 1880, -1.008497344},
        {2284, 1880, -1.010109149},
    }};

    for (const auto& column : kNegative) {
        INFO("column (" << column.x << ", " << column.z << ")");
        CHECK(world.surfaceDepth(column.x, column.z) == -1);
        CHECK(world.surfaceDepthRaw(column.x, column.z) ==
              Catch::Approx(column.raw).epsilon(0.0).margin(5e-9));
        // The clamp would read 0 here, and the whole question is that these
        // are one block apart rather than equal.
        CHECK(std::max(0, world.surfaceDepth(column.x, column.z)) == 0);
    }

    // And the chunk they are in, which is what has to be generated to close
    // the question: floorDiv(2282, 16) == 142, floorDiv(1879, 16) == 117.
    for (const auto& column : kNegative) {
        CHECK(stratum::javamath::floorDiv(column.x, 16) == 142);
        CHECK(stratum::javamath::floorDiv(column.z, 16) == 117);
    }
}

TEST_CASE("the band's lower edge is the condition going false, not the surface pass stopping",
          "[conformance][surface]") {
    // The confound the whole finding rests on. A probe that paints a marker
    // wherever `above_preliminary_surface` holds cannot, on its own, tell
    // "the condition went false at y" from "the vanilla surface pass stopped
    // at y" — a loop bound at the same height would paint exactly the same
    // band.
    //
    // The control is on disk and is the reason the reading stands: in
    // `probes/surf`, one world, one pinned `preliminary_surface_level`, three
    // entries over the same terrain. `bandlands` has NO condition at all and
    // `steep` has one that does not read y; both paint all the way to the
    // world floor. `aps` stops between -8 and -2 — that is,
    // `psl + surfaceDepth - 8` with psl pinned to 0 and the depth in 0..6. A
    // surface pass that stopped at a height would have clipped all three.
    const std::filesystem::path root = fixtures() / "probes" / "surf";
    if (!std::filesystem::is_directory(fixtures() / "worldgen") ||
        !std::filesystem::is_directory(root)) {
        SKIP("no surface-rule probe at " << root);
    }
    for (const char* entry : {"aps", "bandlands", "steep"}) {
        if (!std::filesystem::is_regular_file(root / entry / "r.0.0.mca")) {
            SKIP("no " << entry << " entry under " << root);
        }
    }

    constexpr std::int32_t kMinY = -64;
    constexpr std::int32_t kHeight = 384;

    // `density-probe.sh` pins `preliminary_surface_level` to the constant 0
    // for every entry that does not override it, so the condition's own
    // boundary here is `surfaceDepth - 8`, and the depth over these columns
    // runs 0..6.
    const Painted aps = scorePainted(root / "aps" / "r.0.0.mca", kMinY, kHeight);
    INFO("aps: " << aps.columns << " painted columns, lower edges " << aps.lowestEdge << ".."
                 << aps.highestEdge << ", " << aps.toFloor << " reaching the world floor");
    CHECK(aps.columns == 36864);
    CHECK(aps.lowestEdge == -8);
    CHECK(aps.highestEdge == -2);
    CHECK(aps.toFloor == 0);

    // No condition whatsoever, same world and same terrain: every column down
    // to the floor.
    const Painted bands = scorePainted(root / "bandlands" / "r.0.0.mca", kMinY, kHeight);
    INFO("bandlands: " << bands.columns << " painted columns, " << bands.toFloor
                       << " reaching the world floor");
    CHECK(bands.columns == 36864);
    CHECK(bands.toFloor == bands.columns);
    CHECK(bands.lowestEdge == kMinY);

    // A condition that does not read y: it selects columns rather than
    // heights, and in the columns it selects it paints to the floor too.
    const Painted steep = scorePainted(root / "steep" / "r.0.0.mca", kMinY, kHeight);
    INFO("steep: " << steep.columns << " painted columns, " << steep.toFloor
                   << " reaching the world floor");
    CHECK(steep.columns == 6217);
    CHECK(steep.toFloor == steep.columns);
    CHECK(steep.lowestEdge == kMinY);
}

TEST_CASE("a spatially varying preliminary surface level reaches the condition interpolated",
          "[conformance][surface]") {
    // The open question's own evidence, pinned so that the number in SPEC §11
    // and PROGRESS.md's M4 entry cannot drift from the fixture again. The
    // entry drives `preliminary_surface_level` with a three-valued
    // `range_choice` (-40 / 0 / 60). Were it read per column, as
    // `terrain::ChunkFiller` reads it, the condition would see three values
    // and the band's lower edge could take at most 3 * 7 of them.
    //
    // Two counts, and they are NOT the same number — which is exactly what
    // the documentation had left ambiguous:
    //   * the boundary itself, `psl + surfaceDepth - 8`, takes 104 distinct
    //     values, a contiguous run from -47 to 56.
    //   * the psl that reaches the condition, recovered from it as
    //     `boundary - surfaceDepth + 8`, takes 101, a contiguous run from
    //     -40 to 60 — the range_choice's own two extremes and every integer
    //     between them.
    const std::filesystem::path region = fixtures() / "probes" / "apsb" / "v_psl" / "r.0.0.mca";
    if (!std::filesystem::is_directory(fixtures() / "worldgen") ||
        !std::filesystem::is_regular_file(region)) {
        SKIP("no varying-psl entry at "
             << region << "; generate it with tools/analysis/aps-boundary-probe.sh --accept-eula");
    }

    const World world{42};
    const auto file = stratum::region::RegionFile::open(region);
    std::map<std::int32_t, long long> boundaries;
    std::map<std::int32_t, long long> levels;
    long long columns = 0;

    for (std::int32_t chunkZ = 0; chunkZ < 12; ++chunkZ) {
        for (std::int32_t chunkX = 0; chunkX < 12; ++chunkX) {
            if (!file.hasChunk(chunkX, chunkZ)) {
                continue;
            }
            const auto chunk = stratum::chunk::Chunk::decode(
                stratum::nbt::read(file.readChunk(chunkX, chunkZ)).root);
            for (int localZ = 0; localZ < 16; ++localZ) {
                for (int localX = 0; localX < 16; ++localX) {
                    for (std::int32_t y = -64; y < 320; ++y) {
                        const auto* found = chunk.blockAt(localX, y, localZ);
                        if (found == nullptr || found->name != "minecraft:diamond_block") {
                            continue;
                        }
                        const std::int32_t x = (chunkX * 16) + localX;
                        const std::int32_t z = (chunkZ * 16) + localZ;
                        ++columns;
                        ++boundaries[y];
                        ++levels[y - world.surfaceDepth(x, z) + 8];
                        break;
                    }
                }
            }
        }
    }

    INFO(columns << " columns, " << boundaries.size() << " distinct boundaries, " << levels.size()
                 << " distinct levels");
    REQUIRE(columns == 36864);
    CHECK(boundaries.size() == 104);
    CHECK(boundaries.begin()->first == -47);
    CHECK(boundaries.rbegin()->first == 56);
    CHECK(levels.size() == 101);
    CHECK(levels.begin()->first == -40);
    CHECK(levels.rbegin()->first == 60);
    // Contiguous, which is the part that rules out "three values plus a
    // little quantisation noise": every integer between the range_choice's
    // own extremes is reached by some column.
    CHECK(static_cast<std::int32_t>(levels.size()) ==
          levels.rbegin()->first - levels.begin()->first + 1);
}
