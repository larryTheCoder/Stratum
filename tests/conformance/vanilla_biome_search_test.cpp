// Stratum — the biome search's tree over the server's own tables.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// `ParameterList::find` searches a tree of boxes built over the quantised
// rows, and is held to return exactly the row the linear scan
// (`findRowLinear`) returns — the least fitness, the LATER row of a tie, the
// measured proxy for vanilla's tie-break that
// tests/conformance/vanilla_biome_tie_break_test.cpp attributes. The
// fixture-free tables in tests/unit/biome_search_test.cpp are made to be
// hard; these are the ones that matter: every multi-noise table the server
// dumps (the overworld's 7593 rows, which three presets share, and the
// Nether's), asked over a million samples between the four cases below.
//
// Every comparison is by ROW, not by biome. The overworld's table holds 3795
// pairs of rows that differ only in depth, one at 0 and one at 1.0 and the
// same biome, so a search that took the wrong twin would pass a biome check.
//
// The samples mix what the climate router produces with what is hardest for
// a search: uniform over the table's ranges; uniform with depth over the
// router's [-1.5, 2]; Gaussian climate with depth as a gradient down the
// column plus an offset, through the double-valued ClimateSample path and
// its quantisation; every axis on one of the table's bounds or one quantum
// off it, all six at once or each with probability 0.6; constructed ties —
// inside a row on five axes and on one of its faces on the sixth, or halfway
// in depth between a row and its depth twin; and the extremes the search
// accepts, ±1e9 quantised, where the fitness passes 2^62.
//
// The tables are Mojang-derived and never committed (SPEC §12); without them
// every case here SKIPs.

#include <stratum/biome/parameter_list.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/rng/java_random.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

using stratum::biome::ClimateSample;
using stratum::biome::Entry;
using stratum::biome::kMaxQuantizedSample;
using stratum::biome::ParameterList;
using stratum::biome::QuantizedPoint;
using stratum::biome::QuantizedSample;
using stratum::biome::SearchCounts;
using stratum::data::ResourceLocation;
using stratum::rng::JavaRandom;

/// Samples per case on each table. Four cases, so over a million in all on
/// each — split so that a parallel ctest runs them side by side, since the
/// reference they are held to scans the whole table per sample.
constexpr std::size_t kSamplesPerCase = 262'144;

/// One sample in this many is also scanned by this file's own pass, to count
/// how many of the samples are ties.
constexpr std::size_t kTieCountStride = 8;

[[nodiscard]] std::filesystem::path fixtures() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR};
}

/// The directory tools/fetch-vanilla dumps the server's tables into.
[[nodiscard]] std::filesystem::path findBiomeParameters() {
    if (!std::filesystem::is_directory(fixtures())) {
        return {};
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(fixtures())) {
        if (entry.is_directory() && entry.path().filename() == "biome_parameters") {
            return entry.path();
        }
    }
    return {};
}

/// Every table under it, by id, read as the pipeline reads them.
[[nodiscard]] std::map<std::string, ParameterList> readTables(const std::filesystem::path& dir) {
    std::map<std::string, ParameterList> tables;
    for (const auto& file : std::filesystem::recursive_directory_iterator(dir)) {
        if (!file.is_regular_file() || file.path().extension() != ".json") {
            continue;
        }
        const std::filesystem::path relative = std::filesystem::relative(file.path(), dir);
        const std::string namespaceName = relative.begin()->string();
        const std::string path =
            std::filesystem::relative(relative, namespaceName).replace_extension().generic_string();
        const ResourceLocation id(namespaceName, path);
        std::ifstream stream(file.path());
        tables.emplace(id.toString(), ParameterList::fromJson(nlohmann::json::parse(stream), id));
    }
    return tables;
}

[[nodiscard]] QuantizedSample sampleOf(const std::array<std::int64_t, 6>& axes) {
    return QuantizedSample{.temperature = axes[0],
                           .humidity = axes[1],
                           .continentalness = axes[2],
                           .erosion = axes[3],
                           .depth = axes[4],
                           .weirdness = axes[5]};
}

[[nodiscard]] std::int64_t uniform(JavaRandom& random, std::int64_t lo, std::int64_t hi) {
    return lo + static_cast<std::int64_t>(random.nextDouble() * static_cast<double>(hi - lo + 1));
}

[[nodiscard]] double uniformReal(JavaRandom& random, double lo, double hi) {
    return lo + ((hi - lo) * random.nextDouble());
}

/// The table as the samples need it: each row's quantised box, every
/// distinct bound per axis, the span of the whole table, and each row's
/// depth twin — the row with the same other five ranges and offset.
struct Table {
    std::vector<QuantizedPoint> points;
    std::array<std::vector<std::int64_t>, 6> bounds;
    std::array<std::int64_t, 6> lowest{};
    std::array<std::int64_t, 6> highest{};
    std::vector<std::size_t> twinOf;

