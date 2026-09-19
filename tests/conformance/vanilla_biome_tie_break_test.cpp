// Stratum — what decides a biome when the arithmetic does not.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// WHAT THIS FILE IS ABOUT. `ParameterList::find` compares quantised integers
// and takes the LATER row when two are equal. lib/include/stratum/biome/
// parameter_list.hpp has always said that rule is a MEASURED match to
// vanilla's search tree and not a derivation of it, and that a counterexample
// would be a finding about the tree rather than a bug in the arithmetic.
//
// The counterexample arrived. tools/analysis/legacy-goldens-biome-analyze.cpp
// `--control` spreads the identical decode over the whole 512x512 of all eight
// golden overworld regions and five columns disagree with vanilla's stored
// biome, each at every sampled height:
//
//   seed -1                   (196, 268)  deep_ocean      -> ocean
//   seed -4172144997902289642 (460, 200)  mushroom_fields -> deep_lukewarm_ocean
//   seed 0                    (328,   4)  river           -> beach
//   seed 9223372036854775807  ( 68, 128)  river           -> forest
//   seed 9223372036854775807  ( 72, 128)  river           -> forest
//
// This file is the attribution of those five, pinned so it cannot drift, and
// it says two things.
//
// ONE: THEY ARE TIES, NOT WRONG NUMBERS. `--attribute` dumps, per cell, the
// six climate values, the quantised sample and the fitness of every row of the
// 7593-row table. In all twenty cells (five columns x four heights) the best
// row carrying VANILLA's biome has EXACTLY the fitness of the row we picked —
// margin zero, and no row anywhere in the table beats either. The tie set is a
// pair, and its two members are equidistant on every one of the six axes, not
// merely equal in the sum. Our climate values are therefore not the thing that
// is wrong: no arithmetic can separate two rows that the arithmetic says are
// the same distance away, and a tree searching the same table from the same
// sample could have returned either. Case (a), five times out of five.
//
// Every one of the twenty has the same cause, and it is worth naming because
// it is what makes ties common rather than exotic. On ONE axis the sample's
// quantised value lands EXACTLY on a bound that two adjacent rows share —
// `deep_ocean` is continentalness [-10500,-4550] and `ocean` is [-4550,-1900],
// and the sample quantises to -4550 — so both contain it, both score zero
// there, and the remaining five axes are identical. One quantum either way and
// the tie is gone.
//
// That raises the obvious alternative: perhaps `quantizeCoord` rounds the
// wrong way and vanilla never has the tie at all. `--quantize` settles it by
// measurement rather than by argument. It scores the whole biome decision
// under all thirty-six combinations of six roundings for the sample against
// six for the table's bounds, over both samples below at once. The bounds
// rounding changes nothing at all — all six agree on every bound in the table.
// The sample rounding trades one residual for a bigger one:
//
//   sample rounding      corner (98304)   wide (32768)
//   trunc (the rule)      98304 exact      32748
//   floor / round         98229            32752
//
// Flooring gains four cells on the wide sample — a NET four: it strictly
// separates the ties at (196,268) and (460,200), where the unrounded value
// does lie below the shared bound, and breaks four cells elsewhere that the
// truncation had right. Against that it loses seventy-five in the corner, the
// beach/dark_forest precedent this project already paid for. Nothing in the
// thirty-six is exact on both. The ties are real.
//
// Two of the four axis values say more than the totals do. At seed 0 (328,4)
// the weirdness quantises to 500 under truncation AND under flooring — the
// unrounded value is 0.05001, ABOVE the 0.05 that `river` [-500,500] and
// `beach` [500,2666] share — so no rounding removes that tie, and vanilla
// picks the band the unrounded value is not in. At seed MAX (68,128) and
// (72,128) flooring would move erosion from -3750 to -3751, strictly inside
// `forest` [-10000,-3750] — and vanilla stored `river` [-3750,5500]. Under
// truncation those two columns are ties this engine loses; under flooring
// they would be outright arithmetic disagreements, which is worse than a
// tie-break being a proxy.
//
// TWO: NO ORDER OVER THE LIST CAN BE THE TIE-BREAK, and that is the finding.
// A tie only matters where its members carry different BIOMES, so that is the
// denominator — not "cells", which would bury it. Over the two samples:
//
//   sample                     cells    tied   decisive   later   earlier
//   corner ([biome]'s own)     98304      76         76      76         0
//   wide (--control's)         32768      36         28       8        20
//   total                     131072     112        104      84        20
//
// Both orders are refuted, by the other sample's cells. "Later row wins" is
// right on 84 of 104 and wrong on 20; "earlier row wins" is right on 20 and
// wrong on 84. So the rule is not a fact about the list's order that this
// project has got backwards — it is a PROXY for the order of the leaves in
// vanilla's tree, right wherever the two orders happen to agree. Every one of
// the 104 ties is a pair, and the 15 distinct pairs they form are consistent
// with a single total order on rows (no cycle: row 12 loses to row 10 below it
// and to row 1710 above it), which is what a tree's leaf order would look like
// and is not what any function of the list index looks like.
//
// WHY THE RULE IS NOT CHANGED HERE. "Earlier row wins" is strictly worse on
// this evidence — 84 wrong instead of 20 — and there is no third positional
// rule: the tie sets are pairs, so first and last are the only choices there
// are. Recovering the leaf order itself would mean reconstructing how vanilla
// builds the tree, which is not in the dumped table and is not derivable from
// it; deriving it from the source is forbidden (CLAUDE.md). So the honest
// state is: the arithmetic is exact, the tie-break is right 84 times in 104
// and is named as a proxy, and 20 cells in 131072 are attributed and open.
//
// The fixtures are Mojang-derived and never committed (SPEC §12); without
// them every case here SKIPs.

