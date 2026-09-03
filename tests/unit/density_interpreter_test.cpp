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
#include <cmath>
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
using stratum::density::CellGeometry;
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
          noises_(NoiseRegistry::create(pack, graph_.referencedNoises(), seed,
                                        stratum::density::RandomSource::Xoroshiro)),
          interpreter_(graph_, noises_) {}

    /// Over a cell lattice, which is what gives `interpolated` a meaning.
    Pipeline(const Pack& pack, std::int64_t seed, CellGeometry cells)
        : graph_(Graph::resolveAll(pack)),
          noises_(NoiseRegistry::create(pack, graph_.referencedNoises(), seed,
                                        stratum::density::RandomSource::Xoroshiro)),
          interpreter_(graph_, noises_, cells) {}

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

    // Reached through the walk, not through evaluation: requireEvaluable is
    // what a caller runs at load, and it has to find this before any chunk
    // does.
    CHECK_THROWS_WITH(pipeline.interpreter().requireEvaluable(pipeline.root("nested")),
                      ContainsSubstring("minecraft:interpolated"));
}

TEST_CASE("the caches that do not relocate return their argument unchanged",
          "[density][interpreter]") {
    const TempTree tree;
    tree.defineNoise("test", R"({"firstOctave":-4,"amplitudes":[1.0]})");
    tree.define("raw", R"({"type":"minecraft:noise","noise":"test",
        "xz_scale":1.0,"y_scale":1.0})");
    tree.define("once", R"({"type":"minecraft:cache_once","argument":"raw"})");
    tree.define("twod", R"({"type":"minecraft:cache_2d","argument":{
        "type":"minecraft:noise","noise":"test","xz_scale":1.0,"y_scale":0.0}})");
    tree.define("twod_raw", R"({"type":"minecraft:noise","noise":"test",
        "xz_scale":1.0,"y_scale":0.0})");

    const Pipeline pipeline(tree.pack(), 17);

    // Obvious enough to have gone untested: both were only ever exercised
    // through vanilla's data, so a mutation that made them return zero left
    // the whole unit suite green.
    for (const Point at : {Point{.x = 0, .y = 0, .z = 0}, Point{.x = 5, .y = -3, .z = 11}}) {
        CAPTURE(at.x, at.y, at.z);
        CHECK(bits(pipeline.at("once", at)) == bits(pipeline.at("raw", at)));
        CHECK(bits(pipeline.at("twod", at)) == bits(pipeline.at("twod_raw", at)));
        // A cache over a function that happened to be zero would pass either
        // way, so check there is something to pass through.
        CHECK(bits(pipeline.at("raw", at)) != bits(0.0));
    }
}

TEST_CASE("the three blending types take their no-blending values", "[density][interpreter]") {
    const TempTree tree;
    tree.define("alpha", R"({"type":"minecraft:blend_alpha"})");
    tree.define("offset", R"({"type":"minecraft:blend_offset"})");
    tree.define("density", R"({"type":"minecraft:blend_density","argument":-2.5})");
    tree.define("nested", R"({"type":"minecraft:blend_density","argument":{
        "type":"minecraft:y_clamped_gradient",
        "from_y":0,"to_y":100,"from_value":0.0,"to_value":100.0}})");

    const Pipeline pipeline(tree.pack());

    // This engine generates every chunk itself and never blends against
    // terrain another generator wrote. For the first two that means 1.0 and
    // 0.0, which is what makes vanilla's overworld/offset reduce to its
    // spline; for the third it means leaving the density alone, which is the
    // same statement about the same interface. None of the three is
    // documented anywhere — SPEC §11 carries all three.
    CHECK(bits(pipeline.at("alpha")) == bits(1.0));
    CHECK(bits(pipeline.at("offset")) == bits(0.0));
    CHECK(bits(pipeline.at("density")) == bits(-2.5));
    CHECK(bits(pipeline.at("nested", Point{.x = 0, .y = 37, .z = 0})) == bits(37.0));
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
    CHECK_THROWS_WITH(NoiseRegistry::create(tree.pack(), graph.referencedNoises(), 0,
                                            stratum::density::RandomSource::Xoroshiro),
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

    const NoiseRegistry first =
        NoiseRegistry::create(pack, wanted, 99, stratum::density::RandomSource::Xoroshiro);
    const NoiseRegistry again =
        NoiseRegistry::create(pack, wanted, 99, stratum::density::RandomSource::Xoroshiro);
    const NoiseRegistry other =
        NoiseRegistry::create(pack, wanted, 100, stratum::density::RandomSource::Xoroshiro);

    const auto sample = [&wanted](const NoiseRegistry& registry, std::size_t which) {
        return registry.get(wanted[which]).sample(1.5, -2.25, 3.75);
    };

    CHECK(bits(sample(first, 0)) == bits(sample(again, 0)));
    CHECK(bits(sample(first, 1)) == bits(sample(again, 1)));
    CHECK(bits(sample(first, 0)) != bits(sample(first, 1)));
    CHECK(bits(sample(first, 0)) != bits(sample(other, 0)));
}