    explicit Table(const ParameterList& list) {
        lowest.fill(kMaxQuantizedSample);
        highest.fill(-kMaxQuantizedSample);
        for (const Entry& entry : list.entries()) {
            const QuantizedPoint point = QuantizedPoint::of(entry.parameters);
            for (std::size_t axis = 0; axis < 6; ++axis) {
                bounds[axis].push_back(point.min[axis]);
                bounds[axis].push_back(point.max[axis]);
                lowest[axis] = std::min(lowest[axis], point.min[axis]);
                highest[axis] = std::max(highest[axis], point.max[axis]);
            }
            points.push_back(point);
        }
        for (std::vector<std::int64_t>& axisBounds : bounds) {
            std::ranges::sort(axisBounds);
            const auto repeated = std::ranges::unique(axisBounds);
            axisBounds.erase(repeated.begin(), repeated.end());
        }
        // Rows keyed by everything but depth: rows sharing a key are twins.
        std::map<std::array<std::int64_t, 11>, std::vector<std::size_t>> byKey;
        for (std::size_t row = 0; row < points.size(); ++row) {
            const QuantizedPoint& point = points[row];
            byKey[{point.min[0], point.max[0], point.min[1], point.max[1], point.min[2],
                   point.max[2], point.min[3], point.max[3], point.min[5], point.max[5],
                   point.offset}]
                .push_back(row);
        }
        twinOf.assign(points.size(), points.size());
        for (const auto& [key, rows] : byKey) {
            if (rows.size() == 2) {
                twinOf[rows[0]] = rows[1];
                twinOf[rows[1]] = rows[0];
            }
        }
    }

    [[nodiscard]] std::size_t tiedRows(const QuantizedSample& sample) const {
        std::int64_t best = points[0].fitness(sample);
        std::size_t tied = 1;
        for (std::size_t row = 1; row < points.size(); ++row) {
            const std::int64_t fitness = points[row].fitness(sample);
            if (fitness < best) {
                best = fitness;
                tied = 1;
            } else if (fitness == best) {
                ++tied;
            }
        }
        return tied;
    }
};

struct Tally {
    std::size_t samples = 0;
    std::size_t mismatches = 0;
    std::size_t tieChecked = 0;
    std::size_t ties = 0;
    std::string firstMismatch;
    /// What the quantised-path queries scored, and how many there were.
    SearchCounts counts;
    std::size_t counted = 0;
};

void record(const ParameterList& list, const Table& table, const QuantizedSample& sample,
            std::size_t tree, std::size_t linear, Tally& tally) {
    if (tree != linear) {
        if (tally.mismatches == 0) {
            tally.firstMismatch =
                "(" + std::to_string(sample.temperature) + ", " + std::to_string(sample.humidity) +
                ", " + std::to_string(sample.continentalness) + ", " +
                std::to_string(sample.erosion) + ", " + std::to_string(sample.depth) + ", " +
                std::to_string(sample.weirdness) + "): tree row " + std::to_string(tree) + " (" +
                list.entries()[tree].biome.toString() + "), scan row " + std::to_string(linear) +
                " (" + list.entries()[linear].biome.toString() + ")";
        }
        ++tally.mismatches;
    }
    if (tally.samples % kTieCountStride == 0) {
        ++tally.tieChecked;
        tally.ties += table.tiedRows(sample) > 1 ? 1U : 0U;
    }
    ++tally.samples;
}

/// Asks one quantised sample of both searches.
void ask(const ParameterList& list, const Table& table, const QuantizedSample& sample,
         Tally& tally) {
    const std::size_t tree = list.findRow(sample, tally.counts);
    ++tally.counted;
    record(list, table, sample, tree, list.findRowLinear(sample), tally);
}

/// Asks one climate sample of both, through the double-valued path and its
/// quantisation, and holds `find`'s biome to the row's.
void ask(const ParameterList& list, const Table& table, const ClimateSample& climate,
         Tally& tally) {
    const std::size_t tree = list.findRow(climate);
    const std::size_t linear = list.findRowLinear(climate);
    if (list.find(climate) != list.entries()[linear].biome) {
        ++tally.mismatches;
    }
    record(list, table, QuantizedSample::of(climate), tree, linear, tally);
}

