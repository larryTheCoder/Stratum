// Stratum — density function evaluation tests.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The conformance suite checks this interpreter against cubiomes on vanilla's
// own data, which is the strong evidence. What that cannot reach is the
// behaviour vanilla's data never exercises: the node types it does not use,
// the refusals, and the errors a third-party pack would provoke. Those are
// here, and most of this file is about what the interpreter declines to do.
//
// The arithmetic answers below are computed by hand from the documented
// formulas rather than from a previous run of this code. A test that only
// says "still the same as last time" would have passed just as happily
// before the code was written.

#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/graph.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Catch::Matchers::ContainsSubstring;
using stratum::data::Pack;
using stratum::data::ResourceLocation;
using stratum::density::EvalError;
using stratum::density::Graph;
using stratum::density::Interpreter;
using stratum::density::NoiseError;
using stratum::density::NoiseParameters;
using stratum::density::NoiseRegistry;
using stratum::density::Point;

[[nodiscard]] std::uint64_t bits(double value) noexcept {
    return std::bit_cast<std::uint64_t>(value);
}

/// A worldgen tree written on the fly, with both registries the interpreter
/// needs, removed with the test.
class TempTree {
public:
    TempTree() : path_(std::filesystem::temp_directory_path() / uniqueName()) {
        std::filesystem::create_directories(path_ / "density_function");
        std::filesystem::create_directories(path_ / "noise");
    }

    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;

    ~TempTree() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const TempTree& define(std::string_view name, std::string_view json) const {
        return write("density_function", name, json);
    }

    const TempTree& defineNoise(std::string_view name, std::string_view json) const {
        return write("noise", name, json);
    }

    [[nodiscard]] Pack pack() const { return Pack::openWorldgenTree(path_); }

private:
    const TempTree& write(std::string_view registry, std::string_view name,
                          std::string_view json) const {
        const std::filesystem::path file = path_ / registry / (std::string(name) + ".json");
        std::filesystem::create_directories(file.parent_path());
        std::ofstream out(file);
        out << json;
        return *this;
    }

    [[nodiscard]] static std::string uniqueName() {
        static int counter = 0;
        return "stratum-interpreter-test-" + std::to_string(++counter);
    }

    std::filesystem::path path_;
};

/// A graph, its noises and an interpreter over both. Not movable: the
/// interpreter points at the graph beside it.
class Pipeline {
public:
    Pipeline(const Pack& pack, std::int64_t seed = 0)
        : graph_(Graph::resolveAll(pack)),
          noises_(NoiseRegistry::create(pack, graph_.referencedNoises(), seed)),
          interpreter_(graph_, noises_) {}

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    [[nodiscard]] double at(std::string_view function, Point point) const {
        return interpreter_.evaluate(root(function), point);
    }

    [[nodiscard]] double at(std::string_view function) const {
        return at(function, Point{.x = 0, .y = 0, .z = 0});
    }

    [[nodiscard]] stratum::density::NodeIndex root(std::string_view function) const {
        return graph_.rootOf(ResourceLocation::parse(std::string(function)));
    }

    [[nodiscard]] const Interpreter& interpreter() const noexcept { return interpreter_; }

private:
    Graph graph_;
    NoiseRegistry noises_;
    Interpreter interpreter_;
};

} // namespace

