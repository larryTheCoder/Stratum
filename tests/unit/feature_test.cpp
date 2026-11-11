// Stratum — the decoration framework on a synthetic world.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The framework's rules are confirmed on the server's own chunks
// (tests/conformance/vanilla_feature_probe_test.cpp). These cases pin them
// where no fixture is needed: the seeds against positions the server stored,
// the order on lists small enough to read, what is refused and what is only
// left unrun, and the modifiers on a flat world whose answers are obvious.
#include <stratum/data/resource_location.hpp>
#include <stratum/feature/feature.hpp>
#include <stratum/rng/worldgen_random.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/terrain/filler.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using stratum::data::ResourceLocation;
using stratum::feature::FeatureError;
using stratum::feature::FeatureSet;
using stratum::feature::FeatureSources;
using stratum::rng::WorldgenRandom;
using stratum::settings::BlockState;
using Json = nlohmann::json;

namespace {

constexpr std::int64_t kSeed = 42;
constexpr std::int32_t kSurface = 64;
const stratum::settings::NoiseGeometry kGeometry{
    .minY = -64, .height = 384, .sizeHorizontal = 1, .sizeVertical = 2};

[[nodiscard]] ResourceLocation id(const std::string& text) {
    return ResourceLocation::parse(text);
}

[[nodiscard]] BlockState block(const std::string& name) {
    BlockState state;
    state.name = id(name);
    return state;
}

/// A marker: `simple_block` of @p wool behind @p placement.
[[nodiscard]] Json marker(const std::string& wool, Json placement) {
    return {{"feature",
             {{"type", "minecraft:simple_block"},
              {"config",
               {{"to_place",
                 {{"type", "minecraft:simple_state_provider"},
                  {"state", {{"Name", "minecraft:" + wool + "_wool"}}}}}}}}},
            {"placement", std::move(placement)}};
}

[[nodiscard]] Json fixedAt(const std::int32_t x, const std::int32_t y, const std::int32_t z) {
    return {{"type", "minecraft:fixed_placement"}, {"positions", Json::array({{x, y, z}})}};
}

/// Sources holding @p placed and one biome per entry of @p biomes, each with
/// one step listing the given features.
[[nodiscard]] FeatureSources
sourcesOf(const std::vector<std::pair<std::string, Json>>& placed,
          const std::vector<std::pair<std::string, std::vector<std::string>>>& biomes) {
    FeatureSources sources;
    for (const auto& [name, json] : placed) {
        sources.placed.emplace(id(name), json);
    }
    for (const auto& [name, features] : biomes) {
        sources.biomeFeatures.emplace(id(name), Json::array({features}));
    }
    return sources;
}

[[nodiscard]] std::vector<std::string> orderOf(const FeatureSet& set, const std::size_t step = 0) {
    std::vector<std::string> names;
    for (const FeatureSet::Entry& entry : set.steps().at(step)) {
        names.push_back(entry.id.toString());
    }
    return names;
}

/// Nine chunks around (0, 0): stone below kSurface, air above; every quart
/// the biome `test:a`, but for the chunk at (1, 0), which is all `test:b`.
struct FlatWorld {
    std::vector<std::unique_ptr<stratum::terrain::ChunkBuffer>> buffers;
    std::array<stratum::feature::TerrainHeightmaps, 9> heightmaps{};
    ResourceLocation biomeA = id("test:a");
    ResourceLocation biomeB = id("test:b");
    std::vector<const ResourceLocation*> quartsA;
    std::vector<const ResourceLocation*> quartsB;
    stratum::feature::Region region;

