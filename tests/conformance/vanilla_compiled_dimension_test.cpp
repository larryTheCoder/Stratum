// Stratum — a world generated from its frozen blob is the world its pack makes.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// SPEC §6 promises that generation reads a world's stored pipeline and never
// the live pack. That promise is only worth something if the two produce the
// same terrain, block for block. This compiles vanilla's overworld through
// world::CompiledDimension from a pipeline that has been frozen and thawed,
// and compares every block state and every quart biome against a reference
// built straight from the pack, the way the bindings built it before the
// shared core existed — with every object a local in one scope, so the
// reference cannot share the core's construction or its mistakes.
//
// Mojang-derived fixtures are never committed (SPEC §12); without them this
// SKIPs, naming the command that produces them.

#include <stratum/biome/parameter_list.hpp>
#include <stratum/biome/temperature_table.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/freeze/pipeline.hpp>
#include <stratum/javamath.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/rule_graph.hpp>
#include <stratum/terrain/filler.hpp>
#include <stratum/world/dimension.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using stratum::data::Pack;
using stratum::data::ResourceLocation;
using stratum::world::CompiledDimension;

namespace {

[[nodiscard]] std::filesystem::path versionDir() {
    return std::filesystem::path(STRATUM_FIXTURES_DIR) / "1.21.11";
}

[[nodiscard]] bool haveFixtures() {
    return std::filesystem::is_directory(versionDir() / "worldgen" / "noise_settings") &&
           std::filesystem::is_regular_file(versionDir() / "biome_parameters" / "minecraft" /
                                            "overworld.json");
}

[[nodiscard]] stratum::freeze::Pipeline thawedVanilla() {
    const Pack pack = Pack::open(versionDir() / "worldgen");
    return stratum::freeze::read(
        stratum::freeze::write(stratum::freeze::resolve(pack, versionDir() / "biome_parameters")));
}

} // namespace

TEST_CASE("a dimension compiled from a thawed blob fills what its pack fills",
          "[conformance][world]") {
    if (!haveFixtures()) {
        SKIP("no worldgen or biome_parameters fixtures under " << STRATUM_FIXTURES_DIR
                                                               << "; run tools/fetch-vanilla");
    }
    const auto overworld = ResourceLocation::parse("minecraft:overworld");

    for (const std::int64_t seed : {std::int64_t{0}, std::int64_t{-4172144997902289642}}) {
        CAPTURE(seed);
        const auto dimension =
            CompiledDimension::compile(thawedVanilla(), overworld, overworld, seed);

        // The reference: straight from the pack, every object a local here.
        const Pack pack = Pack::open(versionDir() / "worldgen");
        const stratum::settings::LoadedSettings loaded = stratum::settings::loadAll(pack);
        // Vanilla's overworld verbatim — ore veins included. CompiledDimension
        // used to force the flag off and this reference had to match; now
        // both run the vein system, so the two paths are compared on the
        // real settings rather than on a shared approximation of them.
        const stratum::settings::NoiseSettings settings = loaded.settings.at(overworld);
        std::ifstream parametersFile(versionDir() / "biome_parameters" / "minecraft" /
                                     "overworld.json");
        const auto parameters = stratum::biome::ParameterList::fromJson(
            nlohmann::json::parse(parametersFile), overworld);
        const auto temperatures = stratum::biome::TemperatureTable::fromPack(pack);
        const auto rules = stratum::surface::RuleGraph::resolve(settings.surfaceRule, overworld);
        std::vector<ResourceLocation> wanted = loaded.graph.referencedNoises();
        for (const ResourceLocation& id : rules.referencedNoises()) {
            wanted.push_back(id);
        }
        for (const char* id :
             {"minecraft:surface", "minecraft:surface_secondary", "minecraft:clay_bands_offset"}) {
            wanted.push_back(ResourceLocation::parse(id));
        }
        const auto noises = stratum::density::NoiseRegistry::create(
            pack, wanted, seed, stratum::density::RandomSource::Xoroshiro);
        const auto filler = stratum::terrain::ChunkFiller::compile(
            loaded.graph, noises, settings, &rules, &parameters, &temperatures);
        const stratum::density::Interpreter interpreter(
            loaded.graph, noises,
            stratum::density::CellGeometry{.width = settings.geometry.cellWidth(),
                                           .height = settings.geometry.cellHeight()});

        REQUIRE(dimension->geometry() == settings.geometry);
        // Not a comparison of two bare-stone chunks: the surface rules run.
        REQUIRE(filler.runsSurfaceRules());
        const std::int32_t quartsHigh = stratum::javamath::floorDiv(settings.geometry.height, 4);
        for (const auto& [chunkX, chunkZ] :
             std::array<std::pair<std::int32_t, std::int32_t>, 3>{{{0, 0}, {-3, 2}, {5, -7}}}) {
            CAPTURE(chunkX, chunkZ);

            stratum::terrain::ChunkBuffer fromBlob(dimension->geometry());
            dimension->fillBlocks(chunkX, chunkZ, fromBlob);
            stratum::terrain::ChunkBuffer fromPack(settings.geometry);
            filler.fill(chunkX, chunkZ, fromPack);
            std::size_t differing = 0;
            for (std::int32_t y = settings.geometry.minY; y < settings.geometry.maxY(); ++y) {
                for (int z = 0; z < 16; ++z) {
                    for (int x = 0; x < 16; ++x) {
                        differing += fromBlob.at(x, y, z) == fromPack.at(x, y, z) ? 0U : 1U;
                    }
                }
            }
            CHECK(differing == 0U);
            CHECK(fromPack.paletteSize() > 4U);

            std::vector<const ResourceLocation*> biomes(16U * static_cast<std::size_t>(quartsHigh));
            dimension->fillBiomes(chunkX, chunkZ, biomes);
            stratum::density::Interpreter::CornerCache cache(interpreter.cacheSize());
            std::size_t index = 0;
            std::size_t biomesDiffering = 0;
            for (std::int32_t qy = 0; qy < quartsHigh; ++qy) {
                for (std::int32_t qz = 0; qz < 4; ++qz) {
                    for (std::int32_t qx = 0; qx < 4; ++qx) {
                        const stratum::density::Point at{
                            .x = (chunkX * 4 + qx) * 4,
                            .y = (stratum::javamath::floorDiv(settings.geometry.minY, 4) + qy) * 4,
                            .z = (chunkZ * 4 + qz) * 4};
                        using stratum::settings::RouterEntry;
                        const auto sample = [&](RouterEntry entry) {
                            return interpreter.evaluate(settings.router.at(entry), at, cache);
                        };
                        const stratum::biome::ClimateSample climate{
                            .temperature = sample(RouterEntry::Temperature),
                            .humidity = sample(RouterEntry::Vegetation),
                            .continentalness = sample(RouterEntry::Continents),
                            .erosion = sample(RouterEntry::Erosion),
                            .depth = sample(RouterEntry::Depth),
                            .weirdness = sample(RouterEntry::Ridges)};
                        biomesDiffering += *biomes[index++] == parameters.find(climate) ? 0U : 1U;
                    }
                }
            }
            CHECK(biomesDiffering == 0U);
        }
    }
}

