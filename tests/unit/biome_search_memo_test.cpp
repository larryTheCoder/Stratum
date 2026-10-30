// Stratum — the biome search through a SearchMemo, and fillChunk's two passes.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// ParameterList::findRowFrom's memo form keeps the depth-free part of every
// score it makes, keyed by the sample's five other axes, and adds the depth
// term to it for the next search over the same five. biome::Source::
// fillChunk now reads and checks every quart's climate first, in its old
// order, and searches column by column after, each column through one memo.
// What this holds, fixture-free:
//
//   * the memo form returns the linear scan's row (the definition) on hard
//     random tables — boxes sharing faces, point intervals, duplicates,
//     offsets, ties everywhere — over columns of samples that share five
//     axes, interleaved with samples that do not, with every kind of hint,
//     one memo for all of it; across the epoch's wrap, straight from the
//     epoch counting restarts at; and handed from one list to another, a
//     list built where another was destroyed among them;
//   * checkedSample refuses what findRow refuses, word for word, and what
//     it returns the memo form never refuses;
//   * fillChunk's first refusal is the one its old order met first —
//     layer by layer, z then x — not the first of a column-by-column walk,
//     on a synthetic dimension whose temperature leaves the search's range
//     at scattered quarts.
#include "support/temp_path.hpp"

#include <stratum/biome/parameter_list.hpp>
#include <stratum/biome/source.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_parameters.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/freeze/pipeline.hpp>
#include <stratum/rng/java_random.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/world/dimension.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using stratum::biome::ClimateSample;
using stratum::biome::Entry;
using stratum::biome::Parameter;
using stratum::biome::ParameterList;
using stratum::biome::QuantizedSample;
using stratum::biome::SearchMemo;
using stratum::data::ResourceLocation;
using stratum::rng::JavaRandom;

[[nodiscard]] ResourceLocation listId() {
    return ResourceLocation{"stratum", "search_memo_test"};
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

/// Bounds on a coarse grid, so boxes share faces and ties are everywhere;
/// point intervals; rows copied whole from earlier ones; offsets.
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
        if (random.nextInt(10) == 0 && !entries.empty()) {
            const auto earlier =
                static_cast<std::size_t>(random.nextInt(static_cast<std::int32_t>(entries.size())));
            entry.parameters = entries[earlier].parameters;
        } else {
            for (std::size_t axis = 0; axis < 6; ++axis) {
                double a = gridValue();
                double b = random.nextInt(4) == 0 ? a : gridValue();
                if (b < a) {
                    std::swap(a, b);
                }
                axisOf(entry, axis) = Parameter{.min = a, .max = b};
            }
        }
        if (random.nextInt(6) == 0) {
            entry.parameters.offset = 0.125 * static_cast<double>(random.nextInt(9));
        }
        entries.push_back(entry);
    }
    return entries;
}

/// A coordinate on the table's grid now and then, so that samples sit on
/// shared bounds and tie; otherwise anywhere across the table.
[[nodiscard]] std::int64_t coordinate(JavaRandom& random, std::int32_t gridSteps) {
    if (random.nextBoolean()) {
        const std::int32_t step = random.nextInt(gridSteps + 1);
        return -20000 + (40000 * std::int64_t{step} / gridSteps);
    }
    return static_cast<std::int64_t>(random.nextInt(50001)) - 25000;
}

struct Tally {
    std::size_t queries = 0;
    std::size_t columns = 0;
};