    FlatWorld()
        : quartsA(static_cast<std::size_t>(16 * (kGeometry.height / 4)), &biomeA),
          quartsB(static_cast<std::size_t>(16 * (kGeometry.height / 4)), &biomeB) {
        const BlockState stone = block("minecraft:stone");
        for (std::size_t slot = 0; slot < 9; ++slot) {
            auto& buffer =
                buffers.emplace_back(std::make_unique<stratum::terrain::ChunkBuffer>(kGeometry));
            for (int z = 0; z < 16; ++z) {
                for (int x = 0; x < 16; ++x) {
                    for (std::int32_t y = kGeometry.minY; y < kSurface; ++y) {
                        buffer->set(x, y, z, stone);
                    }
                }
            }
            heightmaps.at(slot) = stratum::feature::terrainHeightmaps(*buffer);
        }
        region.geometry = kGeometry;
        region.worldSeed = kSeed;
        region.biomeOutside = [this](stratum::biome::QuartPos) -> const ResourceLocation& {
            return biomeA;
        };
        for (std::size_t slot = 0; slot < 9; ++slot) {
            // Slot 5 is (dx, dz) = (1, 0).
            region.chunks.at(slot) = {.blocks = buffers[slot].get(),
                                      .heightmaps = &heightmaps.at(slot),
                                      .biomes = slot == 5 ? quartsB : quartsA,
                                      .minQuartY = kGeometry.minY / 4};
        }
    }

    [[nodiscard]] std::string at(const std::int32_t x, const std::int32_t y,
                                 const std::int32_t z) const {
        return buffers[4]->at(x, y, z).name.toString();
    }

    /// How many blocks of the centre chunk are @p name.
    [[nodiscard]] int count(const std::string& name) const {
        int total = 0;
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                for (std::int32_t y = kGeometry.minY; y < kGeometry.maxY(); ++y) {
                    total += at(x, y, z) == name ? 1 : 0;
                }
            }
        }
        return total;
    }
};

const std::vector<ResourceLocation> kBothBiomes = {id("test:a"), id("test:b")};

} // namespace

TEST_CASE("the feature seed draws the offsets the server's markers landed at", "[feature][rng]") {
    // tools/analysis/feature-probe.sh --variant seed, seed 42, chunk (6, 6):
    // each marker's offset from the chunk's corner, by (step, index).
    struct Stored {
        std::int32_t step;
        std::int32_t index;
        std::int32_t x;
        std::int32_t z;
    };

    constexpr std::array<Stored, 12> kStored = {{{0, 0, 10, 9},
                                                 {0, 1, 8, 10},
                                                 {1, 0, 12, 13},
                                                 {2, 0, 3, 5},
                                                 {3, 0, 6, 6},
                                                 {4, 0, 5, 2},
                                                 {5, 0, 7, 5},
                                                 {6, 0, 15, 1},
                                                 {7, 0, 7, 10},
                                                 {8, 0, 12, 13},
                                                 {9, 0, 3, 1},
                                                 {10, 0, 1, 11}}};
    const std::int64_t population = stratum::rng::populationSeed(kSeed, 6 * 16, 6 * 16);
    for (const Stored& stored : kStored) {
        CAPTURE(stored.step, stored.index);
        WorldgenRandom random(stratum::rng::featureSeed(population, stored.index, stored.step));
        CHECK(random.nextInt(16) == stored.x);
        CHECK(random.nextInt(16) == stored.z);
    }
}

TEST_CASE("the population seed wraps, and reads negative corners as they are", "[feature][rng]") {
    // Wrapping arithmetic on both signs; distinct chunks, distinct seeds.
    const std::int64_t a = stratum::rng::populationSeed(-1, -32, 16);
    const std::int64_t b = stratum::rng::populationSeed(-1, 16, -32);
    CHECK(a != b);
    CHECK(stratum::rng::populationSeed(-1, -32, 16) == a);
    CHECK(stratum::rng::featureSeed(INT64_MAX, 1, 0) == INT64_MIN);
    CHECK(stratum::rng::featureSeed(0, 3, 2) == 20003);
}

TEST_CASE("features run in the depth-first order of the biomes' lists", "[feature][order]") {
    const Json plain = marker("white", Json::array());
    const FeatureSources sources =
        sourcesOf({{"test:x", plain}, {"test:y", plain}, {"test:z", plain}, {"test:w", plain}},
                  {{"test:a", {"test:x", "test:y"}},
                   {"test:b", {"test:z", "test:y"}},
                   {"test:c", {"test:w"}}});
    const std::vector<ResourceLocation> biomes = {id("test:a"), id("test:b"), id("test:c")};
    const FeatureSet set = FeatureSet::build(sources, biomes);
    // Nodes x, y, z, w; the walk finishes y, x, z, w; reversed.
    CHECK(orderOf(set) == std::vector<std::string>{"test:w", "test:z", "test:x", "test:y"});
    CHECK(set.lists(id("test:a"), 0, 2));
    CHECK(set.lists(id("test:a"), 0, 3));
    CHECK_FALSE(set.lists(id("test:a"), 0, 1));
    CHECK(set.lists(id("test:c"), 0, 0));
    CHECK_FALSE(set.lists(id("test:missing"), 0, 0));

    // The biomes' order is part of it: the same lists, the biomes reversed.
    const std::vector<ResourceLocation> reversed = {id("test:c"), id("test:b"), id("test:a")};
    CHECK(orderOf(FeatureSet::build(sources, reversed)) ==
          std::vector<std::string>{"test:x", "test:z", "test:y", "test:w"});
}

