// Stratum — the chunk filler.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#include <stratum/biome/parameter_list.hpp>
#include <stratum/biome/temperature_table.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/executor.hpp>
#include <stratum/surface/rule_graph.hpp>
#include <stratum/terrain/filler.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using stratum::biome::ParameterList;
using stratum::biome::TemperatureTable;
using stratum::data::Pack;
using stratum::settings::LoadedSettings;
using stratum::settings::RouterEntry;
using stratum::surface::RuleGraph;
using stratum::terrain::ChunkBuffer;
using stratum::terrain::ChunkFiller;
using stratum::terrain::FillError;

namespace {

class TempTree {
public:
    TempTree() : path_(std::filesystem::temp_directory_path() / uniqueName()) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_ / "density_function");
        std::filesystem::create_directories(path_ / "noise_settings");
    }

    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;

    ~TempTree() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const TempTree& defineSettings(std::string_view name, const nlohmann::json& json) const {
        std::ofstream out(path_ / "noise_settings" / (std::string(name) + ".json"));
        out << json.dump();
        return *this;
    }

    /// Writes `worldgen/noise/clay_bands_offset` — `bandlands`' one
    /// registered noise (spec/bandlands-spec.md Q4.3) — so a filler that
    /// uses bandlands has something to build a NoiseRegistry from.
    const TempTree& defineClayBandsOffsetNoise() const {
        std::filesystem::create_directories(path_ / "noise");
        std::ofstream out(path_ / "noise" / "clay_bands_offset.json");
        out << R"({"firstOctave": -8, "amplitudes": [1.0]})";
        return *this;
    }

    /// Writes `worldgen/biome/<name>.json` declaring only `temperature`, so
    /// a `biome::TemperatureTable` built from this tree's own Pack has
    /// something to read back for `minecraft:temperature`.
    const TempTree& defineBiomeTemperature(std::string_view name, double temperature) const {
        std::filesystem::create_directories(path_ / "biome");
        std::ofstream out(path_ / "biome" / (std::string(name) + ".json"));
        out << nlohmann::json{{"temperature", temperature}}.dump();
        return *this;
    }

    [[nodiscard]] LoadedSettings load() const {
        return stratum::settings::loadAll(Pack::open(path_));
    }

    [[nodiscard]] Pack pack() const { return Pack::open(path_); }

private:
    [[nodiscard]] static std::string uniqueName() {
        static int counter = 0;
        return "stratum-filler-test-" + std::to_string(++counter);
    }

    std::filesystem::path path_;
};

/// A dimension whose final_density is a plain y gradient: positive below y=0
/// and negative above it, so the terrain is a flat plane and every block is
/// predictable without evaluating anything by hand.
[[nodiscard]] nlohmann::json flatSettings(bool aquifers, bool oreVeins, double floodedness = 0.0) {
    nlohmann::json router = nlohmann::json::object();
    for (std::size_t i = 0; i < stratum::settings::kRouterEntryCount; ++i) {
        router[std::string(stratum::settings::routerEntryName(static_cast<RouterEntry>(i)))] = 0.0;
    }
    router["fluid_level_floodedness"] = floodedness;
    router["final_density"] = nlohmann::json{{"type", "minecraft:y_clamped_gradient"},
                                             {"from_y", -1},
                                             {"to_y", 1},
                                             {"from_value", 1.0},
                                             {"to_value", -1.0}};
    return nlohmann::json{
        {"default_block", {{"Name", "minecraft:stone"}}},
        {"default_fluid", {{"Name", "minecraft:water"}}},
        {"sea_level", 8},
        {"disable_mob_generation", false},
        {"aquifers_enabled", aquifers},
        {"ore_veins_enabled", oreVeins},
        {"legacy_random_source", false},
        {"noise", {{"min_y", -16}, {"height", 48}, {"size_horizontal", 1}, {"size_vertical", 1}}},
        {"noise_router", router},
        {"spawn_target", nlohmann::json::array()},
        {"surface_rule", {{"type", "minecraft:bandlands"}}},
    };
}

