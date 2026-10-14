// Stratum — density function resolution tests.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Resolution is where a reference becomes a node. The failure that matters
// is not a crash but a graph that quietly means something else — a cycle
// walked forever, a dangling reference read as zero, a node type skipped —
// so most of what is checked here is that those are refused, by name.

#include "support/temp_path.hpp"

#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/graph.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
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
    void define(std::string_view name, std::string_view json) const {
        const std::filesystem::path file =
            path_ / "density_function" / (std::string(name) + ".json");
        std::filesystem::create_directories(file.parent_path());
        std::ofstream out(file);
        out << json;
    }

    [[nodiscard]] Graph resolve() const { return Graph::resolveAll(Pack::openWorldgenTree(path_)); }

private:
    [[nodiscard]] static std::string uniqueName() {
        return stratum::test::tempName("stratum-density-test");
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

TEST_CASE("a noise field takes either an identifier or the parameters inline",
          "[density][graph][noise]") {
    // mcdoc declares every `noise` field as
    // `#[id="worldgen/noise"] string | NoiseParameters`, so both spellings
    // are legal input. The generated schema used to record only the first,
    // because the alias was matched by name before the union was expanded —
    // which made a datapack that inlines its noise parameters something this
    // engine refused for no reason at all. Both are checked here, and on two
    // node types, because that narrowing affected every field of this kind
    // rather than one.
    SECTION("an identifier") {
        const TempTree tree;
        tree.define("a", R"({"type":"minecraft:shift_a","argument":"minecraft:offset"})");

        const Graph graph = tree.resolve();
        const auto& node = graph.node(graph.rootOf(ResourceLocation::parse("a")));
        REQUIRE(node.noise.has_value());
        CHECK(node.noise->toString() == "minecraft:offset");
        CHECK_FALSE(node.inlineNoise.has_value());
        CHECK(graph.referencedNoises().size() == 1U);
    }

    SECTION("the parameters inline") {
        const TempTree tree;
        tree.define("a", R"({"type":"minecraft:shift_a",
                             "argument":{"firstOctave":-3,"amplitudes":[1.0,0.0,2.5]}})");

        const Graph graph = tree.resolve();
        const auto& node = graph.node(graph.rootOf(ResourceLocation::parse("a")));
        CHECK(node.type == NodeType::ShiftA);
        CHECK(node.arguments.empty());
        CHECK_FALSE(node.noise.has_value());
        REQUIRE(node.inlineNoise.has_value());
        CHECK(node.inlineNoise->firstOctave == -3);
        REQUIRE(node.inlineNoise->amplitudes.size() == 3U);
        CHECK(bits(node.inlineNoise->amplitudes[0]) == bits(1.0));
        // A zero amplitude is not padding: it positions the octaves after it
        // without contributing itself, so it has to survive the trip.
        CHECK(bits(node.inlineNoise->amplitudes[1]) == bits(0.0));
        CHECK(bits(node.inlineNoise->amplitudes[2]) == bits(2.5));

        // It names nothing, so it asks nothing of the noise registry.
        CHECK(graph.referencedNoises().empty());
    }

    SECTION("inline on minecraft:noise, whose field is spelled differently") {
        const TempTree tree;
        tree.define("a", R"({"type":"minecraft:noise","xz_scale":1.0,"y_scale":2.0,
                             "noise":{"firstOctave":-7,"amplitudes":[1.0]}})");

        const Graph graph = tree.resolve();
        const auto& node = graph.node(graph.rootOf(ResourceLocation::parse("a")));
        CHECK(node.type == NodeType::Noise);
        REQUIRE(node.inlineNoise.has_value());
        CHECK(node.inlineNoise->firstOctave == -7);
        // The two numbers still land in `parameters`, in schema order.
        REQUIRE(node.parameters.size() == 2U);
        CHECK(bits(node.parameters[0]) == bits(1.0));
        CHECK(bits(node.parameters[1]) == bits(2.0));
    }

    SECTION("an inline noise that is malformed is refused, and says where it was") {
        const TempTree tree;
        tree.define("a", R"({"type":"minecraft:shift_b","argument":{"firstOctave":-3}})");
        REQUIRE_THROWS_WITH(tree.resolve(),
                            Catch::Matchers::ContainsSubstring("amplitudes") &&
                                Catch::Matchers::ContainsSubstring("minecraft:shift_b") &&
                                Catch::Matchers::ContainsSubstring("argument"));
    }

    SECTION("a noise field that is neither is refused as neither") {
        const TempTree tree;
        tree.define("a", R"({"type":"minecraft:shift","argument":7})");
        REQUIRE_THROWS_WITH(tree.resolve(),
                            Catch::Matchers::ContainsSubstring("name a noise or to be one"));
    }
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

