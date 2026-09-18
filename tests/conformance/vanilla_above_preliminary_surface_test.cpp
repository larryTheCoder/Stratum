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
// WHAT IT MEASURES, and each of the three is a separate case below:
//   * the boundary is `floor(psl) + surfaceDepth(x, z) - 8`, so `psl` enters
//     as a pure per-block offset and the column's own surface depth SUBTRACTS
//     from it. Nineteen constants from -60 to +200 pin the offset.
//   * the double -> y conversion is FLOOR, not truncation toward zero. Only
//     a negative fraction separates them, which vanilla's own integer-valued
//     `find_top_surface` never produces — so it is a datapack-only
//     difference, and it is still measured rather than assumed.
//   * the 8 is a LITERAL. Twelve geometry variants — cell heights 4/8/16,
//     cell widths 4/8/16, three floors, three heights, three sea levels —
//     leave it at 8.
//
// The fixtures are Mojang-derived and never committed (SPEC §12).
#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/executor.hpp>
#include <stratum/surface/rule_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>
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

TEST_CASE("real overworlds place surface blocks the old reading forbids",
          "[conformance][surface]") {
    // The probes settle the boundary; this is what it costs in a world
    // someone would actually play. A `grass_block` is placed by the
    // surface-materials subtree, and that subtree is what
    // `above_preliminary_surface` gates — it is `surface_rule.sequence[1]`
    // in the overworld, the amplified and the large-biomes settings, and the
    // condition appears nowhere else in any of vanilla's seven dimensions.
    // So a grass_block BELOW `preliminary_surface_level` is a block the old
    // reading cannot place at all, and the measured band has to account for
    // it.
    const std::filesystem::path regions = fixtures() / "regions";
    if (!std::filesystem::is_directory(fixtures() / "worldgen") ||
        !std::filesystem::is_directory(regions)) {
        SKIP("no golden regions at " << regions << "; generate them with tools/fetch-vanilla");
    }

    long long grass = 0;
    long long belowOld = 0;
    long long insideBand = 0;
    int scored = 0;

    for (const std::int64_t seed : {std::int64_t{0}, std::int64_t{-1}, std::int64_t{42},
                                    std::int64_t{-4172144997902289642}}) {
        const std::filesystem::path region =
            regions / ("seed-" + std::to_string(seed)) / "overworld" / "r.0.0.mca";
        if (!std::filesystem::is_regular_file(region)) {
            continue;
        }
        const World world{seed};
        const auto file = stratum::region::RegionFile::open(region);
        const std::int32_t minY = world.overworld().geometry.minY;
        const std::int32_t topY = minY + world.overworld().geometry.height;
        long long seedBelowOld = 0;
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
                        for (std::int32_t y = minY; y < topY; ++y) {
                            const auto* found = chunk.blockAt(localX, y, localZ);
                            if (found == nullptr || found->name != "minecraft:grass_block") {
                                continue;
                            }
                            ++grass;
                            const std::int32_t psl = world.preliminarySurface(x, z);
                            if (y >= psl) {
                                continue;
                            }
                            ++belowOld;
                            ++seedBelowOld;
                            insideBand +=
                                static_cast<long long>(y >= psl + world.surfaceDepth(x, z) - 8);
                        }
                    }
                }
            }
        }
        INFO("seed " << seed << ": " << seedBelowOld << " grass_blocks below the old boundary");
        CHECK(seedBelowOld > 0);
        ++scored;
    }

    REQUIRE(scored >= 3);
    INFO(grass << " grass_blocks, " << belowOld << " of them below the old boundary, " << insideBand
               << " inside the measured band");
    // The old reading switches the whole subtree off below `psl`, so each of
    // these is a block it cannot explain.
    CHECK(belowOld > 4000);
    // Most of them land inside the band, per column. The residual is not
    // noise and is not waved away: a spatially varying `preliminary_surface_
    // level` reaches the condition SAMPLED AND INTERPOLATED rather than read
    // per column (SPEC §11, PROGRESS.md's M4 entry), which moves the
    // boundary by a few blocks on steep ground. That is measured, open, and
    // deliberately not implemented here — so this is a floor, not equality.
    CHECK(insideBand * 10 >= belowOld * 8);
}
