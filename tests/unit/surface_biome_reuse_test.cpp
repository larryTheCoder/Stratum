// Stratum — the surface pass's biome lookups, handed on to fillBiomes.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// A fill's surface pass looks up the biome of every quart it paints, and the
// caller then asks CompiledDimension::fillBiomes for the chunk's biomes —
// the same quarts among them. The buffer form of fillBiomes takes those from
// the buffer (terrain::ChunkFiller::surfaceBiomes) instead of looking them up
// again. What this holds, fixture-free, on a synthetic multi-noise dimension
// whose biome changes across x, z and y:
//
//   * the buffer form stores exactly what the form without a buffer stores,
//     over the noise height, PocketMine-MP's taller column and a part of one;
//   * the rows a fill leaves are each the row biome::Source::rowAt answers at
//     that quart, through an interpreter of its own, and there are some;
//   * a buffer another dimension, another filler or another chunk filled
//     hands nothing on — a world of another seed would otherwise lend its
//     biomes — and a copy of the filler is the filler;
//   * a refusal is the one the form without a buffer raises, at the same
//     quart, whether the fill reached it first or only fillBiomes does;
//   * biome::Source's own pieces: rowAt is `at` as a row, and a known row
//     past the list is refused.
#include "support/temp_path.hpp"

#include <stratum/biome/parameter_list.hpp>
#include <stratum/biome/source.hpp>
#include <stratum/biome/temperature_table.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_parameters.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/freeze/pipeline.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/rule_graph.hpp>
#include <stratum/terrain/filler.hpp>
#include <stratum/world/dimension.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using stratum::biome::Entry;
using stratum::biome::Parameter;
using stratum::biome::ParameterList;
using stratum::biome::Source;
using stratum::data::ResourceLocation;
using stratum::world::BiomeSource;
using stratum::world::CompiledDimension;

