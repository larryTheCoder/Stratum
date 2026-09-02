// Stratum — density function resolution tests.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Resolution is where a reference becomes a node. The failure that matters
// is not a crash but a graph that quietly means something else — a cycle
// walked forever, a dangling reference read as zero, a node type skipped —
// so most of what is checked here is that those are refused, by name.

#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/graph.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace {

using stratum::data::Pack;
using stratum::data::ResourceLocation;
using stratum::density::Graph;
using stratum::density::NodeType;
using stratum::density::ResolveError;

/// Exact comparison by bits: the project keeps -Wfloat-equal on, and these
/// are parsed constants that must arrive unchanged, not approximations.
[[nodiscard]] std::uint64_t bits(double value) noexcept {
    return std::bit_cast<std::uint64_t>(value);
}

/// A worldgen tree written on the fly, removed with the test.
class TempTree {
public:
    TempTree() : path_(std::filesystem::temp_directory_path() / uniqueName()) {
        std::filesystem::create_directories(path_ / "density_function");
    }

    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;

    ~TempTree() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    /// Writes `density_function/<name>.json`.
    const TempTree& define(std::string_view name, std::string_view json) const {
        const std::filesystem::path file =
            path_ / "density_function" / (std::string(name) + ".json");
        std::filesystem::create_directories(file.parent_path());
        std::ofstream out(file);
        out << json;
        return *this;
    }

    [[nodiscard]] Graph resolve() const { return Graph::resolveAll(Pack::openWorldgenTree(path_)); }

private:
    [[nodiscard]] static std::string uniqueName() {
        static int counter = 0;
        return "stratum-density-test-" + std::to_string(++counter);
    }

    std::filesystem::path path_;
};

} // namespace

TEST_CASE("the three spellings of a density function all resolve", "[density][graph]") {
    const TempTree tree;
    tree.define("leaf", "3.5");                                            // bare number
    tree.define("byref", R"({"type":"minecraft:abs","argument":"leaf"})"); // reference
    tree.define(
        "inline",
        R"({"type":"minecraft:abs","argument":{"type":"minecraft:constant","argument":-2}})");

    const Graph graph = tree.resolve();
    CHECK(graph.roots().size() == 3U);

    const auto& leaf = graph.node(graph.rootOf(ResourceLocation::parse("leaf")));
    CHECK(leaf.type == NodeType::Constant);
    REQUIRE(leaf.parameters.size() == 1U);
    CHECK(bits(leaf.parameters[0]) == bits(3.5));

    const auto& byRef = graph.node(graph.rootOf(ResourceLocation::parse("byref")));
    CHECK(byRef.type == NodeType::Abs);
    REQUIRE(byRef.arguments.size() == 1U);
    CHECK(graph.node(byRef.arguments[0]).type == NodeType::Constant);

    const auto& inlined = graph.node(graph.rootOf(ResourceLocation::parse("inline")));
    CHECK(bits(graph.node(inlined.arguments[0]).parameters[0]) == bits(-2.0));
}

TEST_CASE("a function referenced twice is resolved once", "[density][graph]") {
    // Shared subgraphs are the norm — vanilla's continents feed a dozen
    // functions — and re-resolving them would multiply the graph.
    const TempTree tree;
    tree.define("shared", R"({"type":"minecraft:constant","argument":1})");
    tree.define("a", R"({"type":"minecraft:abs","argument":"shared"})");
    tree.define("b", R"({"type":"minecraft:cube","argument":"shared"})");

    const Graph graph = tree.resolve();
    const auto& a = graph.node(graph.rootOf(ResourceLocation::parse("a")));
    const auto& b = graph.node(graph.rootOf(ResourceLocation::parse("b")));
    CHECK(a.arguments[0] == b.arguments[0]);
    CHECK(graph.nodeCount() == 3U);
}

TEST_CASE("cycles are refused and the path is named", "[density][graph][malformed]") {
    SECTION("a function referring to itself") {
        const TempTree tree;
        tree.define("loop", R"({"type":"minecraft:abs","argument":"loop"})");
        REQUIRE_THROWS_WITH(tree.resolve(),
                            Catch::Matchers::ContainsSubstring("cycle") &&
                                Catch::Matchers::ContainsSubstring("minecraft:loop"));
    }

    SECTION("a longer ring") {
        const TempTree tree;
        tree.define("a", R"({"type":"minecraft:abs","argument":"b"})");
        tree.define("b", R"({"type":"minecraft:abs","argument":"c"})");
        tree.define("c", R"({"type":"minecraft:abs","argument":"a"})");
        // Every link is named, so the ring can actually be found and broken.
        REQUIRE_THROWS_WITH(tree.resolve(), Catch::Matchers::ContainsSubstring("minecraft:a") &&
                                                Catch::Matchers::ContainsSubstring("minecraft:b") &&
                                                Catch::Matchers::ContainsSubstring("minecraft:c"));
    }
}

TEST_CASE("a dangling reference names what is missing", "[density][graph][malformed]") {
    const TempTree tree;
    tree.define("a", R"({"type":"minecraft:abs","argument":"minecraft:absent"})");
    REQUIRE_THROWS_WITH(tree.resolve(), Catch::Matchers::ContainsSubstring("minecraft:absent") &&
                                            Catch::Matchers::ContainsSubstring("does not define"));
}