#include <stratum/biome/parameter_list.hpp>
#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/javamath.hpp>
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
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

using stratum::biome::ClimateSample;
using stratum::biome::ParameterList;
using stratum::biome::QuantizedPoint;
using stratum::biome::QuantizedSample;
using stratum::data::Pack;
using stratum::data::ResourceLocation;
using stratum::settings::RouterEntry;

/// The four heights `--control` samples, and therefore the four this file
/// attributes. Not every section: the overworld's biome does vary down a
/// column, so the sample has to span y, but twenty-four sections a chunk buys
/// correlated cells at twenty-four times the cost.
constexpr std::array<std::int32_t, 4> kHeights{-48, 16, 64, 112};

/// The eight golden overworld regions. Six distinct worlds — the low 48 bits
/// of the seed are all a Java LCG keeps — but the biome source is seeded
/// through Xoroshiro128++ from the full 64, so all eight are distinct here
/// and all eight are scored.
constexpr std::array<std::int64_t, 8> kAllSeeds{
    0,
    1,
    -1,
    42,
    2891948927356891,
    -4172144997902289642,
    std::numeric_limits<std::int64_t>::max(),
    std::numeric_limits<std::int64_t>::min(),
};

/// The four seeds the [biome] conformance cases use, over their 4x4-chunk
/// corner. Kept in step with tests/conformance/vanilla_biomes_test.cpp: this
/// file's claim about the seventy-five-cell precedent is a claim about THAT
/// sample, so it has to be that sample.
constexpr std::array<std::int64_t, 4> kCornerSeeds{42, 0, -1, -4172144997902289642};

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

[[nodiscard]] std::filesystem::path findParameterList() {
    if (!std::filesystem::is_directory(fixtures())) {
        return {};
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(fixtures())) {
        if (entry.is_regular_file() && entry.path().filename() == "overworld.json" &&
            entry.path().parent_path().parent_path().filename() == "biome_parameters") {
            return entry.path();
        }
    }
    return {};
}

[[nodiscard]] std::filesystem::path regionOf(std::int64_t seed) {
    return fixtures() / "1.21.11" / "regions" / ("seed-" + std::to_string(seed)) / "overworld" /
           "r.0.0.mca";
}

/// The table in the space the search runs in, plus each row's biome. Built
/// once per case: the scans below ask the whole table for its minimum a
/// hundred thousand times, and quantising seven thousand rows inside that loop
/// would dominate everything.
struct Table {
    std::vector<QuantizedPoint> points;
    std::vector<std::string> labels;