namespace {

constexpr std::int32_t kMinY = -64;
constexpr std::int32_t kHeight = 128;
constexpr std::int32_t kNoiseLayers = kHeight / 4;
constexpr std::int32_t kMinQuartY = kMinY / 4;
/// A quart layer's sixteen quarts, and the noise height's quarts.
constexpr std::size_t kLayerQuarts = 16;
constexpr std::size_t kNoiseQuarts = kLayerQuarts * static_cast<std::size_t>(kNoiseLayers);

[[nodiscard]] ResourceLocation settingsId() {
    return ResourceLocation{"minecraft", "test"};
}

[[nodiscard]] ResourceLocation listId() {
    return ResourceLocation{"stratum", "reuse"};
}

[[nodiscard]] ResourceLocation wobble() {
    return ResourceLocation{"stratum", "wobble"};
}

/// A 2D noise over `stratum:wobble`, its x and z scaled by @p xzScale.
[[nodiscard]] nlohmann::json flatNoise(const double xzScale) {
    return {{"type", "minecraft:noise"},
            {"noise", "stratum:wobble"},
            {"xz_scale", xzScale},
            {"y_scale", 0.0}};
}

/// The nine biomes of the list: three temperature bands by three depths.
constexpr std::array<const char*, 9> kBiomes = {
    "minecraft:snowy_plains", "minecraft:plains",      "minecraft:desert",
    "minecraft:ice_spikes",   "minecraft:forest",      "minecraft:badlands",
    "minecraft:frozen_peaks", "minecraft:dark_forest", "minecraft:savanna"};

/// The list: temperature split at -0.15 and 0.15, depth a point at 0, 0.5
/// and 1; the other four axes open across the schema.
[[nodiscard]] ParameterList reuseList() {
    constexpr std::array<Parameter, 3> kTemperatures = {
        {{.min = -2.0, .max = -0.15}, {.min = -0.15, .max = 0.15}, {.min = 0.15, .max = 2.0}}};
    constexpr std::array<double, 3> kDepths = {0.0, 0.5, 1.0};
    const Parameter open{.min = -2.0, .max = 2.0};
    std::vector<Entry> entries;
    for (std::size_t d = 0; d < kDepths.size(); ++d) {
        for (std::size_t t = 0; t < kTemperatures.size(); ++t) {
            Entry entry;
            entry.parameters.temperature = kTemperatures.at(t);
            entry.parameters.humidity = open;
            entry.parameters.continentalness = open;
            entry.parameters.erosion = open;
            entry.parameters.depth = Parameter{.min = kDepths.at(d), .max = kDepths.at(d)};
            entry.parameters.weirdness = open;
            entry.biome = ResourceLocation::parse(kBiomes.at((d * 3) + t));
            entries.push_back(entry);
        }
    }
    return ParameterList::fromEntries(std::move(entries), listId());
}

/// A dimension y -64..63 whose ground rolls about y 0 under a 2D noise, its
/// temperature another 2D noise (or @p temperature, where a case asks for
/// one) fast enough across x and z to change the biome from one quart to
/// the next, so that a row kept at the wrong quart of a layer shows, its
/// depth falling from 1.5 at the floor to -1.5 at the top, and a
/// surface rule that paints four of the nine biomes, so the pass looks the
/// biome up on every block of stone.
class SyntheticWorld {
public:
    explicit SyntheticWorld(const std::optional<nlohmann::json>& temperature = std::nullopt)
        : path_(stratum::test::tempPath("stratum-surface-biome-reuse-test")) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_ / "density_function");
        std::filesystem::create_directories(path_ / "noise_settings");
        nlohmann::json router = nlohmann::json::object();
        for (std::size_t i = 0; i < stratum::settings::kRouterEntryCount; ++i) {
            router[std::string(stratum::settings::routerEntryName(
                static_cast<stratum::settings::RouterEntry>(i)))] = 0.0;
        }
        router["final_density"] = {{"type", "minecraft:add"},
                                   {"argument1",
                                    {{"type", "minecraft:y_clamped_gradient"},
                                     {"from_y", kMinY},
                                     {"to_y", kMinY + kHeight},
                                     {"from_value", 1.0},
                                     {"to_value", -1.0}}},
                                   {"argument2", flatNoise(1.0)}};
        router["temperature"] = temperature.value_or(flatNoise(4.0));
        router["depth"] = {{"type", "minecraft:y_clamped_gradient"},
                           {"from_y", kMinY},
                           {"to_y", kMinY + kHeight},
                           {"from_value", 1.5},
                           {"to_value", -1.5}};
        nlohmann::json rules = nlohmann::json::array();
        const std::array<std::pair<const char*, const char*>, 4> painted = {{
            {"minecraft:snowy_plains", "minecraft:snow_block"},
            {"minecraft:desert", "minecraft:sandstone"},
            {"minecraft:forest", "minecraft:moss_block"},
            {"minecraft:savanna", "minecraft:terracotta"},
        }};
        for (const auto& [biome, block] : painted) {
            rules.push_back(
                {{"type", "minecraft:condition"},
                 {"if_true", {{"type", "minecraft:biome"}, {"biome_is", {biome}}}},
                 {"then_run", {{"type", "minecraft:block"}, {"result_state", {{"Name", block}}}}}});
        }
        const nlohmann::json settings = {
            {"default_block", {{"Name", "minecraft:stone"}}},
            {"default_fluid", {{"Name", "minecraft:water"}, {"Properties", {{"level", "0"}}}}},
            {"sea_level", kMinY},
            {"disable_mob_generation", true},
            {"aquifers_enabled", false},
            {"ore_veins_enabled", false},
            {"legacy_random_source", false},
            {"noise",
             {{"min_y", kMinY}, {"height", kHeight}, {"size_horizontal", 1}, {"size_vertical", 2}}},
            {"noise_router", router},
            {"spawn_target", nlohmann::json::array()},
            {"surface_rule", {{"type", "minecraft:sequence"}, {"sequence", rules}}}};
        std::ofstream out(path_ / "noise_settings" / "test.json");
        out << settings.dump();
    }

    SyntheticWorld(const SyntheticWorld&) = delete;
    SyntheticWorld& operator=(const SyntheticWorld&) = delete;
    SyntheticWorld(SyntheticWorld&&) = delete;
    SyntheticWorld& operator=(SyntheticWorld&&) = delete;

    ~SyntheticWorld() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] stratum::freeze::Pipeline pipeline() const {
        stratum::settings::LoadedSettings loaded =
            stratum::settings::loadAll(stratum::data::Pack::open(path_));
        stratum::freeze::Pipeline pipeline;
        pipeline.graph = std::move(loaded.graph);
        pipeline.settings = std::move(loaded.settings);
        pipeline.noises.emplace(wobble(), stratum::density::NoiseParameters{
                                              .firstOctave = -5, .amplitudes = {1.0, 0.5, 0.25}});
        pipeline.biomeParameters.emplace(listId(), reuseList());
        return pipeline;
    }

    [[nodiscard]] std::unique_ptr<CompiledDimension> compile(const std::int64_t seed) const {
        return CompiledDimension::compile(pipeline(), settingsId(),
                                          BiomeSource::multiNoise(listId()), seed);
    }

