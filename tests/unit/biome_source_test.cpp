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
//     constant-erosion dimensions (the server's biome at each constant);
//   * the y erosion is read at, 4 * quartY, from the same probe's y-varying
//     dimensions: plain y_clamped_gradients, no Mojang data, so the
//     server's layer-by-layer answer can stand here with nothing else.
//
// What stays out of reach without fixtures: the exact block a chunk is
// sampled at within its 8-block island cell (16c + 8 rather than 16c + 9 ..
// 16c + 15). The server says so through end-biome-router-probe.sh --set
// sample, a vanilla noise this file does not carry, so only the
// conformance case and samplePoint's own values below hold it.
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
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_parameters.hpp>
#include <stratum/density/noise_registry.hpp>
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

/// Vanilla's End erosion, cache_2d(end_islands).
[[nodiscard]] nlohmann::json vanillaErosion() {
    return {{"type", "minecraft:cache_2d"}, {"argument", {{"type", "minecraft:end_islands"}}}};
}

/// The one named noise a synthetic tree's frozen pipeline carries.
[[nodiscard]] ResourceLocation wobble() {
    return ResourceLocation{"stratum", "wobble"};
}

/// A worldgen tree holding one settings entry, `minecraft:end`: vanilla's
/// End geometry, solid everywhere, a surface rule that paints end_midlands
/// obsidian, and @p erosion — vanilla's own unless a case says otherwise —
/// under the random source @p legacy names.
class SyntheticEnd {
public:
    explicit SyntheticEnd(const nlohmann::json& erosion = vanillaErosion(), bool legacy = true)
        : path_(stratum::test::tempPath("stratum-biome-source-test")) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_ / "density_function");
        std::filesystem::create_directories(path_ / "noise_settings");
        nlohmann::json router = nlohmann::json::object();
        for (std::size_t i = 0; i < stratum::settings::kRouterEntryCount; ++i) {
            router[std::string(stratum::settings::routerEntryName(
                static_cast<stratum::settings::RouterEntry>(i)))] = 0.0;
        }
        router["erosion"] = erosion;
        router["final_density"] = 1.0;
        const nlohmann::json settings = {
            {"default_block", {{"Name", "minecraft:end_stone"}}},
            {"default_fluid", {{"Name", "minecraft:air"}}},
            {"sea_level", 0},
            {"disable_mob_generation", true},
            {"aquifers_enabled", false},
            {"ore_veins_enabled", false},
            {"legacy_random_source", legacy},
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

    /// The tree as a frozen pipeline would carry it: one noise, for an
    /// erosion that names it, and no multi-noise lists — the End's source
    /// needs none.
    [[nodiscard]] stratum::freeze::Pipeline pipeline() const {
        stratum::settings::LoadedSettings loaded =
            stratum::settings::loadAll(stratum::data::Pack::open(path_));
        stratum::freeze::Pipeline pipeline;
        pipeline.graph = std::move(loaded.graph);
        pipeline.settings = std::move(loaded.settings);
        pipeline.noises.emplace(wobble(), stratum::density::NoiseParameters{
                                              .firstOctave = -4, .amplitudes = {1.0, 0.5}});
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
    // NOT MEASURED, and this build's own choice: nothing on disk makes a
    // NaN. Testing small islands before the barrens fall-through sends one
    // to end_barrens, the biome cubiomes gives its outer rings, by the same
    // overflow test that makes its End density column NaN there; cubiomes'
    // own order of these tests would send it to small_end_islands.
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

TEST_CASE("the End reads a y-varying erosion at y = 4 * quartY, layer by layer as the server did",
          "[biome][end][world]") {
    // end-biome-router-probe.sh, seed 42, r.-2.-2: the server's biome at
    // each of the 64 stored layers of its outer chunks, all of which agree
    // (the gradients read no x or z), as runs of (first layer, biome). Read
    // out of the probe's regions; the erosions are the probe's own
    // y_clamped_gradients, from -1 at from_y to 1 at to_y. ygrad puts two
    // layers exactly on edges — y 60 at -0.0625 and y 80 at 0.25, both
    // midlands — and the steps separate 4 qy from 4 qy + 1 (ystep_0) and
    // from the bare quart index (ystep_4).
    struct Run {
        std::int32_t firstLayer;
        const char* biome;
    };

    struct Gradient {
        const char* name;
        std::int32_t fromY;
        std::int32_t toY;
        std::vector<Run> server;
    };

    const std::array<Gradient, 3> gradients = {{
        {"erosion_ygrad",
         0,
         128,
         {{0, "minecraft:small_end_islands"},
          {13, "minecraft:end_barrens"},
          {15, "minecraft:end_midlands"},
          {21, "minecraft:end_highlands"}}},
        {"erosion_ystep_0",
         0,
         1,
         {{0, "minecraft:small_end_islands"}, {1, "minecraft:end_highlands"}}},
        {"erosion_ystep_4",
         4,
         5,
         {{0, "minecraft:small_end_islands"}, {2, "minecraft:end_highlands"}}},
    }};
    constexpr std::int32_t kLayers = 64;
    const auto endId = ResourceLocation::parse("minecraft:end");
    for (const Gradient& gradient : gradients) {
        CAPTURE(gradient.name);
        const SyntheticEnd tree({{"type", "minecraft:y_clamped_gradient"},
                                 {"from_y", gradient.fromY},
                                 {"to_y", gradient.toY},
                                 {"from_value", -1.0},
                                 {"to_value", 1.0}});
        const auto dimension =
            CompiledDimension::compile(tree.pipeline(), endId, BiomeSource::theEnd(), 42);
        // Three outer chunks of r.-2.-2 the decoded server biomes were read
        // from: its far corner, (-46, -45) just outside the ring at 4141,
        // and (-33, -64).
        for (const auto& [chunkX, chunkZ] : std::array<std::pair<std::int32_t, std::int32_t>, 3>{
                 {{-64, -64}, {-46, -45}, {-33, -64}}}) {
            CAPTURE(chunkX, chunkZ);
            std::vector<const ResourceLocation*> column(std::size_t{16} * kLayers);
            dimension->fillBiomes(chunkX, chunkZ, 0, column);
            std::size_t wrong = 0;
            for (std::int32_t layer = 0; layer < kLayers; ++layer) {
                std::string expected = gradient.server.front().biome;
                for (const Run& run : gradient.server) {
                    if (run.firstLayer <= layer) {
                        expected = run.biome;
                    }
                }
                for (std::size_t quart = 0; quart < 16U; ++quart) {
                    const ResourceLocation* ours =
                        column[(static_cast<std::size_t>(layer) * 16U) + quart];
                    wrong += ours->toString() == expected ? 0U : 1U;
                }
            }
            CHECK(wrong == 0U);
        }
    }
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

TEST_CASE("an End whose erosion reads y is read at each layer's own y, wherever it is asked",
          "[biome][end][world]") {
    // Vanilla's End erosion reads no y, so a chunk's one sample serves every
    // layer, and fillBiomes takes it once when Interpreter::isColumnInvariant
    // says so. A datapack's need not: old_blended_noise and
    // weird_scaled_sampler sample noise at the point's own y, and once had
    // themselves reported invariant — old_blended_noise has no arguments,
    // weird_scaled_sampler's input here is a constant — so fillBiomes read
    // the bottom layer's biome into all 64. biome::Source::at, which the
    // surface pass calls, read each layer's own, as the server does
    // (end-biome-router-probe.sh: erosion is read at block y = 4 * qy).
    // fillBiomes must agree with Source::at quart for quart, over the
    // server's whole 256-block column — the noise height's 32 layers and the
    // 32 above them, through the range form — and the columns must actually
    // vary, or agreeing proves nothing.
    struct Erosion {
        const char* name;
        nlohmann::json json;
        bool legacy;
    };

    const std::array<Erosion, 2> erosions = {{
        {"old_blended_noise",
         {{"type", "minecraft:old_blended_noise"},
          {"xz_scale", 0.25},
          {"y_scale", 0.125},
          {"xz_factor", 80.0},
          {"y_factor", 160.0},
          {"smear_scale_multiplier", 8.0}},
         true},
        {"weird_scaled_sampler",
         {{"type", "minecraft:weird_scaled_sampler"},
          {"rarity_value_mapper", "type_1"},
          {"noise", "stratum:wobble"},
          {"input", 0.5}},
         false},
    }};
    const auto endId = ResourceLocation::parse("minecraft:end");
    constexpr std::int32_t kLayers = 64;
    for (const Erosion& erosion : erosions) {
        CAPTURE(erosion.name);
        const SyntheticEnd tree(erosion.json, erosion.legacy);
        const stratum::freeze::Pipeline pipeline = tree.pipeline();
        const auto dimension =
            CompiledDimension::compile(pipeline, endId, BiomeSource::theEnd(), 20260915);

        // Source::at over its own interpreter on the same pipeline.
        const stratum::settings::NoiseSettings& settings = pipeline.settings.at(endId);
        const std::vector<stratum::density::NodeIndex> roots(settings.router.entries.begin(),
                                                             settings.router.entries.end());
        const stratum::density::NoiseRegistry noises = stratum::density::NoiseRegistry::create(
            pipeline.noises, pipeline.graph.noisesReachableFrom(roots), 20260915,
            erosion.legacy ? stratum::density::RandomSource::Legacy
                           : stratum::density::RandomSource::Xoroshiro);
        const stratum::density::Interpreter interpreter(
            pipeline.graph, noises,
            stratum::density::CellGeometry{.width = settings.geometry.cellWidth(),
                                           .height = settings.geometry.cellHeight()});
        stratum::density::Interpreter::CornerCache cache(interpreter.cacheSize());
        const stratum::biome::Source source = stratum::biome::Source::theEnd();

        std::size_t quarts = 0;
        std::size_t differing = 0;
        std::size_t layersOffTheBottom = 0;
        // Outer chunks, both signs, and one inside the ring.
        for (const auto& [chunkX, chunkZ] : std::array<std::pair<std::int32_t, std::int32_t>, 5>{
                 {{2048, 0}, {2049, 3}, {-2050, -7}, {-70, 11}, {0, 0}}}) {
            CAPTURE(chunkX, chunkZ);
            std::vector<const ResourceLocation*> column(std::size_t{16} * kLayers);
            dimension->fillBiomes(chunkX, chunkZ, 0, column);
            std::vector<const ResourceLocation*> noiseHeight(std::size_t{16} * 32U);
            dimension->fillBiomes(chunkX, chunkZ, noiseHeight);
            std::size_t formsDiffering = 0;
            for (std::size_t i = 0; i < noiseHeight.size(); ++i) {
                formsDiffering += noiseHeight[i] == column[i] ? 0U : 1U;
            }
            CHECK(formsDiffering == 0U);
            for (std::int32_t layer = 0; layer < kLayers; ++layer) {
                for (std::int32_t qz = 0; qz < 4; ++qz) {
                    for (std::int32_t qx = 0; qx < 4; ++qx) {
                        const ResourceLocation& expected =
                            source.at(interpreter, settings.router,
                                      stratum::density::Point{.x = (chunkX * 16) + (qx * 4),
                                                              .y = layer * 4,
                                                              .z = (chunkZ * 16) + (qz * 4)},
                                      cache);
                        const std::size_t at = (static_cast<std::size_t>(layer) * 16U) +
                                               static_cast<std::size_t>((qz * 4) + qx);
                        ++quarts;
                        differing += column[at] == &expected ? 0U : 1U;
                    }
                }
                layersOffTheBottom +=
                    column[static_cast<std::size_t>(layer) * 16U] == column[0] ? 0U : 1U;
            }
        }
        CHECK(quarts == std::size_t{5} * 16U * kLayers);
        CHECK(differing == 0U);
        // Not vacuous: the erosion moves the biome from layer to layer.
        CHECK(layersOffTheBottom > 0U);
    }
}

TEST_CASE("fillBiomes reads any run of whole layers, and refuses a part of one",
          "[biome][end][world]") {
    const SyntheticEnd tree;
    const auto dimension = CompiledDimension::compile(
        tree.pipeline(), ResourceLocation::parse("minecraft:end"), BiomeSource::theEnd(), 31337);
    // Below, at and above the noise height (y -64..319, PocketMine-MP's
    // column): the End's one biome per chunk, at every y. Chunk (-64, 1) is
    // end_midlands on the server (end-biomes r.-2.0).
    std::vector<const ResourceLocation*> column(std::size_t{16} * 96U);
    dimension->fillBiomes(-64, 1, -16, column);
    std::size_t wrong = 0;
    for (const ResourceLocation* biome : column) {
        wrong += biome->toString() == "minecraft:end_midlands" ? 0U : 1U;
    }
    CHECK(wrong == 0U);
    std::vector<const ResourceLocation*> none;
    CHECK_NOTHROW(dimension->fillBiomes(-64, 1, 0, none));
    std::vector<const ResourceLocation*> partial(17);
    CHECK_THROWS_WITH(dimension->fillBiomes(-64, 1, 0, partial),
                      ContainsSubstring("not a whole number of 16-quart layers"));
    // The noise-height form still insists on the noise height.
    CHECK_THROWS_WITH(dimension->fillBiomes(-64, 1, column),
                      ContainsSubstring("for a dimension that needs 512"));
}
