// Stratum — the biome search's tree, held to the linear scan it replaced.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// `ParameterList::find` searches a tree of boxes; `findRowLinear` is the scan
// that DEFINES the answer — every row scored, the later of a tie kept. Every
// case here compares the ROW the two return, not the biome: two rows can carry
// one biome, and a search that took the wrong one of them would pass a biome
// comparison and then, the day the table is edited, move a biome.
//
// The tables are this file's own, built to be hard rather than realistic:
// coarse bounds so boxes coincide, nest and share faces, point intervals,
// offsets, and duplicated rows, queried uniformly, on and one quantum either
// side of every kind of bound, at the extremes of what the search accepts,
// and at constructed ties. The real tables are held to the same comparison in
// tests/conformance/vanilla_biome_search_test.cpp.

#include <stratum/biome/parameter_list.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/rng/java_random.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

using Catch::Matchers::ContainsSubstring;
using stratum::biome::ClimateSample;
using stratum::biome::Entry;
using stratum::biome::kMaxClimateSample;
using stratum::biome::kMaxQuantizedSample;
using stratum::biome::Parameter;
using stratum::biome::ParameterError;
using stratum::biome::ParameterList;
using stratum::biome::QuantizedPoint;
using stratum::biome::QuantizedSample;
using stratum::biome::SearchCounts;
using stratum::data::ResourceLocation;
using stratum::rng::JavaRandom;

[[nodiscard]] ResourceLocation listId() {
    return ResourceLocation::parse("stratum:search_test");
}

[[nodiscard]] QuantizedSample sampleOf(const std::array<std::int64_t, 6>& axes) {
    return QuantizedSample{.temperature = axes[0],
                           .humidity = axes[1],
                           .continentalness = axes[2],
                           .erosion = axes[3],
                           .depth = axes[4],
                           .weirdness = axes[5]};
}

[[nodiscard]] Parameter& axisOf(Entry& entry, std::size_t axis) {
    switch (axis) {
        case 0:
            return entry.parameters.temperature;
        case 1:
            return entry.parameters.humidity;
        case 2:
            return entry.parameters.continentalness;
        case 3:
            return entry.parameters.erosion;
        case 4:
            return entry.parameters.depth;
        default:
            return entry.parameters.weirdness;
    }
}

/// A uniform draw from [lo, hi].
[[nodiscard]] std::int64_t uniform(JavaRandom& random, std::int64_t lo, std::int64_t hi) {
    return lo + static_cast<std::int64_t>(random.nextDouble() * static_cast<double>(hi - lo + 1));
}

/// A table made to be hard. Bounds come from a coarse grid so that rows
/// share faces and ties are everywhere; a quarter of the rows are point
/// intervals on some axis, some rows are nested inside earlier ones, some are
/// exact duplicates of earlier ones carrying another biome, and some carry
/// an offset.
[[nodiscard]] std::vector<Entry> hardTable(JavaRandom& random, std::size_t rows,
                                           std::int32_t gridSteps) {
    std::vector<Entry> entries;
    entries.reserve(rows);
    const auto gridValue = [&]() {
        // [-2, 2] in gridSteps steps: the schema's whole range, ends included.
        const std::int32_t step = random.nextInt(gridSteps + 1);
        return -2.0 + (4.0 * static_cast<double>(step) / static_cast<double>(gridSteps));
    };
    for (std::size_t row = 0; row < rows; ++row) {
        Entry entry;
        entry.biome = ResourceLocation("stratum", "b" + std::to_string(row % 37));
        const std::int32_t kind = random.nextInt(10);
        if (kind == 0 && !entries.empty()) {
            // An exact duplicate of an earlier row: a tie at every sample.
            const auto earlier =
                static_cast<std::size_t>(random.nextInt(static_cast<std::int32_t>(entries.size())));
            entry.parameters = entries[earlier].parameters;
        } else if (kind == 1 && !entries.empty()) {
            // Nested inside an earlier row, sharing at least one face.
            const auto earlier =
                static_cast<std::size_t>(random.nextInt(static_cast<std::int32_t>(entries.size())));
            entry.parameters = entries[earlier].parameters;
            for (std::size_t axis = 0; axis < 6; ++axis) {
                Parameter& parameter = axisOf(entry, axis);
                const double width = parameter.max - parameter.min;
                // At most 0.4 of the width off each end, so it stays a range.
                if (random.nextBoolean()) {
                    parameter.min += width * random.nextDouble() * 0.4;
                }
                if (random.nextBoolean()) {
                    parameter.max -= width * random.nextDouble() * 0.4;
                }
            }
        } else {
            for (std::size_t axis = 0; axis < 6; ++axis) {
                Parameter& parameter = axisOf(entry, axis);
                double a = gridValue();
                double b = random.nextInt(4) == 0 ? a : gridValue();
                if (b < a) {
                    std::swap(a, b);
                }
                parameter = Parameter{.min = a, .max = b};
            }
        }
        const std::int32_t offsetKind = random.nextInt(8);
        if (offsetKind == 0) {
            entry.parameters.offset = 0.125 * static_cast<double>(random.nextInt(9));
        } else if (offsetKind == 1) {
            entry.parameters.offset = random.nextDouble();
        }
        entries.push_back(entry);
    }
    if (entries.size() >= 2) {
        // At least one twin, whatever the draws: the last row duplicates an
        // earlier one, so every table has a tie to be asked about.
        const auto earlier =
            static_cast<std::size_t>(random.nextInt(static_cast<std::int32_t>(entries.size() - 1)));
        entries.back().parameters = entries[earlier].parameters;
    }
    return entries;
}