TEST_CASE("a dimension this build cannot generate is refused by name", "[conformance][world]") {
    if (!haveFixtures()) {
        SKIP("no worldgen or biome_parameters fixtures under " << STRATUM_FIXTURES_DIR);
    }
    const auto overworld = ResourceLocation::parse("minecraft:overworld");
    // The Nether seeds its NAMED noises from the Java LCG, which is not
    // derived yet — and the refusal now says which three, because the
    // refusal is no longer about the flag alone. Vanilla's End declares the
    // same flag and is NOT refused: it names none (SPEC §11, and
    // vanilla_legacy_named_noises_test.cpp).
    CHECK_THROWS_WITH(
        CompiledDimension::compile(thawedVanilla(), ResourceLocation::parse("minecraft:nether"),
                                   ResourceLocation::parse("minecraft:nether"), 1),
        ContainsSubstring("legacy_random_source") && ContainsSubstring("minecraft:temperature") &&
            ContainsSubstring("minecraft:vegetation") && ContainsSubstring("minecraft:offset"));
    // THE END'S BOUNDARY, stated as measured rather than as a universal. A
    // first version of this case said "CompiledDimension::compile still
    // refuses EVERY legacy dimension"; that is false, and a reviewer refuted
    // it by construction. The End is refused on (end, end) for ONE reason
    // only: its `biome_source` is `minecraft:the_end` — neither `multi_noise`
    // nor `fixed` — so vanilla ships no biome parameter list named
    // `minecraft:end`, and the refusal names exactly that. It is not refused
    // for being legacy. Paired with any list that does exist it compiles and
    // generates through this public path; golden_end_test.cpp holds the
    // block-level result (25165824 of 25165824). What is NOT implemented is
    // the End's own biome source, and that is the whole of the boundary.
    CHECK_THROWS_WITH(CompiledDimension::compile(thawedVanilla(),
                                                 ResourceLocation::parse("minecraft:end"),
                                                 ResourceLocation::parse("minecraft:end"), 1),
                      ContainsSubstring("no biome parameter list 'minecraft:end'"));
    CHECK_NOTHROW(CompiledDimension::compile(
        thawedVanilla(), ResourceLocation::parse("minecraft:end"), overworld, 1));

    CHECK_THROWS_WITH(CompiledDimension::compile(thawedVanilla(),
                                                 ResourceLocation::parse("minecraft:not_settings"),
                                                 overworld, 1),
                      ContainsSubstring("no noise settings 'minecraft:not_settings'"));
    CHECK_THROWS_WITH(CompiledDimension::compile(thawedVanilla(), overworld,
                                                 ResourceLocation::parse("minecraft:not_a_list"),
                                                 1),
                      ContainsSubstring("no biome parameter list 'minecraft:not_a_list'"));
}