/// One sample of each kind, @p rounds times.
void askEveryKind(const ParameterList& list, const Table& table, JavaRandom& random,
                  std::size_t rounds, Tally& tally) {
    const auto anyRow = [&]() {
        return static_cast<std::size_t>(
            random.nextInt(static_cast<std::int32_t>(table.points.size())));
    };
    const auto onBound = [&](std::size_t axis) {
        const std::vector<std::int64_t>& axisBounds = table.bounds[axis];
        const std::int64_t bound = axisBounds[static_cast<std::size_t>(
            random.nextInt(static_cast<std::int32_t>(axisBounds.size())))];
        return bound + random.nextInt(3) - 1;
    };
    const auto acrossTable = [&](std::size_t axis) {
        return uniform(random, table.lowest[axis] - 2000, table.highest[axis] + 2000);
    };
    for (std::size_t round = 0; round < rounds; ++round) {
        std::array<std::int64_t, 6> axes{};

        // 1. Uniform over the table's span on every axis.
        for (std::size_t axis = 0; axis < 6; ++axis) {
            axes[axis] = acrossTable(axis);
        }
        ask(list, table, sampleOf(axes), tally);

        // 2. Uniform climate, with depth over the router's [-1.5, 2].
        ask(list, table,
            ClimateSample{.temperature = uniformReal(random, -1.2, 1.2),
                          .humidity = uniformReal(random, -1.2, 1.2),
                          .continentalness = uniformReal(random, -1.3, 1.2),
                          .erosion = uniformReal(random, -1.2, 1.2),
                          .depth = uniformReal(random, -1.5, 2.0),
                          .weirdness = uniformReal(random, -1.2, 1.2)},
            tally);

        // 3. Gaussian climate, depth a gradient down the column (1.5 at
        //    y = -64 to -1.5 at y = 320, the router's) plus an offset.
        const double y = uniformReal(random, -64.0, 320.0);
        ask(list, table,
            ClimateSample{.temperature = 0.5 * random.nextGaussian(),
                          .humidity = 0.5 * random.nextGaussian(),
                          .continentalness = 0.5 * random.nextGaussian(),
                          .erosion = 0.5 * random.nextGaussian(),
                          .depth = 1.5 - (3.0 * (y + 64.0) / 384.0) + (0.2 * random.nextGaussian()),
                          .weirdness = 0.5 * random.nextGaussian()},
            tally);

        // 4. Each axis on a bound (or one quantum off) with probability 0.6.
        for (std::size_t axis = 0; axis < 6; ++axis) {
            axes[axis] = random.nextInt(5) < 3 ? onBound(axis) : acrossTable(axis);
        }
        ask(list, table, sampleOf(axes), tally);

        // 5. Every axis on a bound, or one quantum off.
        for (std::size_t axis = 0; axis < 6; ++axis) {
            axes[axis] = onBound(axis);
        }
        ask(list, table, sampleOf(axes), tally);

        // 6. A constructed tie with a neighbour: inside one row on five axes
        //    and exactly on one of its faces on the sixth, which the next
        //    row over shares.
        const QuantizedPoint& row = table.points[anyRow()];
        for (std::size_t axis = 0; axis < 6; ++axis) {
            axes[axis] = uniform(random, row.min[axis], row.max[axis]);
        }
        const auto face = static_cast<std::size_t>(random.nextInt(6));
        axes[face] = random.nextBoolean() ? row.min[face] : row.max[face];
        ask(list, table, sampleOf(axes), tally);

        // 7. A constructed tie with a depth twin: inside the row on the other
        //    five axes, and halfway between the two depths.
        const std::size_t first = anyRow();
        const std::size_t twin = table.twinOf[first];
        if (twin < table.points.size()) {
            const QuantizedPoint& a = table.points[first];
            const QuantizedPoint& b = table.points[twin];
            for (std::size_t axis = 0; axis < 6; ++axis) {
                axes[axis] = uniform(random, a.min[axis], a.max[axis]);
            }
            const std::int64_t gapLow = std::min(a.max[4], b.max[4]);
            const std::int64_t gapHigh = std::max(a.min[4], b.min[4]);
            axes[4] = gapLow + ((gapHigh - gapLow) / 2);
            ask(list, table, sampleOf(axes), tally);
        }

        // 8. The extremes the search accepts, mixed with ordinary values.
        for (std::size_t axis = 0; axis < 6; ++axis) {
            const std::int32_t pick = random.nextInt(3);
            if (pick == 0) {
                axes[axis] = kMaxQuantizedSample;
            } else if (pick == 1) {
                axes[axis] = -kMaxQuantizedSample;
            } else {
                axes[axis] = acrossTable(axis);
            }
        }
        ask(list, table, sampleOf(axes), tally);
    }
}

