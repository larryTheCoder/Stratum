// Stratum — what the fill's block loop reads of a block's height.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// ChunkFiller works out at compile, for every height of the dimension, what a
// block's y alone decides — the global picker's block where the density is not
// solid and the aquifer is not asked (lava under min(-54, sea_level), the
// default fluid under sea_level, air above), whether veins are read on solid
// ground, and at what richness — and fill() looks them up by y - min_y. This
// holds every block of a chunk to the rule written out here, on dimensions
// whose floor, height and sea differ: a floor under the lava sea's top and
// one above the sea, a sea at the floor, one under the lava line and one
// above the dimension's top, and veins on and off.
#include "support/temp_path.hpp"

#include <stratum/aquifer/lattice.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/ore/vein.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/terrain/filler.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

using stratum::data::ResourceLocation;
using stratum::settings::BlockState;
using stratum::settings::RouterEntry;
using stratum::terrain::ChunkBuffer;
using stratum::terrain::ChunkFiller;

namespace {

class HeightsTree {
public:
    HeightsTree() : path_(std::filesystem::temp_directory_path() / uniqueName()) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_ / "density_function");
        std::filesystem::create_directories(path_ / "noise");
        std::filesystem::create_directories(path_ / "noise_settings");
    }

    HeightsTree(const HeightsTree&) = delete;
    HeightsTree& operator=(const HeightsTree&) = delete;
    HeightsTree(HeightsTree&&) = delete;
    HeightsTree& operator=(HeightsTree&&) = delete;

    ~HeightsTree() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    void defineNoise(std::string_view name, std::string_view json) const {
        std::ofstream out(path_ / "noise" / (std::string(name) + ".json"));
        out << json;
    }

    void defineSettings(std::string_view name, const nlohmann::json& json) const {
        std::ofstream out(path_ / "noise_settings" / (std::string(name) + ".json"));
        out << json.dump();
    }

    [[nodiscard]] stratum::data::Pack pack() const { return stratum::data::Pack::open(path_); }

private:
    [[nodiscard]] static std::string uniqueName() {
        return stratum::test::tempName("stratum-fill-heights-test");
    }

    std::filesystem::path path_;
};

struct Dimension {
    std::string_view name;
    std::int32_t minY = 0;
    std::int32_t height = 0;
    std::int32_t seaLevel = 0;
    bool veins = false;
};

/// A dimension whose density is a noise over a plane through its middle, so
/// that every height of it holds ground that is solid and ground that is
/// not; aquifers off, so the global picker stands wherever it is not solid.
[[nodiscard]] nlohmann::json heightsSettings(const Dimension& dimension) {
    nlohmann::json router = nlohmann::json::object();
    for (std::size_t i = 0; i < stratum::settings::kRouterEntryCount; ++i) {
        router[std::string(stratum::settings::routerEntryName(static_cast<RouterEntry>(i)))] = 0.0;
    }
    const nlohmann::json plane{{"type", "minecraft:y_clamped_gradient"},
                               {"from_y", dimension.minY},
                               {"to_y", dimension.minY + dimension.height},
                               {"from_value", 0.4},
                               {"to_value", -0.4}};
    const nlohmann::json field{{"type", "minecraft:noise"},
                               {"noise", "minecraft:field"},
                               {"xz_scale", 1.0},
                               {"y_scale", 1.0}};
    router["final_density"] =
        nlohmann::json{{"type", "minecraft:add"}, {"argument1", plane}, {"argument2", field}};
    return nlohmann::json{
        {"default_block", {{"Name", "minecraft:stone"}}},
        {"default_fluid", {{"Name", "minecraft:water"}}},
        {"sea_level", dimension.seaLevel},
        {"disable_mob_generation", false},
        {"aquifers_enabled", false},
        {"ore_veins_enabled", dimension.veins},
        {"legacy_random_source", false},
        {"noise",
         {{"min_y", dimension.minY},
          {"height", dimension.height},
          {"size_horizontal", 1},
          {"size_vertical", 1}}},
        {"noise_router", router},
        {"spawn_target", nlohmann::json::array()},
        {"surface_rule", {{"type", "minecraft:bandlands"}}},
    };
}