/// Solid everywhere below y = 1 (open water/air above it, as in
/// flatSettings()) EXCEPT a buried notch at y in [-8, -6]..[-10, -8], carved
/// out by maxing two more y_clamped_gradients against the flat plane's own
/// solid region so the notch sits entirely below solid rock rather than open
/// to the sky — the shape a real underground cave-void has, as opposed to the
/// column's own topmost fluid body.
[[nodiscard]] nlohmann::json buriedNotchSettings() {
    nlohmann::json settings = flatSettings(false, false);
    const nlohmann::json solidBelowSky = nlohmann::json{{"type", "minecraft:y_clamped_gradient"},
                                                         {"from_y", -1},
                                                         {"to_y", 1},
                                                         {"from_value", 1.0},
                                                         {"to_value", -1.0}};
    const nlohmann::json notchFloor = nlohmann::json{{"type", "minecraft:y_clamped_gradient"},
                                                      {"from_y", -10},
                                                      {"to_y", -8},
                                                      {"from_value", 1.0},
                                                      {"to_value", -1.0}};
    const nlohmann::json notchRoof = nlohmann::json{{"type", "minecraft:y_clamped_gradient"},
                                                     {"from_y", -8},
                                                     {"to_y", -6},
                                                     {"from_value", -1.0},
                                                     {"to_value", 1.0}};
    settings["noise_router"]["final_density"] =
        nlohmann::json{{"type", "minecraft:min"},
                       {"argument1", solidBelowSky},
                       {"argument2", nlohmann::json{{"type", "minecraft:max"},
                                                    {"argument1", notchFloor},
                                                    {"argument2", notchRoof}}}};
    return settings;
}

[[nodiscard]] ChunkFiller compileFrom(const TempTree& tree, const LoadedSettings& loaded,
                                      const RuleGraph* surfaceRules = nullptr,
                                      const ParameterList* biomeParameters = nullptr,
                                      const TemperatureTable* biomeTemperatures = nullptr) {
    const auto& settings =
        loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:test"));
    static stratum::density::NoiseRegistry noises = stratum::density::NoiseRegistry::create(
        tree.pack(), loaded.graph.referencedNoises(), 0, stratum::density::RandomSource::Xoroshiro);
    return ChunkFiller::compile(loaded.graph, noises, settings, surfaceRules, biomeParameters,
                                biomeTemperatures);
}

[[nodiscard]] RuleGraph resolveSurface(const nlohmann::json& json) {
    return RuleGraph::resolve(json, stratum::data::ResourceLocation::parse("minecraft:test"));
}

[[nodiscard]] nlohmann::json block(const std::string& name) {
    return nlohmann::json{{"type", "minecraft:block"}, {"result_state", {{"Name", name}}}};
}

[[nodiscard]] nlohmann::json gradient(const std::string& randomName, int trueAt, int falseAt) {
    return nlohmann::json{{"type", "minecraft:vertical_gradient"},
                          {"random_name", randomName},
                          {"true_at_and_below", {{"absolute", trueAt}}},
                          {"false_at_and_above", {{"absolute", falseAt}}}};
}

[[nodiscard]] nlohmann::json condition(const nlohmann::json& ifTrue,
                                       const nlohmann::json& thenRun) {
    return nlohmann::json{
        {"type", "minecraft:condition"}, {"if_true", ifTrue}, {"then_run", thenRun}};
}

/// One entry, matching everywhere: every axis covers the climate the flat
/// dimension's constant-zero router produces. Enough to prove biome
/// resolution is threaded through without needing a real parameter table.
[[nodiscard]] ParameterList plainsEverywhere() {
    const nlohmann::json axis = nlohmann::json::array({-1.0, 1.0});
    const nlohmann::json json{{"biomes",
                               {{{"biome", "minecraft:plains"},
                                 {"parameters",
                                  {{"temperature", axis},
                                   {"humidity", axis},
                                   {"continentalness", axis},
                                   {"erosion", axis},
                                   {"depth", axis},
                                   {"weirdness", axis},
                                   {"offset", 0.0}}}}}}};
    return ParameterList::fromJson(json, stratum::data::ResourceLocation::parse("minecraft:test"));
}

/// `minecraft:plains` declaring @p temperature, read back from @p tree's own
/// Pack — the same tree plainsEverywhere()'s ParameterList names, so a test
/// combining both resolves one consistent biome.
[[nodiscard]] TemperatureTable plainsTemperature(const TempTree& tree, double temperature) {
    tree.defineBiomeTemperature("plains", temperature);
    return TemperatureTable::fromPack(tree.pack());
}

} // namespace