/// One of the four cases: every table, every kind, its own seed.
void compareOnEveryTable(std::int64_t seed) {
    const std::filesystem::path dir = findBiomeParameters();
    if (dir.empty()) {
        SKIP("no biome parameter tables under "
             << STRATUM_FIXTURES_DIR
             << " — Mojang-derived and never committed (SPEC §12). Generate them with: "
                "tools/fetch-vanilla");
    }
    const std::map<std::string, ParameterList> tables = readTables(dir);
    REQUIRE(tables.contains("minecraft:overworld"));
    REQUIRE(tables.contains("minecraft:nether"));
    REQUIRE(tables.at("minecraft:overworld").size() == 7593U);

    for (const auto& [id, list] : tables) {
        CAPTURE(id);
        REQUIRE(list.auditSearchTree().empty());
        const Table table(list);
        JavaRandom random(seed);
        Tally tally;
        // Eight kinds a round, the depth-twin kind only where a twin exists.
        while (tally.samples < kSamplesPerCase) {
            askEveryKind(list, table, random, 1024, tally);
        }
        CAPTURE(tally.samples, tally.tieChecked, tally.ties, tally.firstMismatch);
        CHECK(tally.mismatches == 0U);
        // Ties have to be among the samples for the comparison to say
        // anything about the tie rule: more than a tenth of the overworld's.
        // The Nether's five rows are points with offsets, between which a
        // tie needs a sample equidistant from two of them, so it has few.
        if (id == "minecraft:overworld") {
            CHECK(tally.ties * 10 > tally.tieChecked);
        }
        const auto counted = static_cast<double>(tally.counted);
        WARN("biome search over " << id << " (" << list.size() << " rows), seed " << seed << ": "
                                  << tally.samples << " samples, " << tally.mismatches
                                  << " answered other than the scan; " << tally.ties << " of "
                                  << tally.tieChecked << " counted were ties; per query "
                                  << static_cast<double>(tally.counts.rows) / counted
                                  << " rows and "
                                  << static_cast<double>(tally.counts.nodes) / counted
                                  << " node bounds scored (over the " << tally.counted
                                  << " quantised-path queries)");
    }
}

} // namespace

TEST_CASE("the biome search tree gives the scan's row on the server's tables, set 1 of 4",
          "[conformance][biome-search]") {
    compareOnEveryTable(1);
}

TEST_CASE("the biome search tree gives the scan's row on the server's tables, set 2 of 4",
          "[conformance][biome-search]") {
    compareOnEveryTable(2);
}

TEST_CASE("the biome search tree gives the scan's row on the server's tables, set 3 of 4",
          "[conformance][biome-search]") {
    compareOnEveryTable(3);
}

TEST_CASE("the biome search tree gives the scan's row on the server's tables, set 4 of 4",
          "[conformance][biome-search]") {
    compareOnEveryTable(4);
}

TEST_CASE("the biome search tree over the server's tables is sound and small",
          "[conformance][biome-search]") {
    const std::filesystem::path dir = findBiomeParameters();
    if (dir.empty()) {
        SKIP("no biome parameter tables under "
             << STRATUM_FIXTURES_DIR
             << " — Mojang-derived and never committed (SPEC §12). Generate them with: "
                "tools/fetch-vanilla");
    }
    const std::map<std::string, ParameterList> tables = readTables(dir);
    REQUIRE(tables.contains("minecraft:overworld"));
    for (const auto& [id, list] : tables) {
        CAPTURE(id);
        CHECK(list.auditSearchTree().empty());
        // Built again from its entries, as a frozen pipeline's thaw builds
        // it: the same tree, so the same rows.
        const auto start = std::chrono::steady_clock::now();
        const ParameterList thawed =
            ParameterList::fromEntries(list.entries(), ResourceLocation::parse(id));
        const auto elapsed =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        CHECK(thawed.auditSearchTree().empty());
        const stratum::biome::SearchTreeShape shape = list.searchTreeShape();
        const stratum::biome::SearchTreeShape thawedShape = thawed.searchTreeShape();
        CHECK(thawedShape.nodes == shape.nodes);
        CHECK(thawedShape.depth == shape.depth);
        CHECK(shape.nodes == (2 * shape.leaves) - 1);
        WARN("biome search tree over "
             << id << ": " << list.size() << " rows, " << shape.nodes << " nodes, " << shape.leaves
             << " leaves, depth " << shape.depth << ", " << shape.bytes << " bytes; fromEntries "
             << elapsed << " ms (this build)");
        JavaRandom random(77);
        const Table table(list);
        std::size_t differing = 0;
        for (std::size_t i = 0; i < 4096; ++i) {
            const QuantizedPoint& row = table.points[static_cast<std::size_t>(
                random.nextInt(static_cast<std::int32_t>(table.points.size())))];
            std::array<std::int64_t, 6> axes{};
            for (std::size_t axis = 0; axis < 6; ++axis) {
                axes[axis] = uniform(random, row.min[axis] - 500, row.max[axis] + 500);
            }
            differing += thawed.findRow(sampleOf(axes)) == list.findRow(sampleOf(axes)) ? 0U : 1U;
        }
        CHECK(differing == 0U);
    }
}