/// Columns of samples sharing every axis but depth, each searched with a
/// hint drawn from the previous answer, the answer one below, a random row
/// or none; where @p strays, every few a stray sample of other axes in
/// between. One memo.
void searchColumns(const ParameterList& list, JavaRandom& random, std::int32_t gridSteps,
                   SearchMemo& memo, std::size_t columns, Tally& tally, bool strays) {
    for (std::size_t c = 0; c < columns; ++c) {
        QuantizedSample sample{.temperature = coordinate(random, gridSteps),
                               .humidity = coordinate(random, gridSteps),
                               .continentalness = coordinate(random, gridSteps),
                               .erosion = coordinate(random, gridSteps),
                               .depth = 0,
                               .weirdness = coordinate(random, gridSteps)};
        std::size_t previous = ParameterList::kNoHint;
        for (int layer = 0; layer < 40; ++layer) {
            sample.depth = coordinate(random, gridSteps);
            const std::size_t expected = list.findRowLinear(sample);
            std::size_t hint = ParameterList::kNoHint;
            switch (random.nextInt(4)) {
                case 0:
                    hint = previous;
                    break;
                case 1:
                    hint = expected > 0 ? expected - 1 : expected;
                    break;
                case 2:
                    hint = static_cast<std::size_t>(
                        random.nextInt(static_cast<std::int32_t>(list.size())));
                    break;
                default:
                    break;
            }
            CAPTURE(c, layer, hint);
            REQUIRE(list.findRowFrom(sample, hint, memo) == expected);
            previous = expected;
            ++tally.queries;
            if (strays && random.nextInt(9) == 0) {
                // A stray: another column's five axes, through the same memo.
                const QuantizedSample stray{.temperature = coordinate(random, gridSteps),
                                            .humidity = coordinate(random, gridSteps),
                                            .continentalness = coordinate(random, gridSteps),
                                            .erosion = coordinate(random, gridSteps),
                                            .depth = coordinate(random, gridSteps),
                                            .weirdness = coordinate(random, gridSteps)};
                REQUIRE(list.findRowFrom(stray, ParameterList::kNoHint, memo) ==
                        list.findRowLinear(stray));
                ++tally.queries;
            }
        }
        ++tally.columns;
    }
}

} // namespace

TEST_CASE("the memo form of the biome search returns the scan's row", "[biome][search]") {
    Tally tally;
    for (const auto& [seed, rows, grid] : std::array<std::tuple<std::int64_t, std::size_t, int>, 4>{
             {{1, 40, 4}, {2, 600, 8}, {3, 3000, 16}, {4, 7593, 32}}}) {
        CAPTURE(seed, rows, grid);
        JavaRandom random(seed);
        const ParameterList list =
            ParameterList::fromEntries(hardTable(random, rows, grid), listId());
        SearchMemo memo;
        searchColumns(list, random, grid, memo, 60, tally, true);
    }
    CHECK(tally.columns == 240U);
    CHECK(tally.queries > std::size_t{240} * 40U);
}

TEST_CASE("a search memo forgets everything when its epoch wraps", "[biome][search]") {
    Tally tally;
    for (const std::int64_t seed : {77, 78, 79, 80}) {
        CAPTURE(seed);
        JavaRandom random(seed);
        const ParameterList list = ParameterList::fromEntries(hardTable(random, 2000, 8), listId());
        // Straight across the wrap from the first epoch. One column and
        // nothing else, so every slot it fills is stamped with epoch 1 — the
        // very epoch counting restarts from after the wrap. Then to the last
        // epoch, and straight across: the next column's searches meet
        // epoch-1 slots by the hundred, each another column's sum, which the
        // wrap must have forgotten.
        SearchMemo fresh;
        searchColumns(list, random, 8, fresh, 1, tally, false);
        fresh.advanceEpoch(std::numeric_limits<std::uint32_t>::max());
        searchColumns(list, random, 8, fresh, 6, tally, true);
        // A few epochs short of the wrap, after columns and strays, and on
        // across it.
        SearchMemo used;
        searchColumns(list, random, 8, used, 5, tally, true);
        used.advanceEpoch(std::numeric_limits<std::uint32_t>::max() - 2U);
        searchColumns(list, random, 8, used, 20, tally, true);
    }
    CHECK(tally.columns == std::size_t{4} * 32U);
}