TEST_CASE("lists that order two features both ways are refused by name", "[feature][order]") {
    const Json plain = marker("white", Json::array());
    const FeatureSources sources =
        sourcesOf({{"test:x", plain}, {"test:y", plain}},
                  {{"test:a", {"test:x", "test:y"}}, {"test:b", {"test:y", "test:x"}}});
    REQUIRE_THROWS_WITH(FeatureSet::build(sources, kBothBiomes), ContainsSubstring("test:x") &&
                                                                     ContainsSubstring("test:y") &&
                                                                     ContainsSubstring("step 0"));
}

TEST_CASE("a feature this build does not run is listed and keeps its place", "[feature][order]") {
    const Json tree = {{"feature", {{"type", "minecraft:tree"}, {"config", Json::object()}}},
                       {"placement", Json::array()}};
    const Json noisy =
        marker("white", Json::array({{{"type", "minecraft:count_on_every_layer"}, {"count", 1}}}));
    const Json flower = {{"feature",
                          {{"type", "minecraft:simple_block"},
                           {"config",
                            {{"to_place",
                              {{"type", "minecraft:simple_state_provider"},
                               {"state", {{"Name", "minecraft:poppy"}}}}}}}}},
                         {"placement", Json::array()}};
    const FeatureSources sources = sourcesOf(
        {{"test:tree", tree},
         {"test:noisy", noisy},
         {"test:flower", flower},
         {"test:mark", marker("white", Json::array())}},
        {{"test:a", {"test:tree", "test:noisy", "test:flower", "test:mark"}}, {"test:b", {}}});
    const FeatureSet set = FeatureSet::build(sources, kBothBiomes);
    CHECK(orderOf(set) ==
          std::vector<std::string>{"test:tree", "test:noisy", "test:flower", "test:mark"});
    CHECK(set.supportedCount() == 1);
    REQUIRE(set.unsupported().size() == 3);
    CHECK(set.unsupported()[0].id == id("test:tree"));
    CHECK_THAT(set.unsupported()[0].reason, ContainsSubstring("minecraft:tree"));
    CHECK_THAT(set.unsupported()[1].reason, ContainsSubstring("minecraft:count_on_every_layer"));
    CHECK_THAT(set.unsupported()[2].reason, ContainsSubstring("minecraft:poppy"));
    CHECK(set.steps()[0][0].feature == nullptr);
    CHECK(set.steps()[0][3].feature != nullptr);
    CHECK(set.writtenStates() == std::vector<BlockState>{block("minecraft:white_wool")});
}

TEST_CASE("malformed feature data is refused, naming the entry", "[feature]") {
    const auto build = [](const Json& placed) {
        const FeatureSources sources =
            sourcesOf({{"test:bad", placed}}, {{"test:a", {"test:bad"}}, {"test:b", {}}});
        return FeatureSet::build(sources, kBothBiomes);
    };
    REQUIRE_THROWS_WITH(build(Json{{"feature", "test:absent"}, {"placement", Json::array()}}),
                        ContainsSubstring("test:bad") && ContainsSubstring("test:absent"));
    REQUIRE_THROWS_WITH(build(marker("white", "in_square")), ContainsSubstring("placement"));
    REQUIRE_THROWS_WITH(
        build(marker("white", Json::array({{{"type", "minecraft:count"}, {"count", "three"}}}))),
        ContainsSubstring("test:bad") && ContainsSubstring("count"));
    REQUIRE_THROWS_WITH(
        build(marker("white", Json::array({{{"type", "minecraft:block_predicate_filter"},
                                            {"predicate",
                                             {{"type", "minecraft:matching_blocks"},
                                              {"blocks", "minecraft:stone"},
                                              {"offset", {0, 16, 0}}}}}}))),
        ContainsSubstring("offset"));

    // A biome the dimension generates and the pack lacks; a feature a biome
    // lists and the pack lacks; a list that is not a list.
    FeatureSources sources = sourcesOf({}, {{"test:a", {}}});
    REQUIRE_THROWS_WITH(FeatureSet::build(sources, kBothBiomes), ContainsSubstring("test:b"));
    sources = sourcesOf({}, {{"test:a", {"test:nowhere"}}, {"test:b", {}}});
    REQUIRE_THROWS_WITH(FeatureSet::build(sources, kBothBiomes), ContainsSubstring("test:nowhere"));
    sources.biomeFeatures[id("test:a")] = Json::array({"test:nowhere"});
    REQUIRE_THROWS_AS(FeatureSet::build(sources, kBothBiomes), FeatureError);
}