/// The rows that have an identical twin somewhere in the table, once
/// quantised: inside such a row, the twins tie unless a third row is closer.
[[nodiscard]] std::vector<std::size_t> twinnedRows(const std::vector<QuantizedPoint>& points) {
    std::vector<std::size_t> twinned;
    for (std::size_t row = 0; row < points.size(); ++row) {
        for (std::size_t other = 0; other < points.size(); ++other) {
            if (other != row && points[other] == points[row]) {
                twinned.push_back(row);
                break;
            }
        }
    }
    return twinned;
}

/// Every quantised bound the table has on each axis.
[[nodiscard]] std::array<std::vector<std::int64_t>, 6> boundsOf(const ParameterList& list) {
    std::array<std::vector<std::int64_t>, 6> bounds;
    for (const Entry& entry : list.entries()) {
        const QuantizedPoint point = QuantizedPoint::of(entry.parameters);
        for (std::size_t axis = 0; axis < 6; ++axis) {
            bounds[axis].push_back(point.min[axis]);
            bounds[axis].push_back(point.max[axis]);
        }
    }
    return bounds;
}

/// How the tree and the scan compare over a batch of samples.
struct Tally {
    std::size_t samples = 0;
    std::size_t mismatches = 0;
    /// Samples where two or more rows share the minimum fitness.
    std::size_t ties = 0;
    std::string firstMismatch;
    SearchCounts counts;
};