    explicit Table(const ParameterList& list) {
        points.reserve(list.size());
        labels.reserve(list.size());
        for (const auto& entry : list.entries()) {
            points.push_back(QuantizedPoint::of(entry.parameters));
            labels.push_back(entry.biome.toString());
        }
    }

    /// Every row at the minimum fitness, in list order, and that minimum. One
    /// pass, so this costs what `find` costs.
    [[nodiscard]] std::pair<std::vector<std::size_t>, std::int64_t>
    tieAt(const QuantizedSample& sample) const {
        std::vector<std::size_t> tie;
        std::int64_t best = 0;
        for (std::size_t row = 0; row < points.size(); ++row) {
            const std::int64_t fitness = points[row].fitness(sample);
            if (tie.empty() || fitness < best) {
                best = fitness;
                tie.clear();
                tie.push_back(row);
            } else if (fitness == best) {
                tie.push_back(row);
            }
        }
        return {tie, best};
    }

    /// The best fitness any row carrying @p biome reaches, or nothing when the
    /// table does not contain that biome at all.
    [[nodiscard]] std::optional<std::int64_t> bestFitnessFor(const QuantizedSample& sample,
                                                             const std::string& biome) const {
        std::optional<std::int64_t> best;
        for (std::size_t row = 0; row < points.size(); ++row) {
            if (labels[row] != biome) {
                continue;
            }
            const std::int64_t fitness = points[row].fitness(sample);
            if (!best.has_value() || fitness < *best) {
                best = fitness;
            }
        }
        return best;
    }
};

/// The dimension's climate router, wired to one seed.
///
/// The settings and the noise registry are held behind pointers and the
/// interpreter is built after them, because `Interpreter` keeps REFERENCES to
/// both: an aggregate that moved them after construction would leave it
/// pointing at the moved-from husks.
class Climate {
public:
    Climate(const Pack& pack, std::int64_t seed)
        : loaded_(std::make_unique<stratum::settings::LoadedSettings>(
              stratum::settings::loadAll(pack))) {
        const auto& overworld =
            loaded_->settings.at(ResourceLocation::parse("minecraft:overworld"));
        noises_ = std::make_unique<stratum::density::NoiseRegistry>(
            stratum::density::NoiseRegistry::create(pack, loaded_->graph.referencedNoises(), seed,
                                                    stratum::density::RandomSource::Xoroshiro));
        interpreter_ = std::make_unique<stratum::density::Interpreter>(
            loaded_->graph, *noises_,
            stratum::density::CellGeometry{.width = overworld.geometry.cellWidth(),
                                           .height = overworld.geometry.cellHeight()});
    }

    [[nodiscard]] ClimateSample sample(std::int32_t x, std::int32_t y, std::int32_t z) const {
        const auto& overworld =
            loaded_->settings.at(ResourceLocation::parse("minecraft:overworld"));
        const stratum::density::Point at{.x = x, .y = y, .z = z};
        return ClimateSample{
            .temperature =
                interpreter_->evaluate(overworld.router.at(RouterEntry::Temperature), at),
            .humidity = interpreter_->evaluate(overworld.router.at(RouterEntry::Vegetation), at),
            .continentalness =
                interpreter_->evaluate(overworld.router.at(RouterEntry::Continents), at),
            .erosion = interpreter_->evaluate(overworld.router.at(RouterEntry::Erosion), at),
            .depth = interpreter_->evaluate(overworld.router.at(RouterEntry::Depth), at),
            .weirdness = interpreter_->evaluate(overworld.router.at(RouterEntry::Ridges), at)};
    }

private:
    std::unique_ptr<stratum::settings::LoadedSettings> loaded_;
    std::unique_ptr<stratum::density::NoiseRegistry> noises_;
    std::unique_ptr<stratum::density::Interpreter> interpreter_;
};

[[nodiscard]] ParameterList loadTable(const std::filesystem::path& path) {
    std::ifstream file(path);
    std::stringstream contents;
    contents << file.rdbuf();
    return ParameterList::fromJson(nlohmann::json::parse(contents.str()),
                                   ResourceLocation::parse("minecraft:overworld"));
}

