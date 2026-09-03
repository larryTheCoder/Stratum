// Stratum — the chunk filler.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#include <stratum/data/pack.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/settings/noise_settings.hpp>
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

using Catch::Matchers::ContainsSubstring;
using stratum::data::Pack;
using stratum::settings::LoadedSettings;
using stratum::settings::RouterEntry;
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
[[nodiscard]] nlohmann::json flatSettings(bool aquifers, bool oreVeins) {
    nlohmann::json router = nlohmann::json::object();
    for (std::size_t i = 0; i < stratum::settings::kRouterEntryCount; ++i) {
        router[std::string(stratum::settings::routerEntryName(static_cast<RouterEntry>(i)))] = 0.0;
    }
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

[[nodiscard]] ChunkFiller compileFrom(const TempTree& tree, const LoadedSettings& loaded) {
    const auto& settings =
        loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:test"));
    static stratum::density::NoiseRegistry noises = stratum::density::NoiseRegistry::create(
        tree.pack(), loaded.graph.referencedNoises(), 0, stratum::density::RandomSource::Xoroshiro);
    return ChunkFiller::compile(loaded.graph, noises, settings);
}

} // namespace

TEST_CASE("a dimension with aquifers is refused, by name", "[terrain][filler]") {
    // The whole reason this is a refusal and not a best effort: where aquifers
    // run, the block is not a function of the density, and filling as though
    // it were floods every cave in the world. SPEC §8 puts that in the most
    // severe class there is.
    const TempTree tree;
    tree.defineSettings("test", flatSettings(/*aquifers=*/true, /*oreVeins=*/false));
    const LoadedSettings loaded = tree.load();
    CHECK_THROWS_WITH(compileFrom(tree, loaded),
                      ContainsSubstring("aquifers_enabled") && ContainsSubstring("flood"));
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