[[nodiscard]] BlockState named(std::string_view name) {
    return BlockState{.name = ResourceLocation::parse(name), .properties = {}};
}

} // namespace

TEST_CASE("the fill reads each height's picker and vein range by the height's own index",
          "[terrain][filler]") {
    constexpr std::array<Dimension, 5> kDimensions{{
        {.name = "floor_under_lava_sea", .minY = -64, .height = 128, .seaLevel = 8, .veins = true},
        {.name = "sea_at_floor", .minY = -64, .height = 64, .seaLevel = -64, .veins = true},
        {.name = "floor_above_sea", .minY = 16, .height = 96, .seaLevel = 8, .veins = false},
        {.name = "sea_under_lava_line", .minY = -96, .height = 96, .seaLevel = -70, .veins = false},
        {.name = "sea_over_top", .minY = -32, .height = 48, .seaLevel = 300, .veins = true},
    }};
    const HeightsTree tree;
    tree.defineNoise("field", R"({"firstOctave": -3, "amplitudes": [1.0, 0.5]})");
    for (const Dimension& dimension : kDimensions) {
        tree.defineSettings(dimension.name, heightsSettings(dimension));
    }
    const stratum::data::Pack pack = tree.pack();
    const stratum::settings::LoadedSettings loaded = stratum::settings::loadAll(pack);
    const stratum::density::NoiseRegistry noises = stratum::density::NoiseRegistry::create(
        pack, loaded.graph.referencedNoises(), 0, stratum::density::RandomSource::Xoroshiro);

    const BlockState stone = named("minecraft:stone");
    const BlockState water = named("minecraft:water");
    const BlockState air = named("minecraft:air");
    const BlockState lava{.name = ResourceLocation::parse("minecraft:lava"),
                          .properties = {{"level", "0"}}};
    const std::array<BlockState, 6> veinBlocks{named("minecraft:granite"),
                                               named("minecraft:copper_ore"),
                                               named("minecraft:raw_copper_block"),
                                               named("minecraft:tuff"),
                                               named("minecraft:deepslate_iron_ore"),
                                               named("minecraft:raw_iron_block")};

    for (const Dimension& dimension : kDimensions) {
        CAPTURE(dimension.name);
        const auto& settings =
            loaded.settings.at(ResourceLocation::parse("minecraft:" + std::string(dimension.name)));
        const ChunkFiller filler = ChunkFiller::compile(loaded.graph, noises, settings);
        const std::int32_t lambda = stratum::aquifer::lambdaLevel(dimension.seaLevel);
        std::size_t mismatched = 0;
        std::size_t solid = 0;
        std::size_t veined = 0;
        std::array<std::size_t, 3> open{}; // lava, water, air
        for (const std::int32_t chunk : {0, -3}) {
            ChunkBuffer buffer(settings.geometry);
            filler.fill(chunk, -chunk, buffer);
            for (std::int32_t y = dimension.minY; y < dimension.minY + dimension.height; ++y) {
                for (int z = 0; z < 16; ++z) {
                    for (int x = 0; x < 16; ++x) {
                        const BlockState& got = buffer.at(x, y, z);
                        if (got == stone) {
                            ++solid;
                            continue;
                        }
                        if (std::ranges::find(veinBlocks, got) != veinBlocks.end()) {
                            // Veins only where they are read: on, and in a
                            // vein's range.
                            ++veined;
                            mismatched +=
                                dimension.veins && stratum::ore::inAnyVeinRange(y) ? 0U : 1U;
                            continue;
                        }
                        const BlockState* expected = &air;
                        std::size_t kind = 2;
                        if (y < lambda) {
                            expected = &lava;
                            kind = 0;
                        } else if (y < dimension.seaLevel) {
                            expected = &water;
                            kind = 1;
                        }
                        ++open.at(kind);
                        mismatched += got == *expected ? 0U : 1U;
                    }
                }
            }
        }
        CAPTURE(solid, veined, open[0], open[1], open[2]);
        CHECK(mismatched == 0U);
        CHECK(solid > 0U);
        // Every kind of open ground the dimension's heights can hold is there.
        CHECK((open[0] > 0U) == (lambda > dimension.minY));
        CHECK((open[2] > 0U) == (dimension.seaLevel < dimension.minY + dimension.height));
    }
}