TEST_CASE("types the schema allows but vanilla never uses still resolve",
          "[density][graph][schema]") {
    // The reason the schema is derived from mcdoc rather than from vanilla's
    // own files: a datapack may use any type the version defines, and eight
    // of them appear nowhere in vanilla's data. Before mcdoc-sync these were
    // rejected as unknown.
    for (const std::string_view type :
         {"minecraft:square", "minecraft:squeeze", "minecraft:blend_density", "minecraft:invert",
          "minecraft:slide", "minecraft:cache_all_in_cell"}) {
        CAPTURE(type);
        const TempTree tree;
        tree.define("a", R"({"type":")" + std::string(type) + R"(","argument":1})");
        CHECK_NOTHROW(tree.resolve());
    }

    // shift takes a noise, like its shift_a and shift_b siblings.
    const TempTree shift;
    shift.define("a", R"({"type":"minecraft:shift","argument":"minecraft:offset"})");
    const Graph graph = shift.resolve();
    CHECK(graph.node(graph.rootOf(ResourceLocation::parse("a"))).noise.has_value());
}

TEST_CASE("the schema's per-version restrictions are enforced", "[density][graph][schema]") {
    // At this version clamp's input must be given inline; only the schema
    // records that, and vanilla's own two clamps both obey it.
    const TempTree tree;
    tree.define("target", "1.0");
    tree.define("a", R"({"type":"minecraft:clamp","input":"target","min":0,"max":1})");
    REQUIRE_THROWS_WITH(tree.resolve(),
                        Catch::Matchers::ContainsSubstring("does not accept an identifier"));

    // Given inline, the same node is fine.
    const TempTree inlined;
    inlined.define("a",
                   R"({"type":"minecraft:clamp","input":{"type":"minecraft:constant",
                       "argument":1},"min":0,"max":1})");
    CHECK_NOTHROW(inlined.resolve());
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

