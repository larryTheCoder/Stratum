// Stratum — resolving vanilla's density function graph.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The unit suite resolves graphs this repository wrote. This one resolves
// the 35 vanilla ships, which is the first check that the node set, the
// field names and the reference forms match what Mojang actually publishes
// rather than what the documentation suggests.
//
// Mojang-derived and never committed (SPEC §12); without the fixtures this
// SKIPs, naming the command that produces them.

#include <stratum/data/pack.hpp>
#include <stratum/data/registry.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <map>
#include <string>

namespace {

using stratum::data::Pack;
using stratum::data::Registry;
using stratum::data::ResourceLocation;
using stratum::density::Graph;
using stratum::density::NodeType;

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

TEST_CASE("vanilla's density functions resolve into one graph", "[conformance][density][graph]") {
    const std::filesystem::path tree = findWorldgenTree();
    if (tree.empty()) {
        SKIP("no extracted vanilla worldgen under "
             << STRATUM_FIXTURES_DIR
             << " — Mojang-derived and never committed (SPEC §12). Generate it with: "
                "tools/fetch-vanilla");
    }

    const Pack pack = Pack::openWorldgenTree(tree);
    const Graph graph = Graph::resolveAll(pack);

    // Every file becomes a root: nothing is skipped for being unreadable.
    CHECK(graph.roots().size() == pack.entriesOf(Registry::DensityFunction).size());
    CHECK(graph.roots().size() == 35U);

    // Node counts, checked against what the JSON literally contains. These
    // are not round numbers; they are what 1.21.11 ships, and a change in
    // either the data or the resolver moves them.
    std::map<NodeType, std::size_t> byType;
    for (std::size_t i = 0; i < graph.nodeCount(); ++i) {
        ++byType[graph.node(static_cast<stratum::density::NodeIndex>(i)).type];
    }
    CHECK(byType[NodeType::Add] == 50U);
    CHECK(byType[NodeType::Mul] == 37U);
    CHECK(byType[NodeType::Noise] == 19U);
    CHECK(byType[NodeType::FlatCache] == 16U);
    CHECK(byType[NodeType::CacheOnce] == 12U);
    CHECK(byType[NodeType::BlendAlpha] == 12U);
    CHECK(byType[NodeType::Cache2d] == 11U);
    CHECK(byType[NodeType::Spline] == 9U);
    CHECK(byType[NodeType::OldBlendedNoise] == 3U);
    CHECK(byType[NodeType::EndIslands] == 1U);
    // Constants have no "type" in the JSON at all — they are the bare-number
    // shorthand, so this count exists only because resolution created them.
    CHECK(byType[NodeType::Constant] == 57U);

    CHECK(graph.nodeCount() == 275U);
    CHECK(graph.splineCount() == 354U);

    // Every noise the graph names must exist in the same pack, or generation
    // would fail on whichever chunk first reached that node.
    const auto noises = graph.referencedNoises();
    CHECK(noises.size() == 25U);
    for (const ResourceLocation& noise : noises) {
        CAPTURE(noise.toString());
        CHECK(pack.find(Registry::Noise, noise) != nullptr);
    }
}

TEST_CASE("the terrain roots the pipeline will start from are shaped as expected",
          "[conformance][density][graph]") {
    const std::filesystem::path tree = findWorldgenTree();
    if (tree.empty()) {
        SKIP("no extracted vanilla worldgen under " << STRATUM_FIXTURES_DIR);
    }

    const Graph graph = Graph::resolveAll(Pack::openWorldgenTree(tree));

    for (const std::string_view path :
         {"overworld/base_3d_noise", "nether/base_3d_noise", "end/base_3d_noise"}) {
        CAPTURE(path);
        const auto& node = graph.node(graph.rootOf(ResourceLocation::parse(std::string(path))));
        CHECK(node.type == NodeType::OldBlendedNoise);
        // xz_scale, y_scale, xz_factor, y_factor, smear_scale_multiplier.
        CHECK(node.parameters.size() == 5U);
    }

    // Every spline's coordinate resolves to a real node, and every point is
    // either a value or a nested spline — never both, never neither.
    for (std::size_t i = 0; i < graph.splineCount(); ++i) {
        CAPTURE(i);
        const auto& spline = graph.spline(static_cast<stratum::density::SplineIndex>(i));
        CHECK(spline.coordinate < graph.nodeCount());
        CHECK_FALSE(spline.points.empty());
        for (const auto& point : spline.points) {
            CHECK(point.value.has_value() != point.nested.has_value());
        }
    }
}

TEST_CASE("resolution is deterministic", "[conformance][density][graph]") {
    const std::filesystem::path tree = findWorldgenTree();
    if (tree.empty()) {
        SKIP("no extracted vanilla worldgen under " << STRATUM_FIXTURES_DIR);
    }

    const Pack pack = Pack::openWorldgenTree(tree);
    const Graph first = Graph::resolveAll(pack);
    const Graph second = Graph::resolveAll(pack);

    // Node indices are positions in a vector; if resolution order drifted,
    // two loads of one pack would disagree about what index 100 means.
    REQUIRE(first.nodeCount() == second.nodeCount());
    for (std::size_t i = 0; i < first.nodeCount(); ++i) {
        const auto index = static_cast<stratum::density::NodeIndex>(i);
        CAPTURE(i);
        CHECK(first.node(index).type == second.node(index).type);
        CHECK(first.node(index).arguments == second.node(index).arguments);
    }
}
