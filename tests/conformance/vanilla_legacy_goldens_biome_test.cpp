// Stratum — what the Nether goldens' stored biomes can and cannot say about
// the legacy seeding.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// tools/analysis/legacy-goldens-biome-analyze.cpp reads the legacy-seeded
// climate noises back out of the eight golden Nether regions and scores
// tools/analysis/legacy-seed-analyze.cpp's 270,000 candidates against them.
// This file pins the parts of that apparatus that can be checked with the
// library alone: the table the biomes are decoded against, which noises the
// readback can possibly speak for, what the denominator is, and — first, and
// as its own case — that the decoder recovers a seeding this build already
// knows is right while rejecting one it knows is wrong.
//
// THE CONTROL IS CASE ONE AND IT IS NOT DECORATION. A readback that reports
// "no candidate survives" is worth nothing until the same decoder has been
// shown to score a KNOWN-CORRECT rule at ~100% and a known-wrong one at the
// null. Both arms run here, on the OVERWORLD goldens, where the climate noises
// use the modern seeding this build reproduces — every fourth chunk of all
// eight regions at two heights, 16384 cells:
//
//   worldSeed      16374 / 16384 = 99.939%
//   worldSeed + 1    917 / 16384 =  5.597%
//   null            3618 / 57344 =  6.309%   (vanilla at one seed against
//                                             vanilla at another, same cells)
//
// The analyzer runs the same three arms over four heights instead of two —
// 32768 cells, 32748 / 1858 / 7307-of-114688, i.e. 99.939% / 5.670% / 6.371%.
// Same columns, same conclusion, twice the cells.
//
// The null is the third arm and it is what makes the second readable. A wrong
// seeding is not a random relabeling: it shares the coordinates, the y
// structure and vanilla's own biome marginals, so "two independent draws"
// underestimates what it reaches by chance. Measuring it as vanilla-against-
// vanilla over every pair of the eight worlds keeps every shared structure and
// removes only the seed.
//
// AND THE POSITIVE ARM IS 99.939%, NOT 100%, WHICH IS A FINDING. The [biome]
// conformance cases report the overworld biome source as exact, and over their
// sample it is — a 64x64-block corner of four regions, 98304 cells, no miss.
// Spread the identical decode over the whole 512x512 of eight regions and five
// COLUMNS disagree, each of them at every sampled height, so a horizontal
// disagreement and not a depth one:
//
//   seed -1                   (196, 268)  deep_ocean      -> ocean
//   seed -4172144997902289642 (460, 200)  mushroom_fields -> deep_lukewarm_ocean
//   seed 0                    (328,   4)  river           -> beach
//   seed 9223372036854775807  ( 68, 128)  river           -> forest
//   seed 9223372036854775807  ( 72, 128)  river           -> forest
//
// Five in 8192 columns. It is a residual of the biome source rather than of
// this readback — lib/include/stratum/biome/parameter_list.hpp already records
// that "later row wins" is a measured match to vanilla's search TREE and not a
// derivation of it, and these five are the kind of counterexample that note
// anticipates. It is named here with seeds and coordinates rather than rounded
// into "exact", and it is far too small to blunt a control that has to
// separate 99.94% from 6.37%.
//
// WHAT THE READBACK IS. The Nether's router sets continents, erosion, depth and
// ridges to the constant 0.0, so its biome at a cell is a function of
// temperature and vegetation alone, and those two arrive through
// `minecraft:temperature`, `minecraft:vegetation` and the `minecraft:offset`
// that feeds shift_x/shift_z. Each stored biome is therefore a COARSE reading
// of those three noises at that column — a cell of a five-way partition, not a
// number.
//
// AND WHAT IT IS NOT, which is the number that bounds everything the scan can
// conclude. Over the six independent worlds this suite has:
//
//   cells                                       24576
//   boundary-adjacent (a 4-neighbour differs)     979  (3.98%)
//   interior                                    23597  (96.02%)
//   nether_wastes / crimson_forest / soul_sand_valley / warped_forest /
//   basalt_deltas                27.79 / 38.66 / 13.66 / 7.48 / 12.41 %
//
// A labeling drawn independently with vanilla's own marginals already agrees
// 26.64% of the time. So this readback separates "exactly right" from
// "everything else"; it cannot rank near-misses, and a score of 40% over it is
// not 40% of the way to an answer.
//
// THE MEASUREMENT THE ANALYZER MAKES, recorded here so the claim has a
// denominator in the repository and not only in a run someone remembers:
// 270,000 candidates (900 seed rules x 300 block offsets), NO SURVIVOR. A
// CORRECT rule scores ~100% — that is not an estimate, it is what the control
// measures on the overworld goldens, 99.94%. The best of the 270,000, rescored
// over the full 24576 cells, reaches 53.16%.
//
// The two numbers that comparison needs, and the one it must not use. The null
// IS measured — every candidate in the space is wrong, so their scores are the
// null distribution — but it is measured at stage 1's 1536 cells (mean 26.48%,
// sd 8.19 points), and a maximum read off that sample is not a threshold for a
// score at 24576. It is also not independent of the candidate: the largest
// stage-1 score in the space is the top row of the top-32 table itself, so
// comparing the winner against it compares a candidate with itself. What DOES
// carry: every one of the top 32 FALLS when the denominator grows 16x — mean
// 54.21% at 1536 cells against 48.43% at 24576, no exceptions — and stage 1's
// leader lands third once rescored. That is the shape of selection noise, not
// of a signal.
//
// The null has since been measured at the denominator the answer uses, by
// scoring all 270,000 over all 24576 cells: mean 25.54%, sd 7.25 points, and a
// maximum of 53.16% that IS the candidate above — so the two-stage scan lost
// nothing, and the best of this space is 47 points short of a correct rule
// rather than a hair under a borrowed threshold. SPEC §11 carries that table,
// the rank distribution, and the numbers above; they come from
//   build/release/tools/analysis/stratum_legacy_goldens_biome_analyze .fixtures --scan
// which is not run from CTest because it is minutes of compute; its --control,
// --model and --modern arms are, as conformance.legacy_goldens_biome_control.
//
// WHAT NONE OF IT TOUCHES, and the reason the amplitudes below are pinned as
// they are: a Perlin block is drawn only for a NON-ZERO amplitude, because
// that is what the modern draw does. A legacy draw that consumed one per
// DECLARED octave is outside the candidate space at every block offset, and
// --model cannot see the difference, because at the modern seeding nothing is
// consumed sequentially. `temperature` and `vegetation` declare four zero
// amplitudes each of six and `offset` one of four — nine dead octaves that
// this scan assumes cost nothing.
//
// The fixtures are Mojang-derived and never committed (SPEC §12). Without them
// every case here skips.
#include <stratum/biome/parameter_list.hpp>
#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_parameters.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/settings/noise_settings.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {

using stratum::biome::ClimateSample;
using stratum::biome::ParameterList;
using stratum::data::Pack;
using stratum::data::ResourceLocation;
using stratum::settings::RouterEntry;

/// The eight seeds the goldens carry. SIX WORLDS: a legacy dimension's
/// randomness is the Java LCG, whose scramble keeps only the low 48 bits of the
/// seed, so 0 collides with Long.MIN_VALUE and -1 with Long.MAX_VALUE.
constexpr std::array<std::int64_t, 8> kAllSeeds{
    0,
    1,
    -1,
    42,
    2891948927356891LL,
    -4172144997902289642LL,
    9223372036854775807LL,
    -9223372036854775807LL - 1,
};

/// The six distinct ones, which is what a Nether denominator is built from.
constexpr std::array<std::int64_t, 6> kDistinctSeeds{
    0, 1, -1, 42, 2891948927356891LL, -4172144997902289642LL,
};

constexpr std::int32_t kNetherChunkStride = 2;
constexpr std::int32_t kNetherSampleY = 64;

[[nodiscard]] std::filesystem::path fixtures() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR};
}

[[nodiscard]] std::filesystem::path findWorldgenTree() {
    if (!std::filesystem::is_directory(fixtures())) {
        return {};
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(fixtures())) {
        if (entry.is_directory() && entry.path().filename() == "worldgen" &&
            std::filesystem::is_directory(entry.path() / "density_function")) {
            return entry.path();
        }
    }
    return {};
}