/// Every row at the minimum fitness: how many there are.
[[nodiscard]] std::size_t tiedRows(const std::vector<QuantizedPoint>& points,
                                   const QuantizedSample& sample) {
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

void compare(const ParameterList& list, const std::vector<QuantizedPoint>& points,
             const QuantizedSample& sample, Tally& tally) {
    ++tally.samples;
    const std::size_t tree = list.findRow(sample, tally.counts);
    const std::size_t linear = list.findRowLinear(sample);
    if (tree != linear) {
        if (tally.mismatches == 0) {
            tally.firstMismatch =
                "(" + std::to_string(sample.temperature) + ", " + std::to_string(sample.humidity) +
                ", " + std::to_string(sample.continentalness) + ", " +
                std::to_string(sample.erosion) + ", " + std::to_string(sample.depth) + ", " +
                std::to_string(sample.weirdness) + "): tree row " + std::to_string(tree) +
                ", scan row " + std::to_string(linear);
        }
        ++tally.mismatches;
    }
    tally.ties += tiedRows(points, sample) > 1 ? 1U : 0U;
}

[[nodiscard]] std::vector<QuantizedPoint> pointsOf(const ParameterList& list) {
    std::vector<QuantizedPoint> points;
    points.reserve(list.size());
    for (const Entry& entry : list.entries()) {
        points.push_back(QuantizedPoint::of(entry.parameters));
    }
    return points;
}

/// The sample mix every random table is queried with.
void queryHard(const ParameterList& list, JavaRandom& random, std::size_t perKind, Tally& tally) {
    const std::vector<QuantizedPoint> points = pointsOf(list);
    const std::array<std::vector<std::int64_t>, 6> bounds = boundsOf(list);
    const std::vector<std::size_t> twinned = twinnedRows(points);
    const auto onBound = [&](std::size_t axis) {
        const std::vector<std::int64_t>& axisBounds = bounds[axis];
        const std::int64_t bound = axisBounds[static_cast<std::size_t>(
            random.nextInt(static_cast<std::int32_t>(axisBounds.size())))];
        return bound + random.nextInt(3) - 1;
    };
    for (std::size_t i = 0; i < perKind; ++i) {
        std::array<std::int64_t, 6> axes{};

        // Uniform over a little more than the schema's range.
        for (std::int64_t& value : axes) {
            value = uniform(random, -25000, 25000);
        }
        compare(list, points, sampleOf(axes), tally);

        // Every axis on a bound, or one quantum either side of it.
        for (std::size_t axis = 0; axis < 6; ++axis) {
            axes[axis] = onBound(axis);
        }
        compare(list, points, sampleOf(axes), tally);

        // Each axis on a bound with probability 0.6, uniform otherwise.
        for (std::size_t axis = 0; axis < 6; ++axis) {
            axes[axis] = random.nextInt(5) < 3 ? onBound(axis) : uniform(random, -25000, 25000);
        }
        compare(list, points, sampleOf(axes), tally);

        // Inside one row's box, which a duplicate or a nested row then ties.
        const QuantizedPoint& row = points[static_cast<std::size_t>(
            random.nextInt(static_cast<std::int32_t>(points.size())))];
        for (std::size_t axis = 0; axis < 6; ++axis) {
            axes[axis] = uniform(random, row.min[axis], row.max[axis]);
        }
        compare(list, points, sampleOf(axes), tally);

        // A constructed tie: inside a row that has a twin.
        if (!twinned.empty()) {
            const QuantizedPoint& twin = points[twinned[static_cast<std::size_t>(
                random.nextInt(static_cast<std::int32_t>(twinned.size())))]];
            for (std::size_t axis = 0; axis < 6; ++axis) {
                axes[axis] = uniform(random, twin.min[axis], twin.max[axis]);
            }
            compare(list, points, sampleOf(axes), tally);
        }

        // The extremes the search accepts, mixed with ordinary values: the
        // fitness there is near 6e18, past any sentinel short of int64's max.
        for (std::int64_t& value : axes) {
            const std::int32_t pick = random.nextInt(4);
            if (pick == 0) {
                value = kMaxQuantizedSample;
            } else if (pick == 1) {
                value = -kMaxQuantizedSample;
            } else {
                value = uniform(random, -25000, 25000);
            }
        }
        compare(list, points, sampleOf(axes), tally);
    }
}

} // namespace

TEST_CASE("a thousand identical rows always give the last", "[biome][parameters][search]") {
    // Every sample ties all thousand rows, and the scan keeps the last. The
    // tree has to find row 999 AND prove every other leaf unable to beat it
    // by index alone, since every bound in the tree equals the best fitness.
    Entry entry;
    entry.parameters.temperature = Parameter{.min = -0.5, .max = 0.25};
    entry.parameters.humidity = Parameter{.min = 0.0, .max = 0.0};
    entry.parameters.continentalness = Parameter{.min = -1.0, .max = 1.0};
    entry.parameters.erosion = Parameter{.min = 0.375, .max = 0.375};
    entry.parameters.depth = Parameter{.min = 0.0, .max = 1.0};
    entry.parameters.weirdness = Parameter{.min = -2.0, .max = 2.0};
    entry.parameters.offset = 0.25;
    const std::vector<Entry> entries(1000, entry);
    const ParameterList list = ParameterList::fromEntries(entries, listId());
    REQUIRE(list.auditSearchTree().empty());

    JavaRandom random(1000);
    SearchCounts counts;
    std::size_t queries = 0;
    std::size_t wrong = 0;
    const auto ask = [&](const std::array<std::int64_t, 6>& axes) {
        ++queries;
        const QuantizedSample sample = sampleOf(axes);
        wrong += list.findRow(sample, counts) == 999U ? 0U : 1U;
        wrong += list.findRowLinear(sample) == 999U ? 0U : 1U;
    };
    for (std::size_t i = 0; i < 2000; ++i) {
        std::array<std::int64_t, 6> axes{};
        for (std::int64_t& value : axes) {
            value = uniform(random, -30000, 30000);
        }
        ask(axes);
    }
    // On every bound, and the extremes on every axis at once.
    const QuantizedPoint point = QuantizedPoint::of(entry.parameters);
    for (std::size_t axis = 0; axis < 6; ++axis) {
        std::array<std::int64_t, 6> axes{};
        for (const std::int64_t value :
             {point.min[axis] - 1, point.min[axis], point.max[axis], point.max[axis] + 1}) {
            axes[axis] = value;
            ask(axes);
        }
    }
    for (std::uint32_t corner = 0; corner < 64; ++corner) {
        std::array<std::int64_t, 6> axes{};
        for (std::size_t axis = 0; axis < 6; ++axis) {
            axes[axis] = ((corner >> axis) & 1U) != 0U ? kMaxQuantizedSample : -kMaxQuantizedSample;
        }
        ask(axes);
    }
    CHECK(wrong == 0U);
    // And it proves it cheaply: the ties are settled by the rows' indices,
    // not by scoring all thousand rows each time.
    CAPTURE(counts.rows, counts.nodes, queries);
    CHECK(counts.rows <= 16U * queries);
    CHECK(list.find(ClimateSample{}) == entry.biome);
}