/// The biome vanilla stored at one 4x4x4 cell, read out of the golden region.
/// Nothing is hardcoded: the whole point is that vanilla's own answer decides,
/// so a cell the region does not carry is a missing answer rather than a
/// default.
[[nodiscard]] std::optional<std::string> storedBiomeAt(const stratum::region::RegionFile& file,
                                                       std::int32_t x, std::int32_t y,
                                                       std::int32_t z) {
    using stratum::javamath::floorDiv;
    using stratum::javamath::floorMod;
    const std::int32_t chunkX = floorDiv(x, 16);
    const std::int32_t chunkZ = floorDiv(z, 16);
    if (!file.hasChunk(chunkX, chunkZ)) {
        return std::nullopt;
    }
    const auto chunk =
        stratum::chunk::Chunk::decode(stratum::nbt::read(file.readChunk(chunkX, chunkZ)).root);
    const std::int32_t sectionY = floorDiv(y, 16);
    for (const auto& section : chunk.sections()) {
        if (section.y != sectionY || section.biomes.empty()) {
            continue;
        }
        const std::size_t index = (static_cast<std::size_t>(floorMod(y, 16) / 4) * 16U) +
                                  (static_cast<std::size_t>(floorMod(z, 16) / 4) * 4U) +
                                  static_cast<std::size_t>(floorMod(x, 16) / 4);
        if (index >= section.biomes.size()) {
            return std::nullopt;
        }
        return section.biomePalette[section.biomes[index]];
    }
    return std::nullopt;
}

/// One of the five columns `--control` reports, and what the two sides answer.
struct Residual {
    std::int64_t seed;
    std::int32_t x;
    std::int32_t z;
    const char* stored;
    const char* ours;
};

constexpr std::array<Residual, 5> kResiduals{
    Residual{.seed = -1,
             .x = 196,
             .z = 268,
             .stored = "minecraft:deep_ocean",
             .ours = "minecraft:ocean"},
    Residual{.seed = -4172144997902289642,
             .x = 460,
             .z = 200,
             .stored = "minecraft:mushroom_fields",
             .ours = "minecraft:deep_lukewarm_ocean"},
    Residual{.seed = 0, .x = 328, .z = 4, .stored = "minecraft:river", .ours = "minecraft:beach"},
    Residual{.seed = std::numeric_limits<std::int64_t>::max(),
             .x = 68,
             .z = 128,
             .stored = "minecraft:river",
             .ours = "minecraft:forest"},
    Residual{.seed = std::numeric_limits<std::int64_t>::max(),
             .x = 72,
             .z = 128,
             .stored = "minecraft:river",
             .ours = "minecraft:forest"},
};

/// How the decisive ties of one sample resolve.
struct TieTally {
    std::size_t cells = 0;
    /// More than one row reaches the minimum fitness.
    std::size_t tied = 0;
    /// ... and not all of them carry the same biome, so which one the tree
    /// reaches is visible in what vanilla stored. This is the denominator any
    /// statement about the tie-break has to be quoted against.
    std::size_t decisive = 0;
    std::size_t laterCorrect = 0;
    std::size_t earlierCorrect = 0;
    /// Vanilla's biome is on no row of the tie set. Must be zero: a strictly
    /// worse row winning is not something any tie-break can produce, and would
    /// move the finding from the tree back to the arithmetic.
    std::size_t absent = 0;
    /// A tie set with more than two members — none is expected, and "first or
    /// last are the only choices" depends on it.
    std::size_t widerThanAPair = 0;
};