TEST_CASE("reachability from one root is not the whole graph's reach", "[density][graph][reach]") {
    const TempTree tree;
    tree.define("mine", R"({"type":"minecraft:noise","noise":"wanted",
        "xz_scale":1.0,"y_scale":1.0})");
    tree.define("theirs", R"({"type":"minecraft:noise","noise":"unwanted",
        "xz_scale":1.0,"y_scale":1.0})");

    // A spline's coordinate is reached through NO argument list, and
    // `shift_a`'s argument is a NOISE rather than a nested function. Both are
    // how a naive walk undercounts — vanilla's overworld reaches
    // continentalness only through the first, and every dimension that has
    // climate at all reaches `minecraft:offset` only through the second.
    tree.define("coord", R"({"type":"minecraft:noise","noise":"through_spline",
        "xz_scale":1.0,"y_scale":1.0})");
    tree.define("shifted", R"({"type":"minecraft:shift_a","argument":"through_shift"})");
    tree.define("splined", R"({"type":"minecraft:spline","spline":{
        "coordinate":"coord",
        "points":[{"location":0.0,"derivative":0.0,"value":1.0}]}})");
    tree.define("root", R"({"type":"minecraft:add","argument1":"mine","argument2":{
        "type":"minecraft:add","argument1":"splined","argument2":"shifted"}})");
    tree.define("plain", R"({"type":"minecraft:abs","argument":-3.5})");

    const Graph graph = tree.resolve();

    const auto reach = graph.noisesReachableFrom(graph.rootOf(ResourceLocation::parse("root")));
    CHECK(reach == std::vector<ResourceLocation>{ResourceLocation::parse("through_shift"),
                                                 ResourceLocation::parse("through_spline"),
                                                 ResourceLocation::parse("wanted")});

    // The whole graph reaches one more — `unwanted`, which no root above
    // touches. Asking per root rather than per graph is the entire point.
    CHECK(graph.referencedNoises().size() == reach.size() + 1U);

    // A root that reaches nothing named reaches nothing named. This is the
    // case the legacy dimensions turn on: thirteen of their fifteen router
    // entries are exactly this.
    CHECK(graph.noisesReachableFrom(graph.rootOf(ResourceLocation::parse("plain"))).empty());

    // reachableFrom includes the root itself, and refuses an index naming no
    // node rather than reading past the end of the node table.
    const auto nodes = graph.reachableFrom(graph.rootOf(ResourceLocation::parse("plain")));
    CHECK(nodes.size() == 2U); // the abs and the constant under it
    CHECK_THROWS_AS(
        graph.reachableFrom(static_cast<stratum::density::NodeIndex>(graph.nodeCount())),
        ResolveError);

    // THE MULTI-ROOT FORM IS THE UNION, which is the question a whole
    // dimension asks: its fifteen router entries at once. It is a thin
    // fan-out over the single-root walk rather than a second walk with its
    // own visited set, so that "reaches" has exactly one definition in the
    // library — two walks that could drift apart is the bug this whole area
    // is about.
    const std::vector<stratum::density::NodeIndex> bothRoots{
        graph.rootOf(ResourceLocation::parse("root")),
        graph.rootOf(ResourceLocation::parse("theirs"))};
    const auto unioned = graph.noisesReachableFrom(bothRoots);
    CHECK(unioned == std::vector<ResourceLocation>{ResourceLocation::parse("through_shift"),
                                                   ResourceLocation::parse("through_spline"),
                                                   ResourceLocation::parse("unwanted"),
                                                   ResourceLocation::parse("wanted")});
    // Deduplicated across roots, not concatenated: `root` and itself is
    // still three names, not six.
    const std::vector<stratum::density::NodeIndex> twice{
        graph.rootOf(ResourceLocation::parse("root")),
        graph.rootOf(ResourceLocation::parse("root"))};
    CHECK(graph.noisesReachableFrom(twice) == reach);
    // And no roots reaches nothing, rather than reaching everything.
    CHECK(graph.noisesReachableFrom(std::vector<stratum::density::NodeIndex>{}).empty());
}

TEST_CASE("a frozen node is held to what a pack's is", "[density][graph][ub]") {
    // Graph::Assembler is how freeze::read rebuilds a graph, so what the
    // resolver refuses of a pack it refuses of a blob: a resealed blob must
    // not carry a node the interpreter cannot evaluate defined (SPEC §8).
    const auto constant = [](double value) {
        stratum::density::Node node;
        node.type = NodeType::Constant;
        node.parameters = {value};
        return node;
    };
    const auto clamp = [](double min, double max) {
        stratum::density::Node node;
        node.type = NodeType::Clamp;
        node.arguments = {0};
        node.parameters = {min, max};
        return node;
    };

    Graph::Assembler reversed;
    static_cast<void>(reversed.addNode(constant(0.5)));
    CHECK_THROWS_MATCHES(reversed.addNode(clamp(1.0, 0.0)), ResolveError,
                         Catch::Matchers::MessageMatches(
                             Catch::Matchers::ContainsSubstring("'minecraft:clamp' has min 1")));

    Graph::Assembler ordered;
    static_cast<void>(ordered.addNode(constant(0.5)));
    CHECK_NOTHROW(ordered.addNode(clamp(0.0, 1.0)));

    // A value refusal is an OutOfRangeError, which freeze::read tells apart
    // from a malformed node: a build before SPEC §8's refusals froze such
    // values (pipeline_freeze_test.cpp).
    Graph::Assembler typed;
    static_cast<void>(typed.addNode(constant(0.5)));
    CHECK_THROWS_AS(typed.addNode(clamp(1.0, 0.0)), stratum::density::OutOfRangeError);

    // A node carrying fewer parameters than its type reads would have had
    // the interpreter index past the end.
    stratum::density::Node bare;
    bare.type = NodeType::FindTopSurface;
    Graph::Assembler unfinished;
    CHECK_THROWS_MATCHES(unfinished.addNode(bare), ResolveError,
                         Catch::Matchers::MessageMatches(
                             Catch::Matchers::ContainsSubstring("carries 0 numeric parameter")));
}