TEST_CASE("count and in_square draw a position at a time", "[feature][placement]") {
    const FeatureSources sources = sourcesOf(
        {{"test:mark", marker("white", Json::array({{{"type", "minecraft:count"}, {"count", 5}},
                                                    {{"type", "minecraft:in_square"}},
                                                    {{"type", "minecraft:height_range"},
                                                     {"height", {{"absolute", 100}}}}}))}},
        {{"test:a", {"test:mark"}}, {"test:b", {}}});
    FlatWorld world;
    stratum::feature::decorate(FeatureSet::build(sources, kBothBiomes), world.region);

    // The one feature of step 0: index 0.
    WorldgenRandom random(
        stratum::rng::featureSeed(stratum::rng::populationSeed(kSeed, 0, 0), 0, 0));
    for (int copy = 0; copy < 5; ++copy) {
        const std::int32_t x = random.nextInt(16);
        const std::int32_t z = random.nextInt(16);
        CAPTURE(copy, x, z);
        CHECK(world.at(x, 100, z) == "minecraft:white_wool");
    }
    CHECK(world.count("minecraft:white_wool") <= 5);
}

TEST_CASE("the terrain's heightmaps stay as the terrain left them, the others follow the blocks",
          "[feature][placement]") {
    const auto onColumn = [](const std::string& wool, const char* heightmap) {
        return marker(wool,
                      Json::array({fixedAt(5, 0, 7),
                                   {{"type", "minecraft:heightmap"}, {"heightmap", heightmap}}}));
    };
    const FeatureSources sources = sourcesOf(
        {{"test:first", onColumn("white", "WORLD_SURFACE_WG")},
         {"test:live", onColumn("orange", "MOTION_BLOCKING")},
         {"test:floor", onColumn("pink", "OCEAN_FLOOR")},
         {"test:again", onColumn("magenta", "WORLD_SURFACE_WG")}},
        {{"test:a", {"test:first", "test:live", "test:floor", "test:again"}}, {"test:b", {}}});
    FlatWorld world;
    stratum::feature::decorate(FeatureSet::build(sources, kBothBiomes), world.region);
    // The first lands on the terrain; the live maps then stand one and two
    // higher; the terrain's map still says the terrain.
    CHECK(world.at(5, kSurface, 7) == "minecraft:magenta_wool");
    CHECK(world.at(5, kSurface + 1, 7) == "minecraft:orange_wool");
    CHECK(world.at(5, kSurface + 2, 7) == "minecraft:pink_wool");
}