[[nodiscard]] std::filesystem::path findParameterList(std::string_view filename) {
    if (!std::filesystem::is_directory(fixtures())) {
        return {};
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(fixtures())) {
        if (entry.is_regular_file() && entry.path().filename() == filename &&
            entry.path().parent_path().parent_path().filename() == "biome_parameters") {
            return entry.path();
        }
    }
    return {};
}

[[nodiscard]] std::filesystem::path regionOf(std::int64_t seed, std::string_view dimension) {
    if (!std::filesystem::is_directory(fixtures())) {
        return {};
    }
    const std::string wanted = "seed-" + std::to_string(seed);
    for (const auto& entry : std::filesystem::recursive_directory_iterator(fixtures())) {
        if (!entry.is_regular_file() || entry.path().filename() != "r.0.0.mca") {
            continue;
        }
        const std::filesystem::path parent = entry.path().parent_path();
        if (parent.filename() == dimension && parent.parent_path().filename() == wanted) {
            return entry.path();
        }
    }
    return {};
}

[[nodiscard]] ParameterList readTable(const std::filesystem::path& path, std::string_view id) {
    std::ifstream stream(path);
    return ParameterList::fromJson(nlohmann::json::parse(stream), ResourceLocation::parse(id));
}

} // namespace

TEST_CASE("the decoder recovers a known-correct seeding and rejects a known-wrong one",
          "[conformance][legacy][goldens-biome]") {
    const std::filesystem::path tree = findWorldgenTree();
    const std::filesystem::path table = findParameterList("overworld.json");
    if (tree.empty() || table.empty()) {
        SKIP("no extracted vanilla worldgen or biome parameters under "
             << STRATUM_FIXTURES_DIR
             << " — Mojang-derived and never committed (SPEC §12). Generate them with: "
                "tools/fetch-vanilla");
    }
    const ParameterList overworld = readTable(table, "minecraft:overworld");
    const Pack pack = Pack::open(tree);
    stratum::settings::LoadedSettings loaded = stratum::settings::loadAll(pack);
    const auto& settings = loaded.settings.at(ResourceLocation::parse("minecraft:overworld"));

    // Spread over the whole region, not a corner of it: over 32x32 blocks a
    // world is essentially one biome and the effective sample size is the
    // number of REGIONS. Measured at a corner, the wrong-seed arm read 10.56%
    // against a 3.87% null and looked like a control failure; it was the
    // sampling.
    constexpr std::int32_t kChunkStride = 4;
    constexpr std::array<std::int32_t, 2> kHeights{16, 64};

    std::vector<std::vector<std::string>> goldenPerSeed;
    std::size_t cells = 0;
    std::size_t positive = 0;
    std::size_t negative = 0;
    std::map<std::string, std::string> misses;

    for (const std::int64_t seed : kAllSeeds) {
        const std::filesystem::path region = regionOf(seed, "overworld");
        if (region.empty()) {
            continue;
        }
        const auto build = [&](std::int64_t with) {
            return stratum::density::NoiseRegistry::create(
                pack, loaded.graph.referencedNoises(), with,
                stratum::density::RandomSource::Xoroshiro);
        };
        const auto right = build(seed);
        const auto wrong = build(seed + 1);
        const stratum::density::CellGeometry geometry{.width = settings.geometry.cellWidth(),
                                                      .height = settings.geometry.cellHeight()};
        const stratum::density::Interpreter rightInterpreter(loaded.graph, right, geometry);
        const stratum::density::Interpreter wrongInterpreter(loaded.graph, wrong, geometry);
        const auto file = stratum::region::RegionFile::open(region);
        std::vector<std::string> golden;

        for (std::int32_t cz = 0; cz < stratum::region::kChunksPerAxis; cz += kChunkStride) {
            for (std::int32_t cx = 0; cx < stratum::region::kChunksPerAxis; cx += kChunkStride) {
                if (!file.hasChunk(cx, cz)) {
                    continue;
                }
                const auto chunk =
                    stratum::chunk::Chunk::decode(stratum::nbt::read(file.readChunk(cx, cz)).root);
                for (const auto& section : chunk.sections()) {
                    if (section.biomes.empty()) {
                        continue;
                    }
                    for (int cellY = 0; cellY < 4; ++cellY) {
                        const std::int32_t y = (section.y * 16) + (cellY * 4);
                        if (std::find(kHeights.begin(), kHeights.end(), y) == kHeights.end()) {
                            continue;
                        }
                        for (int cellZ = 0; cellZ < 4; ++cellZ) {
                            for (int cellX = 0; cellX < 4; ++cellX) {
                                const std::size_t index = (static_cast<std::size_t>(cellY) * 16U) +
                                                          (static_cast<std::size_t>(cellZ) * 4U) +
                                                          static_cast<std::size_t>(cellX);
                                if (index >= section.biomes.size()) {
                                    continue;
                                }
                                const std::string& stored =
                                    section.biomePalette[section.biomes[index]];
                                const stratum::density::Point at{.x = (cx * 16) + (cellX * 4),
                                                                 .y = y,
                                                                 .z = (cz * 16) + (cellZ * 4)};
                                const auto evaluate =
                                    [&](const stratum::density::Interpreter& with) {
                                        return ClimateSample{
                                            .temperature = with.evaluate(
                                                settings.router.at(RouterEntry::Temperature), at),
                                            .humidity = with.evaluate(
                                                settings.router.at(RouterEntry::Vegetation), at),
                                            .continentalness = with.evaluate(
                                                settings.router.at(RouterEntry::Continents), at),
                                            .erosion = with.evaluate(
                                                settings.router.at(RouterEntry::Erosion), at),
                                            .depth = with.evaluate(
                                                settings.router.at(RouterEntry::Depth), at),
                                            .weirdness = with.evaluate(
                                                settings.router.at(RouterEntry::Ridges), at)};
                                    };
                                ++cells;
                                const std::string chosen =
                                    overworld.find(evaluate(rightInterpreter)).toString();
                                if (chosen == stored) {
                                    ++positive;
                                } else {
                                    misses[std::to_string(seed) + " (" + std::to_string(at.x) +
                                           "," + std::to_string(at.z) + ")"] =
                                        stored + " -> " + chosen;
                                }
                                if (overworld.find(evaluate(wrongInterpreter)).toString() ==
                                    stored) {
                                    ++negative;
                                }
                                golden.push_back(stored);
                            }
                        }
                    }
                }
            }
        }
        goldenPerSeed.push_back(std::move(golden));
    }

    if (goldenPerSeed.empty()) {
        SKIP("no golden overworld regions under " << STRATUM_FIXTURES_DIR
                                                  << " — run tools/fetch-vanilla "
                                                     "--generate-regions");
    }
    CHECK(goldenPerSeed.size() == kAllSeeds.size());
    REQUIRE(cells > 0U);

    // The null: vanilla's own map at one seed against vanilla's own map at
    // another, cell for cell, over every pair. Not a model of chance — two real
    // worlds, every shared structure intact, only the seed different.
    std::size_t pairCells = 0;
    std::size_t pairAgree = 0;
    for (std::size_t a = 0; a < goldenPerSeed.size(); ++a) {
        for (std::size_t b = a + 1; b < goldenPerSeed.size(); ++b) {
            if (goldenPerSeed[a].size() != goldenPerSeed[b].size()) {
                continue;
            }
            for (std::size_t i = 0; i < goldenPerSeed[a].size(); ++i) {
                ++pairCells;
                if (goldenPerSeed[a][i] == goldenPerSeed[b][i]) {
                    ++pairAgree;
                }
            }
        }
    }
    REQUIRE(pairCells > 0U);

    const double positiveRate = static_cast<double>(positive) / static_cast<double>(cells);
    const double negativeRate = static_cast<double>(negative) / static_cast<double>(cells);
    const double nullRate = static_cast<double>(pairAgree) / static_cast<double>(pairCells);
    WARN("decoder control: worldSeed "
         << positive << " / " << cells << " = " << 100.0 * positiveRate << "%, worldSeed + 1 "
         << negative << " / " << cells << " = " << 100.0 * negativeRate
         << "%, null (vanilla vs vanilla) " << pairAgree << " / " << pairCells << " = "
         << 100.0 * nullRate << "%");
    for (const auto& [where, what] : misses) {
        WARN("    residual column: seed " << where << "  " << what);
    }

    // THE POSITIVE ARM. Not "== cells": the five columns above are a real
    // residual of the biome source and are listed rather than hidden. The bar
    // is that a correct seeding is recovered essentially everywhere.
    CHECK(cells == 16384U);
    CHECK(positiveRate > 0.999);
    // The residual, named rather than rounded away. Five columns, each wrong at
    // both sampled heights; see the header.
    CHECK(misses.size() == 5U);
    CHECK(misses.at("-1 (196,268)") == "minecraft:deep_ocean -> minecraft:ocean");
    CHECK(misses.at("-4172144997902289642 (460,200)") ==
          "minecraft:mushroom_fields -> minecraft:deep_lukewarm_ocean");
    CHECK(misses.at("0 (328,4)") == "minecraft:river -> minecraft:beach");
    CHECK(misses.at("9223372036854775807 (68,128)") == "minecraft:river -> minecraft:forest");
    CHECK(misses.at("9223372036854775807 (72,128)") == "minecraft:river -> minecraft:forest");
    // THE NEGATIVE ARM. A wrong seeding must fall to what an unrelated vanilla
    // world reaches. If it did not, the decoder would be accepting the shared
    // structure rather than the seeding, and every "no survivor" drawn through
    // it would be empty.
    CHECK(negativeRate <= nullRate + 0.02);
    // And the two must be far apart, or the readback has no resolving power at
    // all: an order of magnitude, measured, not asserted.
    CHECK(positiveRate > 10.0 * nullRate);
}