namespace {

/// A pack with a noise to interpolate and a column-varying function to check
/// the surrounding tree against.
void defineCellPack(const TempTree& tree) {
    tree.defineNoise("test", R"({"firstOctave":-4,"amplitudes":[1.0,1.0]})");
    tree.define("field", R"({"type":"minecraft:noise","noise":"test",
        "xz_scale":1.0,"y_scale":1.0})");
    tree.define("smooth", R"({"type":"minecraft:interpolated","argument":"field"})");
    tree.define("flat", R"({"type":"minecraft:interpolated","argument":0.75})");
    tree.define("gradient", R"({"type":"minecraft:y_clamped_gradient",
        "from_y":-64,"to_y":320,"from_value":-1.5,"to_value":1.5})");
    tree.define("smooth_gradient", R"({"type":"minecraft:interpolated","argument":"gradient"})");
    tree.define("cell_cached", R"({"type":"minecraft:cache_all_in_cell","argument":"field"})");
}

constexpr CellGeometry kOverworldCells{.width = 4, .height = 8};

} // namespace

TEST_CASE("interpolation returns the corner exactly at a corner", "[density][cells]") {
    const TempTree tree;
    defineCellPack(tree);
    const Pipeline pipeline(tree.pack(), 55, kOverworldCells);

    // At a lattice point the two ends of every blend are the same value, so
    // the result must be that value and not merely close to it. This is what
    // pins the corner positions: a cell aligned to the wrong multiple would
    // put the "corner" somewhere the argument has a different value.
    for (const Point corner :
         {Point{.x = 0, .y = 0, .z = 0}, Point{.x = 8, .y = 16, .z = 12},
          Point{.x = -4, .y = -8, .z = -16}, Point{.x = -100, .y = 400, .z = 64}}) {
        // Only the points that are genuinely on the lattice.
        if (corner.x % 4 != 0 || corner.y % 8 != 0 || corner.z % 4 != 0) {
            continue;
        }
        CAPTURE(corner.x, corner.y, corner.z);
        CHECK(bits(pipeline.at("smooth", corner)) == bits(pipeline.at("field", corner)));
    }

    // A constant is its own interpolation everywhere, corner or not.
    CHECK(bits(pipeline.at("flat", Point{.x = 3, .y = 5, .z = 1})) == bits(0.75));
}

TEST_CASE("a cell is found with floor division, not truncation", "[density][cells]") {
    const TempTree tree;
    defineCellPack(tree);
    const Pipeline pipeline(tree.pack(), 55, kOverworldCells);

    // Block -1 belongs to the cell whose lower corner is -4, not to the one
    // at 0. Truncating division would put it in the cell above with a
    // *negative* weight — extrapolating below the lattice instead of
    // interpolating within it — and shift every negative coordinate in the
    // world by one cell.
    const auto smooth = [&pipeline](std::int32_t x, std::int32_t y, std::int32_t z) {
        return pipeline.at("smooth", Point{.x = x, .y = y, .z = z});
    };
    const auto field = [&pipeline](std::int32_t x, std::int32_t y, std::int32_t z) {
        return pipeline.at("field", Point{.x = x, .y = y, .z = z});
    };

    CHECK(bits(smooth(-4, -8, -4)) == bits(field(-4, -8, -4)));

    // On a lattice line in x and z, block y = -1 must be three quarters of
    // the way up the cell [-8, 0) — not a quarter of the way *below* the
    // cell [0, 8), which is where truncation would put it. Checking the
    // value rather than merely that it differs from the origin's is the
    // whole difference: both readings differ from the origin.
    const double lower = field(0, -8, 0);
    const double upper = field(0, 0, 0);
    CHECK(bits(smooth(0, -1, 0)) == bits(lower + (0.875 * (upper - lower))));

    const double west = field(-4, 0, 0);
    const double east = field(0, 0, 0);
    CHECK(bits(smooth(-1, 0, 0)) == bits(west + (0.75 * (east - west))));

    const double near = field(0, 0, -4);
    const double far = field(0, 0, 0);
    CHECK(bits(smooth(0, 0, -1)) == bits(near + (0.75 * (far - near))));
}