private:
    std::filesystem::path path_;
};

/// The pieces a ChunkFiller of the synthetic world is compiled from, owned
/// together, the filler last (it points into the rest).
struct Parts {
    stratum::freeze::Pipeline pipeline;
    const stratum::settings::NoiseSettings& settings;
    stratum::surface::RuleGraph rules;
    std::vector<ResourceLocation> wanted{wobble()};
    stratum::density::NoiseRegistry noises;
    Source source;
    stratum::density::Interpreter interpreter;

    Parts(stratum::freeze::Pipeline frozen, const std::int64_t seed)
        : pipeline(std::move(frozen)), settings(pipeline.settings.at(settingsId())),
          rules(stratum::surface::RuleGraph::resolve(settings.surfaceRule, settingsId())),
          noises(stratum::density::NoiseRegistry::create(
              pipeline.noises, wanted, seed, stratum::density::RandomSource::Xoroshiro)),
          source(Source::multiNoise(pipeline.biomeParameters.at(listId()))),
          interpreter(pipeline.graph, noises,
                      stratum::density::CellGeometry{.width = settings.geometry.cellWidth(),
                                                     .height = settings.geometry.cellHeight()}) {}

    [[nodiscard]] stratum::terrain::ChunkFiller filler() const {
        return stratum::terrain::ChunkFiller::compile(pipeline.graph, noises, settings, &rules,
                                                      source, &pipeline.biomeTemperatures);
    }
};

/// Both forms' biomes for @p layers layers from @p minQuartY, compared.
[[nodiscard]] std::size_t differences(const CompiledDimension& dimension, std::int32_t chunkX,
                                      std::int32_t chunkZ, std::int32_t minQuartY,
                                      std::size_t layers,
                                      const stratum::terrain::ChunkBuffer& filled) {
    std::vector<const ResourceLocation*> plain(kLayerQuarts * layers);
    std::vector<const ResourceLocation*> handed(kLayerQuarts * layers);
    dimension.fillBiomes(chunkX, chunkZ, minQuartY, plain);
    dimension.fillBiomes(chunkX, chunkZ, minQuartY, handed, filled);
    std::size_t differing = 0;
    for (std::size_t i = 0; i < plain.size(); ++i) {
        differing += plain[i] == handed[i] ? 0U : 1U;
    }
    return differing;
}

/// What @p call throws, as text; empty when it returns.
template<typename Call>
[[nodiscard]] std::string refusalOf(Call&& call) {
    try {
        call();
    } catch (const std::exception& error) {
        return error.what();
    }
    return {};
}

constexpr std::array<std::pair<std::int32_t, std::int32_t>, 5> kChunks = {
    {{0, 0}, {-3, 5}, {7, -2}, {-1, -1}, {40, -33}}};

} // namespace

