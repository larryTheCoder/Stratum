// Stratum — the End's biome source, `minecraft:the_end`, without fixtures.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// WHERE THE AUTHORITY IS. Every expected biome below is the VANILLA SERVER'S,
// read out of a probe region it wrote — none is a capture of what this build
// returns. tests/conformance/vanilla_end_biome_source_test.cpp scores the
// rule against every quart of every End region on disk; this holds a handful
// of those answers where CI's unit run reaches them, with no fixture at all:
//
//   * the ring, on both sides and at negative coordinates: chunks (-64, 0)
//     and (0, -64) are the_end at exactly 4096 (end-biomes, seeds 31337 and
//     8675309), where the island field alone would say end_midlands and
//     small_end_islands; (-64, 1) and (1, -64), at 4097, are not;
//   * every outer band at six worlds and in every quadrant, from
//     end-islands' r.64.0 (seeds 0, 42) and end-biomes' far regions;
//   * the outer chunks whose erosion sits nearest each edge — 0.2493 and
//     0.2547 about 0.25, -0.0594 about -0.0625, -0.2093 and -0.2204 about
//     -0.21875 — which is where a wrong comparison or a float would show;
//   * the edges themselves, exactly, from end-biome-router-probe.sh's
//     constant-erosion dimensions (the server's biome at each constant).
//
// The End here is a synthetic one: vanilla's own erosion,
// cache_2d(end_islands), in a hand-written settings entry, compiled through
// world::CompiledDimension with world::BiomeSource::theEnd() — the shipped
// path. Its surface rule reads the biome, so ChunkFiller's own lookup is
// held to the same answers.
#include "support/temp_path.hpp"

#include <stratum/biome/source.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/freeze/pipeline.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/terrain/filler.hpp>
#include <stratum/world/dimension.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using stratum::biome::EndRule;
using stratum::data::ResourceLocation;
using stratum::world::BiomeSource;
using stratum::world::CompiledDimension;

namespace {

/// A worldgen tree holding one settings entry, `minecraft:end`: vanilla's
/// End geometry and erosion, solid everywhere, and a surface rule that
/// paints end_midlands obsidian.
class SyntheticEnd {
public:
    SyntheticEnd() : path_(stratum::test::tempPath("stratum-biome-source-test")) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_ / "density_function");
        std::filesystem::create_directories(path_ / "noise_settings");
        nlohmann::json router = nlohmann::json::object();
        for (std::size_t i = 0; i < stratum::settings::kRouterEntryCount; ++i) {
            router[std::string(stratum::settings::routerEntryName(
                static_cast<stratum::settings::RouterEntry>(i)))] = 0.0;
        }
        router["erosion"] = {{"type", "minecraft:cache_2d"},
                             {"argument", {{"type", "minecraft:end_islands"}}}};
        router["final_density"] = 1.0;
        const nlohmann::json settings = {
            {"default_block", {{"Name", "minecraft:end_stone"}}},
            {"default_fluid", {{"Name", "minecraft:air"}}},
            {"sea_level", 0},
            {"disable_mob_generation", true},
            {"aquifers_enabled", false},
            {"ore_veins_enabled", false},
            {"legacy_random_source", true},
            {"noise",
             {{"min_y", 0}, {"height", 128}, {"size_horizontal", 2}, {"size_vertical", 1}}},
            {"noise_router", router},
            {"spawn_target", nlohmann::json::array()},
            {"surface_rule",
             {{"type", "minecraft:condition"},
              {"if_true", {{"type", "minecraft:biome"}, {"biome_is", {"minecraft:end_midlands"}}}},
              {"then_run",
               {{"type", "minecraft:block"},
                {"result_state", {{"Name", "minecraft:obsidian"}}}}}}}};
        std::ofstream out(path_ / "noise_settings" / "end.json");
        out << settings.dump();
    }

    SyntheticEnd(const SyntheticEnd&) = delete;
    SyntheticEnd& operator=(const SyntheticEnd&) = delete;
    SyntheticEnd(SyntheticEnd&&) = delete;
    SyntheticEnd& operator=(SyntheticEnd&&) = delete;

    ~SyntheticEnd() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    /// The tree as a frozen pipeline would carry it: no noises, no
    /// multi-noise lists — the End's source needs neither.
    [[nodiscard]] stratum::freeze::Pipeline pipeline() const {
        stratum::settings::LoadedSettings loaded =
            stratum::settings::loadAll(stratum::data::Pack::open(path_));
        stratum::freeze::Pipeline pipeline;
        pipeline.graph = std::move(loaded.graph);
        pipeline.settings = std::move(loaded.settings);
        return pipeline;
    }