TEST_CASE("the arithmetic nodes compute what their documentation says", "[density][interpreter]") {
    const TempTree tree;
    tree.define("two", "2.0");
    tree.define("minus_three", "-3.0");
    tree.define("add", R"({"type":"minecraft:add","argument1":"two","argument2":"minus_three"})");
    tree.define("mul", R"({"type":"minecraft:mul","argument1":"two","argument2":"minus_three"})");
    tree.define("min", R"({"type":"minecraft:min","argument1":"two","argument2":"minus_three"})");
    tree.define("max", R"({"type":"minecraft:max","argument1":"two","argument2":"minus_three"})");
    tree.define("abs", R"({"type":"minecraft:abs","argument":"minus_three"})");
    tree.define("square", R"({"type":"minecraft:square","argument":"minus_three"})");
    tree.define("cube", R"({"type":"minecraft:cube","argument":"minus_three"})");
    tree.define("half_pos", R"({"type":"minecraft:half_negative","argument":"two"})");
    tree.define("half_neg", R"({"type":"minecraft:half_negative","argument":"minus_three"})");
    tree.define("quarter_pos", R"({"type":"minecraft:quarter_negative","argument":"two"})");
    tree.define("quarter_neg", R"({"type":"minecraft:quarter_negative","argument":"minus_three"})");
    tree.define("clamp",
                R"({"type":"minecraft:clamp","input":{"type":"minecraft:abs",
                    "argument":"minus_three"},"min":-1.0,"max":1.5})");

    const Pipeline pipeline(tree.pack());

    CHECK(bits(pipeline.at("add")) == bits(-1.0));
    CHECK(bits(pipeline.at("mul")) == bits(-6.0));
    CHECK(bits(pipeline.at("min")) == bits(-3.0));
    CHECK(bits(pipeline.at("max")) == bits(2.0));
    CHECK(bits(pipeline.at("abs")) == bits(3.0));
    CHECK(bits(pipeline.at("square")) == bits(9.0));
    CHECK(bits(pipeline.at("cube")) == bits(-27.0));

    // Only the negative side is scaled; the positive side passes through
    // untouched, which is the entire point of these two.
    CHECK(bits(pipeline.at("half_pos")) == bits(2.0));
    CHECK(bits(pipeline.at("half_neg")) == bits(-1.5));
    CHECK(bits(pipeline.at("quarter_pos")) == bits(2.0));
    CHECK(bits(pipeline.at("quarter_neg")) == bits(-0.75));

    CHECK(bits(pipeline.at("clamp")) == bits(1.5));
}

TEST_CASE("squeeze and the reciprocal are computed as documented", "[density][interpreter]") {
    const TempTree tree;
    // Neither type appears anywhere in vanilla 1.21.11's data, so no golden
    // will ever reach them. They are implemented from the documented
    // formulas and flagged as such in SPEC §11; these vectors are the only
    // thing standing behind them.
    tree.define("squeeze_half", R"({"type":"minecraft:squeeze","argument":0.5})");
    tree.define("squeeze_clamped", R"({"type":"minecraft:squeeze","argument":4.0})");
    tree.define("invert", R"({"type":"minecraft:invert","argument":4.0})");

    const Pipeline pipeline(tree.pack());

    // c/2 - c^3/24 at c = 0.5: 0.25 - 0.125/24.
    CHECK(bits(pipeline.at("squeeze_half")) == bits(0.25 - (0.125 / 24.0)));
    // The input is clamped to [-1, 1] first, so 4 and 1 give the same answer.
    CHECK(bits(pipeline.at("squeeze_clamped")) == bits(0.5 - (1.0 / 24.0)));
    CHECK(bits(pipeline.at("invert")) == bits(0.25));
}

TEST_CASE("y_clamped_gradient maps the column and stops at its ends", "[density][interpreter]") {
    const TempTree tree;
    tree.define("gradient", R"({"type":"minecraft:y_clamped_gradient",
        "from_y":-64,"to_y":320,"from_value":1.5,"to_value":-1.5})");

    const Pipeline pipeline(tree.pack());
    const auto at = [&pipeline](std::int32_t y) {
        return pipeline.at("gradient", Point{.x = 0, .y = y, .z = 0});
    };

    // The ends are returned exactly rather than interpolated to, which is
    // what separates clampedMap from clamping the fraction and lerping.
    CHECK(bits(at(-64)) == bits(1.5));
    CHECK(bits(at(320)) == bits(-1.5));
    CHECK(bits(at(-1000)) == bits(1.5));
    CHECK(bits(at(4000)) == bits(-1.5));
    // Halfway up: (128 + 64) / 384 = 0.5 of the way from 1.5 to -1.5.
    CHECK(bits(at(128)) == bits(0.0));
}

TEST_CASE("range_choice picks a branch and evaluates only that one", "[density][interpreter]") {
    const TempTree tree;
    // The out-of-range branch divides by zero through invert. If it were
    // evaluated regardless of the choice, the in-range answer would still be
    // correct and nothing would notice — so the branch is made to produce a
    // value that cannot be mistaken for a real one.
    tree.define("choice", R"({"type":"minecraft:range_choice",
        "input":{"type":"minecraft:y_clamped_gradient",
                 "from_y":0,"to_y":100,"from_value":0.0,"to_value":100.0},
        "min_inclusive":10.0,"max_exclusive":20.0,
        "when_in_range":7.0,
        "when_out_of_range":{"type":"minecraft:invert","argument":0.0}})");

    const Pipeline pipeline(tree.pack());
    const auto at = [&pipeline](std::int32_t y) {
        return pipeline.at("choice", Point{.x = 0, .y = y, .z = 0});
    };

    CHECK(bits(at(10)) == bits(7.0));
    CHECK(bits(at(19)) == bits(7.0));
    // The bounds are half-open: the maximum is excluded.
    CHECK(bits(at(20)) != bits(7.0));
    CHECK(bits(at(9)) != bits(7.0));
}