TEST_CASE("fillBiomes handed the filled buffer stores what it stores without one",
          "[biome][world][surface]") {
    const SyntheticWorld world;
    const std::unique_ptr<CompiledDimension> dimension = world.compile(20261030);
    std::size_t distinct = 0;
    for (const auto& [chunkX, chunkZ] : kChunks) {
        CAPTURE(chunkX, chunkZ);
        stratum::terrain::ChunkBuffer buffer(dimension->geometry());
        dimension->fillBlocks(chunkX, chunkZ, buffer);
        // The noise height, PocketMine-MP's column (y -64..319, most of it
        // above the noise height) and layers part in and part out of it.
        CHECK(differences(*dimension, chunkX, chunkZ, kMinQuartY, kNoiseLayers, buffer) == 0U);
        CHECK(differences(*dimension, chunkX, chunkZ, kMinQuartY, 96U, buffer) == 0U);
        CHECK(differences(*dimension, chunkX, chunkZ, 4, 20U, buffer) == 0U);
        CHECK(differences(*dimension, chunkX, chunkZ, kMinQuartY - 8, 12U, buffer) == 0U);

        std::vector<const ResourceLocation*> plain(kNoiseQuarts);
        std::vector<const ResourceLocation*> handed(kNoiseQuarts);
        dimension->fillBiomes(chunkX, chunkZ, plain);
        dimension->fillBiomes(chunkX, chunkZ, handed, buffer);
        CHECK(plain == handed);
        for (const ResourceLocation* biome : plain) {
            distinct += biome != plain.front() ? 1U : 0U;
        }
    }
    // Not vacuous: the biome moves within the chunks, so a quart taken from
    // the wrong place would show.
    CHECK(distinct > 0U);

    // The noise-height form still insists on the noise height.
    stratum::terrain::ChunkBuffer buffer(dimension->geometry());
    dimension->fillBlocks(0, 0, buffer);
    std::vector<const ResourceLocation*> wrong(16U);
    CHECK_THROWS_WITH(dimension->fillBiomes(0, 0, wrong, buffer),
                      ContainsSubstring("for a dimension that needs 512"));
    std::vector<const ResourceLocation*> partial(17U);
    CHECK_THROWS_WITH(dimension->fillBiomes(0, 0, 0, partial, buffer),
                      ContainsSubstring("not a whole number of 16-quart layers"));
}

TEST_CASE("the rows a fill leaves are the rows its biome source answers at each quart",
          "[biome][terrain][surface]") {
    const SyntheticWorld world;
    const Parts parts(world.pipeline(), 20261030);
    const stratum::terrain::ChunkFiller filler = parts.filler();
    REQUIRE(filler.runsSurfaceRules());
    const ParameterList& list = parts.pipeline.biomeParameters.at(listId());
    std::size_t known = 0;
    std::size_t unknown = 0;
    // Known quarts whose row is not the row of the quart across the layer's
    // diagonal (qz, qx): a row kept with x and z swapped would show there.
    std::size_t asymmetric = 0;
    for (const auto& [chunkX, chunkZ] : kChunks) {
        CAPTURE(chunkX, chunkZ);
        stratum::terrain::ChunkBuffer buffer(parts.settings.geometry);
        filler.fill(chunkX, chunkZ, buffer);
        const Source::KnownRows rows = filler.surfaceBiomes(buffer, chunkX, chunkZ);
        CHECK(rows.minQuartY == kMinQuartY);
        REQUIRE(rows.rows.size() == kNoiseQuarts);
        stratum::density::Interpreter::CornerCache cache(parts.interpreter.cacheSize());
        for (std::int32_t layer = 0; layer < kNoiseLayers; ++layer) {
            for (std::int32_t qz = 0; qz < 4; ++qz) {
                for (std::int32_t qx = 0; qx < 4; ++qx) {
                    const std::uint32_t row = rows.rows[(static_cast<std::size_t>(layer) * 16U) +
                                                        static_cast<std::size_t>((qz * 4) + qx)];
                    if (row == Source::kUnknownRow) {
                        ++unknown;
                        continue;
                    }
                    ++known;
                    const stratum::density::Point corner{.x = (chunkX * 16) + (qx * 4),
                                                         .y = (kMinQuartY + layer) * 4,
                                                         .z = (chunkZ * 16) + (qz * 4)};
                    const std::size_t expected =
                        parts.source.rowAt(parts.interpreter, parts.settings.router, corner, cache,
                                           ParameterList::kNoHint);
                    CHECK(row == expected);
                    CHECK(&list.entries()[row].biome == &parts.source.at(parts.interpreter,
                                                                         parts.settings.router,
                                                                         corner, cache));
                    const stratum::density::Point across{.x = (chunkX * 16) + (qz * 4),
                                                         .y = corner.y,
                                                         .z = (chunkZ * 16) + (qx * 4)};
                    const std::size_t transposed =
                        parts.source.rowAt(parts.interpreter, parts.settings.router, across, cache,
                                           ParameterList::kNoHint);
                    asymmetric += expected != transposed ? 1U : 0U;
                }
            }
        }
    }
    // The pass paints the stone below the rolling ground and never the air
    // above it, so some of each.
    CHECK(known > 0U);
    CHECK(unknown > 0U);
    // Not vacuous across x and z: the biome differs across a layer's
    // diagonal at known quarts, so a row kept at the transposed quart fails.
    CHECK(asymmetric > 0U);
}