TEST_CASE("the tree returns the scan's row on hard random tables", "[biome][parameters][search]") {
    // Small tables with coarse grids are the tie-heavy ones; the large one
    // builds a deep tree. Each table is queried from both of the paths a
    // list is built through, fromJson's and a frozen pipeline's thaw being
    // the same fromEntries.
    struct Shape {
        std::size_t rows;
        std::int32_t gridSteps;
    };

    for (const Shape shape : {Shape{1, 4}, Shape{2, 4}, Shape{7, 4}, Shape{8, 8}, Shape{9, 4},
                              Shape{40, 4}, Shape{200, 8}, Shape{1000, 16}, Shape{3000, 64}}) {
        CAPTURE(shape.rows, shape.gridSteps);
        JavaRandom random(static_cast<std::int64_t>(shape.rows * 7919) + shape.gridSteps);
        const ParameterList list =
            ParameterList::fromEntries(hardTable(random, shape.rows, shape.gridSteps), listId());
        REQUIRE(list.auditSearchTree().empty());
        const ParameterList thawed = ParameterList::fromEntries(list.entries(), listId());
        REQUIRE(thawed.auditSearchTree().empty());

        Tally tally;
        queryHard(list, random, shape.rows < 100 ? 2000 : 400, tally);
        CAPTURE(tally.firstMismatch, tally.samples, tally.ties);
        CHECK(tally.mismatches == 0U);
        if (shape.rows > 1) {
            // The comparison means little unless ties were among the samples.
            CHECK(tally.ties * 10 > tally.samples);
        }
        // The tree scored a small part of the table, or it is not a search.
        if (shape.rows >= 1000) {
            CAPTURE(tally.counts.rows);
            CHECK(tally.counts.rows * 4 < tally.samples * shape.rows);
        }

        JavaRandom again(17);
        Tally thawedTally;
        queryHard(thawed, again, 200, thawedTally);
        CAPTURE(thawedTally.firstMismatch);
        CHECK(thawedTally.mismatches == 0U);
    }
}

TEST_CASE("every sample over a small table's bounds gives the scan's row",
          "[biome][parameters][search]") {
    // Three bound values per axis, so each axis has nine values worth
    // asking — each bound and one quantum either side — and all 9^6 = 531441
    // combinations are asked. Exhaustive where the ties and the faces are.
    const std::array<double, 3> grid{-0.5, 0.0, 0.5};
    JavaRandom random(9);
    std::vector<Entry> entries;
    for (std::size_t row = 0; row < 24; ++row) {
        Entry entry;
        entry.biome = ResourceLocation("stratum", "b" + std::to_string(row % 5));
        for (std::size_t axis = 0; axis < 6; ++axis) {
            const auto a = static_cast<std::size_t>(random.nextInt(3));
            const auto b = static_cast<std::size_t>(random.nextInt(3));
            axisOf(entry, axis) =
                Parameter{.min = grid[std::min(a, b)], .max = grid[std::max(a, b)]};
        }
        entry.parameters.offset = random.nextInt(4) == 0 ? 0.125 : 0.0;
        entries.push_back(entry);
    }
    entries.push_back(entries[3]); // a duplicate, tied everywhere with row 3
    const ParameterList list = ParameterList::fromEntries(entries, listId());
    REQUIRE(list.auditSearchTree().empty());

    std::array<std::int64_t, 9> values{};
    std::size_t next = 0;
    for (const double bound : grid) {
        const std::int64_t quantized = stratum::biome::quantizeCoord(bound);
        for (const std::int64_t delta : {-1, 0, 1}) {
            values[next++] = quantized + delta;
        }
    }
    std::size_t mismatches = 0;
    std::size_t asked = 0;
    std::array<std::size_t, 6> digit{};
    while (true) {
        std::array<std::int64_t, 6> axes{};
        for (std::size_t axis = 0; axis < 6; ++axis) {
            axes[axis] = values[digit[axis]];
        }
        const QuantizedSample sample = sampleOf(axes);
        mismatches += list.findRow(sample) == list.findRowLinear(sample) ? 0U : 1U;
        ++asked;
        std::size_t axis = 0;
        while (axis < 6 && ++digit[axis] == values.size()) {
            digit[axis] = 0;
            ++axis;
        }
        if (axis == 6) {
            break;
        }
    }
    CHECK(asked == 531441U);
    CHECK(mismatches == 0U);
}

