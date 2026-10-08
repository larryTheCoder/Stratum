// Stratum — the chunk filler.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#include "support/temp_path.hpp"

#include <stratum/biome/parameter_list.hpp>
#include <stratum/biome/temperature_table.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/javamath.hpp>
#include <stratum/ore/vein.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/executor.hpp>
#include <stratum/surface/rule_graph.hpp>
#include <stratum/terrain/filler.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <string>
#include <utility>
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

    /// Writes the two noises every surface DEPTH reads, so a filler whose
    /// rules ask for one — `hole`, a depth-adding `stone_depth`, or
    /// `above_preliminary_surface`, whose boundary carries a depth (SPEC §11)
    /// — has something to build a NoiseRegistry from. The parameters are
    /// vanilla's own shape; the numbers they produce are pinned against the
    /// server in tests/conformance, not here.
    const TempTree& defineSurfaceNoises() const {
        std::filesystem::create_directories(path_ / "noise");
        std::ofstream surface(path_ / "noise" / "surface.json");
        surface << R"({"firstOctave": -6, "amplitudes": [1.0, 1.0, 1.0]})";
        std::ofstream secondary(path_ / "noise" / "surface_secondary.json");
        secondary << R"({"firstOctave": -6, "amplitudes": [1.0, 1.0, 0.0, 1.0]})";
        return *this;
    }

    /// Writes `worldgen/noise/<name>.json`, for a router entry that reads a
    /// noise of its own (density_interpreter_test.cpp's helper of the same
    /// name).
    const TempTree& defineNoise(std::string_view name, std::string_view json) const {
        std::filesystem::create_directories(path_ / "noise");
        std::ofstream out(path_ / "noise" / (std::string(name) + ".json"));
        out << json;
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
        return stratum::test::tempName("stratum-filler-test");
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

/// Deep enough to reach the global lava sea: stone at y <= -61, then every
/// non-solid position below min(-54, sea_level) is lava (spec Q1.2/Q2.1, with
/// aquifers off), water from there up to the sea level of 8, open air above.
[[nodiscard]] nlohmann::json lavaSeaSettings() {
    nlohmann::json settings = flatSettings(false, false);
    settings["noise"] = {
        {"min_y", -64}, {"height", 128}, {"size_horizontal", 1}, {"size_vertical", 1}};
    settings["noise_router"]["final_density"] =
        nlohmann::json{{"type", "minecraft:y_clamped_gradient"},
                       {"from_y", -61},
                       {"to_y", -59},
                       {"from_value", 1.0},
                       {"to_value", -1.0}};
    return settings;
}

/// `lavaSeaSettings()`'s depth with `flatSettings()`'s plane — solid stone
/// from y -64 to -1, water above it to sea level — and a
/// `preliminary_surface_level` that VARIES from column to column:
/// `-28 + 12 * psl_field`, a two-octave noise on 8- and 4-block octaves
/// against the lattice's 16, read with `y_scale` 0 so the entry is
/// y-independent. Nothing below the plane is open, so no lava sea forms and
/// every block from -64 to -1 is the default block a surface rule may write.
[[nodiscard]] nlohmann::json varyingPslSettings() {
    nlohmann::json settings = flatSettings(false, false);
    settings["noise"] = {
        {"min_y", -64}, {"height", 128}, {"size_horizontal", 1}, {"size_vertical", 1}};
    settings["noise_router"]["preliminary_surface_level"] =
        nlohmann::json{{"type", "minecraft:add"},
                       {"argument1", -28.0},
                       {"argument2",
                        {{"type", "minecraft:mul"},
                         {"argument1", 12.0},
                         {"argument2",
                          {{"type", "minecraft:noise"},
                           {"noise", "minecraft:psl_field"},
                           {"xz_scale", 1.0},
                           {"y_scale", 0.0}}}}}};
    return settings;
}

/// `flatSettings` with the three vein router entries pinned to constants that
/// clear every deterministic gate, so only the RNG is left to decide.
///
/// A toggle of -1 makes every candidate IRON, whose range [-60, -8] overlaps
/// this dimension's own [-16, 32) exactly where the flat plane is still solid
/// — the only place a vein can land. A toggle of +1 makes them COPPER, whose
/// range [0, 50] lies entirely in the open air above the plane, which is how
/// the "never replaces air" case gets candidates that must all be refused.
[[nodiscard]] nlohmann::json veinSettings(bool aquifers, double toggle = -1.0) {
    nlohmann::json settings = flatSettings(aquifers, /*oreVeins=*/true);
    settings["noise_router"]["vein_toggle"] = toggle;
    settings["noise_router"]["vein_ridged"] = -1.0;
    settings["noise_router"]["vein_gap"] = 0.0;
    return settings;
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

/// The same shape as buriedNotchSettings(), moved entirely above sea_level
/// (8) so the notch is AIR rather than fluid: solid from the world floor up
/// through y = 20, a buried air notch at y in [20, 22]..[22, 24], solid again
/// up through y = 27, and open sky above that — the shape a real underground
/// cave has, as opposed to the column's own open sky.
[[nodiscard]] nlohmann::json buriedAirNotchSettings() {
    nlohmann::json settings = flatSettings(false, false);
    const nlohmann::json solidBelowSky = nlohmann::json{{"type", "minecraft:y_clamped_gradient"},
                                                        {"from_y", 27},
                                                        {"to_y", 29},
                                                        {"from_value", 1.0},
                                                        {"to_value", -1.0}};
    const nlohmann::json notchFloor = nlohmann::json{{"type", "minecraft:y_clamped_gradient"},
                                                     {"from_y", 20},
                                                     {"to_y", 22},
                                                     {"from_value", 1.0},
                                                     {"to_value", -1.0}};
    const nlohmann::json notchRoof = nlohmann::json{{"type", "minecraft:y_clamped_gradient"},
                                                    {"from_y", 22},
                                                    {"to_y", 24},
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

/// `a + (b - a) * t` in x, then in z, over four samples ordered (x0,z0),
/// (x1,z0), (x0,z1), (x1,z1) — the blend `ChunkFiller::preliminarySurfaceIn`
/// uses, with every floor left to the caller. Test-only, and it exists to
/// spell the readings the engine must NOT implement next to the one it must;
/// the known-answer case below ties it back to the engine's own helper.
[[nodiscard]] double bilerp(const std::array<double, 4>& samples, double u, double v) {
    const double low = samples[0] + ((samples[1] - samples[0]) * u);
    const double high = samples[2] + ((samples[3] - samples[2]) * u);
    return low + ((high - low) * v);
}

[[nodiscard]] std::array<double, 4> floored(const std::array<double, 4>& samples) {
    return {std::floor(samples[0]), std::floor(samples[1]), std::floor(samples[2]),
            std::floor(samples[3])};
}

/// Each sample through Java's `(int)` cast, which truncates toward zero.
[[nodiscard]] std::array<double, 4> truncated(const std::array<double, 4>& samples) {
    return {static_cast<double>(stratum::javamath::doubleToInt(samples[0])),
            static_cast<double>(stratum::javamath::doubleToInt(samples[1])),
            static_cast<double>(stratum::javamath::doubleToInt(samples[2])),
            static_cast<double>(stratum::javamath::doubleToInt(samples[3]))};
}

/// SPEC §11's psl lattice at an arbitrary @p pitch: each sample floored,
/// blended, floored again. At 16 it must be `preliminarySurfaceIn` exactly.
[[nodiscard]] std::int32_t latticeAtPitch(const std::array<double, 4>& samples,
                                          std::int32_t offsetX, std::int32_t offsetZ,
                                          std::int32_t pitch) {
    return stratum::javamath::floorToInt(bilerp(floored(samples),
                                                static_cast<double>(offsetX) / pitch,
                                                static_cast<double>(offsetZ) / pitch));
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

TEST_CASE("ore veins place nothing without aquifers", "[terrain][filler][ore]") {
    // Measured coupling, not a refusal: a probe with veins on and aquifers
    // off came back 6291456 of 6291456 plain stone (SPEC §11). Reproducing
    // that means compiling happily and placing nothing, which is what the
    // server does — an exception here would be this build inventing a
    // failure vanilla does not have.
    const TempTree tree;
    tree.defineSettings("test", veinSettings(/*aquifers=*/false));
    const LoadedSettings loaded = tree.load();
    const ChunkFiller filler = compileFrom(tree, loaded);

    ChunkBuffer buffer(
        loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:test")).geometry);
    filler.fill(0, 0, buffer);

    for (std::int32_t y = -16; y < 0; ++y) {
        for (int localX = 0; localX < 16; ++localX) {
            CHECK(buffer.at(localX, y, 0).name.toString() == "minecraft:stone");
        }
    }
}

TEST_CASE("ore veins replace solid blocks inside the iron range", "[terrain][filler][ore]") {
    const TempTree tree;
    tree.defineSettings("test", veinSettings(/*aquifers=*/true));
    const LoadedSettings loaded = tree.load();
    const ChunkFiller filler = compileFrom(tree, loaded);

    ChunkBuffer buffer(
        loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:test")).geometry);
    filler.fill(0, 0, buffer);

    // `veinSettings` pins vein_toggle to -1 (iron), vein_ridged to -1 and
    // vein_gap to 0, so every solid block in [-16, -8] is a candidate and
    // only the RNG decides. The filler's answer is checked against the ore
    // module directly rather than against a hard-coded block list: what this
    // case exists to catch is the WIRING — a filler reading the wrong router
    // entry, passing x/z in the wrong order, or replacing the wrong block.
    const stratum::ore::VeinSource veins(0);
    int placed = 0;
    for (std::int32_t y = -16; y <= -8; ++y) {
        for (int localX = 0; localX < 16; ++localX) {
            for (int localZ = 0; localZ < 16; ++localZ) {
                const stratum::ore::Vein vein =
                    veins.at(localX, y, localZ,
                             stratum::ore::VeinInputs{.toggle = -1.0, .ridged = -1.0, .gap = 0.0});
                const std::string actual = buffer.at(localX, y, localZ).name.toString();
                if (vein.placed()) {
                    ++placed;
                    CHECK(vein.type == stratum::ore::VeinType::Iron);
                    CHECK(actual != "minecraft:stone");
                } else {
                    CHECK(actual == "minecraft:stone");
                }
            }
        }
    }
    // Roughly 70% of 9 * 256 candidates; asserted loosely because the exact
    // count is the RNG's business, and asserted at all so the loop above
    // cannot pass by never placing anything.
    CHECK(placed > 1000);
}

TEST_CASE("ore veins never replace air", "[terrain][filler][ore]") {
    // Confirmed against the server on 25509 air candidates, where the chain
    // would have placed 17813 vein blocks and the server placed none
    // (tools/analysis/ore-vein-placement-probe.sh, SPEC §11). `veinSettings`
    // puts copper's whole range above the flat plane, so every copper
    // candidate here sits in open air.
    const TempTree tree;
    tree.defineSettings("test", veinSettings(/*aquifers=*/true, /*toggle=*/1.0));
    const LoadedSettings loaded = tree.load();
    const ChunkFiller filler = compileFrom(tree, loaded);

    ChunkBuffer buffer(
        loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:test")).geometry);
    filler.fill(0, 0, buffer);

    const stratum::ore::VeinSource veins(0);
    int wouldHavePlaced = 0;
    for (std::int32_t y = 8; y < 32; ++y) { // above sea level, so air not water
        for (int localX = 0; localX < 16; ++localX) {
            for (int localZ = 0; localZ < 16; ++localZ) {
                wouldHavePlaced +=
                    veins.at(localX, y, localZ,
                             stratum::ore::VeinInputs{.toggle = 1.0, .ridged = -1.0, .gap = 0.0})
                            .placed()
                        ? 1
                        : 0;
                CHECK(buffer.at(localX, y, localZ).name.toString() == "minecraft:air");
            }
        }
    }
    CHECK(wouldHavePlaced > 1000);
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
    // tests establish that), so every position the surface pass REACHES is
    // replaced by one of its seven terracotta colours.
    const auto isClay = [](const std::string& name) {
        return name == "minecraft:terracotta" || name.ends_with("_terracotta");
    };
    // The flat plane: stone below y 0, water up to the sea level of 8, open
    // air above it. Only the stone is a surface-rule position: a rule's block
    // replaces the default block and nothing else (SPEC §11, measured on the
    // eight golden overworld regions), so the water at y 0 stays water.
    CHECK(isClay(buffer.at(0, -16, 0).name.toString()));
    CHECK(isClay(buffer.at(0, -1, 0).name.toString()));
    CHECK(buffer.at(0, 0, 0).name.toString() == "minecraft:water");
    // AND THE OPEN AIR IS NOT REACHED. The pass starts at the column's
    // topmost non-air block — here the water at y 7 — so an unconditioned
    // rule cannot paint the sky. This assertion read `isClay` at y 8 until
    // the End was generated against real blocks: vanilla's End has exactly
    // this tree shape and does NOT stack its default block up the sky
    // (golden_end_test.cpp; the sky-start reading scored 441481 of 2097152
    // blocks there).
    CHECK(buffer.at(0, 8, 0).name.toString() == "minecraft:air");
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
                                                {"offset", -9},
                                                {"surface_depth_multiplier", 0},
                                                {"add_stone_depth", false}},
                                 block("minecraft:ice")));
    const ChunkFiller filler = compileFrom(tree, loaded, &surface);
    REQUIRE(filler.runsSurfaceRules());

    ChunkBuffer buffer(
        loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:test")).geometry);
    filler.fill(0, 0, buffer);

    // Water fills y = 0..7, so the latched height is 8 — one above the
    // topmost fluid block. offset -9 puts the fired boundary at y = -1, the
    // plane's top solid block: the condition is true there and false one
    // below. (Here the latched height equals sea_level, so this pins the
    // latch's arithmetic, not its independence from sea_level.)
    //
    // The rule fires only on default-block positions. This case used to
    // assert ice painted INTO the water at y 6 and 7, on the reading that a
    // column's open fluid stays in reach of the rules for exactly this — a
    // rule keyed on `water` freezing a lake's surface. Refuted on the golden
    // overworld regions: vanilla's own unconditioned deepslate gradient
    // leaves every one of 6619641 open and buried water positions alone, so
    // no rule reaches a fluid position at all (SPEC §11).
    CHECK(buffer.at(0, -1, 0).name.toString() == "minecraft:ice");
    CHECK(buffer.at(0, -2, 0).name.toString() == "minecraft:stone");
    CHECK(buffer.at(0, 6, 0).name.toString() == "minecraft:water");
    CHECK(buffer.at(0, 7, 0).name.toString() == "minecraft:water");
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
    // The notch itself — solid rock already crossed above it on the way
    // down — keeps its own fluid untouched.
    CHECK(buffer.at(0, -8, 0).name.toString() == "minecraft:water");
    // And so does the column's OWN topmost fluid, reached with no solid
    // crossed above it. An earlier reading kept that one in reach of the
    // rules; the golden overworld regions refute it — vanilla's
    // unconditioned deepslate gradient leaves open water alone exactly as it
    // leaves buried water alone (SPEC §11). A rule writes over the default
    // block and nothing else.
    CHECK(buffer.at(0, 6, 0).name.toString() == "minecraft:water");
}

TEST_CASE("an unconditioned rule never rewrites a buried air pocket either",
          "[terrain][filler][surface]") {
    // The same mechanism as the buried-fluid case above, but for AIR — and
    // the one that actually matters most in practice: measured against a
    // real aquifer-on overworld region (golden_fill_aquifer_test.cpp,
    // SPEC §11), 451 of that golden's 461 category mismatches were exactly
    // this — a real cave's own drained/air cell, painted solid by the same
    // unconditioned `deepslate` rule, because a fluid-only guard
    // (`Context::stoneDepthAbove == 0`) cannot tell a deep cave's air from
    // the column's own open sky: air resets `stoneDepthAbove` on purpose
    // (Context's own doc), which is exactly what a buried pocket and open
    // sky have in common under that field alone. The real gate is
    // monotonic — has ANY solid been crossed above this position, however
    // many air/solid transitions came before it — not the resetting run.
    const TempTree tree;
    tree.defineSettings("test", buriedAirNotchSettings());
    const LoadedSettings loaded = tree.load();
    const RuleGraph surface = resolveSurface(block("minecraft:end_stone"));
    const ChunkFiller filler = compileFrom(tree, loaded, &surface);
    REQUIRE(filler.runsSurfaceRules());

    ChunkBuffer buffer(
        loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:test")).geometry);
    filler.fill(0, 0, buffer);

    // Solid rock on both sides of the notch is rewritten like anything else
    // an unconditioned rule reaches.
    CHECK(buffer.at(0, 20, 0).name.toString() == "minecraft:end_stone");
    CHECK(buffer.at(0, 27, 0).name.toString() == "minecraft:end_stone");
    // The buried air notch — solid rock already crossed above it — keeps
    // its own air untouched, unlike a fluid-only guard would.
    CHECK(buffer.at(0, 22, 0).name.toString() == "minecraft:air");
    // The open sky above the terrain is NOT reachable, which is a second,
    // separate bound from the buried-notch one above: the pass starts at the
    // column's topmost non-air block, so air that was never under anything
    // is not a surface-rule position at all. This used to assert end_stone
    // here, on the reading that "no solid crossed above it yet" was the only
    // gate. Vanilla's End refutes that — see golden_end_test.cpp. The
    // distinction a rule keyed on `water` needs is untouched, because fluid
    // counts as non-air and so is at or below where the scan starts.
    CHECK(buffer.at(0, 31, 0).name.toString() == "minecraft:air");
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
    // "minecraft:plains" at every position the pass reaches — every
    // default-block position, at any depth.
    CHECK(buffer.at(0, -16, 0).name.toString() == "minecraft:podzol");
    CHECK(buffer.at(3, -4, 9).name.toString() == "minecraft:podzol");
    // The water above the plane is not a surface-rule position at all.
    CHECK(buffer.at(3, 4, 9).name.toString() == "minecraft:water");
    // Open sky above the water is not reached at all; see the scan bound in
    // ChunkFiller::applySurfaceRules. What this case is about is the BIOME
    // lookup, and the two positions above exercise it.
    CHECK(buffer.at(15, 31, 15).name.toString() == "minecraft:air");
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

TEST_CASE("above_preliminary_surface's edge carries surfaceDepth - 8 under a constant level",
          "[terrain][filler][surface]") {
    // Built directly rather than through compileFrom()'s shared registry for
    // the same reason the bandlands case above is: this condition reads a
    // surface DEPTH (SPEC §11, measured — the boundary is
    // `preliminary_surface_level + surfaceDepth - 8`), so the two surface
    // noises have to be in the registry.
    const TempTree tree;
    tree.defineSettings("test", flatSettings(false, false));
    tree.defineSurfaceNoises();
    const LoadedSettings loaded = tree.load();
    const auto& settings =
        loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:test"));
    auto wanted = loaded.graph.referencedNoises();
    wanted.push_back(stratum::data::ResourceLocation::parse("minecraft:surface"));
    wanted.push_back(stratum::data::ResourceLocation::parse("minecraft:surface_secondary"));
    const auto noises = stratum::density::NoiseRegistry::create(
        tree.pack(), wanted, 0, stratum::density::RandomSource::Xoroshiro);
    const RuleGraph surface =
        resolveSurface(condition(nlohmann::json{{"type", "minecraft:above_preliminary_surface"}},
                                 block("minecraft:glowstone")));
    const ChunkFiller filler = ChunkFiller::compile(loaded.graph, noises, settings, &surface);
    REQUIRE(filler.surfaceRulesBlockedBy().empty());
    REQUIRE(filler.runsSurfaceRules());

    ChunkBuffer buffer(settings.geometry);
    filler.fill(0, 0, buffer);

    // `preliminary_surface_level` is the flat dimension's constant zero, so
    // the boundary is `surfaceDepth(x, z) - 8` — below the level the
    // condition is named after, and different from column to column. The
    // Executor computes the same depth the filler's own does, so this asks
    // it rather than hard-coding a number the noise parameters would move.
    // A constant level makes the 16-block lattice and the column the same
    // number, so this case is blind to WHERE the level is sampled; the next
    // two cases hold that.
    const auto executor = stratum::surface::Executor::compile(
        surface, noises.worldSeed(), settings.geometry, &noises, settings.seaLevel);
    for (const auto& [x, z] : {std::pair{0, 0}, std::pair{7, 3}, std::pair{15, 15}}) {
        const std::int32_t boundary = executor.surfaceDepth(x, z) - 8;
        INFO("column (" << x << ", " << z << "), boundary " << boundary);
        CHECK(buffer.at(x, boundary - 1, z).name.toString() == "minecraft:stone");
        CHECK(buffer.at(x, boundary, z).name.toString() == "minecraft:glowstone");
        // And well above it, where the old reading and this one agree.
        CHECK(buffer.at(x, -1, z).name.toString() == "minecraft:glowstone");
    }
}

TEST_CASE("the psl lattice: each sample floored, blended, floored again (known answers)",
          "[terrain][filler][surface]") {
    // ChunkFiller::preliminarySurfaceIn on its own, against SPEC §11's
    // formula worked by hand. The filler case below proves the WIRING (where
    // the four samples are taken, in what order, and that the result reaches
    // the condition); this one proves the arithmetic the wiring hands them to.
    const auto at = [](const std::array<double, 4>& samples, std::int32_t offsetX,
                       std::int32_t offsetZ) {
        return ChunkFiller::preliminarySurfaceIn(samples, offsetX, offsetZ);
    };

    // The sample order is (x0,z0), (x1,z0), (x0,z1), (x1,z1): a sample in the
    // wrong slot blends along the wrong axis.
    CHECK(at({0.0, 16.0, 0.0, 0.0}, 5, 0) == 5);
    CHECK(at({0.0, 16.0, 0.0, 0.0}, 0, 5) == 0);
    CHECK(at({0.0, 0.0, 16.0, 0.0}, 5, 0) == 0);
    CHECK(at({0.0, 0.0, 16.0, 0.0}, 0, 5) == 5);

    // At the cell's own corner the value is that sample, floored.
    CHECK(at({-3.5, 9.0, 9.0, 9.0}, 0, 0) == -4);

    // Where the floor falls. Floors -> 0, -1, 0, -1; blended at u = 1/4 that
    // is -0.25, floored -1. Every alternative filler.hpp lists as refused by
    // the server (`f_half` / `f_quart`) gives 0 on this one vector, so the
    // vector alone separates the measured reading from all six.
    const std::array<double, 4> halves{0.5, -0.5, 0.5, -0.5};
    CHECK(at(halves, 4, 0) == -1);
    // Floored only after the blend: 0.25.
    CHECK(stratum::javamath::floorToInt(bilerp(halves, 0.25, 0.0)) == 0);
    // Truncated at the sample (both halves truncate to 0), then after.
    CHECK(stratum::javamath::doubleToInt(bilerp(truncated(halves), 0.25, 0.0)) == 0);
    // Truncated, or rounded, only after the blend.
    CHECK(stratum::javamath::doubleToInt(bilerp(halves, 0.25, 0.0)) == 0);
    CHECK(std::lround(bilerp(halves, 0.25, 0.0)) == 0L);
    // The cell's lower corner, which at offset 4 of 16 is also the nearest.
    CHECK(stratum::javamath::floorToInt(halves[0]) == 0);

    // Mixed signs and both axes at once. Floors -2, -18, -34, -9; u = 3/16
    // gives low -5 and high -29.3125; v = 11/16 gives -21.71484375.
    CHECK(at({-1.25, -17.75, -33.5, -9.0}, 3, 11) == -22);

    // The census SPEC §11 quotes. Every assignment of the three arms
    // {-40, 0, 60} of `probes/apsb/v_psl`'s range_choice to the four samples,
    // at every offset inside a cell: pitch 16 reaches every integer from -40
    // to 60, which is the 101 the server's own band shows, while pitch 4 and
    // 2 leave gaps — and 8 and 32 do not, which is why the pitch was
    // measured by translation (vanilla_psl_lattice_test.cpp) rather than by
    // this count. At 16 the formula is held equal to the engine's helper on
    // every one of those inputs.
    struct Census {
        std::int32_t pitch;
        std::size_t values;
        std::size_t gaps;
    };

    const std::array<double, 3> arms{-40.0, 0.0, 60.0};
    for (const Census& expected :
         {Census{2, 15, 86}, Census{4, 57, 44}, Census{8, 101, 0},
          Census{ChunkFiller::kPreliminarySurfacePitch, 101, 0}, Census{32, 101, 0}}) {
        CAPTURE(expected.pitch);
        std::set<std::int32_t> reached;
        std::size_t helperDisagrees = 0;
        for (std::size_t assignment = 0; assignment < 81; ++assignment) {
            std::array<double, 4> samples{};
            std::size_t digits = assignment;
            for (double& sample : samples) {
                sample = arms.at(digits % 3);
                digits /= 3;
            }
            for (std::int32_t offsetZ = 0; offsetZ < expected.pitch; ++offsetZ) {
                for (std::int32_t offsetX = 0; offsetX < expected.pitch; ++offsetX) {
                    const std::int32_t value =
                        latticeAtPitch(samples, offsetX, offsetZ, expected.pitch);
                    if (expected.pitch == ChunkFiller::kPreliminarySurfacePitch) {
                        helperDisagrees += static_cast<std::size_t>(
                            ChunkFiller::preliminarySurfaceIn(samples, offsetX, offsetZ) != value);
                    }
                    reached.insert(value);
                }
            }
        }
        CHECK(helperDisagrees == 0U);
        REQUIRE_FALSE(reached.empty());
        CHECK(reached.size() == expected.values);
        CHECK(*reached.begin() == -40);
        CHECK(*reached.rbegin() == 60);
        CHECK(101U - reached.size() == expected.gaps);
    }
}

TEST_CASE("above_preliminary_surface reads the 16-block lattice through the whole filler, not "
          "the column",
          "[terrain][filler][surface]") {
    // The fixture-free guard on the WIRING of SPEC §11's psl lattice. The
    // server measurement lives in vanilla_psl_lattice_test.cpp and the
    // engine's own reproduction of it in golden_overworld_test.cpp, and both
    // need probe worlds or region files CI never generates; the
    // constant-level case above is blind to it by construction. Here the
    // entry varies by at least 8 blocks inside every chunk (required below),
    // so a filler that samples it anywhere else, in any other order, or
    // floors it anywhere else, moves the band's lower edge in a counted
    // share of the columns — and the plane being solid stone from -64 to -1,
    // a moved edge is a different block.
    //
    // The reference is computed beside the engine, not by it: a separate
    // Interpreter for the raw entry at y = 0, the surface Executor for the
    // depth, and the lattice cell found per column through floorDiv rather
    // than from the chunk = cell shortcut the filler takes. It shares
    // preliminarySurfaceIn and the interpreter with the engine, so it proves
    // where the samples go, not that the arithmetic is right — the case
    // above does that.
    const TempTree tree;
    tree.defineSettings("test", varyingPslSettings());
    tree.defineSurfaceNoises();
    tree.defineNoise("psl_field", R"({"firstOctave": -3, "amplitudes": [1.0, 1.0]})");
    const LoadedSettings loaded = tree.load();
    const auto& settings =
        loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:test"));
    auto wanted = loaded.graph.referencedNoises();
    wanted.push_back(stratum::data::ResourceLocation::parse("minecraft:surface"));
    wanted.push_back(stratum::data::ResourceLocation::parse("minecraft:surface_secondary"));
    const auto noises = stratum::density::NoiseRegistry::create(
        tree.pack(), wanted, 0, stratum::density::RandomSource::Xoroshiro);
    const RuleGraph surface =
        resolveSurface(condition(nlohmann::json{{"type", "minecraft:above_preliminary_surface"}},
                                 block("minecraft:glowstone")));
    const ChunkFiller filler = ChunkFiller::compile(loaded.graph, noises, settings, &surface);
    REQUIRE(filler.surfaceRulesBlockedBy().empty());
    REQUIRE(filler.runsSurfaceRules());

    const auto executor = stratum::surface::Executor::compile(
        surface, noises.worldSeed(), settings.geometry, &noises, settings.seaLevel);
    const stratum::density::Interpreter reference(loaded.graph, noises);
    const auto entry = settings.router.at(RouterEntry::PreliminarySurfaceLevel);
    const auto raw = [&](std::int32_t x, std::int32_t z) {
        return reference.evaluate(entry, stratum::density::Point{.x = x, .y = 0, .z = z});
    };
    // The four samples of the cell whose low corner is (x0, z0), @p span
    // blocks on a side, in preliminarySurfaceIn's order.
    const auto cell = [&](std::int32_t x0, std::int32_t z0, std::int32_t span) {
        return std::array<double, 4>{raw(x0, z0), raw(x0 + span, z0), raw(x0, z0 + span),
                                     raw(x0 + span, z0 + span)};
    };

    // Readings a regression could bring back, each scored by how many
    // columns it would have put the edge somewhere else. The per-column read
    // is what the engine did before pipeline engine v2.
    enum Reading : std::size_t {
        kPerColumn,      // floor(R(x, 0, z))
        kTransposed,     // the (16, 0) and (0, 16) samples swapped
        kFarCornerAt15,  // the chunk's own last column as the far sample, /15
        kSingleFloor,    // floored only after the blend
        kTruncated,      // truncated at the sample and after the blend
        kTruncatingCell, // the cell found by truncating division, (x / 16) * 16
        kReadings
    };

    constexpr std::int32_t kPitch = ChunkFiller::kPreliminarySurfacePitch;
    const std::int32_t minY = settings.geometry.minY;
    std::array<std::int32_t, kReadings> separates{};
    std::int32_t columns = 0;
    std::int32_t negativeColumns = 0;
    std::int32_t exact = 0;
    std::int32_t steps = 0;
    for (const auto& [chunkX, chunkZ] : {std::pair{0, 0}, std::pair{-1, -1}, std::pair{3, -2}}) {
        CAPTURE(chunkX, chunkZ);
        ChunkBuffer buffer(settings.geometry);
        filler.fill(chunkX, chunkZ, buffer);
        const bool negative = chunkX < 0 || chunkZ < 0;

        std::int32_t lowestColumn = std::numeric_limits<std::int32_t>::max();
        std::int32_t highestColumn = std::numeric_limits<std::int32_t>::min();
        for (int localZ = 0; localZ < kPitch; ++localZ) {
            for (int localX = 0; localX < kPitch; ++localX) {
                const std::int32_t x = (chunkX * kPitch) + localX;
                const std::int32_t z = (chunkZ * kPitch) + localZ;
                CAPTURE(x, z);
                const std::int32_t depth = executor.surfaceDepth(x, z);
                const auto edge = [depth](std::int32_t psl) { return psl + depth - 8; };

                const std::int32_t x0 = stratum::javamath::floorDiv(x, kPitch) * kPitch;
                const std::int32_t z0 = stratum::javamath::floorDiv(z, kPitch) * kPitch;
                const auto samples = cell(x0, z0, kPitch);
                const std::int32_t expected =
                    edge(ChunkFiller::preliminarySurfaceIn(samples, x - x0, z - z0));
                // The edge must land inside the plane, or this column could
                // not show it.
                REQUIRE(minY < expected);
                REQUIRE(expected <= -1);

                // The lowest glowstone, and whether the column is a clean
                // step: stone below it, glowstone from it up to the plane's
                // top.
                std::int32_t observed = 0;
                for (std::int32_t y = minY; y <= -1; ++y) {
                    if (buffer.at(localX, y, localZ).name.toString() == "minecraft:glowstone") {
                        observed = y;
                        break;
                    }
                }
                bool step = observed != 0;
                for (std::int32_t y = minY; y <= -1 && step; ++y) {
                    step = buffer.at(localX, y, localZ).name.toString() ==
                           (y < observed ? "minecraft:stone" : "minecraft:glowstone");
                }
                ++columns;
                negativeColumns += static_cast<std::int32_t>(negative);
                exact += static_cast<std::int32_t>(observed == expected);
                steps += static_cast<std::int32_t>(step);

                const std::int32_t perColumn = stratum::javamath::floorToInt(raw(x, z));
                lowestColumn = std::min(lowestColumn, perColumn);
                highestColumn = std::max(highestColumn, perColumn);
                if (localX == 0 && localZ == 0) {
                    // At the cell's own corner the two readings are one.
                    CHECK(edge(perColumn) == expected);
                }

                const double u = static_cast<double>(x - x0) / kPitch;
                const double v = static_cast<double>(z - z0) / kPitch;
                std::array<std::int32_t, kReadings> predicted{};
                predicted[kPerColumn] = edge(perColumn);
                predicted[kTransposed] = edge(ChunkFiller::preliminarySurfaceIn(
                    {samples[0], samples[2], samples[1], samples[3]}, x - x0, z - z0));
                predicted[kFarCornerAt15] = edge(stratum::javamath::floorToInt(bilerp(
                    floored(cell(x0, z0, kPitch - 1)), static_cast<double>(x - x0) / (kPitch - 1),
                    static_cast<double>(z - z0) / (kPitch - 1))));
                predicted[kSingleFloor] =
                    edge(stratum::javamath::floorToInt(bilerp(samples, u, v)));
                predicted[kTruncated] =
                    edge(stratum::javamath::doubleToInt(bilerp(truncated(samples), u, v)));
                // Deliberately the WRONG division: the regression this
                // reading stands for is exactly a truncating cell index.
                const std::int32_t xt = stratum::javamath::truncDiv(x, kPitch) * kPitch;
                const std::int32_t zt = stratum::javamath::truncDiv(z, kPitch) * kPitch;
                predicted[kTruncatingCell] =
                    edge(ChunkFiller::preliminarySurfaceIn(cell(xt, zt, kPitch), x - xt, z - zt));
                for (std::size_t reading = 0; reading < kReadings; ++reading) {
                    // A truncating division is floorDiv at x, z >= 0.
                    if (reading == kTruncatingCell && !negative) {
                        continue;
                    }
                    separates.at(reading) +=
                        static_cast<std::int32_t>(predicted.at(reading) != observed);
                }
            }
        }
        // The field must actually vary inside the chunk, or every reading
        // would agree and this case would pass on any of them.
        REQUIRE(highestColumn - lowestColumn >= 8);
    }

    // The semantic claim: every column's edge is where the lattice puts it.
    CHECK(columns == 768);
    CHECK(exact == columns);
    CHECK(steps == columns);

    // And how many columns each rejected reading would have moved: the
    // power of this case, kept separate from the claim above. The floors are
    // what keep it from going vacuous under a smoother field: every reading
    // must move some column, and the per-column read most of them.
    CHECK(negativeColumns == 512);
    CHECK(separates[kPerColumn] >= 512);
    for (std::size_t reading = 0; reading < kReadings; ++reading) {
        CAPTURE(reading);
        CHECK(separates.at(reading) > 0);
    }
    // The exact counts are pure engine arithmetic over a fixed noise and a
    // fixed surface depth, so they double as a cross-architecture check: a
    // change to either noise moves them, and nothing else should. Truncation
    // moves every column because the field is negative throughout, where
    // truncating rounds up; the truncating cell index is scored on the 512
    // columns of the two chunks with a negative coordinate only, since it is
    // floorDiv everywhere else.
    CHECK(separates[kPerColumn] == 672);
    CHECK(separates[kTransposed] == 304);
    CHECK(separates[kFarCornerAt15] == 369);
    CHECK(separates[kSingleFloor] == 363);
    CHECK(separates[kTruncated] == 768);
    CHECK(separates[kTruncatingCell] == 488);
}

TEST_CASE("a tree reading a surface depth without minecraft:surface built is blocked, not crashed",
          "[terrain][filler][surface]") {
    const TempTree tree;
    tree.defineSettings("test", flatSettings(false, false));
    const LoadedSettings loaded = tree.load();
    const RuleGraph surface =
        resolveSurface(condition(nlohmann::json{{"type", "minecraft:above_preliminary_surface"}},
                                 block("minecraft:glowstone")));
    // compileFrom()'s shared registry never asked for the surface noises.
    const ChunkFiller filler = compileFrom(tree, loaded, &surface);

    CHECK_FALSE(filler.runsSurfaceRules());
    REQUIRE(filler.surfaceRulesBlockedBy().size() == 1U);
    CHECK_THAT(filler.surfaceRulesBlockedBy().front(), ContainsSubstring("minecraft:surface"));

    ChunkBuffer buffer(
        loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:test")).geometry);
    filler.fill(0, 0, buffer);
    CHECK(buffer.at(0, -16, 0).name.toString() == "minecraft:stone");
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

TEST_CASE("a surface rule writes over the default block and nothing else",
          "[terrain][filler][surface]") {
    // The fixture-free guard for what golden_overworld_test.cpp measured on
    // the real overworld (SPEC §11): an unconditioned rule — the shape of the
    // overworld's own deepslate gradient — replaces the default block at any
    // depth, and leaves lava, open water and air exactly as the first pass
    // left them. The pass used to repaint lava (categorised Solid, because it
    // is not this dimension's default_fluid) and the column's open water.
    const TempTree tree;
    tree.defineSettings("test", lavaSeaSettings());
    const LoadedSettings loaded = tree.load();
    const RuleGraph surface = resolveSurface(block("minecraft:diamond_block"));
    const ChunkFiller filler = compileFrom(tree, loaded, &surface);
    REQUIRE(filler.runsSurfaceRules());
    ChunkBuffer buffer(
        loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:test")).geometry);
    filler.fill(0, 0, buffer);
    for (int x : {0, 7, 15}) {
        CAPTURE(x);
        CHECK(buffer.at(x, -64, x).name.toString() == "minecraft:diamond_block");
        CHECK(buffer.at(x, -61, x).name.toString() == "minecraft:diamond_block");
        // The aquifers-off global lava sea: below min(-54, sea_level).
        CHECK(buffer.at(x, -60, x).name.toString() == "minecraft:lava");
        CHECK(buffer.at(x, -55, x).name.toString() == "minecraft:lava");
        // ... and default_fluid from -54 up to sea_level.
        CHECK(buffer.at(x, -54, x).name.toString() == "minecraft:water");
        CHECK(buffer.at(x, 7, x).name.toString() == "minecraft:water");
        CHECK(buffer.at(x, 8, x).name.toString() == "minecraft:air");
    }
}

TEST_CASE("a chunk's flat_cache window is its own columns and one quart beyond",
          "[terrain][aquifer]") {
    // Twenty columns a side from the chunk's own corner — the extent is a
    // choice among three the goldens cannot separate (filler.hpp); pinned so
    // that changing it is a decision, not a drift.
    constexpr auto window = ChunkFiller::flatCacheWindow(3, 3);
    CHECK(window.minX == 48);
    CHECK(window.maxX == 67);
    CHECK(window.minZ == 48);
    CHECK(window.maxZ == 67);

    // The measurement turned on one source centre: off chunk (3, 3)'s
    // window, where the server read it wet, and inside chunk (3, 4)'s, where
    // it read it dry.
    CHECK_FALSE(ChunkFiller::flatCacheWindow(3, 3).covers(57, 70));
    CHECK(ChunkFiller::flatCacheWindow(3, 4).covers(57, 70));

    // Below zero the window still starts at the chunk's own corner.
    constexpr auto negative = ChunkFiller::flatCacheWindow(-1, -2);
    CHECK(negative.minX == -16);
    CHECK(negative.maxX == 3);
    CHECK(negative.minZ == -32);
    CHECK(negative.maxZ == -13);
}

TEST_CASE("a legacy random source refuses aquifers and ore veins by name",
          "[terrain][filler][aquifer]") {
    // Both are positional randoms drawn from the dimension's declared source,
    // and no vanilla legacy dimension enables either, so nothing on disk says
    // what they should be. No vanilla pack reaches these throws — which is
    // exactly why they need a test: a regression would quietly generate
    // with the modern derivation instead of failing.
    for (const bool aquifers : {true, false}) {
        const bool oreVeins = !aquifers;
        INFO("aquifers " << aquifers << ", ore veins " << oreVeins);
        nlohmann::json settings = flatSettings(aquifers, oreVeins);
        settings["legacy_random_source"] = true;
        const TempTree tree;
        tree.defineSettings("test", settings);
        const LoadedSettings loaded = tree.load();
        // The flat router names no noise, so the legacy registry is empty
        // and builds: the filler's own refusal is the one that fires.
        const auto noises =
            stratum::density::NoiseRegistry::create(tree.pack(), loaded.graph.referencedNoises(), 0,
                                                    stratum::density::RandomSource::Legacy);
        const auto& dimension =
            loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:test"));
        CHECK_THROWS_WITH(ChunkFiller::compile(loaded.graph, noises, dimension),
                          ContainsSubstring(aquifers ? "aquifers_enabled" : "ore_veins_enabled") &&
                              ContainsSubstring("legacy_random_source"));
    }
}

TEST_CASE("aquifers over a default fluid other than water are refused by name",
          "[terrain][filler][aquifer]") {
    // Q6.3's exception is written for WATER over the lava sea and Q6.4's
    // mixed-type constant for lava against WATER; with any other default
    // fluid those part from "the dimension's default fluid", and nothing has
    // measured which the server takes. Refused, not guessed.
    for (const char* fluid : {"minecraft:lava", "minecraft:air"}) {
        INFO("default_fluid " << fluid);
        nlohmann::json settings = flatSettings(/*aquifers=*/true, /*oreVeins=*/false);
        settings["default_fluid"] = nlohmann::json{{"Name", fluid}};
        const TempTree tree;
        tree.defineSettings("test", settings);
        const LoadedSettings loaded = tree.load();
        const auto noises =
            stratum::density::NoiseRegistry::create(tree.pack(), loaded.graph.referencedNoises(), 0,
                                                    stratum::density::RandomSource::Xoroshiro);
        const auto& dimension =
            loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:test"));
        CHECK_THROWS_WITH(ChunkFiller::compile(loaded.graph, noises, dimension),
                          ContainsSubstring("aquifers_enabled") &&
                              ContainsSubstring(std::string("default_fluid ") + fluid));
    }
    // The control: the same settings with water compile.
    const TempTree tree;
    tree.defineSettings("test", flatSettings(/*aquifers=*/true, /*oreVeins=*/false));
    const LoadedSettings loaded = tree.load();
    const auto noises = stratum::density::NoiseRegistry::create(
        tree.pack(), loaded.graph.referencedNoises(), 0, stratum::density::RandomSource::Xoroshiro);
    CHECK_NOTHROW(ChunkFiller::compile(
        loaded.graph, noises,
        loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:test"))));
}

namespace {

/// `tools/analysis/aquifer-lavarun-probe.sh`'s terrain G1, corner for corner:
/// stone 319..4, an open pool 3..-24 (air to -12, fluid below), stone
/// -25..-32, an enclosed pool -33..-40, stone -41..-58, the lava sea
/// -59..-64. A constant plus one `y_clamped_gradient` per 8-block cell, each
/// boundary at a half-integer y; the script derives these values and checks
/// its own layout before it writes them.
[[nodiscard]] nlohmann::json lavaRunTerrain() {
    nlohmann::json node{{"type", "minecraft:constant"}, {"argument", -5.5}};
    const std::array<std::pair<int, double>, 9> steps{{{-64, 8.0},
                                                       {-56, 1685.0},
                                                       {-48, -1800.0},
                                                       {-40, 120.0},
                                                       {-32, -8.0},
                                                       {-24, -7.5},
                                                       {-8, 4.5},
                                                       {0, 8.0},
                                                       {8, 3.5}}};
    for (const auto& [fromY, step] : steps) {
        node = nlohmann::json{{"type", "minecraft:add"},
                              {"argument1", node},
                              {"argument2",
                               {{"type", "minecraft:y_clamped_gradient"},
                                {"from_y", fromY},
                                {"to_y", fromY + 8},
                                {"from_value", 0.0},
                                {"to_value", step}}}};
    }
    return node;
}

/// The probe's aquifer: the fluid-type probe's arm P, so every source's
/// level is -12 and no barrier is ever placed; `lava` 0.5 types every source
/// lava, 0.0 water.
[[nodiscard]] nlohmann::json lavaRunSettings(bool lavaPools, const nlohmann::json& surfaceRule) {
    nlohmann::json settings = flatSettings(/*aquifers=*/true, /*oreVeins=*/false);
    settings["sea_level"] = -16;
    settings["default_fluid"] = {{"Name", "minecraft:water"}, {"Properties", {{"level", "0"}}}};
    settings["noise"] = {
        {"min_y", -64}, {"height", 384}, {"size_horizontal", 1}, {"size_vertical", 2}};
    settings["noise_router"]["barrier"] = -2.0;
    settings["noise_router"]["lava"] = lavaPools ? 0.5 : 0.0;
    settings["noise_router"]["preliminary_surface_level"] = -12.0;
    settings["noise_router"]["fluid_level_floodedness"] = 0.5;
    settings["noise_router"]["fluid_level_spread"] = 6.0;
    settings["noise_router"]["final_density"] = lavaRunTerrain();
    settings["surface_rule"] = surfaceRule;
    return settings;
}

constexpr std::array<const char*, 16> kWool{
    "white",      "orange", "magenta", "light_blue", "yellow", "lime",  "pink", "gray",
    "light_gray", "cyan",   "purple",  "blue",       "brown",  "green", "red",  "black"};

[[nodiscard]] std::string wool(std::size_t k) {
    return std::string("minecraft:") + kWool.at(k) + "_wool";
}

/// The probe's ladders: sixteen rungs, the k-th placing the k-th wool, so a
/// stone block shows its 0-based depth in the run (`floor`, `ceiling`) or how
/// far below the water height it sits (`water`), and stays stone past 15.
[[nodiscard]] nlohmann::json woolLadder(const std::string& kind) {
    nlohmann::json rungs = nlohmann::json::array();
    for (std::size_t k = 0; k < kWool.size(); ++k) {
        const auto offset = static_cast<int>(k);
        const nlohmann::json test = kind == "water"
                                        ? nlohmann::json{{"type", "minecraft:water"},
                                                         {"offset", -offset},
                                                         {"surface_depth_multiplier", 0},
                                                         {"add_stone_depth", false}}
                                        : nlohmann::json{{"type", "minecraft:stone_depth"},
                                                         {"offset", offset},
                                                         {"add_surface_depth", false},
                                                         {"secondary_depth_range", 0},
                                                         {"surface_type", kind}};
        rungs.push_back(condition(test, block(wool(k))));
    }
    return nlohmann::json{{"type", "minecraft:sequence"}, {"sequence", rungs}};
}

/// One chunk of the lava-run terrain under @p ladder, column (0, 0).
[[nodiscard]] std::vector<std::string> lavaRunColumn(bool lavaPools, const std::string& ladder) {
    const TempTree tree;
    tree.defineSettings("test", lavaRunSettings(lavaPools, woolLadder(ladder)));
    const LoadedSettings loaded = tree.load();
    const RuleGraph surface = resolveSurface(woolLadder(ladder));
    const ChunkFiller filler = compileFrom(tree, loaded, &surface);
    REQUIRE(filler.runsSurfaceRules());
    ChunkBuffer buffer(
        loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:test")).geometry);
    filler.fill(0, 0, buffer);
    std::vector<std::string> column;
    for (std::int32_t y = -64; y < 320; ++y) {
        column.push_back(buffer.at(0, y, 0).name.toString());
    }
    return column;
}

[[nodiscard]] const std::string& at(const std::vector<std::string>& column, std::int32_t y) {
    return column.at(static_cast<std::size_t>(y + 64));
}

} // namespace

TEST_CASE("the lava-run probe's terrain fills as its script records it",
          "[terrain][filler][aquifer][surface]") {
    for (const bool lavaPools : {true, false}) {
        INFO((lavaPools ? "lava" : "water") << " pools");
        const TempTree tree;
        tree.defineSettings("test", lavaRunSettings(lavaPools, block("minecraft:stone")));
        const LoadedSettings loaded = tree.load();
        const ChunkFiller filler = compileFrom(tree, loaded);
        ChunkBuffer buffer(
            loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:test")).geometry);
        filler.fill(0, 0, buffer);
        const std::string pool = lavaPools ? "minecraft:lava" : "minecraft:water";
        long long wrong = 0;
        for (int x = 0; x < 16; ++x) {
            for (int z = 0; z < 16; ++z) {
                for (std::int32_t y = -64; y < 320; ++y) {
                    std::string expected = "minecraft:stone";
                    if (y <= -59) {
                        expected = "minecraft:lava";
                    } else if ((y >= -40 && y <= -33) || (y >= -24 && y <= -13)) {
                        expected = pool;
                    } else if (y >= -12 && y <= 3) {
                        expected = "minecraft:air";
                    }
                    if (buffer.at(x, y, z).name.toString() != expected && ++wrong <= 10) {
                        UNSCOPED_INFO("x " << x << " y " << y << " z " << z << ": "
                                           << buffer.at(x, y, z).name.toString());
                    }
                }
            }
        }
        CHECK(wrong == 0);
    }
}

TEST_CASE("the top-down stone-depth run holds through lava as through water",
          "[terrain][filler][aquifer][surface]") {
    // The fixture-free guard for what aquifer-lavarun-probe.sh measured
    // (SPEC §11): top down, lava neither counts toward the run nor breaks it,
    // exactly as water. Cave A's 12 blocks of pool sit under air, so the
    // stone below them starts at depth 0 whatever the pool does — unless the
    // pool COUNTS, which is what the filler did while it called lava Solid
    // (depth 12 there). Cave B's enclosed pool then carries the run on
    // (depth 8 at its floor) where a reset would restart it at 0.
    for (const bool lavaPools : {true, false}) {
        INFO((lavaPools ? "lava" : "water") << " pools");
        const std::vector<std::string> column = lavaRunColumn(lavaPools, "floor");
        CHECK(at(column, 319) == wool(0));
        CHECK(at(column, -25) == wool(0));
        CHECK(at(column, -32) == wool(7));
        CHECK(at(column, -41) == wool(8));
        CHECK(at(column, -48) == wool(15));
        CHECK(at(column, -49) == "minecraft:stone");
        CHECK(at(column, -58) == "minecraft:stone");
    }
}

TEST_CASE("the bottom-up stone-depth run resets on every fluid, water included",
          "[terrain][filler][aquifer][surface]") {
    // Bottom up, fluid resets the run as air does — lava of the sea, lava of
    // the lattice and water alike (SPEC §11). The lava sea at the world's
    // floor would put depth 6 at y -58 had it counted; cave B's enclosed pool
    // would leave y -32 stone had it held (the run there would be past 16).
    for (const bool lavaPools : {true, false}) {
        INFO((lavaPools ? "lava" : "water") << " pools");
        const std::vector<std::string> column = lavaRunColumn(lavaPools, "ceiling");
        CHECK(at(column, -58) == wool(0));
        CHECK(at(column, -43) == wool(15));
        CHECK(at(column, -42) == "minecraft:stone");
        CHECK(at(column, -32) == wool(0));
        CHECK(at(column, -25) == wool(7));
        CHECK(at(column, 4) == wool(0));
        CHECK(at(column, 19) == wool(15));
        CHECK(at(column, 20) == "minecraft:stone");
    }
}

TEST_CASE("lava latches the water height as water does", "[terrain][filler][aquifer][surface]") {
    // A column whose only fluid is lava has a water height, one above its
    // topmost lava block (-13), exactly as with water (SPEC §11). While the
    // filler called lava Solid the column had none, and `water` was true
    // everywhere: rung 0 on every stone block.
    for (const bool lavaPools : {true, false}) {
        INFO((lavaPools ? "lava" : "water") << " pools");
        const std::vector<std::string> column = lavaRunColumn(lavaPools, "water");
        CHECK(at(column, 4) == wool(0));
        CHECK(at(column, -25) == wool(13));
        CHECK(at(column, -27) == wool(15));
        CHECK(at(column, -28) == "minecraft:stone");
        CHECK(at(column, -41) == "minecraft:stone");
    }
}