private:
    std::filesystem::path path_;
};

struct Known {
    std::int64_t seed;
    std::int32_t chunkX;
    std::int32_t chunkZ;
    const char* biome;
};

// Every one the server's: (seed, chunk) -> the biome stored in every quart of
// that chunk, from the region named beside it.
constexpr std::array<Known, 38> kServer = {{
    // The ring at exactly 4096, both sides, on negative axes (end-biomes).
    {31337, -64, 0, "minecraft:the_end"},      // r.-2.0; outer it would be midlands
    {31337, -64, 1, "minecraft:end_midlands"}, // r.-2.0; 4097
    {8675309, 0, -64, "minecraft:the_end"},    // r.0.-2; outer it would be small
    {8675309, 1, -64, "minecraft:small_end_islands"},
    {2891948927356891, -34, -54, "minecraft:the_end"},     // r.-2.-2; 4072
    {-4172144997902289642, -55, -32, "minecraft:the_end"}, // r.-2.-1; 4049
    {-4172144997902289642, -64, -32, "minecraft:small_end_islands"},
    // Outer by chunk coordinates (4141), inside 1024 blocks by their nearest
    // corner (about 1008): the server says outer (end-biomes r.-2.-2).
    {2891948927356891, -46, -45, "minecraft:small_end_islands"},
    {2891948927356891, -45, -46, "minecraft:small_end_islands"},
    // r.64.0 at seed 0 (end-islands): every band, and the three edges.
    {0, 2048, 0, "minecraft:end_highlands"},
    {0, 2053, 0, "minecraft:end_midlands"},
    {0, 2063, 1, "minecraft:end_barrens"},
    {0, 2064, 0, "minecraft:small_end_islands"},
    {0, 2062, 0, "minecraft:end_midlands"},       // erosion 0.2493
    {0, 2054, 0, "minecraft:end_midlands"},       // -0.0594
    {0, 2063, 13, "minecraft:small_end_islands"}, // -0.2204
    // r.64.0 at seed 42.
    {42, 2067, 0, "minecraft:end_highlands"},
    {42, 2048, 0, "minecraft:end_midlands"},
    {42, 2049, 0, "minecraft:end_barrens"},
    {42, 2058, 0, "minecraft:small_end_islands"},
    {42, 2062, 1, "minecraft:small_end_islands"}, // -0.2193
    // r.-65.3 at 31337: negative x, positive z.
    {31337, -2069, 96, "minecraft:end_highlands"},
    {31337, -2071, 96, "minecraft:end_midlands"},
    {31337, -2072, 96, "minecraft:end_barrens"},
    {31337, -2080, 96, "minecraft:small_end_islands"},
    {31337, -2072, 111, "minecraft:end_highlands"}, // 0.2547
    // r.40.-50 at 8675309: positive x, negative z.
    {8675309, 1291, -1591, "minecraft:end_highlands"},
    {8675309, 1285, -1600, "minecraft:end_midlands"},
    {8675309, 1283, -1600, "minecraft:end_barrens"}, // -0.2093
    {8675309, 1280, -1600, "minecraft:small_end_islands"},
    // r.-30.-45 at 2891948927356891: both negative.
    {2891948927356891, -960, -1440, "minecraft:end_highlands"},
    {2891948927356891, -956, -1440, "minecraft:end_midlands"},
    {2891948927356891, -932, -1440, "minecraft:end_barrens"},
    {2891948927356891, -935, -1440, "minecraft:small_end_islands"},
    {2891948927356891, -955, -1439, "minecraft:end_midlands"}, // -0.0594
    // r.70.20 at -4172144997902289642.
    {-4172144997902289642, 2265, 640, "minecraft:end_highlands"},
    {-4172144997902289642, 2262, 640, "minecraft:end_midlands"},
    {-4172144997902289642, 2261, 640, "minecraft:end_barrens"},
}};

} // namespace