TEST_CASE("a chunk runs its neighbours' biomes' features, and the biome modifier keeps them out",
          "[feature][placement]") {
    const Json inSquare = {{"type", "minecraft:in_square"}};
    const Json atTop = {{"type", "minecraft:heightmap"}, {"heightmap", "WORLD_SURFACE_WG"}};
    const FeatureSources sources = sourcesOf(
        {{"test:free", marker("white", Json::array({inSquare, atTop}))},
         {"test:kept",
          marker("orange", Json::array({inSquare, atTop, {{"type", "minecraft:biome"}}}))},
         {"test:home",
          marker("pink", Json::array({inSquare, atTop, {{"type", "minecraft:biome"}}}))}},
        {{"test:a", {"test:home"}}, {"test:b", {"test:free", "test:kept"}}});
    FlatWorld world;
    stratum::feature::decorate(FeatureSet::build(sources, kBothBiomes), world.region);
    // test:b is the east neighbour's biome alone. Its unfiltered feature runs
    // in this chunk; its filtered one finds this chunk's blocks in test:a.
    CHECK(world.count("minecraft:white_wool") == 1);
    CHECK(world.count("minecraft:orange_wool") == 0);
    CHECK(world.count("minecraft:pink_wool") == 1);

    // With no chunk of test:b among the nine, neither of its features runs.
    FlatWorld alone;
    alone.region.chunks.at(5).biomes = alone.quartsA;
    stratum::feature::decorate(FeatureSet::build(sources, kBothBiomes), alone.region);
    CHECK(alone.count("minecraft:white_wool") == 0);
    CHECK(alone.count("minecraft:pink_wool") == 1);
}

TEST_CASE("the scan walks to its target, and stops where it may not pass", "[feature][placement]") {
    const Json stone = {{"type", "minecraft:matching_blocks"}, {"blocks", "minecraft:stone"}};
    const Json air = {{"type", "minecraft:matching_blocks"}, {"blocks", "minecraft:air"}};
    const auto scan = [&](const std::string& wool, const std::int32_t y, const char* direction,
                          const Json& target, const Json* allowed, const std::int32_t steps) {
        Json modifier = {{"type", "minecraft:environment_scan"},
                         {"direction_of_search", direction},
                         {"target_condition", target},
                         {"max_steps", steps}};
        if (allowed != nullptr) {
            modifier["allowed_search_condition"] = *allowed;
        }
        return marker(wool, Json::array({fixedAt(3, y, 3), modifier}));
    };
    const FeatureSources sources = sourcesOf(
        {// Down through air to the stone, within reach.
         {"test:down", scan("white", kSurface + 5, "down", stone, nullptr, 6)},
         // One step short of it.
         {"test:short", scan("orange", kSurface + 8, "down", stone, nullptr, 7)},
         // Starting in air where only stone may be passed.
         {"test:barred", scan("pink", kSurface + 2, "down", stone, &stone, 6)},
         // Starting on the target.
         {"test:here", scan("lime", kSurface - 10, "down", stone, &air, 1)},
         // The same with nothing barring it: the start is the target.
         {"test:onit", scan("cyan", kSurface - 20, "down", stone, nullptr, 1)}},
        {{"test:a", {"test:down", "test:short", "test:barred", "test:here", "test:onit"}},
         {"test:b", {}}});
    FlatWorld world;
    stratum::feature::decorate(FeatureSet::build(sources, kBothBiomes), world.region);
    CHECK(world.at(3, kSurface - 1, 3) == "minecraft:white_wool");
    CHECK(world.count("minecraft:orange_wool") == 0);
    CHECK(world.count("minecraft:pink_wool") == 0);
    CHECK(world.count("minecraft:lime_wool") == 0);
    CHECK(world.at(3, kSurface - 20, 3) == "minecraft:cyan_wool");
}

TEST_CASE("decorating needs all nine chunks", "[feature]") {
    const FeatureSources sources = sourcesOf({}, {{"test:a", {}}, {"test:b", {}}});
    FlatWorld world;
    world.region.chunks.at(0).blocks = nullptr;
    REQUIRE_THROWS_WITH(
        stratum::feature::decorate(FeatureSet::build(sources, kBothBiomes), world.region),
        ContainsSubstring("nine"));
}

namespace {

/// An ore of @p size writing @p targets, each (rule test, block).
[[nodiscard]] Json oreOf(const std::int32_t size, const double discard, Json targets,
                         Json placement) {
    return {{"feature",
             {{"type", "minecraft:ore"},
              {"config",
               {{"size", size},
                {"discard_chance_on_air_exposure", discard},
                {"targets", std::move(targets)}}}}},
            {"placement", std::move(placement)}};
}

[[nodiscard]] Json replacing(const char* block, const std::string& with) {
    return {{"target", {{"predicate_type", "minecraft:block_match"}, {"block", block}}},
            {"state", {{"Name", with}}}};
}

/// Every position of the centre chunk holding @p name.
[[nodiscard]] std::vector<std::array<std::int32_t, 3>> positionsOf(const FlatWorld& world,
                                                                   const std::string& name) {
    std::vector<std::array<std::int32_t, 3>> found;
    for (int x = 0; x < 16; ++x) {
        for (int z = 0; z < 16; ++z) {
            for (std::int32_t y = kGeometry.minY; y < kGeometry.maxY(); ++y) {
                if (world.at(x, y, z) == name) {
                    found.push_back({x, y, z});
                }
            }
        }
    }
    return found;
}

} // namespace