namespace {

/// A column's five axes, its depth to be set by the caller.
[[nodiscard]] QuantizedSample columnOf(JavaRandom& random, std::int32_t gridSteps) {
    return QuantizedSample{.temperature = coordinate(random, gridSteps),
                           .humidity = coordinate(random, gridSteps),
                           .continentalness = coordinate(random, gridSteps),
                           .erosion = coordinate(random, gridSteps),
                           .depth = 0,
                           .weirdness = coordinate(random, gridSteps)};
}

/// @p depths depths of @p column, each searched through @p memo in every
/// list of @p lists in turn and held to that list's own scan.
void searchInTurn(std::initializer_list<const ParameterList*> lists, QuantizedSample column,
                  JavaRandom& random, std::int32_t gridSteps, SearchMemo& memo, int depths,
                  Tally& tally) {
    for (int layer = 0; layer < depths; ++layer) {
        column.depth = coordinate(random, gridSteps);
        std::size_t which = 0;
        for (const ParameterList* const list : lists) {
            CAPTURE(layer, which);
            REQUIRE(list->findRowFrom(column, ParameterList::kNoHint, memo) ==
                    list->findRowLinear(column));
            ++tally.queries;
            ++which;
        }
    }
    ++tally.columns;
}

} // namespace

TEST_CASE("a search memo handed to another list forgets what the first list's tree kept",
          "[biome][search]") {
    Tally tally;
    for (const std::int64_t seed : {11, 12, 13, 14, 15}) {
        CAPTURE(seed);
        JavaRandom random(seed);
        // Two lists of one shape, whose node and row ids name different
        // boxes: a sum kept for one is wrong for the other. One memo, every
        // depth of a column searched in the first and then the second, the
        // five other axes the same throughout.
        const ParameterList first =
            ParameterList::fromEntries(hardTable(random, 2000, 8), listId());
        const ParameterList second =
            ParameterList::fromEntries(hardTable(random, 2000, 8), listId());
        SearchMemo memo;
        for (int column = 0; column < 4; ++column) {
            searchInTurn({&first, &second}, columnOf(random, 8), random, 8, memo, 24, tally);
        }

        // A list destroyed and another built in its very place, a column
        // searched in each through one memo: the same address and the same
        // five axes, another tree, which no memo keyed by address could tell.
        std::optional<ParameterList> slot;
        slot.emplace(ParameterList::fromEntries(hardTable(random, 2000, 8), listId()));
        const ParameterList* const before = &*slot;
        const QuantizedSample column = columnOf(random, 8);
        searchInTurn({&*slot}, column, random, 8, memo, 12, tally);
        slot.emplace(ParameterList::fromEntries(hardTable(random, 2000, 8), listId()));
        CHECK(&*slot == before);
        searchInTurn({&*slot}, column, random, 8, memo, 12, tally);

        // A copy is the same tree, and searches alike through one memo.
        // NOLINTNEXTLINE(performance-unnecessary-copy-initialization): the copy is the point.
        const ParameterList copy = first;
        searchInTurn({&first, &copy}, columnOf(random, 8), random, 8, memo, 24, tally);
    }
    CHECK(tally.columns == std::size_t{5} * 7U);
}

