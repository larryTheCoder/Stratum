// Stratum — freezing vanilla's whole pipeline and reading it back.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The unit suite freezes a pipeline this repository invented, with one of
// everything in it. This freezes the real one: 730 nodes, 354 splines, 39
// noises and seven dimensions, including the surface rules and spawn targets
// this build does not interpret and has to round-trip anyway.
//
// Mojang-derived fixtures are never committed (SPEC §12); without them this
// SKIPs, naming the command that produces them.

#include <stratum/biome/parameter_list.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/registry.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/graph.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/freeze/pipeline.hpp>
#include <stratum/settings/noise_settings.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using stratum::data::Pack;
using stratum::data::ResourceLocation;
using stratum::freeze::Pipeline;

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

[[nodiscard]] Pipeline pipelineFrom(const Pack& pack, const std::filesystem::path& tree) {
    // biome_parameters/ sits beside worldgen/ in what tools/fetch-vanilla
    // leaves, dumped by the server's data generator.
    return stratum::freeze::resolve(pack, tree.parent_path() / "biome_parameters");
}

} // namespace

TEST_CASE("vanilla's whole pipeline freezes and thaws unchanged", "[conformance][freeze]") {
    const std::filesystem::path tree = findWorldgenTree();
    if (tree.empty()) {
        SKIP("no extracted vanilla worldgen under "
             << STRATUM_FIXTURES_DIR
             << " — Mojang-derived and never committed (SPEC §12). Generate it with: "
                "tools/fetch-vanilla");
    }

    if (!std::filesystem::is_directory(tree.parent_path() / "biome_parameters")) {
        SKIP("no biome_parameters dump beside " << tree.string()
                                                << "; tools/fetch-vanilla produces it");
    }

    const Pack pack = Pack::open(tree);
    const Pipeline before = pipelineFrom(pack, tree);
    REQUIRE(before.graph.nodeCount() == 730U);
    REQUIRE(before.settings.size() == 7U);
    // Every noise vanilla defines, which is more than the 39 the graph names:
    // surface rules and the surface system sample the rest.
    REQUIRE(before.noises.size() == pack.entriesOf(stratum::data::Registry::Noise).size());
    CHECK(before.graph.referencedNoises().size() == 39U);
    for (const char* surfaceNoise :
         {"minecraft:surface", "minecraft:surface_secondary", "minecraft:clay_bands_offset"}) {
        CHECK(before.noises.contains(ResourceLocation::parse(surfaceNoise)));
    }
    REQUIRE(before.biomeParameters.size() == 2U); // overworld and nether
    REQUIRE(before.biomeTemperatures.size() == 65U);

    const std::vector<std::byte> blob = stratum::freeze::write(before);
    const Pipeline after = stratum::freeze::read(blob);

    CHECK(after.graph.nodeCount() == before.graph.nodeCount());
    CHECK(after.graph.splineCount() == before.graph.splineCount());
    CHECK(after.graph.roots() == before.graph.roots());
    CHECK(after.noises.size() == before.noises.size());
    CHECK(after.settings.size() == before.settings.size());

    // Every node, bit for bit. A freeze that rounded one spline knot would
    // still load and would generate a different world.
    for (std::size_t i = 0; i < before.graph.nodeCount(); ++i) {
        const auto index = static_cast<stratum::density::NodeIndex>(i);
        const auto& a = before.graph.node(index);
        const auto& b = after.graph.node(index);
        CAPTURE(i);
        CHECK(a.type == b.type);
        CHECK(a.arguments == b.arguments);
        CHECK(a.noise == b.noise);
        // Always empty on vanilla's own data — nothing it ships inlines a
        // noise — and checked anyway: the field has to be *compared*, or a
        // freeze that dropped it would round-trip vanilla's pack clean and
        // lose a third-party pack's noise in silence.
        CHECK(a.inlineNoise == b.inlineNoise);
        CHECK(a.spline == b.spline);
        CHECK(a.selector == b.selector);
        REQUIRE(a.parameters.size() == b.parameters.size());
        for (std::size_t k = 0; k < a.parameters.size(); ++k) {
            CHECK(std::bit_cast<std::uint64_t>(a.parameters[k]) ==
                  std::bit_cast<std::uint64_t>(b.parameters[k]));
        }
    }
    for (std::size_t i = 0; i < before.graph.splineCount(); ++i) {
        const auto index = static_cast<stratum::density::SplineIndex>(i);
        const auto& a = before.graph.spline(index);
        const auto& b = after.graph.spline(index);
        CAPTURE(i);
        CHECK(a.coordinate == b.coordinate);
        REQUIRE(a.points.size() == b.points.size());
        for (std::size_t k = 0; k < a.points.size(); ++k) {
            CHECK(std::bit_cast<std::uint64_t>(a.points[k].location) ==
                  std::bit_cast<std::uint64_t>(b.points[k].location));
            CHECK(std::bit_cast<std::uint64_t>(a.points[k].derivative) ==
                  std::bit_cast<std::uint64_t>(b.points[k].derivative));
            CHECK(a.points[k].nested == b.points[k].nested);
            REQUIRE(a.points[k].value.has_value() == b.points[k].value.has_value());
            if (a.points[k].value.has_value()) {
                CHECK(std::bit_cast<std::uint64_t>(*a.points[k].value) ==
                      std::bit_cast<std::uint64_t>(*b.points[k].value));
            }
        }
    }

    // The parts this build does not interpret have to survive too, or a
    // world frozen today could not be generated by the build that finally
    // understands surface rules (SPEC §6).
    for (const auto& [id, one] : before.settings) {
        CAPTURE(id.toString());
        const auto& other = after.settings.at(id);
        CHECK(other.surfaceRule == one.surfaceRule);
        CHECK(other.spawnTarget == one.spawnTarget);
        CHECK(other.router.entries == one.router.entries);
        CHECK(other.geometry == one.geometry);
        CHECK(other.defaultBlock == one.defaultBlock);
        CHECK(other.defaultFluid == one.defaultFluid);
        CHECK(other.legacyRandomSource == one.legacyRandomSource);
    }

    // The biome tables format 3 added, bit for bit and in order: ties in the
    // biome search go to the later entry, so order is meaning here.
    REQUIRE(after.biomeParameters.size() == before.biomeParameters.size());
    for (const auto& [id, list] : before.biomeParameters) {
        CAPTURE(id.toString());
        const auto& other = after.biomeParameters.at(id);
        REQUIRE(other.size() == list.size());
        for (std::size_t k = 0; k < list.size(); ++k) {
            const auto& a = list.entries()[k];
            const auto& b = other.entries()[k];
            CAPTURE(k);
            CHECK(a.biome == b.biome);
            const auto same = [](const stratum::biome::Parameter& x,
                                 const stratum::biome::Parameter& y) {
                return std::bit_cast<std::uint64_t>(x.min) == std::bit_cast<std::uint64_t>(y.min) &&
                       std::bit_cast<std::uint64_t>(x.max) == std::bit_cast<std::uint64_t>(y.max);
            };
            CHECK(same(a.parameters.temperature, b.parameters.temperature));
            CHECK(same(a.parameters.humidity, b.parameters.humidity));
            CHECK(same(a.parameters.continentalness, b.parameters.continentalness));
            CHECK(same(a.parameters.erosion, b.parameters.erosion));
            CHECK(same(a.parameters.depth, b.parameters.depth));
            CHECK(same(a.parameters.weirdness, b.parameters.weirdness));
            CHECK(std::bit_cast<std::uint64_t>(a.parameters.offset) ==
                  std::bit_cast<std::uint64_t>(b.parameters.offset));
        }
    }
    for (const auto& [id, temperature] : before.biomeTemperatures.entries()) {
        CAPTURE(id.toString());
        CHECK(std::bit_cast<std::uint32_t>(after.biomeTemperatures.at(id)) ==
              std::bit_cast<std::uint32_t>(temperature));
    }
}

TEST_CASE("vanilla's pipeline freezes to the same bytes every time", "[conformance][freeze]") {
    const std::filesystem::path tree = findWorldgenTree();
    if (tree.empty()) {
        SKIP("no extracted vanilla worldgen under " << STRATUM_FIXTURES_DIR);
    }

    if (!std::filesystem::is_directory(tree.parent_path() / "biome_parameters")) {
        SKIP("no biome_parameters dump beside " << tree.string());
    }
    const Pack pack = Pack::open(tree);

    // Resolved twice from the same pack rather than written twice from one
    // in-memory pipeline: SPEC §5.6 is about a *stored* pipeline being
    // reproducible, and that only means something if loading is
    // deterministic too.
    const std::vector<std::byte> first = stratum::freeze::write(pipelineFrom(pack, tree));
    const std::vector<std::byte> second = stratum::freeze::write(pipelineFrom(pack, tree));
    CHECK(first == second);

    // And reading then rewriting is a fixed point, which is what lets two
    // engine builds compare their stored blobs at all.
    CHECK(stratum::freeze::write(stratum::freeze::read(first)) == first);
}
