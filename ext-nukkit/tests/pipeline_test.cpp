// Stratum — the Nukkit binding's own pipeline, without a JVM in the loop.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// What the Java side does at world load and per chunk, with a synthetic id
// table in place of Nukkit's BlockStateMapping: the table's own checks, the
// closed list of states a dimension can place, the refusals, and fills
// checked block for block against world::CompiledDimension itself — on one
// thread and on several. jni_bridge.cpp and the Java classes are exercised
// through real JNI by ext-nukkit/java/test (the `nukkit` ctest label).
//
// The first cases need no fixtures; the rest skip without the pinned
// version's worldgen, as every fixture-reading case does.
#include "support/temp_path.hpp"

#include <stratum/data/pack.hpp>
#include <stratum/freeze/pipeline.hpp>
#include <stratum/mapping/biome.hpp>
#include <stratum/mapping/block_state.hpp>
#include <stratum/terrain/filler.hpp>
#include <stratum/world/dimension.hpp>

#include <nlohmann/json.hpp>
#include <stratum_nukkit/pipeline.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using stratum::data::ResourceLocation;
using stratum::nukkit::BlockIdTable;
using stratum::nukkit::Pipeline;
using stratum::settings::BlockState;
using stratum::world::BiomeSource;

namespace {

[[nodiscard]] std::filesystem::path versionRoot() {
    return std::filesystem::path(STRATUM_FIXTURES_DIR) / "1.21.11";
}

[[nodiscard]] bool haveFixtures() {
    return std::filesystem::is_directory(versionRoot() / "worldgen");
}

[[nodiscard]] BlockState state(const char* name,
                               std::map<std::string, std::string> properties = {}) {
    return BlockState{.name = ResourceLocation::parse(name), .properties = std::move(properties)};
}

[[nodiscard]] stratum::freeze::Pipeline frozenVanilla() {
    return stratum::freeze::resolve(stratum::data::Pack::open(versionRoot() / "worldgen"),
                                    versionRoot() / "biome_parameters");
}

const ResourceLocation kOverworld = ResourceLocation::parse("minecraft:overworld");

/// A table resolving every state @p pipeline can place to 1000 + its index,
/// leaving out the one named @p except.
[[nodiscard]] BlockIdTable syntheticTable(const Pipeline& pipeline, const char* except = nullptr) {
    std::vector<std::int32_t> ids;
    std::vector<std::int32_t> fullIds;
    for (std::size_t i = 0; i < pipeline.emittableStateIds().size(); ++i) {
        if (except != nullptr &&
            stratum::nukkit::describe(pipeline.emittableStates()[i]) == except) {
            continue;
        }
        ids.push_back(static_cast<std::int32_t>(pipeline.emittableStateIds()[i]));
        fullIds.push_back(1000 + static_cast<std::int32_t>(i));
    }
    return BlockIdTable::fromResolved(ids, fullIds);
}

struct Filled {
    std::vector<std::int32_t> blocks;
    std::vector<std::int32_t> biomes;
    std::vector<std::int32_t> fluids;

    [[nodiscard]] bool operator==(const Filled&) const = default;
};

[[nodiscard]] Filled fill(const Pipeline& pipeline, const BlockIdTable& table, const int chunkX,
                          const int chunkZ) {
    Filled out;
    out.blocks.resize(static_cast<std::size_t>(pipeline.height()) * 16 * 16);
    out.biomes.resize(static_cast<std::size_t>(pipeline.height() / 4) * 16);
    pipeline.fill(chunkX, chunkZ, table, out.blocks, out.biomes, &out.fluids);
    return out;
}

// Two, far apart: a Debug fill takes most of a second, and these cases run in
// the dev preset whenever the fixtures are present.
constexpr std::array<std::array<int, 2>, 2> kChunks{{{0, 0}, {-40, 250}}};

} // namespace