TEST_CASE("only the filler that filled a buffer, for its own chunk, hands its rows on",
          "[biome][terrain][world][surface]") {
    const SyntheticWorld world;
    const Parts parts(world.pipeline(), 7);
    const stratum::terrain::ChunkFiller filler = parts.filler();
    stratum::terrain::ChunkBuffer buffer(parts.settings.geometry);
    filler.fill(-3, 5, buffer);
    CHECK_FALSE(filler.surfaceBiomes(buffer, -3, 5).rows.empty());
    // Another chunk's coordinates.
    CHECK(filler.surfaceBiomes(buffer, 5, -3).rows.empty());
    CHECK(filler.surfaceBiomes(buffer, -3, 6).rows.empty());
    // Another filler — even one compiled from the very same parts.
    const stratum::terrain::ChunkFiller twin = parts.filler();
    CHECK(twin.surfaceBiomes(buffer, -3, 5).rows.empty());
    // A copy is the same filler, and fills alike: what it fills the
    // original hands on.
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization): the copy is the point.
    const stratum::terrain::ChunkFiller copy = filler;
    CHECK_FALSE(copy.surfaceBiomes(buffer, -3, 5).rows.empty());
    stratum::terrain::ChunkBuffer byCopy(parts.settings.geometry);
    copy.fill(7, -2, byCopy);
    CHECK_FALSE(filler.surfaceBiomes(byCopy, 7, -2).rows.empty());
    // A buffer nothing filled.
    const stratum::terrain::ChunkBuffer fresh(parts.settings.geometry);
    CHECK(filler.surfaceBiomes(fresh, -3, 5).rows.empty());

    // Through CompiledDimension: a buffer another seed's dimension filled
    // with this very chunk lends it nothing — its biomes are another
    // world's, and the list is the same object type, so taking them would
    // go unnoticed but for this.
    const std::unique_ptr<CompiledDimension> one = world.compile(1);
    const std::unique_ptr<CompiledDimension> two = world.compile(2);
    std::size_t worldsDiffer = 0;
    for (const auto& [chunkX, chunkZ] : kChunks) {
        CAPTURE(chunkX, chunkZ);
        stratum::terrain::ChunkBuffer filledByTwo(two->geometry());
        two->fillBlocks(chunkX, chunkZ, filledByTwo);
        std::vector<const ResourceLocation*> plain(kNoiseQuarts);
        std::vector<const ResourceLocation*> handed(kNoiseQuarts);
        std::vector<const ResourceLocation*> others(kNoiseQuarts);
        one->fillBiomes(chunkX, chunkZ, plain);
        one->fillBiomes(chunkX, chunkZ, handed, filledByTwo);
        two->fillBiomes(chunkX, chunkZ, others, filledByTwo);
        CHECK(plain == handed);
        worldsDiffer += plain == others ? 0U : 1U;
        // And a buffer filled with another chunk lends nothing either.
        stratum::terrain::ChunkBuffer elsewhere(one->geometry());
        one->fillBlocks(chunkX + 1, chunkZ, elsewhere);
        one->fillBiomes(chunkX, chunkZ, handed, elsewhere);
        CHECK(plain == handed);
    }
    // Not vacuous: the two worlds' biomes differ somewhere.
    CHECK(worldsDiffer > 0U);
}