TEST_CASE("blending along one axis is exactly the lerp of that axis's corners",
          "[density][cells]") {
    const TempTree tree;
    defineCellPack(tree);
    const Pipeline pipeline(tree.pack(), 7, kOverworldCells);

    // On a lattice line in x and z, the x and z weights are zero, so the
    // whole trilinear blend collapses to a single lerp in y — which the test
    // can compute exactly from the two corner values.
    const double lower = pipeline.at("field", Point{.x = 0, .y = 0, .z = 0});
    const double upper = pipeline.at("field", Point{.x = 0, .y = 8, .z = 0});
    for (const std::int32_t y : {1, 2, 4, 7}) {
        const double part = static_cast<double>(y) / 8.0;
        CAPTURE(y);
        CHECK(bits(pipeline.at("smooth", Point{.x = 0, .y = y, .z = 0})) ==
              bits(lower + (part * (upper - lower))));
    }
}

TEST_CASE("the order the axes are blended in is a decision, not an accident", "[density][cells]") {
    const TempTree tree;
    defineCellPack(tree);
    const Pipeline pipeline(tree.pack(), 99, kOverworldCells);

    const auto corner = [&pipeline](std::int32_t dx, std::int32_t dy, std::int32_t dz) {
        return pipeline.at("field", Point{.x = dx * 4, .y = dy * 8, .z = dz * 4});
    };
    const double v000 = corner(0, 0, 0);
    const double v001 = corner(0, 0, 1);
    const double v010 = corner(0, 1, 0);
    const double v011 = corner(0, 1, 1);
    const double v100 = corner(1, 0, 0);
    const double v101 = corner(1, 0, 1);
    const double v110 = corner(1, 1, 0);
    const double v111 = corner(1, 1, 1);

    const auto lerp = [](double part, double from, double to) {
        return from + (part * (to - from));
    };

    // Every block of one cell, not one chosen point: two orders agree bitwise
    // far more often than intuition suggests, and a single sample let a
    // y-then-z-then-x mutation through unnoticed.
    std::size_t differingYzx = 0;
    std::size_t differingZxy = 0;
    for (std::int32_t x = 0; x < 4; ++x) {
        for (std::int32_t y = 0; y < 8; ++y) {
            for (std::int32_t z = 0; z < 4; ++z) {
                const double tx = static_cast<double>(x) / 4.0;
                const double ty = static_cast<double>(y) / 8.0;
                const double tz = static_cast<double>(z) / 4.0;

                const double yThenXThenZ =
                    lerp(tz, lerp(tx, lerp(ty, v000, v010), lerp(ty, v100, v110)),
                         lerp(tx, lerp(ty, v001, v011), lerp(ty, v101, v111)));
                const double yThenZThenX =
                    lerp(tx, lerp(tz, lerp(ty, v000, v010), lerp(ty, v001, v011)),
                         lerp(tz, lerp(ty, v100, v110), lerp(ty, v101, v111)));
                const double zThenXThenY =
                    lerp(ty, lerp(tx, lerp(tz, v000, v001), lerp(tz, v100, v101)),
                         lerp(tx, lerp(tz, v010, v011), lerp(tz, v110, v111)));

                CAPTURE(x, y, z);
                CHECK(bits(pipeline.at("smooth", Point{.x = x, .y = y, .z = z})) ==
                      bits(yThenXThenZ));

                differingYzx += bits(yThenXThenZ) != bits(yThenZThenX) ? 1U : 0U;
                differingZxy += bits(yThenXThenZ) != bits(zThenXThenY) ? 1U : 0U;
            }
        }
    }

    // Trilinear interpolation is order-independent in exact arithmetic and is
    // not in floating point. That is the whole reason the order is written
    // down rather than left to whoever edits the loop next — and the counts
    // are checked so that this stays a real distinction rather than becoming
    // a test of nothing.
    CHECK(differingYzx > 0U);
    CHECK(differingZxy > 0U);
}

