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

[[nodiscard]] Pipeline pipelineFrom(const Pack& pack) {
    stratum::settings::LoadedSettings loaded = stratum::settings::loadAll(pack);
    Pipeline pipeline;
    pipeline.settings = std::move(loaded.settings);
    pipeline.graph = std::move(loaded.graph);
    for (const ResourceLocation& id : pipeline.graph.referencedNoises()) {
        pipeline.noises.emplace(id, stratum::density::NoiseParameters::fromJson(
                                        pack.find(stratum::data::Registry::Noise, id)->json, id));
    }
    return pipeline;
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

    const Pack pack = Pack::open(tree);
    const Pipeline before = pipelineFrom(pack);
    REQUIRE(before.graph.nodeCount() == 730U);
    REQUIRE(before.settings.size() == 7U);
    REQUIRE(before.noises.size() == 39U);

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
}

TEST_CASE("vanilla's pipeline freezes to the same bytes every time", "[conformance][freeze]") {
    const std::filesystem::path tree = findWorldgenTree();
    if (tree.empty()) {
        SKIP("no extracted vanilla worldgen under " << STRATUM_FIXTURES_DIR);
    }

    const Pack pack = Pack::open(tree);

    // Resolved twice from the same pack rather than written twice from one
    // in-memory pipeline: SPEC §5.6 is about a *stored* pipeline being
    // reproducible, and that only means something if loading is
    // deterministic too.
    const std::vector<std::byte> first = stratum::freeze::write(pipelineFrom(pack));
    const std::vector<std::byte> second = stratum::freeze::write(pipelineFrom(pack));
    CHECK(first == second);

    // And reading then rewriting is a fixed point, which is what lets two
    // engine builds compare their stored blobs at all.
    CHECK(stratum::freeze::write(stratum::freeze::read(first)) == first);
}
