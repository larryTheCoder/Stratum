// Stratum — how wrong the Nether would be if its climate used the modern rule.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// SPEC §11 used to say only that substituting the modern derivation for a
// legacy dimension's named noises "would produce a Nether that is not
// vanilla's". That is an assertion, not a measurement, and the difference it
// glosses over is the one that matters to anyone deciding what to do next:
// between "cannot generate" and "generates, with the wrong things in the
// wrong places". This file measures it for the Nether's biome layout.
//
// NOTHING HERE IS SHIPPED. The modern derivation is not offered as a
// fallback, and no shipped path can reach it for a legacy dimension —
// `NoiseRegistry::create` still refuses a non-empty `wanted` under Legacy.
// This test reaches around that refusal ON PURPOSE, by asking for a
// Xoroshiro registry and evaluating the nether's climate with it, precisely
// so that the size of the resulting error is a number in the repository
// rather than a claim about one.
//
// WHY THE NETHER'S BIOMES ARE PURE CLIMATE. Its router sets continents,
// erosion, depth and ridges to the constant 0.0 — measured in
// vanilla_legacy_named_noises_test.cpp, where those four entries reach
// no noise because they ARE numbers. So of the six climate parameters only
// temperature and vegetation vary, and the Nether's biome at a cell is a
// function of those two alone. Get their seeding wrong and the whole biome
// map is redrawn; nothing else in the dimension can compensate.
//
// WHAT COMES OUT. Every second chunk of each 32x32 golden region — 4096
// biome columns a seed spanning the full 512x512 blocks, 32768 over the eight
// — which are SIX INDEPENDENT WORLDS, since java.util.Random discards bit 63
// and so 0 collides with Long.MIN_VALUE and -1 with Long.MAX_VALUE; their
// rows below are identical, which is the collision showing itself —
// — with the biome read at one y per column, which the run itself justifies:
// ZERO columns of vanilla's own stored biomes vary down their height, as
// `y_scale: 0.0` requires.
//
//   agreement                      8873 / 32768 = 27.08%
//   chance baseline                               30.23%
//
// The baseline is not a guess either: it is sum over biomes of
// p_vanilla(b) * p_modern(b), the agreement two independent labelings with
// these exact marginals would reach. The modern derivation does not beat it.
// It comes in BELOW it, and per seed it sits on it — on three seeds to the
// last cell, which is what happens when the wrong seeding makes one biome
// swallow the sample:
//
//   seed                    agreement          baseline
//   0                       510 / 4096 12.45%    15.36%
//   1                       717 / 4096 17.50%    18.35%
//   -1                     1019 / 4096 24.88%    23.50%
//   42                      765 / 4096 18.68%    20.01%
//   2891948927356891       2448 / 4096 59.77%    59.77%
//   -4172144997902289642    855 / 4096 20.87%    20.87%
//   9223372036854775807     842 / 4096 20.56%    25.72%
//   -9223372036854775808   1717 / 4096 41.92%    41.92%
//
// AND THE MARGINALS ARE WRONG TOO, which is the part that would actually be
// visible standing in the world. Over the 32768 columns:
//
//   biome                vanilla   modern rule
//   basalt_deltas           3938             0
//   crimson_forest         11109         12090
//   nether_wastes           9566         19397
//   soul_sand_valley        4804           283
//   warped_forest           3351           998
//
// Total variation distance 0.33. Basalt deltas — 12% of vanilla's sample —
// do not occur AT ALL under the modern derivation, and soul sand valleys
// occur at a seventeenth of their rate, while nether wastes double.
//
// WHAT MAKES 27.08% ATTRIBUTABLE TO THE SEEDING RATHER THAN TO THIS BUILD.
// A biome number this bad has two possible causes, and only one of them is
// the one being claimed: the wrong seeding, or a broken
// climate-to-ParameterList path that would score badly wherever it ran. The
// control that separates them is already in this suite and passing. The
// `[biome]` conformance cases reproduce vanilla's OVERWORLD biomes exactly,
// through the SAME `Interpreter` router entries, the SAME
// `biome::ClimateSample`, and the SAME `biome::ParameterList::find` this
// case uses — the only difference between the two runs is which noises were
// built and how they were seeded. So the machinery is exonerated by a
// measurement rather than by assertion, and the 3.15-point shortfall below
// chance is about the seeding. (What this does NOT control for is the
// Nether's own parameter list being read differently from the overworld's;
// nothing here tests that, and the marginals above — five biomes, all of
// them the Nether's own, in the wrong proportions — are the evidence
// against it rather than a test of it.)
//
// SO THE HONEST STATEMENT OF THE GAP, and it is now a measurement rather
// than the assertion SPEC §11 used to carry: the Nether's terrain under the
// legacy source is vanilla's to within one block on 4.09e-5 of positions
// (vanilla_legacy_nether_terrain_test.cpp), and its biome layout under the
// modern rule would be an unrelated draw — at chance, over the right five
// biomes in the wrong proportions, with one of them missing entirely. Right
// blocks, right terrain, wrong world. Not "slightly off".
//
// The fixtures are Mojang-derived and never committed (SPEC §12). Without
// them this skips.
#include <stratum/biome/parameter_list.hpp>
#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/settings/noise_settings.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
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