TEST_CASE("a block id table refuses what it cannot hold, by id", "[nukkit]") {
    const std::vector<std::int32_t> one{1};
    const std::vector<std::int32_t> two{1, 2};
    CHECK_THROWS_WITH(BlockIdTable::fromResolved(one, two), ContainsSubstring("1 resolved"));
    const std::vector<std::int32_t> outOfRange{
        static_cast<std::int32_t>(stratum::mapping::javaBlockStateCount())};
    CHECK_THROWS_WITH(BlockIdTable::fromResolved(outOfRange, one),
                      ContainsSubstring("outside the pinned version"));
    const std::vector<std::int32_t> negative{-5};
    CHECK_THROWS_WITH(BlockIdTable::fromResolved(one, negative),
                      ContainsSubstring("negative Nukkit full id"));
    const std::vector<std::int32_t> twice{7, 7};
    const std::vector<std::int32_t> differing{70, 71};
    CHECK_THROWS_WITH(BlockIdTable::fromResolved(twice, differing),
                      ContainsSubstring("resolved twice"));

    const std::vector<std::int32_t> agreeing{70, 70};
    const BlockIdTable table = BlockIdTable::fromResolved(twice, agreeing);
    CHECK(table.size() == 1);
    CHECK(table.find(7) == 70);
    CHECK_FALSE(table.find(8).has_value());
    CHECK_FALSE(table.find(4'000'000'000U).has_value());
}

TEST_CASE("a Java block state is described the way the game spells one", "[nukkit]") {
    CHECK(stratum::nukkit::describe(state("minecraft:stone")) == "minecraft:stone");
    CHECK(stratum::nukkit::describe(state("minecraft:snow_layer", {{"layers", "1"}})) ==
          "minecraft:snow_layer[layers=1]");
    CHECK(stratum::nukkit::describe(state("minecraft:x", {{"b", "2"}, {"a", "1"}})) ==
          "minecraft:x[a=1,b=2]");
}

TEST_CASE("the states a dimension can place: fill, veins, surface rules and bandlands",
          "[nukkit]") {
    stratum::settings::NoiseSettings settings;
    settings.defaultBlock = state("minecraft:stone");
    settings.defaultFluid = state("minecraft:water", {{"level", "0"}});
    settings.surfaceRule = nlohmann::json::parse(R"({
        "type": "minecraft:sequence",
        "sequence": [
            {"type": "minecraft:block", "result_state": {"Name": "minecraft:grass_block",
                                                          "Properties": {"snowy": "false"}}},
            {"type": "minecraft:block", "result_state": {"Name": "minecraft:stone"}},
            {"type": "minecraft:bandlands"}
        ]})");

    const std::vector<BlockState> withoutVeins = stratum::nukkit::emittableBlockStates(settings);
    const std::vector<BlockState> expected{
        state("minecraft:air"),
        state("minecraft:lava", {{"level", "0"}}),
        state("minecraft:stone"),
        state("minecraft:water", {{"level", "0"}}),
        state("minecraft:grass_block", {{"snowy", "false"}}),
        state("minecraft:terracotta"),
        state("minecraft:orange_terracotta"),
        state("minecraft:yellow_terracotta"),
        state("minecraft:brown_terracotta"),
        state("minecraft:red_terracotta"),
        state("minecraft:white_terracotta"),
        state("minecraft:light_gray_terracotta"),
    };
    CHECK(withoutVeins == expected);

    settings.oreVeinsEnabled = true;
    const std::vector<BlockState> withVeins = stratum::nukkit::emittableBlockStates(settings);
    CHECK(withVeins.size() == expected.size() + 6);
    CHECK(std::find(withVeins.begin(), withVeins.end(), state("minecraft:raw_iron_block")) !=
          withVeins.end());

    settings.surfaceRule = nlohmann::json::parse(R"({"type": "minecraft:no_such_rule"})");
    CHECK_THROWS_WITH(stratum::nukkit::emittableBlockStates(settings),
                      ContainsSubstring("minecraft:no_such_rule"));
}

TEST_CASE("freezing refuses to replace a pipeline, or to read what is no version root",
          "[nukkit]") {
    const std::filesystem::path scratch = stratum::test::tempPath("stratum-nukkit-freeze");
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch);
    const std::filesystem::path existing = scratch / "stratum-pipeline.blob";
    std::ofstream(existing) << "a world's pipeline";

    CHECK_THROWS_WITH(stratum::nukkit::freezePack(scratch, existing),
                      ContainsSubstring("refusing to replace"));
    CHECK_THROWS_WITH(stratum::nukkit::freezePack(scratch, scratch / "new.blob"),
                      ContainsSubstring("is not a version root"));
    CHECK_FALSE(std::filesystem::exists(scratch / "new.blob"));
    std::filesystem::remove_all(scratch);
}

TEST_CASE("a world outside the binding's scope is refused by name when it opens", "[nukkit]") {
    if (!haveFixtures()) {
        SKIP("no worldgen fixtures under " << STRATUM_FIXTURES_DIR
                                           << "; run tools/fetch-vanilla first");
    }
    CHECK_THROWS_WITH(Pipeline::compile(frozenVanilla(),
                                        ResourceLocation::parse("minecraft:nether"),
                                        BiomeSource::parse("minecraft:nether"), 1),
                      ContainsSubstring("'minecraft:nether' span y 0..127"));
    CHECK_THROWS_WITH(Pipeline::compile(frozenVanilla(), ResourceLocation::parse("minecraft:end"),
                                        BiomeSource::theEnd(), 1),
                      ContainsSubstring("outside this binding's scope"));
    // A name the pipeline does not carry is lib's to refuse, in its words.
    CHECK_THROWS_WITH(Pipeline::compile(frozenVanilla(),
                                        ResourceLocation::parse("minecraft:nowhere"),
                                        BiomeSource::multiNoise(kOverworld), 1),
                      ContainsSubstring("minecraft:nowhere"));

    // Amplified and large_biomes are the overworld's shape: they open.
    for (const char* settings : {"minecraft:amplified", "minecraft:large_biomes"}) {
        const auto pipeline = Pipeline::compile(frozenVanilla(), ResourceLocation::parse(settings),
                                                BiomeSource::multiNoise(kOverworld), 1);
        CHECK(pipeline->height() == stratum::nukkit::kOverworldHeight);
    }
}

