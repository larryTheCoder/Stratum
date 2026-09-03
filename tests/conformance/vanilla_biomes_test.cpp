// Stratum — the biome source against the biomes vanilla itself recorded.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// This is the first thing in the project to be checked against vanilla's own
// *output* rather than against another reimplementation of it. The golden
// regions store a biome per 4x4x4 cell; the climate chain and the parameter
// table between them decide one; the two are compared directly.
//
// The parameter table is not in the jar as data — the pack ships only
// `{"preset": "minecraft:overworld"}` — so tools/fetch-vanilla asks the
// server's own data generator to dump it. Mojang-derived and never committed
// (SPEC §12), so without the fixtures this SKIPs.

#include <stratum/biome/parameter_list.hpp>
#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/settings/noise_settings.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace {

using stratum::biome::ClimateSample;
using stratum::biome::ParameterList;
using stratum::data::Pack;
using stratum::data::ResourceLocation;
using stratum::settings::RouterEntry;

[[nodiscard]] std::filesystem::path fixtures() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR};
}

[[nodiscard]] std::filesystem::path findWorldgenTree() {
    if (!std::filesystem::is_directory(fixtures())) {
        return {};
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(fixtures())) {
        if (entry.is_directory() && entry.path().filename() == "worldgen" &&
            std::filesystem::is_directory(entry.path() / "density_function")) {
            return entry.path();
        }
    }
    return {};
}

[[nodiscard]] std::filesystem::path findParameterList() {
    if (!std::filesystem::is_directory(fixtures())) {
        return {};
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(fixtures())) {
        if (entry.is_regular_file() && entry.path().filename() == "overworld.json" &&
            entry.path().parent_path().parent_path().filename() == "biome_parameters") {
            return entry.path();
        }
    }
    return {};
}

/// Compares the biome this engine chooses against the one vanilla stored,
/// over @p span by @p span chunks of one golden region.
struct Comparison {
    std::size_t cells = 0;
    std::size_t exact = 0;
    std::map<std::string, std::size_t> misses;
};

[[nodiscard]] Comparison compare(const Pack& pack, const ParameterList& table, std::int64_t seed,
                                 const std::filesystem::path& regionPath, std::int32_t span) {
    stratum::settings::LoadedSettings loaded = stratum::settings::loadAll(pack);
    const auto& overworld = loaded.settings.at(ResourceLocation::parse("minecraft:overworld"));
    const auto noises = stratum::density::NoiseRegistry::create(
        pack, loaded.graph.referencedNoises(), seed, stratum::density::RandomSource::Xoroshiro);
    const stratum::density::Interpreter interpreter(
        loaded.graph, noises,
        stratum::density::CellGeometry{.width = overworld.geometry.cellWidth(),
                                       .height = overworld.geometry.cellHeight()});

    const auto region = stratum::region::RegionFile::open(regionPath);
    Comparison result;
    for (std::int32_t chunkZ = 0; chunkZ < span; ++chunkZ) {
        for (std::int32_t chunkX = 0; chunkX < span; ++chunkX) {
            if (!region.hasChunk(chunkX, chunkZ)) {
                continue;
            }
            const auto document = stratum::nbt::read(region.readChunk(chunkX, chunkZ));
            const auto chunk = stratum::chunk::Chunk::decode(document.root);
            for (const auto& section : chunk.sections()) {
                if (section.biomes.empty()) {
                    continue;
                }
                // Biomes are stored one per 4x4x4 cell, 64 to a section.
                for (int cellY = 0; cellY < 4; ++cellY) {
                    for (int cellZ = 0; cellZ < 4; ++cellZ) {
                        for (int cellX = 0; cellX < 4; ++cellX) {
                            const std::size_t index = (static_cast<std::size_t>(cellY) * 16U) +
                                                      (static_cast<std::size_t>(cellZ) * 4U) +
                                                      static_cast<std::size_t>(cellX);
                            if (index >= section.biomes.size()) {
                                continue;
                            }
                            const std::string& golden = section.biomePalette[section.biomes[index]];
                            const stratum::density::Point at{.x = (chunkX * 16) + (cellX * 4),
                                                             .y = (section.y * 16) + (cellY * 4),
                                                             .z = (chunkZ * 16) + (cellZ * 4)};
                            const auto sample = ClimateSample{
                                .temperature = interpreter.evaluate(
                                    overworld.router.at(RouterEntry::Temperature), at),
                                .humidity = interpreter.evaluate(
                                    overworld.router.at(RouterEntry::Vegetation), at),
                                .continentalness = interpreter.evaluate(
                                    overworld.router.at(RouterEntry::Continents), at),
                                .erosion = interpreter.evaluate(
                                    overworld.router.at(RouterEntry::Erosion), at),
                                .depth = interpreter.evaluate(
                                    overworld.router.at(RouterEntry::Depth), at),
                                .weirdness = interpreter.evaluate(
                                    overworld.router.at(RouterEntry::Ridges), at)};
                            ++result.cells;
                            const std::string chosen = table.find(sample).toString();
                            if (chosen == golden) {
                                ++result.exact;
                            } else {
                                ++result.misses[golden + " -> " + chosen];
                            }
                        }
                    }
                }
            }
        }
    }
    return result;
}

} // namespace