constexpr std::array<std::int64_t, 8> kSeeds{
    0,
    1,
    -1,
    42,
    2891948927356891LL,
    -4172144997902289642LL,
    9223372036854775807LL,
    -9223372036854775807LL - 1,
};

/// Every second chunk of the whole 32x32 region, so the sample spans the
/// full 512x512 blocks. That span is the point and an earlier version of this
/// file got it wrong: over 64x64 blocks the Nether is essentially ONE biome,
/// so "agreement" there measured only whether the modern rule also picked
/// crimson_forest, and read as high as 69% on one seed for that reason alone.
/// A biome statistic needs an area with several biomes in it.
constexpr std::int32_t kChunkStride = 2;

/// The nether's `temperature` and `vegetation` are `shifted_noise` with
/// `y_scale: 0.0` and `shift_y: 0.0`, so its climate — and therefore its
/// biome — is constant down a column. Asserted against vanilla's own stored
/// biomes below rather than assumed, and then used: one y per column instead
/// of thirty-two.
constexpr std::int32_t kSampleY = 64;

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

[[nodiscard]] std::filesystem::path findNetherParameterList() {
    if (!std::filesystem::is_directory(fixtures())) {
        return {};
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(fixtures())) {
        if (entry.is_regular_file() && entry.path().filename() == "nether.json" &&
            entry.path().parent_path().parent_path().filename() == "biome_parameters") {
            return entry.path();
        }
    }
    return {};
}

[[nodiscard]] std::filesystem::path netherRegionOf(std::int64_t seed) {
    if (!std::filesystem::is_directory(fixtures())) {
        return {};
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(fixtures())) {
        if (!entry.is_regular_file() || entry.path().filename() != "r.0.0.mca") {
            continue;
        }
        const std::filesystem::path parent = entry.path().parent_path();
        if (parent.filename() == "nether" &&
            parent.parent_path().filename() == "seed-" + std::to_string(seed)) {
            return entry.path();
        }
    }
    return {};
}

struct Score {
    std::size_t cells = 0;
    std::size_t agree = 0;
    std::map<std::string, std::size_t> vanillaCounts;
    std::map<std::string, std::size_t> ourCounts;
    /// Columns where vanilla's own stored biome is NOT the same all the way
    /// down. Expected to be zero, because the nether's climate has
    /// `y_scale: 0.0`; counted rather than assumed, because sampling one y
    /// per column is only sound if it is.
    std::size_t columnsVaryingWithY = 0;

    /// The agreement two independent labelings with these marginals would
    /// reach. This is the baseline the measurement is read against — quoting
    /// a bare percentage without it says nothing, because five biomes of
    /// unequal frequency already agree by chance a good part of the time.
    [[nodiscard]] double chanceBaseline() const {
        if (cells == 0) {
            return 0.0;
        }
        double sum = 0.0;
        for (const auto& [biome, count] : vanillaCounts) {
            const auto found = ourCounts.find(biome);
            if (found == ourCounts.end()) {
                continue;
            }
            sum += (static_cast<double>(count) / static_cast<double>(cells)) *
                   (static_cast<double>(found->second) / static_cast<double>(cells));
        }
        return sum;
    }

    [[nodiscard]] double rate() const {
        return cells == 0 ? 0.0 : static_cast<double>(agree) / static_cast<double>(cells);
    }
};

} // namespace