TEST_CASE("a node type this engine does not implement is named, not skipped",
          "[density][graph][malformed]") {
    const TempTree tree;
    tree.define("a", R"({"type":"minecraft:teleporting_noise","argument":1})");
    REQUIRE_THROWS_WITH(tree.resolve(),
                        Catch::Matchers::ContainsSubstring("minecraft:teleporting_noise") &&
                            Catch::Matchers::ContainsSubstring("not implemented"));
}

TEST_CASE("a malformed node names the field that is wrong", "[density][graph][malformed]") {
    SECTION("a missing field") {
        const TempTree tree;
        tree.define("a", R"({"type":"minecraft:add","argument1":1})");
        REQUIRE_THROWS_WITH(tree.resolve(), Catch::Matchers::ContainsSubstring("argument2"));
    }

    SECTION("a number where a noise identifier belongs") {
        const TempTree tree;
        tree.define("a", R"({"type":"minecraft:noise","noise":3,"xz_scale":1,"y_scale":1})");
        REQUIRE_THROWS_WITH(tree.resolve(), Catch::Matchers::ContainsSubstring("name a noise"));
    }

    SECTION("a function where a number belongs") {
        const TempTree tree;
        tree.define(
            "a",
            R"({"type":"minecraft:clamp","input":1,"min":{"type":"minecraft:constant","argument":0},"max":1})");
        REQUIRE_THROWS_WITH(tree.resolve(), Catch::Matchers::ContainsSubstring("to be a number"));
    }

    SECTION("no type at all") {
        const TempTree tree;
        tree.define("a", R"({"argument":1})");
        REQUIRE_THROWS_WITH(tree.resolve(), Catch::Matchers::ContainsSubstring("no \"type\""));
    }

    SECTION("an unknown rarity mapper") {
        const TempTree tree;
        tree.define("a", R"({"type":"minecraft:weird_scaled_sampler","input":1,
                             "noise":"minecraft:x","rarity_value_mapper":"type_9"})");
        REQUIRE_THROWS_WITH(tree.resolve(), Catch::Matchers::ContainsSubstring("type_9"));
    }
}

TEST_CASE("shift_a takes a noise, not a nested function", "[density][graph]") {
    // The one place `argument` does not mean what it means everywhere else;
    // reading it as a function would look for a density function named
    // minecraft:offset and fail confusingly, or worse, find one.
    const TempTree tree;
    tree.define("a", R"({"type":"minecraft:shift_a","argument":"minecraft:offset"})");

    const Graph graph = tree.resolve();
    const auto& node = graph.node(graph.rootOf(ResourceLocation::parse("a")));
    CHECK(node.type == NodeType::ShiftA);
    CHECK(node.arguments.empty());
    REQUIRE(node.noise.has_value());
    CHECK(node.noise->toString() == "minecraft:offset");
    CHECK(graph.referencedNoises().size() == 1U);
}

TEST_CASE("splines resolve, nest, and carry their coordinate", "[density][graph][spline]") {
    const TempTree tree;
    tree.define("coord", "0.5");
    tree.define("a", R"({"type":"minecraft:spline","spline":{
        "coordinate":"coord",
        "points":[
            {"location":-1.0,"derivative":0.0,"value":0.0},
            {"location":0.0,"derivative":1.0,"value":{
                "coordinate":"coord",
                "points":[{"location":0.0,"derivative":0.0,"value":2.0}]}},
            {"location":1.0,"derivative":0.0,"value":3.0}]}})");

    const Graph graph = tree.resolve();
    const auto& node = graph.node(graph.rootOf(ResourceLocation::parse("a")));
    REQUIRE(node.spline.has_value());

    const auto& spline = graph.spline(*node.spline);
    REQUIRE(spline.points.size() == 3U);
    CHECK(graph.node(spline.coordinate).type == NodeType::Constant);
    CHECK(spline.points[0].value.has_value());
    REQUIRE(spline.points[1].nested.has_value());
    CHECK_FALSE(spline.points[1].value.has_value());
    CHECK(graph.spline(*spline.points[1].nested).points.size() == 1U);
}

TEST_CASE("a spline that could not be interpolated is refused", "[density][graph][spline]") {
    SECTION("points out of order") {
        // Interpolation walks the points in order; unsorted points would
        // silently sample the wrong segment rather than fail.
        const TempTree tree;
        tree.define("coord", "0.0");
        tree.define("a", R"({"type":"minecraft:spline","spline":{"coordinate":"coord","points":[
            {"location":1.0,"derivative":0.0,"value":0.0},
            {"location":-1.0,"derivative":0.0,"value":0.0}]}})");
        REQUIRE_THROWS_WITH(tree.resolve(), Catch::Matchers::ContainsSubstring("ascending"));
    }

    SECTION("no points") {
        const TempTree tree;
        tree.define("coord", "0.0");
        tree.define("a",
                    R"({"type":"minecraft:spline","spline":{"coordinate":"coord","points":[]}})");
        REQUIRE_THROWS_WITH(tree.resolve(), Catch::Matchers::ContainsSubstring("no points"));
    }
}

TEST_CASE("every node type has a name that round-trips", "[density][graph]") {
    for (const std::string_view name :
         {"minecraft:add", "minecraft:shifted_noise", "minecraft:old_blended_noise",
          "minecraft:weird_scaled_sampler", "minecraft:end_islands", "minecraft:constant"}) {
        CAPTURE(name);
        const auto type = stratum::density::nodeTypeFromName(name);
        REQUIRE(type.has_value());
        CHECK(stratum::density::nodeTypeName(*type) == name);
    }
    CHECK_FALSE(stratum::density::nodeTypeFromName("minecraft:nonsense").has_value());
}