TEST_CASE("the End's central test is chunkX^2 + chunkZ^2 <= 4096, in 64 bits", "[biome][end]") {
    // The server's ring chunks (end-biomes): 4096 inside, 4097 outside.
    CHECK(EndRule::isCentral(-64, 0));
    CHECK(EndRule::isCentral(0, -64));
    CHECK(EndRule::isCentral(64, 0));
    CHECK(EndRule::isCentral(0, 64));
    CHECK_FALSE(EndRule::isCentral(-64, 1));
    CHECK_FALSE(EndRule::isCentral(1, -64));
    CHECK_FALSE(EndRule::isCentral(65, 0));
    // In chunk coordinates, not by the nearest corner: (-46, -45) is 4141
    // chunks squared from the origin though its nearest corner is about 1008
    // blocks out (the wiki's "distance from the chunk edges"); the server
    // stores it outer (the known answers below).
    CHECK(EndRule::isCentral(45, 45));
    CHECK_FALSE(EndRule::isCentral(-46, -45));
    CHECK_FALSE(EndRule::isCentral(45, 46));
    // A 32-bit square would wrap: 65536^2 is 2^32, zero in an int.
    CHECK_FALSE(EndRule::isCentral(65536, 65536));
    CHECK_FALSE(EndRule::isCentral(46341, 0));
    CHECK_FALSE(EndRule::isCentral(std::numeric_limits<std::int32_t>::min(), 0));
}

TEST_CASE("the End's erosion edges are the server's, in double", "[biome][end]") {
    // end-biome-router-probe.sh: each constant erosion the server turned
    // into one outer biome. The values 1e-10 off an edge are what pins the
    // comparison to double: as floats they are the edge itself.
    const std::array<std::pair<double, const char*>, 14> server = {{
        {0.5, "minecraft:end_highlands"},
        {0.25001, "minecraft:end_highlands"},
        {0.2500000001, "minecraft:end_highlands"},
        {0.25, "minecraft:end_midlands"},
        {0.1, "minecraft:end_midlands"},
        {0.0, "minecraft:end_midlands"},
        {-0.0625, "minecraft:end_midlands"},
        {-0.0625000001, "minecraft:end_barrens"},
        {-0.06251, "minecraft:end_barrens"},
        {-0.1, "minecraft:end_barrens"},
        {-0.21875, "minecraft:end_barrens"},
        {-0.2187500001, "minecraft:small_end_islands"},
        {-0.21876, "minecraft:small_end_islands"},
        {-0.5, "minecraft:small_end_islands"},
    }};
    for (const auto& [erosion, biome] : server) {
        CAPTURE(erosion);
        CHECK(EndRule::outer(erosion).toString() == biome);
    }
    // Not measured: nothing on disk makes a NaN. cubiomes' rings — an int
    // overflow in the island field — come out barrens, and this order of
    // the tests is the one that sends a NaN there.
    CHECK(EndRule::outer(std::numeric_limits<double>::quiet_NaN()).toString() ==
          "minecraft:end_barrens");
    CHECK(EndRule::outer(std::numeric_limits<double>::infinity()).toString() ==
          "minecraft:end_highlands");
    CHECK(EndRule::outer(-std::numeric_limits<double>::infinity()).toString() ==
          "minecraft:small_end_islands");
    CHECK(EndRule::central().toString() == "minecraft:the_end");
}

TEST_CASE("the End samples a chunk at its centre, wrapping as Java's int does", "[biome][end]") {
    const auto at = EndRule::samplePoint(2048, 36, 0);
    CHECK(at.x == 32776);
    CHECK(at.y == 36);
    CHECK(at.z == 8);
    const auto negative = EndRule::samplePoint(-64, 0, 1);
    CHECK(negative.x == -1016);
    CHECK(negative.z == 24);
    // (2^31 - 1) * 16 + 8 is 2^35 - 8: -8 once wrapped to 32 bits.
    CHECK(EndRule::samplePoint(std::numeric_limits<std::int32_t>::max(), 0, 0).x == -8);
}

TEST_CASE("a biome source reads only the router entries its type reads", "[biome]") {
    const auto end = stratum::biome::Source::theEnd();
    REQUIRE(end.routerEntries().size() == 1U);
    CHECK(end.routerEntries()[0] == stratum::settings::RouterEntry::Erosion);
    const stratum::biome::ParameterList empty;
    CHECK(stratum::biome::Source::multiNoise(empty).routerEntries().size() == 6U);
}

