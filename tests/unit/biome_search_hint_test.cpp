// Stratum — the biome search's warm start, held to the linear scan.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// `ParameterList::findRowFrom` scores a hinted row before it enters the tree
// and prunes with that row's fitness from the root down. The claim is that
// the hint changes how much of the tree is scored and never which row comes
// back: every case here compares the ROW against `findRowLinear`, the scan
// that defines the answer (least fitness, the LATER row of a tie), as
// tests/unit/biome_search_test.cpp does for the unhinted search.
//
// The hints are chosen to break a wrong pruning rule rather than to help:
// the answer itself; the rows tied with the answer at lower and at higher
// indices (a hint at a tied EARLIER row must not hide the later one, which a
// non-strict comparison on rows would do); the row that scores worst of all;
// neighbours of the answer by index; the first and last rows; and hints past
// the end of the list, which are no hint. And the one caller,
// biome::Source::fillChunk, which threads the hints through a chunk, is held
// row for row to `Source::at` (which searches unhinted) on a router whose
// every climate entry reads x, y and z, so most of its hints are wrong.

#include "support/temp_path.hpp"

#include <stratum/biome/parameter_list.hpp>
#include <stratum/biome/source.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_parameters.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/density/random_source.hpp>
#include <stratum/rng/java_random.hpp>
#include <stratum/settings/noise_settings.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
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
    return ResourceLocation::parse("stratum:search_hint_test");
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