TEST_CASE("checkedSample refuses what findRow refuses, in its words", "[biome][search]") {
    JavaRandom random(5);
    const ParameterList list = ParameterList::fromEntries(hardTable(random, 100, 4), listId());
    const auto refusalOf = [](const auto& call) -> std::string {
        try {
            static_cast<void>(call());
        } catch (const std::exception& error) {
            return error.what();
        }
        return {};
    };
    for (const double bad : {std::numeric_limits<double>::quiet_NaN(), 1.0e5 + 1.0, -2.0e9}) {
        for (std::size_t axis = 0; axis < 6; ++axis) {
            ClimateSample sample{.temperature = 0.1,
                                 .humidity = -0.2,
                                 .continentalness = 0.3,
                                 .erosion = 0.0,
                                 .depth = 0.5,
                                 .weirdness = -0.7};
            std::array<double*, 6> fields = {&sample.temperature,     &sample.humidity,
                                             &sample.continentalness, &sample.erosion,
                                             &sample.depth,           &sample.weirdness};
            *fields.at(axis) = bad;
            CAPTURE(bad, axis);
            const std::string expected = refusalOf([&] { return list.findRow(sample); });
            REQUIRE_FALSE(expected.empty());
            CHECK(refusalOf([&] { return list.checkedSample(sample); }) == expected);
            SearchMemo memo;
            CHECK(refusalOf([&] { return list.findRowFrom(sample, 0, memo); }) == expected);
        }
    }
    const ParameterList empty;
    CHECK(refusalOf([&] { return empty.checkedSample(ClimateSample{}); }) ==
          refusalOf([&] { return empty.findRow(ClimateSample{}); }));
    // The widest readings it lets through quantise inside what the
    // quantised forms accept, and search to the scan's row.
    for (const double edge : {1.0e5, -1.0e5, 99999.99999, -99999.99999}) {
        const ClimateSample sample{.temperature = edge,
                                   .humidity = edge,
                                   .continentalness = -edge,
                                   .erosion = edge,
                                   .depth = -edge,
                                   .weirdness = edge};
        CAPTURE(edge);
        const QuantizedSample checked = list.checkedSample(sample);
        CHECK(checked == QuantizedSample::of(sample));
        SearchMemo memo;
        CHECK(list.findRowFrom(checked, ParameterList::kNoHint, memo) ==
              list.findRowLinear(checked));
    }
}

namespace {

[[nodiscard]] ResourceLocation settingsId() {
    return ResourceLocation{"minecraft", "test"};
}

/// A multi-noise dimension whose temperature is a 3D noise times 2e5: past
/// the search's ±1e5 wherever the noise is past ±0.5, at quarts scattered
/// through every column and layer.
class ScatteredRefusals {
public:
    ScatteredRefusals() : path_(stratum::test::tempPath("stratum-biome-search-memo-test")) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_ / "density_function");
        std::filesystem::create_directories(path_ / "noise_settings");
        nlohmann::json router = nlohmann::json::object();
        for (std::size_t i = 0; i < stratum::settings::kRouterEntryCount; ++i) {
            router[std::string(stratum::settings::routerEntryName(
                static_cast<stratum::settings::RouterEntry>(i)))] = 0.0;
        }
        router["final_density"] = -1.0;
        router["temperature"] = {{"type", "minecraft:mul"},
                                 {"argument1",
                                  {{"type", "minecraft:noise"},
                                   {"noise", "stratum:wobble"},
                                   {"xz_scale", 4.0},
                                   {"y_scale", 4.0}}},
                                 {"argument2", 2.0e5}};
        const nlohmann::json settings = {
            {"default_block", {{"Name", "minecraft:stone"}}},
            {"default_fluid", {{"Name", "minecraft:water"}, {"Properties", {{"level", "0"}}}}},
            {"sea_level", -64},
            {"disable_mob_generation", true},
            {"aquifers_enabled", false},
            {"ore_veins_enabled", false},
            {"legacy_random_source", false},
            {"noise",
             {{"min_y", -64}, {"height", 128}, {"size_horizontal", 1}, {"size_vertical", 2}}},
            {"noise_router", router},
            {"spawn_target", nlohmann::json::array()},
            {"surface_rule",
             {{"type", "minecraft:sequence"}, {"sequence", nlohmann::json::array()}}}};
        std::ofstream out(path_ / "noise_settings" / "test.json");
        out << settings.dump();
    }

    ScatteredRefusals(const ScatteredRefusals&) = delete;
    ScatteredRefusals& operator=(const ScatteredRefusals&) = delete;
    ScatteredRefusals(ScatteredRefusals&&) = delete;
    ScatteredRefusals& operator=(ScatteredRefusals&&) = delete;

    ~ScatteredRefusals() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] stratum::freeze::Pipeline pipeline() const {
        stratum::settings::LoadedSettings loaded =
            stratum::settings::loadAll(stratum::data::Pack::open(path_));
        stratum::freeze::Pipeline pipeline;
        pipeline.graph = std::move(loaded.graph);
        pipeline.settings = std::move(loaded.settings);
        pipeline.noises.emplace(
            ResourceLocation{"stratum", "wobble"},
            stratum::density::NoiseParameters{.firstOctave = -2, .amplitudes = {1.0, 1.0}});
        JavaRandom random(9);
        pipeline.biomeParameters.emplace(
            listId(), ParameterList::fromEntries(hardTable(random, 50, 4), listId()));
        return pipeline;
    }