TEST_CASE("an ore that runs draws three and two a node; one in the open, three", "[feature][ore]") {
    // Eight attempts, each underground or far above the terrain by an even
    // draw. Replaying the stream under the measured rule says where each
    // underground one is; every ore block must be within its blob's reach of
    // one of them, and each of them must have left blocks.
    constexpr std::int32_t kSize = 9;
    const Json height = {
        {"type", "minecraft:height_range"},
        {"height",
         {{"type", "minecraft:weighted_list"},
          {"distribution", Json::array({{{"data", {{"absolute", 250}}}, {"weight", 1}},
                                        {{"data", {{"absolute", 20}}}, {"weight", 1}}})}}}};
    const FeatureSources sources = sourcesOf(
        {{"test:ore",
          oreOf(kSize, 0.0, Json::array({replacing("minecraft:stone", "minecraft:white_wool")}),
                Json::array({{{"type", "minecraft:count"}, {"count", 8}},
                             {{"type", "minecraft:in_square"}},
                             height}))}},
        {{"test:a", {"test:ore"}}, {"test:b", {}}});
    FlatWorld world;
    stratum::feature::decorate(FeatureSet::build(sources, kBothBiomes), world.region);

    WorldgenRandom random(
        stratum::rng::featureSeed(stratum::rng::populationSeed(kSeed, 0, 0), 0, 0));
    std::vector<std::array<std::int32_t, 3>> origins;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const std::int32_t x = random.nextInt(16);
        const std::int32_t z = random.nextInt(16);
        const bool sky = random.nextInt(2) < 1;
        random.skip(sky ? 3 : 3 + (2 * kSize));
        if (!sky) {
            origins.push_back({x, 20, z});
        }
    }
    REQUIRE(origins.size() >= 2);
    REQUIRE(origins.size() <= 6);

    // A size-9 blob: centres within 9/8 across and 2 below, radii at most
    // 9/16 + 1/2.
    const auto reaches = [](const std::array<std::int32_t, 3>& origin,
                            const std::array<std::int32_t, 3>& block) {
        return std::abs(block[0] - origin[0]) <= 3 && std::abs(block[2] - origin[2]) <= 3 &&
               block[1] >= origin[1] - 4 && block[1] <= origin[1] + 1;
    };
    std::size_t blocksInChunk = 0;
    for (int dz = 0; dz < 16; ++dz) {
        for (int dx = 0; dx < 16; ++dx) {
            for (std::int32_t y = 0; y < 300; ++y) {
                if (world.at(dx, y, dz) != "minecraft:white_wool") {
                    continue;
                }
                ++blocksInChunk;
                const std::array<std::int32_t, 3> block = {dx, y, dz};
                CAPTURE(dx, y, dz);
                CHECK(std::ranges::any_of(
                    origins, [&](const auto& origin) { return reaches(origin, block); }));
            }
        }
    }
    CHECK(blocksInChunk >= origins.size());
    CHECK(positionsOf(world, "minecraft:white_wool").size() == blocksInChunk);
}