TEST_CASE("flat_cache samples at the corner of its column, not where it is asked",
          "[density][interpreter]") {
    const TempTree tree;
    tree.defineNoise("test", R"({"firstOctave":-3,"amplitudes":[1.0,1.0]})");
    tree.define("raw", R"({"type":"minecraft:noise","noise":"test",
        "xz_scale":1.0,"y_scale":1.0})");
    tree.define("flat", R"({"type":"minecraft:flat_cache","argument":"raw"})");

    const Pipeline pipeline(tree.pack(), 12345);
    const auto flat = [&pipeline](std::int32_t x, std::int32_t y, std::int32_t z) {
        return pipeline.at("flat", Point{.x = x, .y = y, .z = z});
    };
    const auto raw = [&pipeline](std::int32_t x, std::int32_t y, std::int32_t z) {
        return pipeline.at("raw", Point{.x = x, .y = y, .z = z});
    };

    // Every block of a 4x4 column reads the corner's value, at y = 0.
    const std::uint64_t corner = bits(raw(0, 0, 0));
    for (std::int32_t x = 0; x < 4; ++x) {
        for (std::int32_t z = 0; z < 4; ++z) {
            CAPTURE(x, z);
            CHECK(bits(flat(x, 0, z)) == corner);
            CHECK(bits(flat(x, 137, z)) == corner);
            CHECK(bits(flat(x, -55, z)) == corner);
        }
    }

    // The next column over is a different value, or the relocation would be
    // indistinguishable from returning a constant.
    CHECK(bits(flat(4, 0, 0)) != corner);
    CHECK(bits(flat(4, 0, 0)) == bits(raw(4, 0, 0)));

    // Negative coordinates are where a truncating division would round the
    // wrong way: block -1 belongs to the column whose corner is -4, not 0.
    CHECK(bits(flat(-1, 0, -1)) == bits(raw(-4, 0, -4)));
    CHECK(bits(flat(-4, 0, -4)) == bits(raw(-4, 0, -4)));
    CHECK(bits(flat(-5, 0, -5)) == bits(raw(-8, 0, -8)));
}

TEST_CASE("cache_2d over a column-varying function is refused", "[density][interpreter]") {
    const TempTree tree;
    tree.define("gradient", R"({"type":"minecraft:y_clamped_gradient",
        "from_y":0,"to_y":100,"from_value":0.0,"to_value":1.0})");
    tree.define("bad", R"({"type":"minecraft:cache_2d","argument":"gradient"})");
    tree.define("good", R"({"type":"minecraft:cache_2d","argument":2.0})");

    const Pipeline pipeline(tree.pack());

    // Vanilla caches cache_2d on (x, z) alone, so a y-varying argument would
    // give the whole column whichever y happened to be asked for first. That
    // is order-dependent, and this build will not pick an order quietly.
    CHECK_THROWS_WITH(pipeline.interpreter().requireEvaluable(pipeline.root("bad")),
                      ContainsSubstring("cache_2d") && ContainsSubstring("varies with y"));
    CHECK_NOTHROW(pipeline.interpreter().requireEvaluable(pipeline.root("good")));

    CHECK_FALSE(pipeline.interpreter().isColumnInvariant(pipeline.root("gradient")));
    CHECK(pipeline.interpreter().isColumnInvariant(pipeline.root("good")));
}

TEST_CASE("a cache over something unevaluable blames the right thing", "[density][interpreter]") {
    const TempTree tree;
    // Exactly the shape of vanilla's end/erosion. end_islands is as
    // column-invariant as a function gets, but this build cannot evaluate it,
    // and column invariance is deliberately conservative about anything it
    // cannot evaluate — so checking the cache before its contents produced a
    // true refusal with a false reason stapled to it.
    tree.define("islands", R"({"type":"minecraft:cache_2d",
        "argument":{"type":"minecraft:end_islands"}})");

    const Pipeline pipeline(tree.pack());

    CHECK_THROWS_WITH(pipeline.interpreter().requireEvaluable(pipeline.root("islands")),
                      ContainsSubstring("minecraft:end_islands"));
    CHECK_THROWS_WITH(pipeline.interpreter().requireEvaluable(pipeline.root("islands")),
                      !ContainsSubstring("varies with y"));
}