TEST_CASE("only the interpolated part of a tree is interpolated", "[density][cells]") {
    const TempTree tree;
    defineCellPack(tree);
    tree.define("mixed", R"({"type":"minecraft:add","argument1":"smooth_gradient",
        "argument2":"gradient"})");
    const Pipeline pipeline(tree.pack(), 3, kOverworldCells);

    // The second term must be the gradient at the actual y, not at a corner:
    // vanilla replaces the marker node, not the tree around it. If the whole
    // tree were interpolated the two terms would agree at every y.
    const Point at{.x = 0, .y = 5, .z = 0};
    const double whole = pipeline.at("mixed", at);
    const double smoothed = pipeline.at("smooth_gradient", at);
    const double exact = pipeline.at("gradient", at);

    CHECK(bits(whole) == bits(smoothed + exact));
    // The gradient is linear in y, so interpolating it changes almost
    // nothing — but "almost" is the point: they are not the same number.
    CHECK(std::abs(smoothed - exact) < 1e-9);
}

TEST_CASE("cache_all_in_cell keeps a value per block, so it changes none", "[density][cells]") {
    const TempTree tree;
    defineCellPack(tree);
    const Pipeline pipeline(tree.pack(), 21, kOverworldCells);

    // Unlike flat_cache it does not relocate the sample: vanilla stores one
    // value for every block of the cell, not one for the cell.
    for (const Point at : {Point{.x = 1, .y = 3, .z = 2}, Point{.x = -7, .y = -5, .z = 9}}) {
        CAPTURE(at.x, at.y, at.z);
        CHECK(bits(pipeline.at("cell_cached", at)) == bits(pipeline.at("field", at)));
    }
}

TEST_CASE("the cell lattice is part of the answer", "[density][cells]") {
    const TempTree tree;
    defineCellPack(tree);
    const Pipeline overworld(tree.pack(), 42, CellGeometry{.width = 4, .height = 8});
    const Pipeline end(tree.pack(), 42, CellGeometry{.width = 8, .height = 4});

    // The same function on the same seed at the same point, sampled on two
    // lattices. A dimension's cell size is not a performance knob.
    const Point at{.x = 5, .y = 5, .z = 5};
    CHECK(bits(overworld.at("smooth", at)) != bits(end.at("smooth", at)));
    CHECK(overworld.interpreter().cells()->width == 4);
    CHECK(end.interpreter().cells()->height == 4);
}

TEST_CASE("an unusable cell geometry is refused where it is given", "[density][cells]") {
    const TempTree tree;
    defineCellPack(tree);
    const Pack pack = tree.pack();
    const Graph graph = Graph::resolveAll(pack);
    const NoiseRegistry noises = NoiseRegistry::create(pack, graph.referencedNoises(), 0,
                                                       stratum::density::RandomSource::Xoroshiro);

    // Better here than as a division by zero on the first interpolation.
    CHECK_THROWS_WITH(Interpreter(graph, noises, CellGeometry{.width = 0, .height = 8}),
                      ContainsSubstring("positive"));
    CHECK_THROWS_WITH(Interpreter(graph, noises, CellGeometry{.width = 4, .height = -8}),
                      ContainsSubstring("positive"));
    CHECK_NOTHROW(Interpreter(graph, noises, CellGeometry{.width = 1, .height = 1}));
}

TEST_CASE("without a lattice the cell types are still refused, and say why", "[density][cells]") {
    const TempTree tree;
    defineCellPack(tree);
    const Pipeline pipeline(tree.pack(), 0);

    // A density function file belongs to no dimension, so on its own it has
    // no cell size — which is a different thing from the type being
    // unimplemented, and the message has to say so.
    CHECK_THROWS_WITH(pipeline.interpreter().requireEvaluable(pipeline.root("smooth")),
                      ContainsSubstring("cell geometry") && ContainsSubstring("noise settings"));
    CHECK(pipeline.interpreter().cells() == std::nullopt);
}

TEST_CASE("a legacy random source is refused rather than approximated", "[density][noise]") {
    const TempTree tree;
    tree.defineNoise("test", R"({"firstOctave":-4,"amplitudes":[1.0,1.0]})");
    tree.define("field", R"({"type":"minecraft:noise","noise":"test",
        "xz_scale":1.0,"y_scale":0.0})");

    const Pack pack = tree.pack();
    const Graph graph = Graph::resolveAll(pack);
    const auto wanted = graph.referencedNoises();

    // The modern derivation is not a near-enough stand-in for the legacy
    // one: it would seed every noise differently and produce a Nether that
    // generates and is not vanilla's, with nothing to say so.
    CHECK_THROWS_WITH(
        NoiseRegistry::create(pack, wanted, 0, stratum::density::RandomSource::Legacy),
        ContainsSubstring("legacy_random_source") && ContainsSubstring("will not substitute"));

    const NoiseRegistry modern =
        NoiseRegistry::create(pack, wanted, 0, stratum::density::RandomSource::Xoroshiro);
    CHECK(modern.source() == stratum::density::RandomSource::Xoroshiro);
    CHECK(modern.size() == 1U);

    CHECK(stratum::density::randomSourceName(stratum::density::RandomSource::Xoroshiro) ==
          "xoroshiro");
    CHECK(stratum::density::randomSourceName(stratum::density::RandomSource::Legacy) == "legacy");
}