namespace {

/// A node of @p type with exactly the shape its schema row declares: one
/// argument per Function field (each naming node 0), one parameter of 1.0
/// per Number field — inside every range the value checks hold a parameter
/// to — and a noise, a spline and a selector where the type has one.
[[nodiscard]] stratum::density::Node exactShape(NodeType type) {
    stratum::density::Node node;
    node.type = type;
    for (const stratum::density::SchemaField& field : stratum::density::fieldsOf(type)) {
        switch (field.kind) {
            case stratum::density::FieldKind::Function:
                node.arguments.push_back(0);
                break;
            case stratum::density::FieldKind::Number:
                node.parameters.push_back(1.0);
                break;
            case stratum::density::FieldKind::Noise:
                node.noise = ResourceLocation::parse("stratum:shaped");
                break;
            case stratum::density::FieldKind::Spline:
                node.spline = 0;
                break;
            case stratum::density::FieldKind::Selector:
                node.selector = std::string(field.selectorValues.front());
                break;
        }
    }
    return node;
}

/// What an assembler that already holds one constant says to @p node: the
/// refusal's message, or nothing if it takes it.
[[nodiscard]] std::string refusalOf(const stratum::density::Node& node) {
    Graph::Assembler assembler;
    stratum::density::Node constant;
    constant.type = NodeType::Constant;
    constant.parameters = {0.5};
    static_cast<void>(assembler.addNode(constant));
    try {
        static_cast<void>(assembler.addNode(node));
    } catch (const ResolveError& error) {
        return error.what();
    }
    return {};
}

} // namespace