TEST_CASE("the node types this build cannot evaluate are refused by name",
          "[density][interpreter]") {
    const TempTree tree;
    tree.defineNoise("test", R"({"firstOctave":-3,"amplitudes":[1.0]})");
    tree.define("interpolated", R"({"type":"minecraft:interpolated","argument":1.0})");
    tree.define("cell", R"({"type":"minecraft:cache_all_in_cell","argument":1.0})");
    tree.define("blended", R"({"type":"minecraft:old_blended_noise","xz_scale":1.0,
        "y_scale":1.0,"xz_factor":80.0,"y_factor":160.0,"smear_scale_multiplier":8.0})");
    tree.define("islands", R"({"type":"minecraft:end_islands"})");
    tree.define("weird", R"({"type":"minecraft:weird_scaled_sampler",
        "rarity_value_mapper":"type_1","noise":"test","input":1.0})");
    tree.define("blend_density", R"({"type":"minecraft:blend_density","argument":1.0})");
    // Buried two levels down, to make sure the walk descends rather than
    // only inspecting the root.
    tree.define("nested", R"({"type":"minecraft:add","argument1":1.0,
        "argument2":{"type":"minecraft:mul","argument1":2.0,"argument2":"interpolated"}})");

    const Pipeline pipeline(tree.pack());
    const auto refusal = [&pipeline](std::string_view function) {
        try {
            static_cast<void>(pipeline.at(function));
        } catch (const EvalError& error) {
            return std::string(error.what());
        }
        return std::string("no refusal");
    };

    CHECK_THAT(refusal("interpolated"),
               ContainsSubstring("minecraft:interpolated") && ContainsSubstring("cell"));
    CHECK_THAT(refusal("cell"), ContainsSubstring("minecraft:cache_all_in_cell"));
    CHECK_THAT(refusal("blended"), ContainsSubstring("minecraft:old_blended_noise"));
    CHECK_THAT(refusal("islands"), ContainsSubstring("minecraft:end_islands"));
    CHECK_THAT(refusal("weird"), ContainsSubstring("minecraft:weird_scaled_sampler"));
    CHECK_THAT(refusal("blend_density"), ContainsSubstring("minecraft:blend_density"));

    // Reached through the walk, not through evaluation: requireEvaluable is
    // what a caller runs at load, and it has to find this before any chunk
    // does.
    CHECK_THROWS_WITH(pipeline.interpreter().requireEvaluable(pipeline.root("nested")),
                      ContainsSubstring("minecraft:interpolated"));
}

TEST_CASE("blend_alpha and blend_offset take their no-blending values", "[density][interpreter]") {
    const TempTree tree;
    tree.define("alpha", R"({"type":"minecraft:blend_alpha"})");
    tree.define("offset", R"({"type":"minecraft:blend_offset"})");

    const Pipeline pipeline(tree.pack());

    // This engine generates every chunk itself and never blends against
    // terrain another generator wrote, so these are constants — which is
    // what makes vanilla's overworld/offset reduce to its spline.
    CHECK(bits(pipeline.at("alpha")) == bits(1.0));
    CHECK(bits(pipeline.at("offset")) == bits(0.0));
}

TEST_CASE("splines pass through their knots and extrapolate off the ends",
          "[density][interpreter]") {
    const TempTree tree;
    tree.define("gradient", R"({"type":"minecraft:y_clamped_gradient",
        "from_y":0,"to_y":100,"from_value":0.0,"to_value":100.0})");
    tree.define("spline", R"({"type":"minecraft:spline","spline":{
        "coordinate":"gradient",
        "points":[
            {"location":10.0,"value":1.0,"derivative":2.0},
            {"location":20.0,"value":5.0,"derivative":0.0},
            {"location":40.0,"value":-1.0,"derivative":-0.5}
        ]}})");

    const Pipeline pipeline(tree.pack());
    const auto at = [&pipeline](std::int32_t y) {
        return pipeline.at("spline", Point{.x = 0, .y = y, .z = 0});
    };

    // At a knot the interpolation collapses to that knot's own value: the
    // fraction is 0 or 1 and the cubic correction term vanishes.
    CHECK(bits(at(10)) == bits(1.0));
    CHECK(bits(at(20)) == bits(5.0));
    CHECK(bits(at(40)) == bits(-1.0));

    // Off either end it continues as the straight line the nearest knot's
    // derivative describes, rather than flattening: 1.0 + 2.0 * (5 - 10).
    CHECK(bits(at(5)) == bits(-9.0));
    // -1.0 + -0.5 * (60 - 40).
    CHECK(bits(at(60)) == bits(-11.0));

    // Between two knots with zero derivatives the cubic is a plain smooth
    // step, so the midpoint of the second segment is the mean of its ends
    // only when both derivatives vanish — here the second knot's does and
    // the third's does not, so the value simply has to lie between them.
    const double middle = at(30);
    CHECK(middle < 5.0);
    CHECK(middle > -1.0);
}