TEST_CASE("the modern derivation would redraw the Nether's biome map entirely",
          "[conformance][legacy][nether][climate]") {
    const std::filesystem::path tree = findWorldgenTree();
    const std::filesystem::path parameters = findNetherParameterList();
    if (tree.empty() || parameters.empty()) {
        SKIP("no extracted vanilla worldgen or nether biome parameter list under "
             << STRATUM_FIXTURES_DIR
             << " — Mojang-derived and never committed (SPEC §12). Generate them with: "
                "tools/fetch-vanilla");
    }

    const Pack pack = Pack::open(tree);
    const auto loaded = stratum::settings::loadAll(pack);
    const auto& nether = loaded.settings.at(ResourceLocation::parse("minecraft:nether"));
    REQUIRE(nether.legacyRandomSource);

    std::ifstream stream(parameters);
    const ParameterList table = ParameterList::fromJson(
        nlohmann::json::parse(stream), ResourceLocation::parse("minecraft:nether"));
    REQUIRE(table.size() > 0U);

    // Exactly the noises the nether's climate reaches, which is the whole
    // point of Graph::noisesReachableFrom: temperature, vegetation and the
    // `offset` that arrives through the shared shift_x/shift_z.
    std::vector<ResourceLocation> wanted =
        loaded.graph.noisesReachableFrom(nether.router.at(RouterEntry::Temperature));
    for (const auto& id :
         loaded.graph.noisesReachableFrom(nether.router.at(RouterEntry::Vegetation))) {
        wanted.push_back(id);
    }
    REQUIRE(wanted.size() == 4U); // offset twice, temperature, vegetation

    Score overall;
    std::size_t seedsMeasured = 0;

    for (const std::int64_t seed : kSeeds) {
        const std::filesystem::path region = netherRegionOf(seed);
        if (region.empty()) {
            continue;
        }
        CAPTURE(seed);

        // DELIBERATELY the modern derivation on a legacy dimension. This is
        // the thing the shipped build refuses to do; measuring its error is
        // why it refuses.
        const auto noises = stratum::density::NoiseRegistry::create(
            pack, wanted, seed, stratum::density::RandomSource::Xoroshiro);
        const stratum::density::Interpreter interpreter(
            loaded.graph, noises,
            stratum::density::CellGeometry{.width = nether.geometry.cellWidth(),
                                           .height = nether.geometry.cellHeight()});

        const auto file = stratum::region::RegionFile::open(region);
        Score perSeed;

        for (std::int32_t cz = 0; cz < stratum::region::kChunksPerAxis; cz += kChunkStride) {
            for (std::int32_t cx = 0; cx < stratum::region::kChunksPerAxis; cx += kChunkStride) {
                if (!file.hasChunk(cx, cz)) {
                    continue;
                }
                const auto chunk =
                    stratum::chunk::Chunk::decode(stratum::nbt::read(file.readChunk(cx, cz)).root);

                // One biome per 4x4x4 cell; this walks the 4x4 COLUMNS of the
                // chunk and reads vanilla's biome at every section of each, so
                // the column-invariance claim is checked on real data before
                // it is used to justify sampling one y.
                for (int cellZ = 0; cellZ < 4; ++cellZ) {
                    for (int cellX = 0; cellX < 4; ++cellX) {
                        const std::size_t offset = (static_cast<std::size_t>(cellZ) * 4U) +
                                                   static_cast<std::size_t>(cellX);
                        const std::string* columnBiome = nullptr;
                        const std::string* atSampleY = nullptr;
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
                                if (columnBiome == nullptr) {
                                    columnBiome = &here;
                                } else if (here != *columnBiome) {
                                    ++perSeed.columnsVaryingWithY;
                                }
                                if ((section.y * 16) + (cellY * 4) == kSampleY) {
                                    atSampleY = &here;
                                }
                            }
                        }
                        if (atSampleY == nullptr) {
                            continue;
                        }

                        const stratum::density::Point at{.x = (chunk.x() * 16) + (cellX * 4),
                                                         .y = kSampleY,
                                                         .z = (chunk.z() * 16) + (cellZ * 4)};
                        const ClimateSample sample{
                            .temperature = interpreter.evaluate(
                                nether.router.at(RouterEntry::Temperature), at),
                            .humidity =
                                interpreter.evaluate(nether.router.at(RouterEntry::Vegetation), at),
                            .continentalness =
                                interpreter.evaluate(nether.router.at(RouterEntry::Continents), at),
                            .erosion =
                                interpreter.evaluate(nether.router.at(RouterEntry::Erosion), at),
                            .depth = interpreter.evaluate(nether.router.at(RouterEntry::Depth), at),
                            .weirdness =
                                interpreter.evaluate(nether.router.at(RouterEntry::Ridges), at)};
                        const std::string chosen = table.find(sample).toString();

                        ++perSeed.cells;
                        ++perSeed.vanillaCounts[*atSampleY];
                        ++perSeed.ourCounts[chosen];
                        if (chosen == *atSampleY) {
                            ++perSeed.agree;
                        }
                    }
                }
            }
        }

        REQUIRE(perSeed.cells > 0U);
        ++seedsMeasured;
        WARN("seed " << seed << ": modern derivation picks vanilla's nether biome at "
                     << perSeed.agree << " / " << perSeed.cells << " = " << 100.0 * perSeed.rate()
                     << "% of cells; chance baseline for these marginals "
                     << 100.0 * perSeed.chanceBaseline() << "%; " << perSeed.columnsVaryingWithY
                     << " column(s) varying with y");

        overall.columnsVaryingWithY += perSeed.columnsVaryingWithY;
        overall.cells += perSeed.cells;
        overall.agree += perSeed.agree;
        for (const auto& [biome, count] : perSeed.vanillaCounts) {
            overall.vanillaCounts[biome] += count;
        }
        for (const auto& [biome, count] : perSeed.ourCounts) {
            overall.ourCounts[biome] += count;
        }
    }

    if (seedsMeasured == 0) {
        SKIP("no golden nether regions under " << STRATUM_FIXTURES_DIR
                                               << " — run tools/fetch-vanilla --generate-regions");
    }

    WARN("nether climate under the modern derivation, "
         << seedsMeasured << " seed(s): " << overall.agree << " / " << overall.cells << " = "
         << 100.0 * overall.rate() << "%, chance baseline " << 100.0 * overall.chanceBaseline()
         << "%");
    for (const auto& [biome, count] : overall.vanillaCounts) {
        const auto found = overall.ourCounts.find(biome);
        WARN("    " << biome << ": vanilla " << count << ", modern rule "
                    << (found == overall.ourCounts.end() ? 0U : found->second));
    }

    CHECK(seedsMeasured == kSeeds.size());

    // The sampling premise, checked on vanilla's own data: the nether's
    // climate carries `y_scale: 0.0`, so its biome must not change down a
    // column — and in the golden regions it never does.
    CHECK(overall.columnsVaryingWithY == 0U);

    // THE CLAIM. Not "agreement is low" — low compared to what? The modern
    // derivation does not BEAT the agreement two unrelated labelings of these
    // same marginals would reach. Stated so it could be refuted: a derivation
    // carrying any real signal about vanilla's layout would clear the
    // baseline, and this one does not clear it at all.
    CHECK(overall.rate() <= overall.chanceBaseline() + 0.02);

    // The marginals are wrong as well as the placement, which is the part a
    // player would see. Total variation distance between the two biome
    // distributions, measured at 0.33 over 32768 columns.
    double variation = 0.0;
    for (const auto& [biome, count] : overall.vanillaCounts) {
        const auto found = overall.ourCounts.find(biome);
        const auto ours = found == overall.ourCounts.end() ? std::size_t{0} : found->second;
        variation += std::abs(static_cast<double>(count) - static_cast<double>(ours));
    }
    for (const auto& [biome, count] : overall.ourCounts) {
        if (!overall.vanillaCounts.contains(biome)) {
            variation += static_cast<double>(count);
        }
    }
    variation = variation / (2.0 * static_cast<double>(overall.cells));
    WARN("total variation distance between the two biome distributions: " << variation);
    CHECK(variation > 0.25);

    // And the concrete, quotable consequence: a biome vanilla places over
    // 12% of this sample is never placed at all under the modern rule. This
    // is what "would produce a Nether that is not vanilla's" actually means.
    CHECK(overall.vanillaCounts.contains("minecraft:basalt_deltas"));
    CHECK(overall.vanillaCounts.at("minecraft:basalt_deltas") > 0U);
    CHECK_FALSE(overall.ourCounts.contains("minecraft:basalt_deltas"));
}