/// A table made to be hard, as biome_search_test.cpp's: bounds on a coarse
/// grid so boxes share faces and ties are everywhere, point intervals, rows
/// nested in earlier ones, exact duplicates carrying another biome, offsets.
[[nodiscard]] std::vector<Entry> hardTable(JavaRandom& random, std::size_t rows,
                                           std::int32_t gridSteps) {
    std::vector<Entry> entries;
    entries.reserve(rows);
    const auto gridValue = [&]() {
        const std::int32_t step = random.nextInt(gridSteps + 1);
        return -2.0 + (4.0 * static_cast<double>(step) / static_cast<double>(gridSteps));
    };
    for (std::size_t row = 0; row < rows; ++row) {
        Entry entry;
        entry.biome = ResourceLocation("stratum", "b" + std::to_string(row % 37));
        const std::int32_t kind = random.nextInt(10);
        if (kind == 0 && !entries.empty()) {
            const auto earlier =
                static_cast<std::size_t>(random.nextInt(static_cast<std::int32_t>(entries.size())));
            entry.parameters = entries[earlier].parameters;
        } else if (kind == 1 && !entries.empty()) {
            const auto earlier =
                static_cast<std::size_t>(random.nextInt(static_cast<std::int32_t>(entries.size())));
            entry.parameters = entries[earlier].parameters;
            for (std::size_t axis = 0; axis < 6; ++axis) {
                Parameter& parameter = axisOf(entry, axis);
                const double width = parameter.max - parameter.min;
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
        const auto earlier =
            static_cast<std::size_t>(random.nextInt(static_cast<std::int32_t>(entries.size() - 1)));
        entries.back().parameters = entries[earlier].parameters;
    }
    return entries;
}

[[nodiscard]] std::vector<QuantizedPoint> pointsOf(const ParameterList& list) {
    std::vector<QuantizedPoint> points;
    points.reserve(list.size());
    for (const Entry& entry : list.entries()) {
        points.push_back(QuantizedPoint::of(entry.parameters));
    }
    return points;
}

/// Every hint worth trying for @p sample, the adversarial ones first.
[[nodiscard]] std::vector<std::size_t> hintsFor(const std::vector<QuantizedPoint>& points,
                                                const QuantizedSample& sample, std::size_t answer,
                                                JavaRandom& random) {
    std::vector<std::size_t> hints;
    const std::int64_t best = points[answer].fitness(sample);
    std::size_t worst = 0;
    std::int64_t worstFitness = points[0].fitness(sample);
    for (std::size_t row = 0; row < points.size(); ++row) {
        const std::int64_t fitness = points[row].fitness(sample);
        // Every row tied with the answer, earlier ones (which lose the tie)
        // and — when the answer is not the last of them, which the scan
        // makes impossible, so this would show a scan defect — later ones.
        if (row != answer && fitness == best) {
            hints.push_back(row);
        }
        if (fitness > worstFitness) {
            worstFitness = fitness;
            worst = row;
        }
    }
    hints.push_back(answer);
    hints.push_back(worst);
    if (answer > 0) {
        hints.push_back(answer - 1);
    }
    if (answer + 1 < points.size()) {
        hints.push_back(answer + 1);
    }
    hints.push_back(0);
    hints.push_back(points.size() - 1);
    hints.push_back(
        static_cast<std::size_t>(random.nextInt(static_cast<std::int32_t>(points.size()))));
    // Past the end: no hint at all.
    hints.push_back(points.size());
    hints.push_back(points.size() + 12345);
    hints.push_back(ParameterList::kNoHint);
    return hints;
}

struct Tally {
    std::size_t asked = 0;
    std::size_t mismatches = 0;
    /// Samples where some row other than the answer ties it.
    std::size_t ties = 0;
    std::string firstMismatch;
};

void compareHinted(const ParameterList& list, const std::vector<QuantizedPoint>& points,
                   const QuantizedSample& sample, JavaRandom& random, Tally& tally) {
    const std::size_t answer = list.findRowLinear(sample);
    const std::vector<std::size_t> hints = hintsFor(points, sample, answer, random);
    const std::int64_t best = points[answer].fitness(sample);
    tally.ties += std::ranges::any_of(hints,
                                      [&](std::size_t hint) {
                                          return hint < points.size() && hint != answer &&
                                                 points[hint].fitness(sample) == best;
                                      })
                      ? 1U
                      : 0U;
    for (const std::size_t hint : hints) {
        ++tally.asked;
        const std::size_t hinted = list.findRowFrom(sample, hint);
        if (hinted != answer) {
            if (tally.mismatches == 0) {
                tally.firstMismatch = "hint " + std::to_string(hint) + ": row " +
                                      std::to_string(hinted) + ", scan row " +
                                      std::to_string(answer);
            }
            ++tally.mismatches;
        }
    }
}

/// The sample mix biome_search_test.cpp queries with, each sample asked
/// under every hint hintsFor names.
void queryHard(const ParameterList& list, JavaRandom& random, std::size_t perKind, Tally& tally) {
    const std::vector<QuantizedPoint> points = pointsOf(list);
    std::array<std::vector<std::int64_t>, 6> bounds;
    for (const QuantizedPoint& point : points) {
        for (std::size_t axis = 0; axis < 6; ++axis) {
            bounds[axis].push_back(point.min[axis]);
            bounds[axis].push_back(point.max[axis]);
        }
    }
    const auto onBound = [&](std::size_t axis) {
        const std::vector<std::int64_t>& axisBounds = bounds[axis];
        const std::int64_t bound = axisBounds[static_cast<std::size_t>(
            random.nextInt(static_cast<std::int32_t>(axisBounds.size())))];
        return bound + random.nextInt(3) - 1;
    };
    for (std::size_t i = 0; i < perKind; ++i) {
        std::array<std::int64_t, 6> axes{};
        for (std::int64_t& value : axes) {
            value = uniform(random, -25000, 25000);
        }
        compareHinted(list, points, sampleOf(axes), random, tally);

        for (std::size_t axis = 0; axis < 6; ++axis) {
            axes[axis] = onBound(axis);
        }
        compareHinted(list, points, sampleOf(axes), random, tally);

        // Inside one row's box, which its duplicates and nested rows tie.
        const QuantizedPoint& row = points[static_cast<std::size_t>(
            random.nextInt(static_cast<std::int32_t>(points.size())))];
        for (std::size_t axis = 0; axis < 6; ++axis) {
            axes[axis] = uniform(random, row.min[axis], row.max[axis]);
        }
        compareHinted(list, points, sampleOf(axes), random, tally);

        // The extremes the search accepts, where a fitness passes 2^62.
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
        compareHinted(list, points, sampleOf(axes), random, tally);
    }
}

} // namespace

TEST_CASE("a warm-started search returns the scan's row under every hint",
          "[biome][parameters][search][hint]") {
    struct Shape {
        std::size_t rows;
        std::int32_t gridSteps;
    };

    for (const Shape shape : {Shape{1, 4}, Shape{2, 4}, Shape{9, 4}, Shape{40, 4}, Shape{200, 8},
                              Shape{1000, 16}, Shape{3000, 64}}) {
        CAPTURE(shape.rows, shape.gridSteps);
        JavaRandom random(static_cast<std::int64_t>(shape.rows * 104729) + shape.gridSteps);
        const ParameterList list =
            ParameterList::fromEntries(hardTable(random, shape.rows, shape.gridSteps), listId());
        REQUIRE(list.auditSearchTree().empty());
        Tally tally;
        queryHard(list, random, shape.rows < 100 ? 600 : 150, tally);
        CAPTURE(tally.firstMismatch, tally.asked, tally.ties);
        CHECK(tally.mismatches == 0U);
        if (shape.rows > 1) {
            // Hints at tied rows were among those asked, or the case proves
            // little about the tie rule.
            CHECK(tally.ties > 0U);
        }
    }
}

TEST_CASE("a hint at a tied row, earlier or later, gives the later row",
          "[biome][parameters][search][hint]") {
    // Five copies of one row at indices 1, 4, 6, 9 and 11 between filler
    // rows far away: inside the copies every sample ties all five at the
    // least fitness, and the scan keeps row 11. Every hint — each copy, each
    // filler, and none — must give 11. A rule that pruned every node whose
    // bound EQUALS the best (`>=` on bounds, rows not consulted) would stop
    // at the hinted copy; this case and the random tables both fail under
    // it, which was checked by making that change. (`<=` on rows instead of
    // `<` would be harmless: a node whose largest row IS the best holds no
    // later row.)
    Entry copy;
    copy.biome = ResourceLocation("stratum", "copy");
    copy.parameters.temperature = Parameter{.min = -0.25, .max = 0.25};
    copy.parameters.humidity = Parameter{.min = 0.0, .max = 0.5};
    copy.parameters.continentalness = Parameter{.min = -1.0, .max = 1.0};
    copy.parameters.erosion = Parameter{.min = 0.125, .max = 0.125};
    copy.parameters.depth = Parameter{.min = 0.0, .max = 1.0};
    copy.parameters.weirdness = Parameter{.min = -2.0, .max = 2.0};
    Entry filler;
    filler.biome = ResourceLocation("stratum", "filler");
    filler.parameters.temperature = Parameter{.min = 1.5, .max = 2.0};
    filler.parameters.humidity = Parameter{.min = 1.5, .max = 2.0};
    filler.parameters.continentalness = Parameter{.min = -1.0, .max = 1.0};
    filler.parameters.erosion = Parameter{.min = 1.5, .max = 2.0};
    filler.parameters.depth = Parameter{.min = 0.0, .max = 1.0};
    filler.parameters.weirdness = Parameter{.min = -2.0, .max = 2.0};
    std::vector<Entry> entries;
    for (std::size_t row = 0; row < 12; ++row) {
        const bool isCopy = row == 1 || row == 4 || row == 6 || row == 9 || row == 11;
        entries.push_back(isCopy ? copy : filler);
    }
    // And the same around a tree deep enough to have inner nodes: 400 more
    // fillers spread out, then a sixth copy LAST at row 412, so the tied
    // later row is a leaf away from the earlier ones.
    JavaRandom random(412);
    for (std::size_t row = 0; row < 400; ++row) {
        Entry spread = filler;
        const double shift = -1.75 + (0.25 * static_cast<double>(random.nextInt(8)));
        spread.parameters.temperature = Parameter{.min = 1.5, .max = 2.0};
        spread.parameters.weirdness = Parameter{.min = shift, .max = shift + 0.25};
        entries.push_back(spread);
    }
    for (const bool withLast : {false, true}) {
        CAPTURE(withLast);
        std::vector<Entry> table = entries;
        if (withLast) {
            table.push_back(copy);
        } else {
            table.resize(12);
        }
        const ParameterList list = ParameterList::fromEntries(table, listId());
        REQUIRE(list.auditSearchTree().empty());
        const std::size_t expected = withLast ? 412U : 11U;
        std::size_t wrong = 0;
        std::size_t asked = 0;
        for (std::size_t i = 0; i < 64; ++i) {
            const std::array<std::int64_t, 6> axes{
                uniform(random, -2500, 2500),   uniform(random, 0, 5000),
                uniform(random, -10000, 10000), 1250,
                uniform(random, 0, 10000),      uniform(random, -20000, 20000)};
            const QuantizedSample sample = sampleOf(axes);
            REQUIRE(list.findRowLinear(sample) == expected);
            for (std::size_t hint = 0; hint <= table.size(); ++hint) {
                ++asked;
                wrong += list.findRowFrom(sample, hint) == expected ? 0U : 1U;
            }
            wrong += list.findRowFrom(sample, ParameterList::kNoHint) == expected ? 0U : 1U;
        }
        CAPTURE(asked);
        CHECK(wrong == 0U);
    }
}

TEST_CASE("a thousand identical rows give the last whatever the hint",
          "[biome][parameters][search][hint]") {
    Entry entry;
    entry.parameters.temperature = Parameter{.min = -0.5, .max = 0.25};
    entry.parameters.humidity = Parameter{.min = 0.0, .max = 0.0};
    entry.parameters.continentalness = Parameter{.min = -1.0, .max = 1.0};
    entry.parameters.erosion = Parameter{.min = 0.375, .max = 0.375};
    entry.parameters.depth = Parameter{.min = 0.0, .max = 1.0};
    entry.parameters.weirdness = Parameter{.min = -2.0, .max = 2.0};
    entry.parameters.offset = 0.25;
    const ParameterList list =
        ParameterList::fromEntries(std::vector<Entry>(1000, entry), listId());
    JavaRandom random(1001);
    std::size_t wrong = 0;
    for (std::size_t i = 0; i < 200; ++i) {
        std::array<std::int64_t, 6> axes{};
        for (std::int64_t& value : axes) {
            value = uniform(random, -30000, 30000);
        }
        const QuantizedSample sample = sampleOf(axes);
        for (const std::size_t hint :
             {std::size_t{0}, std::size_t{1}, std::size_t{500}, std::size_t{998}, std::size_t{999},
              std::size_t{1000}, ParameterList::kNoHint}) {
            wrong += list.findRowFrom(sample, hint) == 999U ? 0U : 1U;
        }
    }
    CHECK(wrong == 0U);
}

TEST_CASE("every sample over a small table's bounds gives the scan's row from chained hints",
          "[biome][parameters][search][hint]") {
    // biome_search_test.cpp's exhaustive 9^6 sweep, each sample hinted with
    // the previous sample's answer — the way Source::fillChunk chains them —
    // and with the row scoring worst for it.
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
    const Entry later = entries[3];
    const Entry earlier = entries[7];
    entries.push_back(later);                 // tied everywhere with row 3, and later
    entries.insert(entries.begin(), earlier); // tied with the old row 7, and earlier
    const ParameterList list = ParameterList::fromEntries(entries, listId());
    REQUIRE(list.auditSearchTree().empty());
    const std::vector<QuantizedPoint> points = pointsOf(list);

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
    std::size_t previous = ParameterList::kNoHint;
    std::array<std::size_t, 6> digit{};
    while (true) {
        std::array<std::int64_t, 6> axes{};
        for (std::size_t axis = 0; axis < 6; ++axis) {
            axes[axis] = values[digit[axis]];
        }
        const QuantizedSample sample = sampleOf(axes);
        const std::size_t answer = list.findRowLinear(sample);
        std::size_t worst = 0;
        for (std::size_t row = 1; row < points.size(); ++row) {
            worst = points[row].fitness(sample) > points[worst].fitness(sample) ? row : worst;
        }
        const std::size_t chained = list.findRowFrom(sample, previous);
        mismatches += chained == answer ? 0U : 1U;
        mismatches += list.findRowFrom(sample, worst) == answer ? 0U : 1U;
        previous = chained;
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

TEST_CASE("a hint at the answer scores no more of the tree than no hint",
          "[biome][parameters][search][hint]") {
    // With the answer as the best from the start every pruning test is at
    // its strongest and the order of the walk is unchanged, so the nodes
    // scored are a subset of the unhinted search's and the rows the same
    // plus the hinted one. What the warm start is for, held as a property.
    JavaRandom random(77);
    const ParameterList list = ParameterList::fromEntries(hardTable(random, 2000, 32), listId());
    std::size_t worse = 0;
    SearchCounts hintedTotal;
    SearchCounts coldTotal;
    for (std::size_t i = 0; i < 2000; ++i) {
        std::array<std::int64_t, 6> axes{};
        for (std::int64_t& value : axes) {
            value = uniform(random, -22000, 22000);
        }
        const QuantizedSample sample = sampleOf(axes);
        SearchCounts cold;
        const std::size_t answer = list.findRow(sample, cold);
        SearchCounts hinted;
        REQUIRE(list.findRowFrom(sample, answer, hinted) == answer);
        worse += hinted.nodes <= cold.nodes && hinted.rows <= cold.rows + 1 ? 0U : 1U;
        hintedTotal.nodes += hinted.nodes;
        hintedTotal.rows += hinted.rows;
        coldTotal.nodes += cold.nodes;
        coldTotal.rows += cold.rows;
    }
    CAPTURE(hintedTotal.nodes, hintedTotal.rows, coldTotal.nodes, coldTotal.rows);
    CHECK(worse == 0U);
    CHECK(hintedTotal.nodes + hintedTotal.rows < coldTotal.nodes + coldTotal.rows);
}

TEST_CASE("a warm-started search refuses what the cold one refuses, whatever the hint",
          "[biome][parameters][search][hint]") {
    JavaRandom random(42);
    const ParameterList list = ParameterList::fromEntries(hardTable(random, 300, 8), listId());
    std::size_t mismatches = 0;
    std::size_t previous = ParameterList::kNoHint;
    for (std::size_t i = 0; i < 3000; ++i) {
        const ClimateSample sample{.temperature = random.nextGaussian(),
                                   .humidity = random.nextGaussian(),
                                   .continentalness = random.nextGaussian(),
                                   .erosion = random.nextGaussian(),
                                   .depth = random.nextGaussian(),
                                   .weirdness = random.nextGaussian()};
        const std::size_t row = list.findRowFrom(sample, previous);
        mismatches += row == list.findRowLinear(sample) ? 0U : 1U;
        previous = row;
    }
    CHECK(mismatches == 0U);

    const ClimateSample widest{.temperature = kMaxClimateSample,
                               .humidity = -kMaxClimateSample,
                               .continentalness = kMaxClimateSample,
                               .erosion = -kMaxClimateSample,
                               .depth = kMaxClimateSample,
                               .weirdness = -kMaxClimateSample};
    CHECK(list.findRowFrom(widest, 17) == list.findRowLinear(widest));

    // The refusals are findRow's, before the hint is looked at: a valid
    // hint does not let a sample through, nor an invalid one change the
    // message.
    ClimateSample tooWide;
    tooWide.depth = 2.0 * kMaxClimateSample;
    for (const std::size_t hint : {std::size_t{0}, std::size_t{299}, ParameterList::kNoHint}) {
        CHECK_THROWS_WITH(list.findRowFrom(tooWide, hint), ContainsSubstring("depth of 200000"));
        QuantizedSample quantizedTooWide;
        quantizedTooWide.erosion = kMaxQuantizedSample + 1;
        CHECK_THROWS_WITH(list.findRowFrom(quantizedTooWide, hint),
                          ContainsSubstring("erosion of 1000000001"));
        SearchCounts counts;
        CHECK_THROWS_WITH(list.findRowFrom(quantizedTooWide, hint, counts),
                          ContainsSubstring("erosion of 1000000001"));
    }

    const ParameterList empty = ParameterList::fromEntries({}, listId());
    CHECK_THROWS_AS(empty.findRowFrom(QuantizedSample{}, 0), ParameterError);
    CHECK_THROWS_AS(empty.findRowFrom(ClimateSample{}, ParameterList::kNoHint), ParameterError);

    const ParameterList one = ParameterList::fromEntries({list.entries()[0]}, listId());
    for (const std::size_t hint : {std::size_t{0}, std::size_t{1}, ParameterList::kNoHint}) {
        CHECK(one.findRowFrom(QuantizedSample{}, hint) == 0U);
    }
}

namespace {

/// The one named noise the synthetic router below reads.
[[nodiscard]] ResourceLocation wobble() {
    return ResourceLocation{"stratum", "wobble"};
}

/// `wobble` at the given scales, as a density function.
[[nodiscard]] nlohmann::json noiseAt(double xzScale, double yScale) {
    return {{"type", "minecraft:noise"},
            {"noise", wobble().toString()},
            {"xz_scale", xzScale},
            {"y_scale", yScale}};
}

/// A worldgen tree holding one settings entry, `minecraft:hinted`, whose
/// climate entries are @p router's and every other entry zero.
class SyntheticClimate {
public:
    explicit SyntheticClimate(const std::map<std::string, nlohmann::json>& climate)
        : path_(stratum::test::tempPath("stratum-biome-search-hint-test")) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_ / "density_function");
        std::filesystem::create_directories(path_ / "noise_settings");
        nlohmann::json router = nlohmann::json::object();
        for (std::size_t i = 0; i < stratum::settings::kRouterEntryCount; ++i) {
            router[std::string(stratum::settings::routerEntryName(
                static_cast<stratum::settings::RouterEntry>(i)))] = 0.0;
        }
        for (const auto& [name, function] : climate) {
            router[name] = function;
        }
        router["final_density"] = 1.0;
        const nlohmann::json settings = {
            {"default_block", {{"Name", "minecraft:stone"}}},
            {"default_fluid", {{"Name", "minecraft:water"}}},
            {"sea_level", 0},
            {"disable_mob_generation", true},
            {"aquifers_enabled", false},
            {"ore_veins_enabled", false},
            {"legacy_random_source", false},
            {"noise",
             {{"min_y", -64}, {"height", 384}, {"size_horizontal", 1}, {"size_vertical", 2}}},
            {"noise_router", router},
            {"spawn_target", nlohmann::json::array()},
            {"surface_rule",
             {{"type", "minecraft:block"}, {"result_state", {{"Name", "minecraft:stone"}}}}}};
        std::ofstream out(path_ / "noise_settings" / "hinted.json");
        out << settings.dump();
    }

    SyntheticClimate(const SyntheticClimate&) = delete;
    SyntheticClimate& operator=(const SyntheticClimate&) = delete;
    SyntheticClimate(SyntheticClimate&&) = delete;
    SyntheticClimate& operator=(SyntheticClimate&&) = delete;

    ~SyntheticClimate() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace

TEST_CASE("a multi-noise chunk's warm-started biomes are the ones `at` finds, row for row",
          "[biome][search][hint][world]") {
    // Two routers: one where every climate entry reads y as well as x and
    // z, so a column's answer in the layer below is often not this layer's
    // and the hints are mostly wrong; and one shaped like vanilla's, five
    // entries flat in y and depth a gradient plus a noise, where they are
    // mostly right. fillChunk's pointers are compared with `at`'s, and both
    // point into the list's own entries, so equal pointers are equal ROWS —
    // and each is held to the linear scan too.
    const std::map<std::string, nlohmann::json> everyAxis = {
        {"temperature", noiseAt(1.3, 2.1)}, {"vegetation", noiseAt(1.7, 0.9)},
        {"continents", noiseAt(1.1, 1.7)},  {"erosion", noiseAt(2.3, 1.9)},
        {"depth", noiseAt(0.5, 3.1)},       {"ridges", noiseAt(2.7, 0.5)}};
    const std::map<std::string, nlohmann::json> vanillaShaped = {
        {"temperature", noiseAt(1.3, 0.0)},
        {"vegetation", noiseAt(1.7, 0.0)},
        {"continents", noiseAt(1.1, 0.0)},
        {"erosion", noiseAt(2.3, 0.0)},
        {"depth",
         {{"type", "minecraft:add"},
          {"argument1",
           {{"type", "minecraft:y_clamped_gradient"},
            {"from_y", -64},
            {"to_y", 320},
            {"from_value", 1.5},
            {"to_value", -1.5}}},
          {"argument2", noiseAt(0.5, 0.0)}}},
        {"ridges", noiseAt(2.7, 0.0)}};

    JavaRandom tableRandom(4242);
    const ParameterList list =
        ParameterList::fromEntries(hardTable(tableRandom, 1500, 24), listId());
    REQUIRE(list.auditSearchTree().empty());
    const auto source = stratum::biome::Source::multiNoise(list);
    const auto settingsId = ResourceLocation::parse("minecraft:hinted");

    for (const auto* router : {&everyAxis, &vanillaShaped}) {
        CAPTURE(router == &everyAxis);
        const SyntheticClimate tree(*router);
        const stratum::settings::LoadedSettings loaded =
            stratum::settings::loadAll(stratum::data::Pack::open(tree.path()));
        const stratum::settings::NoiseSettings& settings = loaded.settings.at(settingsId);
        const std::vector<ResourceLocation> wanted{wobble()};
        const auto noises = stratum::density::NoiseRegistry::create(
            std::map<ResourceLocation, stratum::density::NoiseParameters>{
                {wobble(), stratum::density::NoiseParameters{.firstOctave = -3,
                                                             .amplitudes = {1.0, 1.0, 0.5}}}},
            wanted, 1234, stratum::density::RandomSource::Xoroshiro);
        const stratum::density::Interpreter interpreter(loaded.graph, noises);

        constexpr std::int32_t kMinQuartY = -16;
        constexpr std::size_t kLayers = 96;
        std::size_t differing = 0;
        std::size_t changed = 0;
        std::size_t quarts = 0;
        for (const auto& [chunkX, chunkZ] : std::array<std::pair<std::int32_t, std::int32_t>, 4>{
                 {{0, 0}, {-3, 2}, {5, -7}, {-11, -13}}}) {
            CAPTURE(chunkX, chunkZ);
            std::vector<const ResourceLocation*> biomes(16U * kLayers);
            stratum::density::Interpreter::CornerCache fillCache(interpreter.cacheSize());
            source.fillChunk(interpreter, settings.router, chunkX, chunkZ, kMinQuartY, biomes,
                             fillCache);
            stratum::density::Interpreter::CornerCache atCache(interpreter.cacheSize());
            std::size_t index = 0;
            for (std::size_t layer = 0; layer < kLayers; ++layer) {
                for (std::int32_t qz = 0; qz < 4; ++qz) {
                    for (std::int32_t qx = 0; qx < 4; ++qx) {
                        const stratum::density::Point corner{
                            .x = ((chunkX * 4) + qx) * 4,
                            .y = (kMinQuartY + static_cast<std::int32_t>(layer)) * 4,
                            .z = ((chunkZ * 4) + qz) * 4};
                        const ResourceLocation* atBiome =
                            &source.at(interpreter, settings.router, corner, atCache);
                        differing += biomes[index] == atBiome ? 0U : 1U;
                        // The same climate `at` read, scanned.
                        using stratum::settings::RouterEntry;
                        const auto sample = [&](RouterEntry entry) {
                            return interpreter.evaluate(settings.router.at(entry), corner, atCache);
                        };
                        const ClimateSample climate{.temperature = sample(RouterEntry::Temperature),
                                                    .humidity = sample(RouterEntry::Vegetation),
                                                    .continentalness =
                                                        sample(RouterEntry::Continents),
                                                    .erosion = sample(RouterEntry::Erosion),
                                                    .depth = sample(RouterEntry::Depth),
                                                    .weirdness = sample(RouterEntry::Ridges)};
                        differing +=
                            biomes[index] == &list.entries()[list.findRowLinear(climate)].biome
                                ? 0U
                                : 1U;
                        if (layer > 0) {
                            // How often the column's answer below, the hint
                            // this quart was given, was not its answer.
                            changed += biomes[index] == biomes[index - 16] ? 0U : 1U;
                        }
                        ++index;
                        ++quarts;
                    }
                }
            }
        }
        CAPTURE(quarts, changed);
        CHECK(differing == 0U);
        UNSCOPED_INFO("rows changed from the layer below at " << changed << " of " << quarts);
        // Biomes are pointers into entries, so a change of pointer is a
        // change of row: the y-reading router has to move rows from layer to
        // layer often, or it tests nothing the flat one does not.
        if (router == &everyAxis) {
            CHECK(changed * 4 > quarts);
        }
    }
}