TEST_CASE("a candidate reading can be put in front of the pipeline, and only that way",
          "[density][interpreter][unsettled]") {
    const TempTree tree;
    tree.defineNoise("test", R"({"firstOctave":-4,"amplitudes":[1.0]})");
    tree.define("blended", R"({"type":"minecraft:old_blended_noise","xz_scale":0.25,
        "y_scale":0.125,"xz_factor":80.0,"y_factor":160.0,"smear_scale_multiplier":8.0})");
    tree.define("weird", R"({"type":"minecraft:weird_scaled_sampler",
        "rarity_value_mapper":"type_1","noise":"test","input":0.5})");

    const Pack pack = tree.pack();
    const Graph graph = Graph::resolveAll(pack);
    const NoiseRegistry noises = NoiseRegistry::create(pack, graph.referencedNoises(), 0,
                                                       stratum::density::RandomSource::Xoroshiro);

    const auto blendedRoot = graph.rootOf(ResourceLocation::parse("minecraft:blended"));
    const auto weirdRoot = graph.rootOf(ResourceLocation::parse("minecraft:weird"));

    SECTION("without a candidate both stay refused") {
        const Interpreter plain(graph, noises);
        CHECK_THROWS_WITH(plain.requireEvaluable(blendedRoot), ContainsSubstring("normalisation"));
        CHECK_THROWS_WITH(plain.requireEvaluable(weirdRoot), ContainsSubstring("rarity mapping"));
    }

    SECTION("a candidate is handed the node's own parameters and position") {
        Interpreter running(graph, noises);
        stratum::density::UnsettledSubstitutions subs;
        stratum::noise::BlendedNoise::Parameters seen;
        Point seenAt{};
        subs.blendedNoise = [&](const stratum::noise::BlendedNoise::Parameters& parameters,
                                Point at) {
            seen = parameters;
            seenAt = at;
            return 7.5;
        };
        running.substitute(std::move(subs));

        CHECK_NOTHROW(running.requireEvaluable(blendedRoot));
        const Point at{.x = 3, .y = -17, .z = 91};
        CHECK(bits(running.evaluate(blendedRoot, at)) == bits(7.5));

        // All five, in the order the schema declares them — a candidate that
        // received them shuffled would be testing the wrong function.
        CHECK(bits(seen.xzScale) == bits(0.25));
        CHECK(bits(seen.yScale) == bits(0.125));
        CHECK(bits(seen.xzFactor) == bits(80.0));
        CHECK(bits(seen.yFactor) == bits(160.0));
        CHECK(bits(seen.smearScaleMultiplier) == bits(8.0));
        CHECK(seenAt == at);

        // Substituting one says nothing about the other.
        CHECK_THROWS_WITH(running.requireEvaluable(weirdRoot), ContainsSubstring("rarity mapping"));
    }

    SECTION("the weird_scaled_sampler constant is a constant, input and all") {
        Interpreter running(graph, noises);
        stratum::density::UnsettledSubstitutions subs;
        subs.weirdScaledSampler = 1.0e6;
        running.substitute(std::move(subs));

        CHECK_NOTHROW(running.requireEvaluable(weirdRoot));
        // It ignores the noise and the input entirely: this is an isolation
        // device for an experiment, not a reading of the type.
        CHECK(bits(running.evaluate(weirdRoot, Point{.x = 0, .y = 0, .z = 0})) == bits(1.0e6));
        CHECK(bits(running.evaluate(weirdRoot, Point{.x = 900, .y = -60, .z = -12})) ==
              bits(1.0e6));

        CHECK_THROWS_WITH(running.requireEvaluable(blendedRoot),
                          ContainsSubstring("normalisation"));
    }

    SECTION("an empty set of substitutions is the same as none") {
        Interpreter running(graph, noises);
        running.substitute(stratum::density::UnsettledSubstitutions{});
        CHECK(stratum::density::UnsettledSubstitutions{}.empty());
        CHECK_THROWS_WITH(running.requireEvaluable(blendedRoot),
                          ContainsSubstring("normalisation"));
    }
}