TEST_CASE("the biome source reproduces the biomes vanilla recorded", "[conformance][biome]") {
    const std::filesystem::path tree = findWorldgenTree();
    const std::filesystem::path parameters = findParameterList();
    if (tree.empty() || parameters.empty()) {
        SKIP("no extracted vanilla worldgen or biome parameters under "
             << STRATUM_FIXTURES_DIR
             << " — Mojang-derived and never committed (SPEC §12). Generate them with: "
                "tools/fetch-vanilla");
    }
    const std::filesystem::path region =
        fixtures() / "1.21.11" / "regions" / "seed-42" / "overworld" / "r.0.0.mca";
    if (!std::filesystem::is_regular_file(region)) {
        SKIP("no golden regions; generate them with: tools/fetch-vanilla "
             "--generate-regions --accept-eula");
    }

    std::ifstream file(parameters);
    std::stringstream contents;
    contents << file.rdbuf();
    const ParameterList table = ParameterList::fromJson(
        nlohmann::json::parse(contents.str()), ResourceLocation::parse("minecraft:overworld"));
    // 7593 rows over 54 biomes. Pinned because it is what the pinned version
    // dumps, and a change in either number means the schema pin moved.
    CHECK(table.size() == 7593U);

    const Pack pack = Pack::open(tree);
    const Comparison result = compare(pack, table, 42, region, 4);

    // Every cell, not a threshold. This seed used to sit at 24500: the
    // residual was seventy-five cells in a single column where `beach` and
    // `dark_forest` are 9e-9 apart in double arithmetic and *exactly equal*
    // once quantised. Quantising plus taking the later of a tied pair closes
    // it. Pinned at the whole so that any regression is visible as one.
    CHECK(result.cells == 24576U);
    CHECK(result.exact == 24576U);
    CHECK(result.misses.empty());
}

TEST_CASE("three other seeds are exact too", "[conformance][biome]") {
    const std::filesystem::path tree = findWorldgenTree();
    const std::filesystem::path parameters = findParameterList();
    if (tree.empty() || parameters.empty()) {
        SKIP("no extracted vanilla worldgen or biome parameters under " << STRATUM_FIXTURES_DIR);
    }

    std::ifstream file(parameters);
    std::stringstream contents;
    contents << file.rdbuf();
    const ParameterList table = ParameterList::fromJson(
        nlohmann::json::parse(contents.str()), ResourceLocation::parse("minecraft:overworld"));
    const Pack pack = Pack::open(tree);

    // Three more seeds, 73728 further cells. Seed -4172144997902289642 is
    // the load-bearing one: it is exact under the old double-precision
    // search *and* under the new one, but quantising without the tie-break
    // breaks it. That it takes both changes together to keep all four seeds
    // whole is why the tie-break is not just seed 42 being fitted.
    for (const std::int64_t seed :
         {std::int64_t{0}, std::int64_t{-1}, std::int64_t{-4172144997902289642}}) {
        const std::filesystem::path region = fixtures() / "1.21.11" / "regions" /
                                             ("seed-" + std::to_string(seed)) / "overworld" /
                                             "r.0.0.mca";
        if (!std::filesystem::is_regular_file(region)) {
            continue;
        }
        CAPTURE(seed);
        const Comparison result = compare(pack, table, seed, region, 4);
        CHECK(result.cells == 24576U);
        CHECK(result.exact == result.cells);
    }
}