TEST_CASE("the climate path searches the tree and refuses what the scan refuses",
          "[biome][parameters][search]") {
    JavaRandom random(42);
    const ParameterList list = ParameterList::fromEntries(hardTable(random, 300, 8), listId());
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < 3000; ++i) {
        const ClimateSample sample{.temperature = random.nextGaussian(),
                                   .humidity = random.nextGaussian(),
                                   .continentalness = random.nextGaussian(),
                                   .erosion = random.nextGaussian(),
                                   .depth = random.nextGaussian(),
                                   .weirdness = random.nextGaussian()};
        const std::size_t row = list.findRow(sample);
        mismatches += row == list.findRowLinear(sample) ? 0U : 1U;
        mismatches += list.find(sample) == list.entries()[row].biome ? 0U : 1U;
    }
    CHECK(mismatches == 0U);

    // The bound itself still searches, by both paths.
    const ClimateSample widest{.temperature = kMaxClimateSample,
                               .humidity = -kMaxClimateSample,
                               .continentalness = kMaxClimateSample,
                               .erosion = -kMaxClimateSample,
                               .depth = kMaxClimateSample,
                               .weirdness = -kMaxClimateSample};
    CHECK(list.findRow(widest) == list.findRowLinear(widest));

    ClimateSample tooWide;
    tooWide.depth = 2.0 * kMaxClimateSample;
    CHECK_THROWS_WITH(list.findRow(tooWide), ContainsSubstring("depth of 200000"));
    CHECK_THROWS_WITH(list.findRowLinear(tooWide), ContainsSubstring("depth of 200000"));
    QuantizedSample quantizedTooWide;
    quantizedTooWide.erosion = kMaxQuantizedSample + 1;
    CHECK_THROWS_WITH(list.findRow(quantizedTooWide), ContainsSubstring("erosion of 1000000001"));
    CHECK_THROWS_WITH(list.findRowLinear(quantizedTooWide),
                      ContainsSubstring("erosion of 1000000001"));

    const ParameterList empty = ParameterList::fromEntries({}, listId());
    CHECK(empty.auditSearchTree().empty());
    CHECK(empty.searchTreeShape().nodes == 0U);
    CHECK_THROWS_AS(empty.findRow(QuantizedSample{}), ParameterError);
    CHECK_THROWS_AS(empty.findRowLinear(QuantizedSample{}), ParameterError);
}

TEST_CASE("the search tree's shape is reported", "[biome][parameters][search]") {
    JavaRandom random(5);
    const ParameterList list = ParameterList::fromEntries(hardTable(random, 1000, 16), listId());
    const stratum::biome::SearchTreeShape shape = list.searchTreeShape();
    // A binary tree over 1000 rows in leaves of at most eight.
    CHECK(shape.leaves >= 125U);
    CHECK(shape.nodes == (2 * shape.leaves) - 1);
    CHECK(shape.depth >= 7U);
    CHECK(shape.bytes > 0U);

    // One row: a single leaf, the root.
    const ParameterList one = ParameterList::fromEntries({list.entries()[0]}, listId());
    CHECK(one.searchTreeShape().nodes == 1U);
    CHECK(one.searchTreeShape().depth == 0U);
    CHECK(one.findRow(QuantizedSample{}) == 0U);
}