TEST_CASE("a world opens from its frozen pipeline with the states and biomes it can place",
          "[nukkit]") {
    if (!haveFixtures()) {
        SKIP("no worldgen fixtures under " << STRATUM_FIXTURES_DIR
                                           << "; run tools/fetch-vanilla first");
    }
    const std::filesystem::path scratch = stratum::test::tempPath("stratum-nukkit-open");
    std::filesystem::remove_all(scratch);
    const std::filesystem::path blob = scratch / "world" / stratum::nukkit::kBlobFileName;
    stratum::nukkit::freezePack(versionRoot(), blob);
    REQUIRE(std::filesystem::is_regular_file(blob));
    CHECK_FALSE(std::filesystem::exists(blob.string() + ".partial"));

    const auto pipeline = Pipeline::open(blob, kOverworld, BiomeSource::multiNoise(kOverworld), 7);
    CHECK(pipeline->minY() == stratum::nukkit::kOverworldMinY);
    CHECK(pipeline->height() == stratum::nukkit::kOverworldHeight);

    std::vector<std::string> names;
    for (const BlockState& placed : pipeline->emittableStates()) {
        names.push_back(stratum::nukkit::describe(placed));
    }
    for (const char* expected :
         {"minecraft:air", "minecraft:stone", "minecraft:water[level=0]", "minecraft:lava[level=0]",
          "minecraft:bedrock", "minecraft:deepslate[axis=y]", "minecraft:powder_snow",
          "minecraft:copper_ore", "minecraft:light_gray_terracotta"}) {
        CHECK(std::find(names.begin(), names.end(), expected) != names.end());
    }
    REQUIRE(pipeline->emittableStateIds().size() == pipeline->emittableStates().size());
    std::vector<std::uint32_t> sortedIds = pipeline->emittableStateIds();
    std::sort(sortedIds.begin(), sortedIds.end());
    CHECK(std::adjacent_find(sortedIds.begin(), sortedIds.end()) == sortedIds.end());

    // Every biome of the overworld's list, each with its Bedrock id.
    CHECK(pipeline->emittableBiomes().size() == 54);
    for (const Pipeline::Biome& biome : pipeline->emittableBiomes()) {
        CHECK(stratum::mapping::bedrockBiomeId(biome.java) == biome.bedrockId);
    }
    std::filesystem::remove_all(scratch);
}

TEST_CASE("a world whose id table misses a state it can place is refused, naming the state",
          "[nukkit]") {
    if (!haveFixtures()) {
        SKIP("no worldgen fixtures under " << STRATUM_FIXTURES_DIR
                                           << "; run tools/fetch-vanilla first");
    }
    const auto pipeline =
        Pipeline::compile(frozenVanilla(), kOverworld, BiomeSource::multiNoise(kOverworld), 1);
    const BlockIdTable missingSnow = syntheticTable(*pipeline, "minecraft:powder_snow");
    CHECK_THROWS_WITH(pipeline->requireComplete(missingSnow),
                      ContainsSubstring("1 of the") && ContainsSubstring("minecraft:powder_snow"));
    CHECK_NOTHROW(pipeline->requireComplete(syntheticTable(*pipeline)));

    // Should a table reach fill() incomplete anyway, the chunk refuses by
    // name rather than writing a stand-in.
    const BlockIdTable missingStone = syntheticTable(*pipeline, "minecraft:stone");
    CHECK_THROWS_WITH(fill(*pipeline, missingStone, 0, 0),
                      ContainsSubstring("places 'minecraft:stone'"));
}