TEST_CASE("a dimension with aquifers now fills, draining where floodedness says to",
          "[terrain][filler][aquifer]") {
    // Every router entry constant zero: `fluid_level_floodedness` at 0.0
    // clears neither of the level rule's gates (0.4, 0.8), so every cell
    // falls through to `lambda` — `min(kLavaLevel, sea_level)` = -54 here,
    // far below this dimension's own floor of -16. No cell can ever be wet:
    // the aquifer drains what the old sea-level shortcut would have flooded.
    const TempTree tree;
    tree.defineSettings("test", flatSettings(/*aquifers=*/true, /*oreVeins=*/false));
    const LoadedSettings loaded = tree.load();
    const ChunkFiller filler = compileFrom(tree, loaded);

    ChunkBuffer buffer(
        loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:test")).geometry);
    filler.fill(0, 0, buffer);

    // The gradient is still positive below y = 0: Q2.2 is unconditional, so
    // the aquifer never touches this regardless of its own level.
    CHECK(buffer.at(0, -16, 0).name.toString() == "minecraft:stone");
    CHECK(buffer.at(7, -1, 9).name.toString() == "minecraft:stone");
    // Where the plain rule placed water (y < sea_level = 8), the aquifer's
    // own level rule now decides instead, and drains it.
    CHECK(buffer.at(0, 0, 0).name.toString() == "minecraft:air");
    CHECK(buffer.at(15, 7, 15).name.toString() == "minecraft:air");
    CHECK(buffer.at(0, 8, 0).name.toString() == "minecraft:air");
    CHECK(buffer.at(3, 31, 12).name.toString() == "minecraft:air");

    // No barrier either: every cell's own level agrees (uniform router
    // inputs), so no pair ever disagrees for placesBarrier to weigh.
    CHECK(buffer.paletteSize() == 2U);
}

TEST_CASE("an aquifer above its own floodedness gate reproduces the plain sea, through the "
          "real mechanism",
          "[terrain][filler][aquifer]") {
    // `fluid_level_floodedness` at 0.9 clears the ocean branch's sea gate
    // (0.8) for every cell, so cellFluidLevel returns `sea_level` itself —
    // not through the old shortcut, but through the level rule actually
    // computing it.
    const TempTree tree;
    tree.defineSettings("test",
                        flatSettings(/*aquifers=*/true, /*oreVeins=*/false, /*floodedness=*/0.9));
    const LoadedSettings loaded = tree.load();
    const ChunkFiller filler = compileFrom(tree, loaded);

    ChunkBuffer buffer(
        loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:test")).geometry);
    filler.fill(0, 0, buffer);

    CHECK(buffer.at(0, -16, 0).name.toString() == "minecraft:stone");
    // Water below sea_level = 8, matching the plain rule exactly — this
    // time via selectSources, cellFluidLevel and placesBarrier all running,
    // not a shortcut.
    CHECK(buffer.at(0, 0, 0).name.toString() == "minecraft:water");
    CHECK(buffer.at(15, 7, 15).name.toString() == "minecraft:water");
    // sea_level itself is the first air, same as the plain rule.
    CHECK(buffer.at(0, 8, 0).name.toString() == "minecraft:air");
    CHECK(buffer.at(3, 31, 12).name.toString() == "minecraft:air");

    CHECK(buffer.paletteSize() == 3U);
}

TEST_CASE("a dimension with ore veins is refused, by name", "[terrain][filler]") {
    const TempTree tree;
    tree.defineSettings("test", flatSettings(/*aquifers=*/false, /*oreVeins=*/true));
    const LoadedSettings loaded = tree.load();
    CHECK_THROWS_WITH(compileFrom(tree, loaded), ContainsSubstring("ore_veins_enabled"));
}

TEST_CASE("the fill rule is density, then sea level, then air", "[terrain][filler]") {
    const TempTree tree;
    tree.defineSettings("test", flatSettings(false, false));
    const LoadedSettings loaded = tree.load();
    const ChunkFiller filler = compileFrom(tree, loaded);

    ChunkBuffer buffer(
        loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:test")).geometry);
    filler.fill(0, 0, buffer);

    // The gradient is positive below y = 0, so stone from the floor to y = -1.
    CHECK(buffer.at(0, -16, 0).name.toString() == "minecraft:stone");
    CHECK(buffer.at(7, -1, 9).name.toString() == "minecraft:stone");
    // Above it, water up to but NOT INCLUDING sea_level, which is 8 here.
    CHECK(buffer.at(0, 0, 0).name.toString() == "minecraft:water");
    CHECK(buffer.at(15, 7, 15).name.toString() == "minecraft:water");
    // sea_level itself is the first air. Measured against vanilla: an
    // inclusive comparison puts one extra water block on every column.
    CHECK(buffer.at(0, 8, 0).name.toString() == "minecraft:air");
    CHECK(buffer.at(3, 31, 12).name.toString() == "minecraft:air");

    // Three states and no more, which is also a check that the palette is not
    // silently accumulating duplicates.
    CHECK(buffer.paletteSize() == 3U);
}

TEST_CASE("a buffer refuses positions outside the chunk or the dimension", "[terrain][filler]") {
    const TempTree tree;
    tree.defineSettings("test", flatSettings(false, false));
    const LoadedSettings loaded = tree.load();
    ChunkBuffer buffer(
        loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:test")).geometry);
    CHECK_THROWS_WITH(buffer.at(16, 0, 0), ContainsSubstring("outside the chunk"));
    CHECK_THROWS_WITH(buffer.at(-1, 0, 0), ContainsSubstring("outside the chunk"));
    CHECK_THROWS_WITH(buffer.at(0, -17, 0), ContainsSubstring("outside this dimension"));
    CHECK_THROWS_WITH(buffer.at(0, 32, 0), ContainsSubstring("outside this dimension"));
}

TEST_CASE("the corner cache changes speed and not values", "[terrain][filler]") {
    // The load-bearing claim of the whole filler. `interpolated` is defined
    // over a cell, so a filler that walks a cell can compute its eight corners
    // once instead of 128 times — measured at 87 times faster. It is only
    // allowed to be faster: a cache that returned a neighbouring cell's
    // corners would be undetectable in a test that samples one point at a
    // time, so this compares the two paths bit-for-bit over a whole cell and
    // across cell boundaries in every direction.
    const TempTree tree;
    nlohmann::json settings = flatSettings(false, false);
    settings["noise_router"]["final_density"] =
        nlohmann::json{{"type", "minecraft:interpolated"},
                       {"argument",
                        {{"type", "minecraft:y_clamped_gradient"},
                         {"from_y", -1},
                         {"to_y", 1},
                         {"from_value", 1.0},
                         {"to_value", -1.0}}}};
    tree.defineSettings("test", settings);
    const LoadedSettings loaded = tree.load();
    const auto& entry =
        loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:test"));
    const auto noises = stratum::density::NoiseRegistry::create(
        tree.pack(), loaded.graph.referencedNoises(), 0, stratum::density::RandomSource::Xoroshiro);
    const stratum::density::Interpreter interpreter(
        loaded.graph, noises,
        stratum::density::CellGeometry{.width = entry.geometry.cellWidth(),
                                       .height = entry.geometry.cellHeight()});
    const auto root = entry.router.at(RouterEntry::FinalDensity);

    stratum::density::Interpreter::CornerCache cache(interpreter.cacheSize());
    std::size_t compared = 0;
    for (int x = -9; x <= 9; ++x) {
        for (int z = -9; z <= 9; ++z) {
            for (int y = -16; y < 32; ++y) {
                const stratum::density::Point at{.x = x, .y = y, .z = z};
                const double uncached = interpreter.evaluate(root, at);
                const double cached = interpreter.evaluate(root, at, cache);
                ++compared;
                REQUIRE(std::bit_cast<std::uint64_t>(uncached) ==
                        std::bit_cast<std::uint64_t>(cached));
            }
        }
    }
    CHECK(compared == 19U * 19U * 48U);
}

TEST_CASE("bandlands runs through the whole filler pipeline, not just the executor",
          "[terrain][filler][surface]") {
    // `bandlands` was the last construct this build could not run at all
    // (spec/bandlands-spec.md, SPEC §11); surface_executor_test.cpp checks
    // its own table against the server directly, so this only needs to
    // prove ChunkFiller actually wires it through: the noise it needs
    // reaches Executor::compile, and a placed block reaches the buffer.
    // Built directly rather than through compileFrom()/TempTree's shared
    // static registry, since this is the one test here that needs a real
    // noise in it.
    const TempTree tree;
    tree.defineSettings("test", flatSettings(false, false));
    tree.defineClayBandsOffsetNoise();
    const LoadedSettings loaded = tree.load();
    const auto& settings =
        loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:test"));
    // referencedNoises() is the DENSITY graph's own contract and knows
    // nothing about what a surface tree needs — `clay_bands_offset` has to
    // be asked for explicitly, the same way ChunkFiller::compile's own doc
    // says a caller must for `minecraft:surface`/`minecraft:surface_secondary`.
    auto wanted = loaded.graph.referencedNoises();
    wanted.push_back(stratum::data::ResourceLocation::parse("minecraft:clay_bands_offset"));
    const auto noises = stratum::density::NoiseRegistry::create(
        tree.pack(), wanted, 0, stratum::density::RandomSource::Xoroshiro);
    const RuleGraph surface = resolveSurface(nlohmann::json{{"type", "minecraft:bandlands"}});
    const ChunkFiller filler =
        ChunkFiller::compile(loaded.graph, noises, settings, &surface, /*biomeParameters=*/nullptr);

    REQUIRE(filler.runsSurfaceRules());
    CHECK(filler.surfaceRulesBlockedBy().empty());

    ChunkBuffer buffer(settings.geometry);
    filler.fill(0, 0, buffer);
    // bandlands always places something (its table has no "place nothing"
    // entry, only colours — surface_executor_test.cpp's own golden-table
    // tests establish that), so every block the density chain left solid,
    // fluid or air is replaced by one of its seven terracotta colours.
    const auto isClay = [](const std::string& name) {
        return name == "minecraft:terracotta" || name.ends_with("_terracotta");
    };
    CHECK(isClay(buffer.at(0, -16, 0).name.toString()));
    CHECK(isClay(buffer.at(0, 0, 0).name.toString()));
    CHECK(isClay(buffer.at(0, 8, 0).name.toString()));
}

TEST_CASE("a runnable tree's replacement reaches the buffer through the whole pipeline",
          "[terrain][filler][surface]") {
    const TempTree tree;
    tree.defineSettings("test", flatSettings(false, false));
    const LoadedSettings loaded = tree.load();
    const RuleGraph surface = resolveSurface(
        condition(gradient("minecraft:bedrock_floor", -16, -15), block("minecraft:bedrock")));
    const ChunkFiller filler = compileFrom(tree, loaded, &surface);
    REQUIRE(filler.runsSurfaceRules());
    CHECK(filler.surfaceRulesBlockedBy().empty());

    ChunkBuffer buffer(
        loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:test")).geometry);
    filler.fill(0, 0, buffer);

    // y = -16 is at-and-below the gradient's certain floor: no draw, always
    // true, on every column.
    CHECK(buffer.at(0, -16, 0).name.toString() == "minecraft:bedrock");
    CHECK(buffer.at(15, -16, 15).name.toString() == "minecraft:bedrock");
    // y = -15 is at-and-above the certain ceiling: always false, so the
    // filler's own stone stands.
    CHECK(buffer.at(0, -15, 0).name.toString() == "minecraft:stone");
}

TEST_CASE("stone_depth reads the run the filler itself placed, not a fresh scan",
          "[terrain][filler][surface]") {
    const TempTree tree;
    tree.defineSettings("test", flatSettings(false, false));
    const LoadedSettings loaded = tree.load();
    const RuleGraph surface =
        resolveSurface(condition(nlohmann::json{{"type", "minecraft:stone_depth"},
                                                {"offset", 1},
                                                {"add_surface_depth", false},
                                                {"secondary_depth_range", 0},
                                                {"surface_type", "floor"}},
                                 block("minecraft:andesite")));
    const ChunkFiller filler = compileFrom(tree, loaded, &surface);
    REQUIRE(filler.runsSurfaceRules());

    ChunkBuffer buffer(
        loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:test")).geometry);
    filler.fill(0, 0, buffer);

    // Solid from the floor up to y = -1, so the run counts 1 at y = -1 and
    // grows going down. depth is the run minus one, so its top two blocks
    // — depth 0 and depth 1 — pass the <= 1 threshold and the third does
    // not. Undecorated by an outer y_above or water check, the way vanilla
    // always nests stone_depth, so this says nothing about the fluid or air
    // above; only these three positions are asserted.
    CHECK(buffer.at(0, -1, 0).name.toString() == "minecraft:andesite");
    CHECK(buffer.at(0, -2, 0).name.toString() == "minecraft:andesite");
    CHECK(buffer.at(0, -3, 0).name.toString() == "minecraft:stone");
}

TEST_CASE("water reads the filler's own latched height, not sea_level directly",
          "[terrain][filler][surface]") {
    const TempTree tree;
    tree.defineSettings("test", flatSettings(false, false));
    const LoadedSettings loaded = tree.load();
    const RuleGraph surface =
        resolveSurface(condition(nlohmann::json{{"type", "minecraft:water"},
                                                {"offset", -2},
                                                {"surface_depth_multiplier", 0},
                                                {"add_stone_depth", false}},
                                 block("minecraft:ice")));
    const ChunkFiller filler = compileFrom(tree, loaded, &surface);
    REQUIRE(filler.runsSurfaceRules());

    ChunkBuffer buffer(
        loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:test")).geometry);
    filler.fill(0, 0, buffer);

    // Water fills y = 0..7, so the latched height is 8 — one above the
    // topmost fluid block, not sea_level compared directly. offset -2 moves
    // the fired boundary down to y = 6.
    CHECK(buffer.at(0, 5, 0).name.toString() == "minecraft:water");
    CHECK(buffer.at(0, 6, 0).name.toString() == "minecraft:ice");
    CHECK(buffer.at(0, 7, 0).name.toString() == "minecraft:ice");
}

TEST_CASE("an unconditioned rule never rewrites a buried fluid pocket's own fluid",
          "[terrain][filler][surface]") {
    // golden_fill_test.cpp's own residual, reproduced by hand: the
    // overworld's `deepslate` rule is a bare, unconditioned
    // `vertical_gradient` — no water check, no stone_depth, nothing — yet
    // the real server never lets it (or anything else unconditioned) paint
    // over a fluid-filled cave-void, only over the solid rock around it.
    // Measured against the real server with the overworld's ENTIRE
    // surface_rule replaced by just that one bare rule (SPEC §11): it still
    // leaves exactly the same 475 fluid blocks alone that vanilla's real
    // 287-rule tree does, in the same aquifer-free probe golden_fill_test.cpp
    // reads. This uses an unconditioned `block` rule instead of
    // `vertical_gradient` so the assertion needs no RNG — a `block` rule
    // fires at every position it is asked about, unconditionally, same as
    // the deepslate gradient does throughout the "certain" band the real
    // 475-block residual sat in.
    const TempTree tree;
    tree.defineSettings("test", buriedNotchSettings());
    const LoadedSettings loaded = tree.load();
    const RuleGraph surface = resolveSurface(block("minecraft:end_stone"));
    const ChunkFiller filler = compileFrom(tree, loaded, &surface);
    REQUIRE(filler.runsSurfaceRules());

    ChunkBuffer buffer(
        loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:test")).geometry);
    filler.fill(0, 0, buffer);

    // Solid rock on both sides of the notch is rewritten like anything else
    // an unconditioned rule reaches.
    CHECK(buffer.at(0, -10, 0).name.toString() == "minecraft:end_stone");
    CHECK(buffer.at(0, -6, 0).name.toString() == "minecraft:end_stone");
    // The notch itself — solid rock already crossed above it, so
    // stoneDepthAbove is nonzero going in — keeps its own fluid untouched.
    CHECK(buffer.at(0, -8, 0).name.toString() == "minecraft:water");
    // The column's OWN topmost fluid, reached with nothing solid above it
    // yet (stoneDepthAbove == 0 throughout), stays reachable — the same
    // distinction "water reads the filler's own latched height" exercises
    // through a condition instead of a bare rule.
    CHECK(buffer.at(0, 6, 0).name.toString() == "minecraft:end_stone");
}

TEST_CASE("biome reads the biome the climate router and parameter list compute",
          "[terrain][filler][surface]") {
    const TempTree tree;
    tree.defineSettings("test", flatSettings(false, false));
    const LoadedSettings loaded = tree.load();
    const RuleGraph surface = resolveSurface(
        condition(nlohmann::json{{"type", "minecraft:biome"}, {"biome_is", {"minecraft:plains"}}},
                  block("minecraft:podzol")));
    const ParameterList biomes = plainsEverywhere();
    const ChunkFiller filler = compileFrom(tree, loaded, &surface, &biomes);
    REQUIRE(filler.runsSurfaceRules());
    CHECK(filler.surfaceRulesBlockedBy().empty());

    ChunkBuffer buffer(
        loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:test")).geometry);
    filler.fill(0, 0, buffer);

    // The flat dimension's climate router is constant zero everywhere, and
    // the table's one entry matches it everywhere, so the biome is
    // "minecraft:plains" for every block — solid, fluid and air alike.
    CHECK(buffer.at(0, -16, 0).name.toString() == "minecraft:podzol");
    CHECK(buffer.at(3, 4, 9).name.toString() == "minecraft:podzol");
    CHECK(buffer.at(15, 31, 15).name.toString() == "minecraft:podzol");
}

TEST_CASE("a tree that reads the biome without a parameter list is blocked, not crashed",
          "[terrain][filler][surface]") {
    const TempTree tree;
    tree.defineSettings("test", flatSettings(false, false));
    const LoadedSettings loaded = tree.load();
    const RuleGraph surface = resolveSurface(
        condition(nlohmann::json{{"type", "minecraft:biome"}, {"biome_is", {"minecraft:plains"}}},
                  block("minecraft:podzol")));
    const ChunkFiller filler = compileFrom(tree, loaded, &surface); // no ParameterList

    CHECK_FALSE(filler.runsSurfaceRules());
    REQUIRE(filler.surfaceRulesBlockedBy().size() == 1U);
    CHECK_THAT(filler.surfaceRulesBlockedBy().front(), ContainsSubstring("minecraft:biome"));

    ChunkBuffer buffer(
        loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:test")).geometry);
    filler.fill(0, 0, buffer);
    CHECK(buffer.at(0, -16, 0).name.toString() == "minecraft:stone");
}

TEST_CASE("a tree that uses bandlands without its noise built is blocked, not crashed",
          "[terrain][filler][surface]") {
    const TempTree tree;
    tree.defineSettings("test", flatSettings(false, false));
    const LoadedSettings loaded = tree.load();
    const RuleGraph surface = resolveSurface(nlohmann::json{{"type", "minecraft:bandlands"}});
    // compileFrom()'s shared registry never asked for clay_bands_offset —
    // see the dedicated test above for a filler that runs bandlands with it.
    const ChunkFiller filler = compileFrom(tree, loaded, &surface);

    CHECK_FALSE(filler.runsSurfaceRules());
    REQUIRE(filler.surfaceRulesBlockedBy().size() == 1U);
    CHECK_THAT(filler.surfaceRulesBlockedBy().front(), ContainsSubstring("minecraft:bandlands"));

    ChunkBuffer buffer(
        loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:test")).geometry);
    filler.fill(0, 0, buffer);
    CHECK(buffer.at(0, -16, 0).name.toString() == "minecraft:stone");
}

TEST_CASE("a tree that reads temperature needs the biome's identity too, and names both gaps",
          "[terrain][filler][surface]") {
    // `temperature` never names `biome` itself, but it cannot answer without
    // knowing WHICH biome a block sits in first — the same climate search
    // `biome` needs. Neither is supplied here, so both gaps are named,
    // rather than the second only surfacing once the first is fixed.
    const TempTree tree;
    tree.defineSettings("test", flatSettings(false, false));
    const LoadedSettings loaded = tree.load();
    const RuleGraph surface = resolveSurface(condition(
        nlohmann::json{{"type", "minecraft:temperature"}}, block("minecraft:packed_ice")));
    const ChunkFiller filler = compileFrom(tree, loaded, &surface);

    CHECK_FALSE(filler.runsSurfaceRules());
    REQUIRE(filler.surfaceRulesBlockedBy().size() == 2U);
    CHECK_THAT(filler.surfaceRulesBlockedBy()[0], ContainsSubstring("minecraft:biome"));
    CHECK_THAT(filler.surfaceRulesBlockedBy()[1], ContainsSubstring("minecraft:temperature"));
}

TEST_CASE("a tree that reads temperature without a temperature table is blocked, not crashed",
          "[terrain][filler][surface]") {
    // Its biome identity is resolvable here — only the biome's own DECLARED
    // temperature is missing, so this isolates that one gap from the last
    // test's two.
    const TempTree tree;
    tree.defineSettings("test", flatSettings(false, false));
    const LoadedSettings loaded = tree.load();
    const RuleGraph surface = resolveSurface(condition(
        nlohmann::json{{"type", "minecraft:temperature"}}, block("minecraft:packed_ice")));
    const ParameterList biomes = plainsEverywhere();
    const ChunkFiller filler = compileFrom(tree, loaded, &surface, &biomes); // no TemperatureTable

    CHECK_FALSE(filler.runsSurfaceRules());
    REQUIRE(filler.surfaceRulesBlockedBy().size() == 1U);
    CHECK_THAT(filler.surfaceRulesBlockedBy().front(), ContainsSubstring("minecraft:temperature"));
}

TEST_CASE("temperature reads the biome's own declared value, not a made-up one",
          "[terrain][filler][surface]") {
    const TempTree tree;
    tree.defineSettings("test", flatSettings(false, false));
    const LoadedSettings loaded = tree.load();
    const RuleGraph surface = resolveSurface(condition(
        nlohmann::json{{"type", "minecraft:temperature"}}, block("minecraft:packed_ice")));
    const ParameterList biomes = plainsEverywhere();
    // Below `sea_level + 17` (25 here), freezing() compares the declared
    // value unadjusted (surface::Executor::freezing's own doc); 0.1 sits
    // comfortably under its 0.15 threshold, so this fires everywhere the
    // flat dimension's floor reaches, without leaning on the height term.
    const TemperatureTable temperatures = plainsTemperature(tree, 0.1);
    const ChunkFiller filler = compileFrom(tree, loaded, &surface, &biomes, &temperatures);
    REQUIRE(filler.runsSurfaceRules());
    CHECK(filler.surfaceRulesBlockedBy().empty());

    ChunkBuffer buffer(
        loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:test")).geometry);
    filler.fill(0, 0, buffer);
    CHECK(buffer.at(0, -16, 0).name.toString() == "minecraft:packed_ice");
    CHECK(buffer.at(15, -1, 9).name.toString() == "minecraft:packed_ice");
}

TEST_CASE("above_preliminary_surface reads the column's own level, not a per-block guess",
          "[terrain][filler][surface]") {
    const TempTree tree;
    tree.defineSettings("test", flatSettings(false, false));
    const LoadedSettings loaded = tree.load();
    const RuleGraph surface =
        resolveSurface(condition(nlohmann::json{{"type", "minecraft:above_preliminary_surface"}},
                                 block("minecraft:glowstone")));
    const ChunkFiller filler = compileFrom(tree, loaded, &surface);
    REQUIRE(filler.runsSurfaceRules());

    ChunkBuffer buffer(
        loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:test")).geometry);
    filler.fill(0, 0, buffer);

    // preliminary_surface_level is the flat dimension's constant zero: below
    // it the filler's own solid stone stands, and at y = 0 the rule fires.
    CHECK(buffer.at(0, -1, 0).name.toString() == "minecraft:stone");
    CHECK(buffer.at(0, 0, 0).name.toString() == "minecraft:glowstone");
}

TEST_CASE("steep reads neighbours clamped to this chunk, never a block outside it",
          "[terrain][filler][surface]") {
    // The flat dimension's height is the same in every column, so steep
    // never fires anywhere on it — this is a sanity check that the
    // world-surface scan and the local-coordinate clamping at the chunk's
    // own edges (fillSteepNeighbours' own semantics are covered directly in
    // surface_executor_test.cpp) run cleanly end to end, not a claim that
    // every geometry is exercised.
    const TempTree tree;
    tree.defineSettings("test", flatSettings(false, false));
    const LoadedSettings loaded = tree.load();
    const RuleGraph surface = resolveSurface(
        condition(nlohmann::json{{"type", "minecraft:steep"}}, block("minecraft:magma_block")));
    const ChunkFiller filler = compileFrom(tree, loaded, &surface);
    REQUIRE(filler.runsSurfaceRules());

    ChunkBuffer buffer(
        loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:test")).geometry);
    filler.fill(0, 0, buffer);

    CHECK(buffer.at(0, -1, 0).name.toString() == "minecraft:stone");
    CHECK(buffer.at(15, -1, 15).name.toString() == "minecraft:stone");
    CHECK(buffer.paletteSize() == 3U);
}