TEST_CASE("a noise a density function names but the pack lacks is refused",
          "[density][interpreter]") {
    const TempTree tree;
    tree.define("uses_noise", R"({"type":"minecraft:noise","noise":"missing",
        "xz_scale":1.0,"y_scale":1.0})");

    const Graph graph = Graph::resolveAll(tree.pack());
    // Refused where the noise is built, naming it, rather than on whichever
    // chunk first reached that node.
    CHECK_THROWS_WITH(NoiseRegistry::create(tree.pack(), graph.referencedNoises(), 0),
                      ContainsSubstring("minecraft:missing"));
}

TEST_CASE("malformed noise parameters are refused by name", "[density][noise]") {
    const auto parse = [](std::string_view json) {
        return NoiseParameters::fromJson(nlohmann::json::parse(json),
                                         ResourceLocation::parse("test:noise"));
    };

    CHECK_THROWS_WITH(parse(R"({"amplitudes":[1.0]})"), ContainsSubstring("firstOctave"));
    CHECK_THROWS_WITH(parse(R"({"firstOctave":-3})"), ContainsSubstring("amplitudes"));
    CHECK_THROWS_WITH(parse(R"({"firstOctave":-3.5,"amplitudes":[1.0]})"),
                      ContainsSubstring("must be an integer"));
    CHECK_THROWS_WITH(parse(R"({"firstOctave":-3,"amplitudes":[]})"), ContainsSubstring("empty"));
    CHECK_THROWS_WITH(parse(R"({"firstOctave":-3,"amplitudes":["x"]})"),
                      ContainsSubstring("not a number"));
    CHECK_THROWS_AS(parse(R"({"firstOctave":-9999,"amplitudes":[1.0]})"), NoiseError);

    // Every message names the entry, because a pack with sixty noises needs
    // to be told which one is wrong.
    CHECK_THROWS_WITH(parse(R"({"amplitudes":[1.0]})"), ContainsSubstring("test:noise"));

    const NoiseParameters good = parse(R"({"firstOctave":-7,"amplitudes":[1.0,0.0,2.5]})");
    CHECK(good.firstOctave == -7);
    REQUIRE(good.amplitudes.size() == 3U);
    CHECK(bits(good.amplitudes[2]) == bits(2.5));
}

TEST_CASE("a noise is a pure function of the world seed and its own name", "[density][noise]") {
    const TempTree tree;
    tree.defineNoise("alpha", R"({"firstOctave":-4,"amplitudes":[1.0,1.0]})");
    // Identical parameters, different name: the MD5 salt is the only thing
    // separating them, so equal samples would mean the salt is not applied.
    tree.defineNoise("beta", R"({"firstOctave":-4,"amplitudes":[1.0,1.0]})");

    const Pack pack = tree.pack();
    const std::vector<ResourceLocation> wanted{ResourceLocation::parse("alpha"),
                                               ResourceLocation::parse("beta")};

    const NoiseRegistry first = NoiseRegistry::create(pack, wanted, 99);
    const NoiseRegistry again = NoiseRegistry::create(pack, wanted, 99);
    const NoiseRegistry other = NoiseRegistry::create(pack, wanted, 100);

    const auto sample = [&wanted](const NoiseRegistry& registry, std::size_t which) {
        return registry.get(wanted[which]).sample(1.5, -2.25, 3.75);
    };

    CHECK(bits(sample(first, 0)) == bits(sample(again, 0)));
    CHECK(bits(sample(first, 1)) == bits(sample(again, 1)));
    CHECK(bits(sample(first, 0)) != bits(sample(first, 1)));
    CHECK(bits(sample(first, 0)) != bits(sample(other, 0)));
}