TEST_CASE("the Nether's biomes are decoded against the server's own preset table",
          "[conformance][legacy][goldens-biome]") {
    const std::filesystem::path table = findParameterList("nether.json");
    if (table.empty()) {
        SKIP("no biome_parameters under " << STRATUM_FIXTURES_DIR
                                          << " — Mojang-derived and never committed (SPEC §12); "
                                             "tools/fetch-vanilla dumps them with the server's own "
                                             "--reports data generator");
    }
    const ParameterList nether = readTable(table, "minecraft:nether");

    // WHERE THESE NUMBERS COME FROM. The pack ships
    // worldgen/multi_noise_biome_source_parameter_list/nether.json as nothing
    // but {"preset": "minecraft:nether"}; the five points are compiled into the
    // jar. tools/fetch-vanilla runs the server's own data generator and keeps
    // what it dumps. That is OBSERVED SERVER OUTPUT, which CLAUDE.md permits
    // where it forbids the source — so neither minecraft.wiki nor the jar's
    // code is a dependency of this build. Pinned so that a schema-pin move, or
    // a fetch that silently produced a different table, is a red test rather
    // than a quietly different world.
    REQUIRE(nether.size() == 5U);
    std::map<std::string, std::pair<double, double>> byBiome;
    for (const auto& entry : nether.entries()) {
        const auto& p = entry.parameters;
        // Every axis but temperature and humidity is a single point at zero,
        // which is what makes the Nether's biome a function of two numbers.
        // Compared as whole Parameters rather than field by field: these are
        // two DECLARATIONS, not two computed values, and the project builds
        // with -Wfloat-equal.
        constexpr stratum::biome::Parameter kZero{.min = 0.0, .max = 0.0};
        CHECK(p.temperature ==
              stratum::biome::Parameter{.min = p.temperature.min, .max = p.temperature.min});
        CHECK(p.humidity ==
              stratum::biome::Parameter{.min = p.humidity.min, .max = p.humidity.min});
        CHECK(p.continentalness == kZero);
        CHECK(p.erosion == kZero);
        CHECK(p.depth == kZero);
        CHECK(p.weirdness == kZero);
        byBiome[entry.biome.toString()] = {p.temperature.min, p.humidity.min};
    }
    CHECK(byBiome.size() == 5U);
    CHECK(byBiome.at("minecraft:nether_wastes") == std::pair{0.0, 0.0});
    CHECK(byBiome.at("minecraft:soul_sand_valley") == std::pair{0.0, -0.5});
    CHECK(byBiome.at("minecraft:crimson_forest") == std::pair{0.4, 0.0});
    CHECK(byBiome.at("minecraft:warped_forest") == std::pair{0.0, 0.5});
    CHECK(byBiome.at("minecraft:basalt_deltas") == std::pair{-0.5, 0.0});
}