TEST_CASE("every node type is held to its own shape, from one table", "[density][graph][ub]") {
    // Graph::Assembler is what freeze::read rebuilds a graph through, and a
    // blob's counts are whatever its bytes say. The interpreter indexes
    // arguments and parameters by position, so a node with fewer of either
    // than its type reads — a parameterless `constant`, an argumentless
    // `clamp` — read past the end of a vector at its first evaluation. Every
    // type, not the three a per-type check once covered: the loop runs to the
    // first number that names no type, so a type the schema adds is in it.
    using Catch::Matchers::ContainsSubstring;
    std::size_t types = 0;
    for (int raw = 0; stratum::density::nodeTypeName(static_cast<NodeType>(raw)) != "unknown";
         ++raw) {
        const auto type = static_cast<NodeType>(raw);
        const std::string name = "'" + std::string(stratum::density::nodeTypeName(type)) + "'";
        CAPTURE(name);
        ++types;

        const stratum::density::Node exact = exactShape(type);
        CHECK(refusalOf(exact).empty());

        stratum::density::Node extraParameter = exact;
        extraParameter.parameters.push_back(1.0);
        CHECK_THAT(refusalOf(extraParameter),
                   ContainsSubstring(name + " carries " +
                                     std::to_string(exact.parameters.size() + 1) +
                                     " numeric parameter(s) where its type reads " +
                                     std::to_string(exact.parameters.size())));
        if (!exact.parameters.empty()) {
            stratum::density::Node missingParameter = exact;
            missingParameter.parameters.pop_back();
            CHECK_THAT(refusalOf(missingParameter),
                       ContainsSubstring(name + " carries " +
                                         std::to_string(exact.parameters.size() - 1) +
                                         " numeric parameter(s)"));
        }

        stratum::density::Node extraArgument = exact;
        extraArgument.arguments.push_back(0);
        CHECK_THAT(refusalOf(extraArgument),
                   ContainsSubstring(name + " carries " +
                                     std::to_string(exact.arguments.size() + 1) +
                                     " argument(s) where its type reads " +
                                     std::to_string(exact.arguments.size())));
        if (!exact.arguments.empty()) {
            stratum::density::Node missingArgument = exact;
            missingArgument.arguments.pop_back();
            CHECK_THAT(refusalOf(missingArgument),
                       ContainsSubstring(name + " carries " +
                                         std::to_string(exact.arguments.size() - 1) +
                                         " argument(s)"));
        }

        stratum::density::Node noise = exact;
        if (exact.noise.has_value()) {
            noise.noise.reset();
            CHECK_THAT(refusalOf(noise), ContainsSubstring(name + " carries no noise"));
            noise = exact;
            noise.inlineNoise =
                stratum::density::NoiseParameters{.firstOctave = -4, .amplitudes = {1.0}};
            CHECK_THAT(refusalOf(noise), ContainsSubstring("both a named and an inline noise"));
        } else {
            noise.noise = ResourceLocation::parse("stratum:stray");
            CHECK_THAT(refusalOf(noise),
                       ContainsSubstring(name + " carries a noise its type has no field for"));
        }

        stratum::density::Node spline = exact;
        if (exact.spline.has_value()) {
            spline.spline.reset();
            CHECK_THAT(refusalOf(spline), ContainsSubstring(name + " carries no spline"));
        } else {
            spline.spline = 0;
            CHECK_THAT(refusalOf(spline),
                       ContainsSubstring(name + " carries a spline its type has no field for"));
        }

        stratum::density::Node selector = exact;
        selector.selector = "type_3";
        CHECK_THAT(refusalOf(selector),
                   ContainsSubstring(name) && ContainsSubstring("selector 'type_3'"));
    }
    CHECK(types == 34U);

    // The four the review drove through a sealed blob, spelled out here rather
    // than derived from the schema the check itself reads
    // (pipeline_freeze_test.cpp drives them through freeze::read).
    stratum::density::Node constant;
    constant.type = NodeType::Constant;
    CHECK_THAT(refusalOf(constant), ContainsSubstring("'minecraft:constant' carries 0 numeric "
                                                      "parameter(s) where its type reads 1"));
    stratum::density::Node gradient;
    gradient.type = NodeType::YClampedGradient;
    CHECK_THAT(refusalOf(gradient), ContainsSubstring("'minecraft:y_clamped_gradient' carries 0 "
                                                      "numeric parameter(s) where its type "
                                                      "reads 4"));
    stratum::density::Node clamp;
    clamp.type = NodeType::Clamp;
    clamp.parameters = {0.0, 1.0};
    CHECK_THAT(refusalOf(clamp),
               ContainsSubstring("'minecraft:clamp' carries 0 argument(s) where its type reads 1"));
    stratum::density::Node abs;
    abs.type = NodeType::Abs;
    CHECK_THAT(refusalOf(abs),
               ContainsSubstring("'minecraft:abs' carries 0 argument(s) where its type reads 1"));
}

TEST_CASE("a frozen spline with no points is refused", "[density][graph][ub]") {
    // The resolver refuses one; the interpreter reads the first point wherever
    // the coordinate falls, so from a blob it would read past the end.
    Graph::Assembler assembler;
    const stratum::density::SplineDefinition empty;
    CHECK_THROWS_MATCHES(assembler.addSpline(empty), ResolveError,
                         Catch::Matchers::MessageMatches(
                             Catch::Matchers::ContainsSubstring("frozen spline 0 has no points")));
    stratum::density::SplineDefinition one;
    one.points.push_back(stratum::density::SplinePoint{
        .location = 0.0, .derivative = 0.0, .value = 1.0, .nested = std::nullopt});
    CHECK_NOTHROW(assembler.addSpline(one));
}