TEST_CASE("an ore offers each block to its targets in turn, and discards beside air",
          "[feature][ore]") {
    // Sixteen blobs across the terrain's surface, at y 62..64.
    const Json placement = Json::array({{{"type", "minecraft:count"}, {"count", 16}},
                                        {{"type", "minecraft:in_square"}},
                                        {{"type", "minecraft:height_range"},
                                         {"height",
                                          {{"type", "minecraft:uniform"},
                                           {"min_inclusive", {{"absolute", kSurface - 2}}},
                                           {"max_inclusive", {{"absolute", kSurface}}}}}}});
    const Json air = replacing("minecraft:air", "minecraft:orange_wool");
    const auto decorated = [&](const double discard, const Json& targets) {
        const FeatureSources sources =
            sourcesOf({{"test:ore", oreOf(12, discard, targets, placement)}},
                      {{"test:a", {"test:ore"}}, {"test:b", {}}});
        auto world = std::make_unique<FlatWorld>();
        stratum::feature::decorate(FeatureSet::build(sources, kBothBiomes), world->region);
        return world;
    };
    const Json stoneThenAir =
        Json::array({replacing("minecraft:stone", "minecraft:white_wool"), air});

    // Discarding nothing: stone takes the first target, the air above the
    // terrain the second.
    const auto none = decorated(0.0, stoneThenAir);
    const auto white = positionsOf(*none, "minecraft:white_wool");
    const auto orange = positionsOf(*none, "minecraft:orange_wool");
    REQUIRE_FALSE(white.empty());
    REQUIRE_FALSE(orange.empty());
    CHECK(std::ranges::all_of(white, [](const auto& at) { return at[1] < kSurface; }));
    CHECK(std::ranges::all_of(orange, [](const auto& at) { return at[1] >= kSurface; }));
    CHECK(std::ranges::any_of(white, [](const auto& at) { return at[1] == kSurface - 1; }));

    // Discarding every block beside air: no ore in the terrain's top layer
    // under open air, and the blobs' deeper blocks still there.
    const auto all =
        decorated(1.0, Json::array({replacing("minecraft:stone", "minecraft:white_wool")}));
    const auto kept = positionsOf(*all, "minecraft:white_wool");
    REQUIRE_FALSE(kept.empty());
    CHECK(std::ranges::none_of(kept, [](const auto& at) { return at[1] == kSurface - 1; }));
}

TEST_CASE("an ore's data is held to its schema, and a test that draws is left unrun",
          "[feature][ore]") {
    const auto build = [](const Json& placed) {
        const FeatureSources sources =
            sourcesOf({{"test:ore", placed}}, {{"test:a", {"test:ore"}}, {"test:b", {}}});
        return FeatureSet::build(sources, kBothBiomes);
    };
    const Json stone = Json::array({replacing("minecraft:stone", "minecraft:white_wool")});
    REQUIRE_THROWS_WITH(build(oreOf(65, 0.0, stone, Json::array())), ContainsSubstring("size"));
    REQUIRE_THROWS_WITH(build(oreOf(9, 1.5, stone, Json::array())),
                        ContainsSubstring("discard_chance_on_air_exposure"));
    REQUIRE_THROWS_WITH(build(oreOf(9, 0.0,
                                    Json::array({{{"target",
                                                   {{"predicate_type", "minecraft:tag_match"},
                                                    {"tag", "minecraft:absent"}}},
                                                  {"state", {{"Name", "minecraft:white_wool"}}}}}),
                                    Json::array())),
                        ContainsSubstring("minecraft:absent"));

    const FeatureSet random =
        build(oreOf(9, 0.0,
                    Json::array({{{"target",
                                   {{"predicate_type", "minecraft:random_block_match"},
                                    {"block", "minecraft:stone"},
                                    {"probability", 0.5}}},
                                  {"state", {{"Name", "minecraft:white_wool"}}}}}),
                    Json::array()));
    REQUIRE(random.unsupported().size() == 1);
    CHECK_THAT(random.unsupported()[0].reason, ContainsSubstring("random_block_match"));
}