TEST_CASE("the Nether's stored biome can only speak for three noises",
          "[conformance][legacy][goldens-biome]") {
    const std::filesystem::path tree = findWorldgenTree();
    if (tree.empty()) {
        SKIP("no extracted vanilla worldgen under " << STRATUM_FIXTURES_DIR);
    }
    const Pack pack = Pack::open(tree);
    const auto loaded = stratum::settings::loadAll(pack);
    const auto& nether = loaded.settings.at(ResourceLocation::parse("minecraft:nether"));
    REQUIRE(nether.legacyRandomSource);

    // The four constant entries. A readback of the stored biome says NOTHING
    // about these, because they reach no noise at all — they ARE numbers.
    for (const RouterEntry entry :
         {RouterEntry::Continents, RouterEntry::Erosion, RouterEntry::Depth, RouterEntry::Ridges}) {
        CHECK(loaded.graph.noisesReachableFrom(nether.router.at(entry)).empty());
    }

    std::vector<std::string> reachable;
    for (const RouterEntry entry : {RouterEntry::Temperature, RouterEntry::Vegetation}) {
        for (const auto& id : loaded.graph.noisesReachableFrom(nether.router.at(entry))) {
            const std::string name = id.toString();
            if (std::find(reachable.begin(), reachable.end(), name) == reachable.end()) {
                reachable.push_back(name);
            }
        }
    }
    std::sort(reachable.begin(), reachable.end());
    CHECK(reachable == std::vector<std::string>{"minecraft:offset", "minecraft:temperature",
                                                "minecraft:vegetation"});

    // And their declared parameters, which the analyzer's stack layout is built
    // from. Pinned because a layout built from the wrong amplitude list would
    // make the whole scan a scan of something else.
    const auto parametersOf = [&](std::string_view id) {
        const ResourceLocation location = ResourceLocation::parse(id);
        const auto* entry = pack.find(stratum::data::Registry::Noise, location);
        REQUIRE(entry != nullptr);
        return stratum::density::NoiseParameters::fromJson(entry->json, location);
    };
    const auto temperature = parametersOf("minecraft:temperature");
    CHECK(temperature.firstOctave == -10);
    CHECK(temperature.amplitudes == std::vector<double>{1.5, 0.0, 1.0, 0.0, 0.0, 0.0});
    const auto vegetation = parametersOf("minecraft:vegetation");
    CHECK(vegetation.firstOctave == -8);
    CHECK(vegetation.amplitudes == std::vector<double>{1.0, 1.0, 0.0, 0.0, 0.0, 0.0});
    const auto offset = parametersOf("minecraft:offset");
    CHECK(offset.firstOctave == -3);
    CHECK(offset.amplitudes == std::vector<double>{1.0, 1.0, 1.0, 0.0});
}