TEST_CASE("fill refuses output spans that do not match the dimension", "[nukkit]") {
    if (!haveFixtures()) {
        SKIP("no worldgen fixtures under " << STRATUM_FIXTURES_DIR
                                           << "; run tools/fetch-vanilla first");
    }
    const auto pipeline =
        Pipeline::compile(frozenVanilla(), kOverworld, BiomeSource::multiNoise(kOverworld), 1);
    const BlockIdTable table = syntheticTable(*pipeline);
    std::vector<std::int32_t> tooSmall(1);
    std::vector<std::int32_t> blocks(static_cast<std::size_t>(pipeline->height()) * 16 * 16);
    std::vector<std::int32_t> biomes(static_cast<std::size_t>(pipeline->height() / 4) * 16);
    CHECK_THROWS_WITH(pipeline->fill(0, 0, table, tooSmall, biomes),
                      ContainsSubstring("fullBlockIds holds 1 entries"));
    CHECK_THROWS_WITH(pipeline->fill(0, 0, table, blocks, tooSmall),
                      ContainsSubstring("biomeIds holds 1 entries"));
}

TEST_CASE("fill writes, block for block and quart for quart, what the compiled dimension places",
          "[nukkit]") {
    if (!haveFixtures()) {
        SKIP("no worldgen fixtures under " << STRATUM_FIXTURES_DIR
                                           << "; run tools/fetch-vanilla first");
    }
    constexpr std::int64_t kSeed = -4172144997902289642;
    const auto pipeline =
        Pipeline::compile(frozenVanilla(), kOverworld, BiomeSource::multiNoise(kOverworld), kSeed);
    const BlockIdTable table = syntheticTable(*pipeline);
    const auto reference = stratum::world::CompiledDimension::compile(
        frozenVanilla(), kOverworld, BiomeSource::multiNoise(kOverworld), kSeed);
    const stratum::settings::NoiseGeometry& geometry = reference->geometry();

    for (const auto& [chunkX, chunkZ] : kChunks) {
        const Filled got = fill(*pipeline, table, chunkX, chunkZ);

        stratum::terrain::ChunkBuffer buffer(geometry);
        reference->fillBlocks(chunkX, chunkZ, buffer);
        std::size_t wrongBlocks = 0;
        for (std::int32_t ly = 0; ly < geometry.height; ++ly) {
            for (int lz = 0; lz < 16; ++lz) {
                for (int lx = 0; lx < 16; ++lx) {
                    const auto expected = table.find(
                        stratum::mapping::javaBlockStateId(buffer.at(lx, geometry.minY + ly, lz)));
                    const std::size_t index =
                        (static_cast<std::size_t>(ly) * 16 + static_cast<std::size_t>(lz)) * 16 +
                        static_cast<std::size_t>(lx);
                    if (!expected.has_value() || got.blocks[index] != *expected) {
                        ++wrongBlocks;
                    }
                }
            }
        }
        CHECK(wrongBlocks == 0);

        std::vector<const ResourceLocation*> javaBiomes(got.biomes.size());
        reference->fillBiomes(chunkX, chunkZ, javaBiomes);
        std::size_t wrongBiomes = 0;
        for (std::size_t i = 0; i < javaBiomes.size(); ++i) {
            if (stratum::mapping::bedrockBiomeId(*javaBiomes[i]) != got.biomes[i]) {
                ++wrongBiomes;
            }
        }
        CHECK(wrongBiomes == 0);

        std::vector<std::int32_t> fluids;
        for (const auto& update : buffer.fluidUpdates()) {
            fluids.push_back(update.localX | (update.localZ << 4) |
                             ((update.y - geometry.minY) << 8));
        }
        CHECK(got.fluids == fluids);
    }
}

TEST_CASE("fills on several threads at once match one thread's, exactly", "[nukkit]") {
    if (!haveFixtures()) {
        SKIP("no worldgen fixtures under " << STRATUM_FIXTURES_DIR
                                           << "; run tools/fetch-vanilla first");
    }
    const auto pipeline =
        Pipeline::compile(frozenVanilla(), kOverworld, BiomeSource::multiNoise(kOverworld), 99);
    const BlockIdTable table = syntheticTable(*pipeline);

    std::vector<Filled> serial;
    serial.reserve(kChunks.size());
    for (const auto& [chunkX, chunkZ] : kChunks) {
        serial.push_back(fill(*pipeline, table, chunkX, chunkZ));
    }

    // Two threads, each over both chunks, each starting at a different one:
    // each chunk is filled on one thread while the other fills the other.
    constexpr std::size_t kThreads = 2;
    std::vector<std::vector<Filled>> parallel(kThreads, std::vector<Filled>(kChunks.size()));
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (std::size_t t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (std::size_t step = 0; step < kChunks.size(); ++step) {
                const std::size_t index = (t + step) % kChunks.size();
                parallel[t][index] = fill(*pipeline, table, kChunks[index][0], kChunks[index][1]);
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    for (std::size_t t = 0; t < kThreads; ++t) {
        for (std::size_t index = 0; index < kChunks.size(); ++index) {
            CHECK(parallel[t][index] == serial[index]);
        }
    }
}