TEST_CASE("a refusal is the one fillBiomes raises without the buffer, at the same quart",
          "[biome][world][surface]") {
    // A temperature past the search's ±1e5 is refused by name, with its
    // value — which a gradient makes different at every y, so the message
    // says WHICH quart refused first.
    SECTION("above the ground, which only fillBiomes reaches") {
        // From y 100 up, past the noise height: the fill never looks there.
        const SyntheticWorld world(nlohmann::json{{"type", "minecraft:y_clamped_gradient"},
                                                  {"from_y", 99},
                                                  {"to_y", 400},
                                                  {"from_value", 0.0},
                                                  {"to_value", 3.0e7}});
        const std::unique_ptr<CompiledDimension> dimension = world.compile(3);
        for (const auto& [chunkX, chunkZ] : kChunks) {
            CAPTURE(chunkX, chunkZ);
            stratum::terrain::ChunkBuffer buffer(dimension->geometry());
            dimension->fillBlocks(chunkX, chunkZ, buffer);
            std::vector<const ResourceLocation*> column(kLayerQuarts * 96U);
            const std::string plain =
                refusalOf([&] { dimension->fillBiomes(chunkX, chunkZ, kMinQuartY, column); });
            const std::string handed = refusalOf(
                [&] { dimension->fillBiomes(chunkX, chunkZ, kMinQuartY, column, buffer); });
            CHECK_THAT(plain, ContainsSubstring("temperature"));
            CHECK(handed == plain);
        }
    }
    SECTION("inside the ground, which the fill reaches first") {
        // From y -40 up, below the ground: the surface pass refuses, part
        // way through its quarts, and a caller that goes on to fillBiomes
        // with what the refused fill left meets the same refusal.
        const SyntheticWorld world(nlohmann::json{{"type", "minecraft:y_clamped_gradient"},
                                                  {"from_y", -41},
                                                  {"to_y", 400},
                                                  {"from_value", 0.0},
                                                  {"to_value", 3.0e7}});
        const std::unique_ptr<CompiledDimension> dimension = world.compile(3);
        for (const auto& [chunkX, chunkZ] : kChunks) {
            CAPTURE(chunkX, chunkZ);
            stratum::terrain::ChunkBuffer buffer(dimension->geometry());
            CHECK_THAT(refusalOf([&] { dimension->fillBlocks(chunkX, chunkZ, buffer); }),
                       ContainsSubstring("temperature"));
            std::vector<const ResourceLocation*> column(kNoiseQuarts);
            const std::string plain =
                refusalOf([&] { dimension->fillBiomes(chunkX, chunkZ, column); });
            const std::string handed =
                refusalOf([&] { dimension->fillBiomes(chunkX, chunkZ, column, buffer); });
            CHECK_THAT(plain, ContainsSubstring("temperature"));
            CHECK(handed == plain);
        }
    }
}

TEST_CASE("a multi-noise source's row is its biome, and a known row must be one", "[biome]") {
    const SyntheticWorld world;
    const Parts parts(world.pipeline(), 11);
    const ParameterList& list = parts.pipeline.biomeParameters.at(listId());
    CHECK(parts.source.parameters() == &list);
    stratum::density::Interpreter::CornerCache cache(parts.interpreter.cacheSize());
    for (const std::int32_t y : {-64, -12, 0, 8, 60, 200}) {
        for (const std::size_t hint : {ParameterList::kNoHint, std::size_t{0}, std::size_t{8}}) {
            CAPTURE(y, hint);
            const stratum::density::Point corner{.x = 36, .y = y, .z = -20};
            const std::size_t row =
                parts.source.rowAt(parts.interpreter, parts.settings.router, corner, cache, hint);
            CHECK(&list.entries()[row].biome ==
                  &parts.source.at(parts.interpreter, parts.settings.router, corner, cache));
        }
    }
    // The End's source has no rows.
    const Source end = Source::theEnd();
    CHECK(end.parameters() == nullptr);
    CHECK_THROWS_AS(end.rowAt(parts.interpreter, parts.settings.router, stratum::density::Point{},
                              cache, ParameterList::kNoHint),
                    std::logic_error);

    // Known rows: taken where they are rows, refused where they are not.
    std::vector<const ResourceLocation*> plain(kLayerQuarts * 4U);
    parts.source.fillChunk(parts.interpreter, parts.settings.router, 2, 3, -2, plain, cache);
    std::vector<std::uint32_t> rows(kLayerQuarts * 2U, Source::kUnknownRow);
    rows[5] = static_cast<std::uint32_t>(parts.source.rowAt(
        parts.interpreter, parts.settings.router,
        stratum::density::Point{.x = 36, .y = -4, .z = 52}, cache, ParameterList::kNoHint));
    std::vector<const ResourceLocation*> handed(kLayerQuarts * 4U);
    parts.source.fillChunk(parts.interpreter, parts.settings.router, 2, 3, -2, handed, cache,
                           Source::KnownRows{.minQuartY = -1, .rows = rows});
    CHECK(plain == handed);
    rows[7] = static_cast<std::uint32_t>(list.size());
    CHECK_THROWS_AS(parts.source.fillChunk(parts.interpreter, parts.settings.router, 2, 3, -2,
                                           handed, cache,
                                           Source::KnownRows{.minQuartY = -1, .rows = rows}),
                    std::out_of_range);
}