TEST_CASE("the End compiled with its own biome source stores the server's biomes",
          "[biome][end][world]") {
    const SyntheticEnd tree;
    const auto endId = ResourceLocation::parse("minecraft:end");
    std::map<std::int64_t, std::unique_ptr<CompiledDimension>> bySeed;
    for (const Known& known : kServer) {
        CAPTURE(known.seed, known.chunkX, known.chunkZ, known.biome);
        auto& dimension = bySeed[known.seed];
        if (!dimension) {
            dimension = CompiledDimension::compile(tree.pipeline(), endId, BiomeSource::theEnd(),
                                                   known.seed);
        }
        std::vector<const ResourceLocation*> biomes(std::size_t{16} * 32U);
        dimension->fillBiomes(known.chunkX, known.chunkZ, biomes);
        std::size_t wrong = 0;
        for (const ResourceLocation* biome : biomes) {
            wrong += biome->toString() == known.biome ? 0U : 1U;
        }
        CHECK(wrong == 0U);
    }
}

TEST_CASE("ChunkFiller's surface pass reads the End's biome where fillBiomes does",
          "[biome][end][surface]") {
    // The tree paints end_midlands obsidian. At seed 31337, chunk (-64, 1) is
    // end_midlands and (-64, 0) — one chunk over, on the ring — is the_end:
    // the surface pass must agree with fillBiomes across that edge, with
    // floorDiv from block to chunk at x = -1024..-1009.
    const SyntheticEnd tree;
    const auto dimension = CompiledDimension::compile(
        tree.pipeline(), ResourceLocation::parse("minecraft:end"), BiomeSource::theEnd(), 31337);
    const auto count = [&dimension](std::int32_t chunkX, std::int32_t chunkZ) {
        stratum::terrain::ChunkBuffer blocks(dimension->geometry());
        dimension->fillBlocks(chunkX, chunkZ, blocks);
        std::size_t obsidian = 0;
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                for (std::int32_t y = 0; y < 128; y += 9) {
                    obsidian +=
                        blocks.at(x, y, z).name.toString() == "minecraft:obsidian" ? 1U : 0U;
                }
            }
        }
        return obsidian;
    };
    // 15 sampled layers (0, 9, .., 126) of 256 columns.
    CHECK(count(-64, 1) == std::size_t{15} * 256U);
    CHECK(count(-64, 0) == 0U);
}

TEST_CASE("a biome source is named by type, and the types this build lacks are refused by name",
          "[biome][world]") {
    CHECK(BiomeSource::parse("minecraft:the_end") == BiomeSource::theEnd());
    CHECK(BiomeSource::parse("the_end") == BiomeSource::theEnd());
    CHECK(BiomeSource::parse("minecraft:overworld") ==
          BiomeSource::multiNoise(ResourceLocation::parse("minecraft:overworld")));
    CHECK(BiomeSource::parse("nether") ==
          BiomeSource::multiNoise(ResourceLocation::parse("minecraft:nether")));
    CHECK(BiomeSource::parse("minecraft:the_end").toString() == "minecraft:the_end");
    CHECK(BiomeSource::parse("overworld").toString() == "minecraft:overworld");
    CHECK(BiomeSource::theEnd().type() == BiomeSource::Type::TheEnd);
    CHECK_FALSE(BiomeSource::theEnd().parameterList().has_value());

    CHECK_THROWS_WITH(BiomeSource::parse("minecraft:fixed"),
                      ContainsSubstring("biome source 'minecraft:fixed' is not implemented"));
    CHECK_THROWS_WITH(
        BiomeSource::parse("checkerboard"),
        ContainsSubstring("biome source 'minecraft:checkerboard' is not implemented"));
    CHECK_THROWS_WITH(BiomeSource::parse("minecraft:multi_noise"),
                      ContainsSubstring("names no parameter list"));
    // Another namespace's `fixed` is not vanilla's type: it names a list.
    CHECK(BiomeSource::parse("stratum:fixed").type() == BiomeSource::Type::MultiNoise);

    // The End's source needs no list, so a pipeline with none compiles it;
    // a multi-noise source still needs its own.
    const SyntheticEnd tree;
    const auto end = ResourceLocation::parse("minecraft:end");
    CHECK_NOTHROW(CompiledDimension::compile(tree.pipeline(), end, BiomeSource::theEnd(), 1));
    CHECK_THROWS_WITH(CompiledDimension::compile(tree.pipeline(), end,
                                                 BiomeSource::parse("minecraft:overworld"), 1),
                      ContainsSubstring("no biome parameter list 'minecraft:overworld'"));
}
