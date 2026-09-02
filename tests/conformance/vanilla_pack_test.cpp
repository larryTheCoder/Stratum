// Stratum — loading vanilla's own worldgen data.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The unit suite loads packs this repository wrote. This one loads the 952
// files vanilla ships, which is the first check that the loader copes with
// real identifiers, real nesting and the real spread of registries.
//
// Mojang-derived and never committed (SPEC §12); without the fixtures this
// SKIPs, naming the command that produces them.

#include <stratum/data/pack.hpp>
#include <stratum/data/registry.hpp>
#include <stratum/data/resource_location.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

namespace {

using stratum::data::Pack;
using stratum::data::Registry;
using stratum::data::ResourceLocation;

[[nodiscard]] std::filesystem::path findWorldgenTree() {
    const std::filesystem::path root{STRATUM_FIXTURES_DIR};
    if (!std::filesystem::is_directory(root)) {
        return {};
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_directory() && entry.path().filename() == "worldgen" &&
            std::filesystem::is_directory(entry.path() / "density_function")) {
            return entry.path();
        }
    }
    return {};
}

} // namespace

TEST_CASE("vanilla's worldgen data loads whole", "[conformance][data][pack]") {
    const std::filesystem::path tree = findWorldgenTree();
    if (tree.empty()) {
        SKIP("no extracted vanilla worldgen under "
             << STRATUM_FIXTURES_DIR
             << " — Mojang-derived and never committed (SPEC §12). Generate it with: "
                "tools/fetch-vanilla");
    }

    const Pack pack = Pack::openWorldgenTree(tree);

    // Every file is either executed or named. Nothing is quietly skipped,
    // which is the property SPEC §8 cares about most.
    std::size_t jsonFiles = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(tree)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            ++jsonFiles;
        }
    }
    CHECK(pack.size() + pack.rejected().size() == jsonFiles);
    CHECK(jsonFiles > 900);

    // The registries v1 executes, at the sizes 1.21.11 actually ships.
    CHECK(pack.entriesOf(Registry::DensityFunction).size() == 35U);
    CHECK(pack.entriesOf(Registry::Noise).size() == 60U);
    CHECK(pack.entriesOf(Registry::NoiseSettings).size() == 7U);
    CHECK(pack.entriesOf(Registry::Biome).size() == 65U);
    CHECK(pack.entriesOf(Registry::MultiNoiseBiomeSourceParameterList).size() == 2U);
    CHECK(pack.entriesOf(Registry::WorldPreset).size() == 6U);

    // And the ones it does not, which must have been reported rather than
    // silently ignored.
    std::map<Registry, std::size_t> rejectedByRegistry;
    for (const auto& rejected : pack.rejected()) {
        ++rejectedByRegistry[rejected.registry];
        CHECK_FALSE(stratum::data::isSupported(rejected.registry));
    }
    CHECK(rejectedByRegistry[Registry::PlacedFeature] == 258U);
    CHECK(rejectedByRegistry[Registry::ConfiguredFeature] == 224U);
    CHECK(rejectedByRegistry[Registry::TemplatePool] == 188U);
    CHECK(rejectedByRegistry[Registry::ConfiguredCarver] == 4U);

    for (const auto& entry : pack.entries()) {
        CAPTURE(entry.id.toString());
        CHECK(entry.id.namespaceName() == "minecraft");
        // A density function may be a bare number, the documented shorthand
        // for a constant, so "an entry is an object" is not true of all of
        // them — exactly one file in 1.21.11 takes that form, which only
        // real data would have told us.
        CHECK((entry.json.is_object() || entry.json.is_number()));
        // An identifier must survive being printed and read back, or an
        // error message could name something that does not resolve.
        CHECK(ResourceLocation::parse(entry.id.toString()) == entry.id);
        CHECK(pack.find(entry.registry, entry.id) == &entry);
    }
}

TEST_CASE("the settings the pipeline will need are all present and addressable",
          "[conformance][data][pack]") {
    const std::filesystem::path tree = findWorldgenTree();
    if (tree.empty()) {
        SKIP("no extracted vanilla worldgen under " << STRATUM_FIXTURES_DIR);
    }

    const Pack pack = Pack::openWorldgenTree(tree);

    // One noise_settings per dimension, each with the router M3 will walk.
    for (const std::string_view name : {"overworld", "nether", "end"}) {
        CAPTURE(name);
        const auto* settings =
            pack.find(Registry::NoiseSettings, ResourceLocation::parse(std::string(name)));
        REQUIRE(settings != nullptr);
        CHECK(settings->json.contains("noise_router"));
        CHECK(settings->json.contains("surface_rule"));
        CHECK(settings->json.at("noise").contains("min_y"));
    }

    // The constant shorthand, pinned because the evaluator must handle it:
    // density_function/zero.json is the number 0.0, not an object.
    const auto* zero = pack.find(Registry::DensityFunction, ResourceLocation::parse("zero"));
    REQUIRE(zero != nullptr);
    CHECK(zero->json.is_number());
    // Compared as bits: the project keeps -Wfloat-equal on, and this is an
    // exact-value check, not a tolerance.
    CHECK(std::bit_cast<std::uint64_t>(zero->json.get<double>()) == 0U);

    // The density function every dimension's terrain is built on.
    for (const std::string_view path :
         {"overworld/base_3d_noise", "nether/base_3d_noise", "end/base_3d_noise"}) {
        CAPTURE(path);
        const auto* function =
            pack.find(Registry::DensityFunction, ResourceLocation::parse(std::string(path)));
        REQUIRE(function != nullptr);
        CHECK(function->json.at("type") == "minecraft:old_blended_noise");
    }
}