TEST_CASE("the readback's denominator, and the floor under every score it prints",
          "[conformance][legacy][goldens-biome]") {
    const std::filesystem::path table = findParameterList("nether.json");
    if (table.empty()) {
        SKIP("no biome_parameters under " << STRATUM_FIXTURES_DIR);
    }
    const ParameterList nether = readTable(table, "minecraft:nether");

    std::size_t cells = 0;
    std::size_t boundary = 0;
    std::size_t varyingWithY = 0;
    std::size_t worlds = 0;
    std::map<std::string, std::size_t> counts;

    for (const std::int64_t seed : kDistinctSeeds) {
        const std::filesystem::path region = regionOf(seed, "nether");
        if (region.empty()) {
            continue;
        }
        ++worlds;
        const auto file = stratum::region::RegionFile::open(region);
        for (std::int32_t cz = 0; cz < stratum::region::kChunksPerAxis; cz += kNetherChunkStride) {
            for (std::int32_t cx = 0; cx < stratum::region::kChunksPerAxis;
                 cx += kNetherChunkStride) {
                if (!file.hasChunk(cx, cz)) {
                    continue;
                }
                const auto chunk =
                    stratum::chunk::Chunk::decode(stratum::nbt::read(file.readChunk(cx, cz)).root);
                std::array<const std::string*, 16> local{};
                for (int cellZ = 0; cellZ < 4; ++cellZ) {
                    for (int cellX = 0; cellX < 4; ++cellX) {
                        const auto offset = (static_cast<std::size_t>(cellZ) * 4U) +
                                            static_cast<std::size_t>(cellX);
                        const std::string* column = nullptr;
                        for (const auto& section : chunk.sections()) {
                            if (section.biomes.empty()) {
                                continue;
                            }
                            for (int cellY = 0; cellY < 4; ++cellY) {
                                const std::size_t index =
                                    (static_cast<std::size_t>(cellY) * 16U) + offset;
                                if (index >= section.biomes.size()) {
                                    continue;
                                }
                                const std::string& here =
                                    section.biomePalette[section.biomes[index]];
                                if (column == nullptr) {
                                    column = &here;
                                } else if (here != *column) {
                                    ++varyingWithY;
                                }
                                if ((section.y * 16) + (cellY * 4) == kNetherSampleY) {
                                    local[offset] = &here;
                                }
                            }
                        }
                    }
                }
                constexpr std::array<std::array<int, 2>, 4> kSteps{
                    {{-1, 0}, {1, 0}, {0, -1}, {0, 1}}};
                for (int cellZ = 0; cellZ < 4; ++cellZ) {
                    for (int cellX = 0; cellX < 4; ++cellX) {
                        const auto offset = (static_cast<std::size_t>(cellZ) * 4U) +
                                            static_cast<std::size_t>(cellX);
                        if (local[offset] == nullptr) {
                            continue;
                        }
                        ++cells;
                        ++counts[*local[offset]];
                        for (const auto& step : kSteps) {
                            const int nx = cellX + step[0];
                            const int nz = cellZ + step[1];
                            if (nx < 0 || nx > 3 || nz < 0 || nz > 3) {
                                continue;
                            }
                            const auto at =
                                (static_cast<std::size_t>(nz) * 4U) + static_cast<std::size_t>(nx);
                            if (local[at] != nullptr && *local[at] != *local[offset]) {
                                ++boundary;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    if (worlds == 0) {
        SKIP("no golden nether regions under " << STRATUM_FIXTURES_DIR
                                               << " — run tools/fetch-vanilla "
                                                  "--generate-regions");
    }
    CHECK(worlds == kDistinctSeeds.size());

    // The sampling premise, on vanilla's own data: `y_scale: 0.0` means the
    // biome cannot change down a column, and in the goldens it never does. That
    // is what licenses one y per column instead of thirty-two.
    CHECK(varyingWithY == 0U);

    // 6 worlds x 256 chunks-at-stride-2 x 16 cells.
    CHECK(cells == 24576U);
    CHECK(counts.size() == 5U);

    const double boundaryShare = static_cast<double>(boundary) / static_cast<double>(cells);
    WARN("nether readback denominator: " << cells << " cells over " << worlds << " worlds, "
                                         << boundary << " boundary-adjacent ("
                                         << 100.0 * boundaryShare << "%)");
    CHECK(boundary == 979U);

    // THE FLOOR. Two labelings drawn independently with vanilla's own marginals
    // already agree this often, so no score below it is evidence of anything,
    // and a score a little above it is evidence of very little: the readback's
    // resolving power is between here and 100%, and it is not a gradient.
    double floorAgreement = 0.0;
    for (const auto& [name, count] : counts) {
        const double share = static_cast<double>(count) / static_cast<double>(cells);
        floorAgreement += share * share;
        WARN("    " << name << ": " << count << " (" << 100.0 * share << "%)");
    }
    WARN("chance floor for these marginals: " << 100.0 * floorAgreement << "%");
    CHECK(floorAgreement > 0.26);
    CHECK(floorAgreement < 0.27);
}