TEST_CASE("a spring is placed in a wall with one opening, and marked", "[feature][spring]") {
    const auto spring = [](const std::int32_t x, const std::int32_t y, const std::int32_t z) {
        return Json{
            {"feature",
             {{"type", "minecraft:spring_feature"},
              {"config",
               {{"state", {{"Name", "minecraft:water"}, {"Properties", {{"falling", "true"}}}}},
                {"rock_count", 4},
                {"hole_count", 1},
                {"requires_block_below", true},
                {"valid_blocks", Json::array({"minecraft:stone"})}}}}},
            {"placement", Json::array({fixedAt(x, y, z)})}};
    };
    const FeatureSources sources = sourcesOf(
        {{"test:wall", spring(4, 30, 5)},       // one opening, to the east
         {"test:buried", spring(10, 30, 10)},   // none
         {"test:corner", spring(4, 40, 5)},     // two
         {"test:roofless", spring(4, 50, 5)},   // air above
         {"test:floorless", spring(4, 56, 5)}}, // air below, which is the one opening
        {{"test:a", {"test:wall", "test:buried", "test:corner", "test:roofless", "test:floorless"}},
         {"test:b", {}}});
    FlatWorld world;
    const BlockState air = block("minecraft:air");
    stratum::terrain::ChunkBuffer& centre = *world.buffers[4];
    centre.set(5, 30, 5, air);
    centre.set(5, 40, 5, air);
    centre.set(4, 40, 6, air);
    centre.set(5, 50, 5, air);
    centre.set(4, 51, 5, air);
    centre.set(4, 55, 5, air);
    stratum::feature::decorate(FeatureSet::build(sources, kBothBiomes), world.region);

    CHECK(world.at(4, 30, 5) == "minecraft:water");
    CHECK(centre.at(4, 30, 5).properties == std::map<std::string, std::string>{{"level", "0"}});
    CHECK(world.count("minecraft:water") == 1);
    REQUIRE(centre.fluidUpdates().size() == 1);
    CHECK(centre.fluidUpdates()[0] ==
          stratum::terrain::ChunkBuffer::FluidUpdate{.localX = 4, .y = 30, .localZ = 5});
}

TEST_CASE("a disk rewrites its target within its radius and half height", "[feature][disk]") {
    const Json disk = {
        {"feature",
         {{"type", "minecraft:disk"},
          {"config",
           {{"radius", 3},
            {"half_height", 2},
            {"target", {{"type", "minecraft:matching_blocks"}, {"blocks", "minecraft:dirt"}}},
            {"state_provider",
             {{"fallback",
               {{"type", "minecraft:simple_state_provider"},
                {"state", {{"Name", "minecraft:white_wool"}}}}},
              {"rules", Json::array({{{"if_true",
                                       {{"type", "minecraft:matching_blocks"},
                                        {"blocks", "minecraft:air"},
                                        {"offset", {0, -1, 0}}}},
                                      {"then",
                                       {{"type", "minecraft:simple_state_provider"},
                                        {"state", {{"Name", "minecraft:orange_wool"}}}}}}})}}}}}}},
        {"placement", Json::array({fixedAt(8, kSurface - 2, 8)})}};
    const FeatureSources sources =
        sourcesOf({{"test:disk", disk}}, {{"test:a", {"test:disk"}}, {"test:b", {}}});
    FlatWorld world;
    stratum::terrain::ChunkBuffer& centre = *world.buffers[4];
    // Five layers of dirt under the surface, and a hollow under one block.
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            for (std::int32_t y = kSurface - 5; y < kSurface; ++y) {
                centre.set(x, y, z, block("minecraft:dirt"));
            }
        }
    }
    centre.set(9, kSurface - 3, 8, block("minecraft:air"));
    stratum::feature::decorate(FeatureSet::build(sources, kBothBiomes), world.region);

    // y 60..63 of the position's 62 +- 2 hold dirt; 64 is air; 59 is below.
    CHECK(world.at(8, kSurface - 1, 8) == "minecraft:white_wool");
    CHECK(world.at(8, kSurface - 4, 8) == "minecraft:white_wool");
    CHECK(world.at(8, kSurface - 5, 8) == "minecraft:dirt");
    CHECK(world.at(8, kSurface, 8) == "minecraft:air");
    // The radius: (3, 0) is in, (3, 1) is out.
    CHECK(world.at(11, kSurface - 1, 8) == "minecraft:white_wool");
    CHECK(world.at(11, kSurface - 1, 9) == "minecraft:dirt");
    CHECK(world.at(5, kSurface - 1, 8) == "minecraft:white_wool");
    CHECK(world.at(4, kSurface - 1, 8) == "minecraft:dirt");
    // The rule: the block over the hollow, and no other.
    CHECK(world.at(9, kSurface - 2, 8) == "minecraft:orange_wool");
    CHECK(world.count("minecraft:orange_wool") == 1);
    // 29 columns of four blocks, less the hollow.
    CHECK(world.count("minecraft:white_wool") + world.count("minecraft:orange_wool") ==
          (29 * 4) - 1);
}