private:
    std::filesystem::path path_;
};

} // namespace

TEST_CASE("fillChunk's first refusal is the first its old order met, layer by layer",
          "[biome][world]") {
    const ScatteredRefusals world;
    const stratum::freeze::Pipeline pipeline = world.pipeline();
    const auto dimension = stratum::world::CompiledDimension::compile(
        world.pipeline(), settingsId(), stratum::world::BiomeSource::multiNoise(listId()), 31);
    // Source::at quart by quart over its own interpreter: the refusal the
    // old fillChunk met first (layer-major), and the one a column-major
    // walk would meet first.
    const stratum::settings::NoiseSettings& settings = pipeline.settings.at(settingsId());
    const std::vector<ResourceLocation> wanted{ResourceLocation{"stratum", "wobble"}};
    const auto noises = stratum::density::NoiseRegistry::create(
        pipeline.noises, wanted, 31, stratum::density::RandomSource::Xoroshiro);
    const stratum::density::Interpreter interpreter(
        pipeline.graph, noises,
        stratum::density::CellGeometry{.width = settings.geometry.cellWidth(),
                                       .height = settings.geometry.cellHeight()});
    const stratum::biome::Source source =
        stratum::biome::Source::multiNoise(pipeline.biomeParameters.at(listId()));
    constexpr std::int32_t kLayers = 32;
    const auto refusalAt = [&](std::int32_t chunkX, std::int32_t chunkZ, std::int32_t layer,
                               std::int32_t column) -> std::string {
        stratum::density::Interpreter::CornerCache cache(interpreter.cacheSize());
        try {
            static_cast<void>(
                source.at(interpreter, settings.router,
                          stratum::density::Point{.x = (chunkX * 16) + ((column % 4) * 4),
                                                  .y = (-16 + layer) * 4,
                                                  .z = (chunkZ * 16) + ((column / 4) * 4)},
                          cache));
        } catch (const std::exception& error) {
            return error.what();
        }
        return {};
    };
    std::size_t telling = 0;
    for (std::int32_t chunkX = -6; chunkX < 6; ++chunkX) {
        for (std::int32_t chunkZ = -6; chunkZ < 6; ++chunkZ) {
            std::string layerMajor;
            for (std::int32_t layer = 0; layer < kLayers && layerMajor.empty(); ++layer) {
                for (std::int32_t column = 0; column < 16 && layerMajor.empty(); ++column) {
                    layerMajor = refusalAt(chunkX, chunkZ, layer, column);
                }
            }
            std::string columnMajor;
            for (std::int32_t column = 0; column < 16 && columnMajor.empty(); ++column) {
                for (std::int32_t layer = 0; layer < kLayers && columnMajor.empty(); ++layer) {
                    columnMajor = refusalAt(chunkX, chunkZ, layer, column);
                }
            }
            std::vector<const ResourceLocation*> biomes(std::size_t{16} * kLayers);
            std::string refused;
            try {
                dimension->fillBiomes(chunkX, chunkZ, biomes);
            } catch (const std::exception& error) {
                refused = error.what();
            }
            CAPTURE(chunkX, chunkZ, layerMajor, columnMajor);
            CHECK(refused == layerMajor);
            telling += layerMajor != columnMajor ? 1U : 0U;
        }
    }
    // Not vacuous: on some chunks the two orders meet different refusals.
    CHECK(telling > 0U);
}