/// Walks a sample and tallies its decisive ties. @p chunkStride subsamples
/// chunks; an empty @p heights takes every cell of every section.
[[nodiscard]] TieTally tally(const Pack& pack, const Table& table,
                             std::span<const std::int64_t> seeds, std::int32_t chunkStride,
                             std::int32_t chunkSpan, std::span<const std::int32_t> heights) {
    TieTally result;
    for (const std::int64_t seed : seeds) {
        const std::filesystem::path path = regionOf(seed);
        if (!std::filesystem::is_regular_file(path)) {
            continue;
        }
        const Climate climate(pack, seed);
        const auto file = stratum::region::RegionFile::open(path);
        for (std::int32_t chunkZ = 0; chunkZ < chunkSpan; chunkZ += chunkStride) {
            for (std::int32_t chunkX = 0; chunkX < chunkSpan; chunkX += chunkStride) {
                if (!file.hasChunk(chunkX, chunkZ)) {
                    continue;
                }
                const auto chunk = stratum::chunk::Chunk::decode(
                    stratum::nbt::read(file.readChunk(chunkX, chunkZ)).root);
                for (const auto& section : chunk.sections()) {
                    if (section.biomes.empty()) {
                        continue;
                    }
                    for (int cellY = 0; cellY < 4; ++cellY) {
                        const std::int32_t y = (section.y * 16) + (cellY * 4);
                        if (!heights.empty() &&
                            std::find(heights.begin(), heights.end(), y) == heights.end()) {
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
                                ++result.cells;
                                // The minimum itself is not wanted here, only
                                // who reaches it.
                                const std::vector<std::size_t> tie =
                                    table
                                        .tieAt(QuantizedSample::of(
                                            climate.sample((chunkX * 16) + (cellX * 4), y,
                                                           (chunkZ * 16) + (cellZ * 4))))
                                        .first;
                                if (tie.size() < 2) {
                                    continue;
                                }
                                ++result.tied;
                                const bool oneBiome =
                                    std::all_of(tie.begin(), tie.end(), [&](std::size_t row) {
                                        return table.labels[row] == table.labels[tie.front()];
                                    });
                                if (oneBiome) {
                                    continue;
                                }
                                ++result.decisive;
                                if (tie.size() > 2) {
                                    ++result.widerThanAPair;
                                }
                                if (table.labels[tie.back()] == stored) {
                                    ++result.laterCorrect;
                                }
                                if (table.labels[tie.front()] == stored) {
                                    ++result.earlierCorrect;
                                }
                                if (std::none_of(tie.begin(), tie.end(), [&](std::size_t row) {
                                        return table.labels[row] == stored;
                                    })) {
                                    ++result.absent;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return result;
}

} // namespace

TEST_CASE("the five residual columns are ties, not climate values that differ",
          "[conformance][biome][tiebreak]") {
    const std::filesystem::path tree = findWorldgenTree();
    const std::filesystem::path parameters = findParameterList();
    if (tree.empty() || parameters.empty()) {
        SKIP("no extracted vanilla worldgen or biome parameters under "
             << STRATUM_FIXTURES_DIR
             << " — Mojang-derived and never committed (SPEC §12). Generate them with: "
                "tools/fetch-vanilla");
    }
    const ParameterList list = loadTable(parameters);
    REQUIRE(list.size() == 7593U);
    const Table table{list};
    const Pack pack = Pack::open(tree);

    std::size_t checked = 0;
    for (const Residual& residual : kResiduals) {
        const std::filesystem::path path = regionOf(residual.seed);
        if (!std::filesystem::is_regular_file(path)) {
            continue;
        }
        CAPTURE(residual.seed, residual.x, residual.z);
        const Climate climate(pack, residual.seed);
        const auto file = stratum::region::RegionFile::open(path);
        for (const std::int32_t y : kHeights) {
            CAPTURE(y);
            const auto stored = storedBiomeAt(file, residual.x, y, residual.z);
            REQUIRE(stored.has_value());
            // The column is a residual at all only because these two differ.
            // Asserted rather than assumed: if the biome source ever agrees
            // here, this whole case is describing something that no longer
            // happens and must be re-derived, not quietly passed.
            CHECK(*stored == residual.stored);

            const ClimateSample sample = climate.sample(residual.x, y, residual.z);
            const QuantizedSample quantized = QuantizedSample::of(sample);
            CHECK(list.find(sample).toString() == residual.ours);

            // THE CLAIM. The best row carrying vanilla's biome reaches exactly
            // the minimum fitness. Not "close to" — equal, in the integer
            // space the comparison happens in. Our climate values are
            // therefore consistent with vanilla's answer, and only the
            // tie-break separates the two.
            const auto [tie, best] = table.tieAt(quantized);
            const auto theirs = table.bestFitnessFor(quantized, *stored);
            REQUIRE(theirs.has_value());
            CHECK(*theirs == best);

            // And the tie is a PAIR whose members are our answer and
            // vanilla's, with vanilla's the EARLIER row — which is precisely
            // the case "later row wins" cannot get right.
            REQUIRE(tie.size() == 2U);
            CHECK(table.labels[tie.front()] == residual.stored);
            CHECK(table.labels[tie.back()] == residual.ours);
            ++checked;
        }
    }
    // Five columns at four heights. Quoted so a missing golden region cannot
    // turn this case into a pass over nothing.
    CHECK(checked == 20U);
}

TEST_CASE("no rule based on list position is vanilla's tie-break",
          "[conformance][biome][tiebreak]") {
    const std::filesystem::path tree = findWorldgenTree();
    const std::filesystem::path parameters = findParameterList();
    if (tree.empty() || parameters.empty()) {
        SKIP("no extracted vanilla worldgen or biome parameters under " << STRATUM_FIXTURES_DIR);
    }
    if (!std::filesystem::is_regular_file(regionOf(42))) {
        SKIP("no golden regions; generate them with: tools/fetch-vanilla "
             "--generate-regions --accept-eula");
    }
    const ParameterList list = loadTable(parameters);
    const Table table{list};
    const Pack pack = Pack::open(tree);

    // The [biome] cases' own sample: four seeds, the 4x4-chunk corner, every
    // cell of every section.
    const TieTally corner = tally(pack, table, kCornerSeeds, 1, 4, {});
    // `--control`'s sample: eight regions, every fourth chunk of the whole
    // 512x512, four heights.
    const TieTally wide =
        tally(pack, table, kAllSeeds, 4, stratum::region::kChunksPerAxis, kHeights);

    CHECK(corner.cells == 98304U);
    CHECK(wide.cells == 32768U);

    // Every tie is a pair, in both samples. It is what makes "first or last"
    // an exhaustive list of the positional rules rather than two of many.
    CHECK(corner.widerThanAPair == 0U);
    CHECK(wide.widerThanAPair == 0U);

    // THE ARITHMETIC IS EXACT. In every cell where the minimum is shared,
    // vanilla's stored biome is on one of the rows sharing it. Not once does a
    // strictly worse row win — which is what a difference in the climate
    // values would look like, and what would make this a bug rather than a
    // finding about the tree.
    CHECK(corner.absent == 0U);
    CHECK(wide.absent == 0U);

    // THE TIE-BREAK IS A PROXY. On the corner, "later row wins" is right on
    // all 76 decisive ties — the 75-cell beach/dark_forest precedent plus one
    // lukewarm_ocean/lush_caves cell. On the wide sample it is right on 8 of
    // 28 and WRONG on 20, where "earlier row wins" is right instead. Each rule
    // is refuted by the other sample's cells, so neither is vanilla's: both
    // are approximations of the order of the leaves in its tree.
    CHECK(corner.tied == 76U);
    CHECK(corner.decisive == 76U);
    CHECK(corner.laterCorrect == 76U);
    CHECK(corner.earlierCorrect == 0U);

    CHECK(wide.tied == 36U);
    CHECK(wide.decisive == 28U);
    CHECK(wide.laterCorrect == 8U);
    CHECK(wide.earlierCorrect == 20U);

    // The two together, which is the number to quote: 104 decisive ties over
    // 131072 cells; the rule this engine ships is right on 84 of them.
    CHECK(corner.decisive + wide.decisive == 104U);
    CHECK(corner.laterCorrect + wide.laterCorrect == 84U);
    CHECK(corner.earlierCorrect + wide.earlierCorrect == 20U);
    // Neither positional rule is even close to whole, stated as the
    // inequality it is so that flipping `find` to take the earlier row cannot
    // be presented as an improvement.
    CHECK(corner.laterCorrect + wide.laterCorrect > corner.earlierCorrect + wide.earlierCorrect);
    CHECK(corner.laterCorrect + wide.laterCorrect < corner.decisive + wide.decisive);
}
