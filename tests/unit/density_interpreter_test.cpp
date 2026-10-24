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

#include "support/temp_path.hpp"

#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/graph.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Catch::Matchers::ContainsSubstring;
using stratum::data::Pack;
using stratum::data::ResourceLocation;
using stratum::density::CellGeometry;
using stratum::density::EvalError;
using stratum::density::Graph;
using stratum::density::Interpreter;
using stratum::density::NodeType;
using stratum::density::NoiseError;
using stratum::density::NoiseParameters;
using stratum::density::NoiseRegistry;
using stratum::density::Point;
using stratum::density::rarityValueMapper;

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

    void define(std::string_view name, std::string_view json) const {
        write("density_function", name, json);
    }

    void defineNoise(std::string_view name, std::string_view json) const {
        write("noise", name, json);
    }

    [[nodiscard]] Pack pack() const { return Pack::openWorldgenTree(path_); }

private:
    void write(std::string_view registry, std::string_view name, std::string_view json) const {
        const std::filesystem::path file = path_ / registry / (std::string(name) + ".json");
        std::filesystem::create_directories(file.parent_path());
        std::ofstream out(file);
        out << json;
    }

    [[nodiscard]] static std::string uniqueName() {
        return stratum::test::tempName("stratum-interpreter-test");
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

TEST_CASE("flat_cache relocates only inside its chunk's window", "[density][interpreter]") {
    const TempTree tree;
    tree.defineNoise("test", R"({"firstOctave":-3,"amplitudes":[1.0,1.0]})");
    tree.define("raw", R"({"type":"minecraft:noise","noise":"test",
        "xz_scale":1.0,"y_scale":1.0})");
    tree.define("flat", R"({"type":"minecraft:flat_cache","argument":"raw"})");
    // Nested: an outer flat_cache over a sum that reads an inner one. Off the
    // window, NEITHER relocates — the window rides into nested scopes.
    // Off the window a read is where it was asked, its own y included:
    // measured through every aquifer entry a datapack can wrap in it, with a
    // y-varying argument (SPEC §11, "Cache markers in a datapack's aquifer
    // entries"), where pinning y to 0 was wrong on 67 771 to 284 101 blocks
    // an arm and seed.
    tree.define("nested", R"({"type":"minecraft:flat_cache","argument":
        {"type":"minecraft:add","argument1":"flat","argument2":1.0}})");

    const Pipeline pipeline(tree.pack(), 12345);
    const Interpreter& interpreter = pipeline.interpreter();
    Interpreter::CornerCache cache(interpreter.cacheSize());
    const auto raw = [&pipeline](std::int32_t x, std::int32_t y, std::int32_t z) {
        return pipeline.at("raw", Point{.x = x, .y = y, .z = z});
    };
    const auto read = [&](std::string_view function, Point at,
                          const stratum::density::FlatCacheWindow& window) {
        return interpreter.evaluate(pipeline.root(function), at, cache, window);
    };

    // The chunk at (16, 32): its window is that chunk's own sixteen columns.
    const stratum::density::FlatCacheWindow chunk{.minX = 16, .maxX = 31, .minZ = 32, .maxZ = 47};

    // Inside: relocated exactly as with no window at all.
    for (const Point at : {Point{.x = 17, .y = -30, .z = 33}, Point{.x = 31, .y = 5, .z = 47}}) {
        CAPTURE(at.x, at.y, at.z);
        CHECK(bits(read("flat", at, chunk)) == bits(pipeline.at("flat", at)));
        CHECK(bits(read("flat", at, chunk)) == bits(raw((at.x / 4) * 4, 0, (at.z / 4) * 4)));
    }

    // Outside, by one column on each side: the point itself, at its own y.
    for (const Point at : {Point{.x = 33, .y = -30, .z = 34}, Point{.x = 18, .y = 7, .z = 49},
                           Point{.x = 13, .y = 0, .z = 35}, Point{.x = 19, .y = 0, .z = 30}}) {
        CAPTURE(at.x, at.y, at.z);
        CHECK(bits(read("flat", at, chunk)) == bits(raw(at.x, at.y, at.z)));
        CHECK(bits(read("flat", at, chunk)) != bits(pipeline.at("flat", at)));
        CHECK(bits(read("nested", at, chunk)) == bits(raw(at.x, at.y, at.z) + 1.0));
    }
    // ... which is not the column at y = 0 when y is not 0.
    CHECK(bits(read("flat", Point{.x = 33, .y = -30, .z = 34}, chunk)) != bits(raw(33, 0, 34)));

    // `none()` covers nothing: every read is at its own column.
    const Point inside{.x = 17, .y = 0, .z = 33};
    CHECK(bits(read("flat", inside, stratum::density::FlatCacheWindow::none())) ==
          bits(raw(17, 0, 33)));
    // An aligned column is the same either way — which is why the aquifer's
    // 4-aligned `preliminary_surface_level` reads never see the window.
    const Point aligned{.x = 36, .y = 0, .z = 52};
    CHECK(bits(read("flat", aligned, chunk)) == bits(pipeline.at("flat", aligned)));
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

TEST_CASE("flat_cache is only as column-invariant as what it wraps", "[density][interpreter]") {
    // Inside a chunk's window it reads its argument at y = 0; outside one it
    // reads the argument where it was asked (SPEC §11). So a cache_2d over a
    // flat_cache of something y-varying varies with y wherever an aquifer
    // reads it off the window, and is refused like any other.
    const TempTree tree;
    tree.define("gradient", R"({"type":"minecraft:y_clamped_gradient",
        "from_y":0,"to_y":100,"from_value":0.0,"to_value":1.0})");
    tree.define("flat_gradient", R"({"type":"minecraft:flat_cache","argument":"gradient"})");
    tree.define("flat_constant", R"({"type":"minecraft:flat_cache","argument":2.0})");
    tree.define("bad", R"({"type":"minecraft:cache_2d","argument":"flat_gradient"})");
    tree.define("good", R"({"type":"minecraft:cache_2d","argument":"flat_constant"})");
    const Pipeline pipeline(tree.pack());

    CHECK_FALSE(pipeline.interpreter().isColumnInvariant(pipeline.root("flat_gradient")));
    CHECK(pipeline.interpreter().isColumnInvariant(pipeline.root("flat_constant")));
    CHECK_THROWS_WITH(pipeline.interpreter().requireEvaluable(pipeline.root("bad")),
                      ContainsSubstring("cache_2d") && ContainsSubstring("varies with y"));
    CHECK_NOTHROW(pipeline.interpreter().requireEvaluable(pipeline.root("good")));
}

TEST_CASE("every node type that reads y is column-varying, whatever its arguments",
          "[density][interpreter]") {
    // old_blended_noise has no arguments at all and weird_scaled_sampler's
    // input is a constant here, so "every argument invariant" would call both
    // invariant; both sample noise at the point's own y. Measured below, not
    // only asserted: each has two y in one column that differ.
    const TempTree tree;
    tree.defineNoise("test", R"({"firstOctave":-4,"amplitudes":[1.0]})");
    tree.define("blended", R"({"type":"minecraft:old_blended_noise","xz_scale":0.25,
        "y_scale":0.125,"xz_factor":80.0,"y_factor":160.0,"smear_scale_multiplier":8.0})");
    tree.define("weird", R"({"type":"minecraft:weird_scaled_sampler",
        "rarity_value_mapper":"type_1","noise":"test","input":0.5})");
    tree.define("shift", R"({"type":"minecraft:shift","argument":"minecraft:test"})");
    tree.define("islands", R"({"type":"minecraft:end_islands"})");
    tree.define("flat_noise", R"({"type":"minecraft:noise","noise":"test",
        "xz_scale":1.0,"y_scale":0.0})");
    tree.define("bad_blended", R"({"type":"minecraft:cache_2d","argument":"blended"})");
    tree.define("bad_weird", R"({"type":"minecraft:cache_2d","argument":"weird"})");
    const Pipeline pipeline(tree.pack(), 17);

    for (const std::string_view varying : {"blended", "weird", "shift"}) {
        CAPTURE(varying);
        CHECK_FALSE(pipeline.interpreter().isColumnInvariant(pipeline.root(varying)));
        bool differs = false;
        const double base = pipeline.at(varying, Point{.x = 37, .y = 0, .z = -91});
        for (std::int32_t y = 1; y < 128 && !differs; ++y) {
            differs = bits(pipeline.at(varying, Point{.x = 37, .y = y, .z = -91})) != bits(base);
        }
        CHECK(differs);
    }
    // The 2D ones stay invariant: end_islands reads no y, and a noise with
    // y_scale 0 samples every y at the same place.
    CHECK(pipeline.interpreter().isColumnInvariant(pipeline.root("islands")));
    CHECK(pipeline.interpreter().isColumnInvariant(pipeline.root("flat_noise")));
    // And cache_2d over either 3D sampler is refused, like any y-varying one.
    CHECK_THROWS_WITH(pipeline.interpreter().requireEvaluable(pipeline.root("bad_blended")),
                      ContainsSubstring("cache_2d") && ContainsSubstring("varies with y"));
    CHECK_THROWS_WITH(pipeline.interpreter().requireEvaluable(pipeline.root("bad_weird")),
                      ContainsSubstring("cache_2d") && ContainsSubstring("varies with y"));
}

TEST_CASE("a cache over something unevaluable blames the right thing", "[density][interpreter]") {
    const TempTree tree;
    // The shape of vanilla's end/erosion, with an unevaluable argument in
    // place of the `end_islands` that used to be one. The point is
    // unchanged: column invariance is deliberately conservative about
    // anything it cannot evaluate, so checking the cache before its contents
    // produced a true refusal with a false reason stapled to it. `slide` is
    // as column-dependent as end_islands was column-invariant, and the
    // refusal must still blame the argument rather than the cache.
    tree.define("islands", R"({"type":"minecraft:cache_2d",
        "argument":{"type":"minecraft:slide","argument":0.0}})");

    const Pipeline pipeline(tree.pack());

    CHECK_THROWS_WITH(pipeline.interpreter().requireEvaluable(pipeline.root("islands")),
                      ContainsSubstring("minecraft:slide"));
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
    tree.define("slide", R"({"type":"minecraft:slide","argument":0.0})");
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
    // Not old_blended_noise any more: it is settled and evaluates. Kept in
    // the tree above so that the walk still has to pass over it, and asserted
    // here so that a regression which re-refuses it is a failure rather than
    // a quiet loss of terrain.
    CHECK_THAT(refusal("blended"), ContainsSubstring("no refusal"));
    // Not end_islands any more: settled against the golden End regions and
    // two probe regions, so it evaluates. Left in the tree so the walk still
    // passes over it, and asserted so a regression that re-refuses it is a
    // failure rather than a quiet loss of the End.
    CHECK_THAT(refusal("islands"), ContainsSubstring("no refusal"));
    CHECK_THAT(refusal("slide"), ContainsSubstring("minecraft:slide"));
    // Not weird_scaled_sampler any more either: its formula and both rarity
    // ladders are settled, so it evaluates. Left in the tree so the walk
    // still passes over it.
    CHECK_THAT(refusal("weird"), ContainsSubstring("no refusal"));

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

TEST_CASE("a firstOctave past int64 is refused, not wrapped into range", "[density][noise][ub]") {
    // get<std::int64_t>() takes an unsigned value past INT64_MAX modulo 2^64,
    // so 2^64 - 1 read as -1 — inside [-64, 64] — and loaded (SPEC §8).
    const auto parse = [](std::string_view json) {
        return NoiseParameters::fromJson(nlohmann::json::parse(json),
                                         ResourceLocation::parse("test:noise"));
    };
    CHECK_THROWS_WITH(parse(R"({"firstOctave":18446744073709551615,"amplitudes":[1.0]})"),
                      ContainsSubstring("test:noise") &&
                          ContainsSubstring("\"firstOctave\" is 18446744073709551615") &&
                          ContainsSubstring("[-64, 64]"));
    CHECK_THROWS_WITH(parse(R"({"firstOctave":18446744073709551552,"amplitudes":[1.0]})"),
                      ContainsSubstring("18446744073709551552"));
    CHECK_THROWS_WITH(
        NoiseParameters::fromInline(
            nlohmann::json::parse(R"({"firstOctave":18446744073709551615,"amplitudes":[1.0]})"),
            "an inline noise"),
        ContainsSubstring("an inline noise") && ContainsSubstring("18446744073709551615"));
    CHECK(parse(R"({"firstOctave":-1,"amplitudes":[1.0]})").firstOctave == -1);
    CHECK(parse(R"({"firstOctave":64,"amplitudes":[1.0]})").firstOctave == 64);
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

TEST_CASE("a detached read of interpolated is its argument at the point", "[density][cells]") {
    // The block being generated blends over its cell; a point that block
    // reads elsewhere — an aquifer source's centre, its contracted indices —
    // reads the argument itself. Measured on every aquifer entry, both
    // halves (SPEC §11, "Cache markers in a datapack's aquifer entries").
    using stratum::density::ReadContext;
    const TempTree tree;
    defineCellPack(tree);
    tree.define("flat_smooth", R"({"type":"minecraft:flat_cache","argument":"smooth"})");
    const Pipeline pipeline(tree.pack(), 13, kOverworldCells);
    const Interpreter& interpreter = pipeline.interpreter();
    Interpreter::CornerCache cache(interpreter.cacheSize());
    const auto read = [&](std::string_view function, Point at, ReadContext context) {
        return interpreter.evaluate(pipeline.root(function), at, cache,
                                    stratum::density::FlatCacheWindow::none(), context);
    };

    for (const Point at : {Point{.x = 1, .y = 3, .z = 2}, Point{.x = -7, .y = -5, .z = 9},
                           Point{.x = 37, .y = -33, .z = 70}}) {
        CAPTURE(at.x, at.y, at.z);
        CHECK(bits(read("smooth", at, ReadContext::Detached)) == bits(pipeline.at("field", at)));
        CHECK(bits(read("smooth", at, ReadContext::Block)) == bits(pipeline.at("smooth", at)));
        // Off the lattice the two are different numbers, or this would test
        // nothing.
        CHECK(bits(read("smooth", at, ReadContext::Block)) !=
              bits(read("smooth", at, ReadContext::Detached)));
    }
    // At a lattice point there is nothing to blend: the two agree.
    const Point lattice{.x = 8, .y = 16, .z = -4};
    CHECK(bits(read("smooth", lattice, ReadContext::Block)) ==
          bits(read("smooth", lattice, ReadContext::Detached)));

    // The context rides into the scope a node opens for another point: a
    // flat_cache that relocates reads its corner detached too. On an
    // 8-wide lattice the 4x4 corner (36, 0, 68) is a cell midpoint, so the
    // blend there is not the argument.
    const Pipeline wide(tree.pack(), 13, CellGeometry{.width = 8, .height = 8});
    Interpreter::CornerCache wideCache(wide.interpreter().cacheSize());
    const stratum::density::FlatCacheWindow everywhere{
        .minX = -1000, .maxX = 1000, .minZ = -1000, .maxZ = 1000};
    const Point at{.x = 37, .y = -33, .z = 70};
    const Point corner{.x = 36, .y = 0, .z = 68};
    const auto relocated = [&](ReadContext context) {
        return wide.interpreter().evaluate(wide.root("flat_smooth"), at, wideCache, everywhere,
                                           context);
    };
    CHECK(bits(relocated(ReadContext::Detached)) == bits(wide.at("field", corner)));
    CHECK(bits(relocated(ReadContext::Block)) == bits(wide.at("smooth", corner)));
    CHECK(bits(relocated(ReadContext::Block)) != bits(relocated(ReadContext::Detached)));
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
    CHECK(overworld.interpreter().cells().value().width == 4);
    CHECK(end.interpreter().cells().value().height == 4);
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

TEST_CASE("a legacy source with nothing to seed is not refused", "[density][noise][legacy]") {
    const TempTree tree;
    tree.defineNoise("test", R"({"firstOctave":-4,"amplitudes":[1.0,1.0]})");
    tree.define("field", R"({"type":"minecraft:noise","noise":"test",
        "xz_scale":1.0,"y_scale":0.0})");
    tree.define("plain", R"({"type":"minecraft:add","argument1":1.5,"argument2":-0.25})");

    const Pack pack = tree.pack();
    const Graph graph = Graph::resolveAll(pack);

    // The refusal is about turning an IDENTIFIER into a Java LCG seed. With
    // no identifier asked for there is nothing to derive and nothing to
    // approximate, so this is not a softening of the rule — it is the rule
    // applied to what it is actually about. Every legacy dimension's
    // `final_density` is this case (SPEC §11).
    const std::vector<ResourceLocation> nothing;
    const NoiseRegistry empty =
        NoiseRegistry::create(pack, nothing, 7, stratum::density::RandomSource::Legacy);
    CHECK(empty.size() == 0U);
    CHECK(empty.source() == stratum::density::RandomSource::Legacy);
    CHECK(empty.worldSeed() == 7);

    // Asking for even one name brings the refusal straight back.
    CHECK_THROWS_WITH(NoiseRegistry::create(pack, graph.referencedNoises(), 7,
                                            stratum::density::RandomSource::Legacy),
                      ContainsSubstring("legacy_random_source"));
}

TEST_CASE("an unbuilt named noise is refused at the node that samples it, not at construction",
          "[density][interpreter][noise]") {
    const TempTree tree;
    tree.defineNoise("test", R"({"firstOctave":-4,"amplitudes":[1.0,1.0]})");
    tree.define("field", R"({"type":"minecraft:noise","noise":"test",
        "xz_scale":1.0,"y_scale":0.0})");
    tree.define("plain", R"({"type":"minecraft:add","argument1":1.5,"argument2":-0.25})");

    const Pack pack = tree.pack();
    const Graph graph = Graph::resolveAll(pack);
    const std::vector<ResourceLocation> nothing;
    const NoiseRegistry empty =
        NoiseRegistry::create(pack, nothing, 7, stratum::density::RandomSource::Legacy);

    // Constructing over the WHOLE graph used to throw here, because the graph
    // holds a node naming `test`. One graph carries every dimension a pack
    // defines, so that made one dimension's missing noise block every other
    // dimension's roots — which is what kept the nether's terrain, itself
    // naming nothing, unreachable.
    const Interpreter interpreter(graph, empty);

    // A root that never reaches the unbuilt noise evaluates.
    const stratum::density::NodeIndex plain = graph.rootOf(ResourceLocation::parse("plain"));
    interpreter.requireEvaluable(plain);
    CHECK(bits(interpreter.evaluate(plain, Point{.x = 0, .y = 0, .z = 0})) == bits(1.25));

    // And a root that does reach it is refused, by the noise's own name,
    // from both the up-front check and the evaluation. Nothing is silently
    // treated as zero (SPEC §8).
    const stratum::density::NodeIndex field = graph.rootOf(ResourceLocation::parse("field"));
    CHECK_THROWS_WITH(interpreter.requireEvaluable(field),
                      ContainsSubstring("test") && ContainsSubstring("not built"));
    CHECK_THROWS_WITH(interpreter.evaluate(field, Point{.x = 0, .y = 0, .z = 0}),
                      ContainsSubstring("test") && ContainsSubstring("not built"));
}

TEST_CASE("a noise written inline loads, and is refused because vanilla refuses it too",
          "[density][interpreter][noise]") {
    // Two separate questions, and the answers differ. Is the input legal?
    // Yes — mcdoc declares a `noise` field as an identifier OR a noise, so
    // the graph resolves it and the pack parses. Can it be sampled? No, and
    // not because this build is behind: a noise is seeded from the MD5 of
    // its identifier, this one has none, and the vanilla server does not
    // work around that either — it refuses to build a world whose router
    // reaches such a node, which is measured in
    // tests/conformance/vanilla_inline_noise_test.cpp (SPEC §11).
    const TempTree tree;
    tree.defineNoise("named", R"({"firstOctave":-4,"amplitudes":[1.0]})");
    tree.define("by_name", R"({"type":"minecraft:shift_a","argument":"named"})");
    tree.define("inlined", R"({"type":"minecraft:shift_a",
        "argument":{"firstOctave":-4,"amplitudes":[1.0]}})");

    const Pipeline pipeline(tree.pack());

    // The named one is unaffected: the two spellings share a node type, and
    // widening the field must not cost the spelling that already worked.
    CHECK_NOTHROW(pipeline.interpreter().requireEvaluable(pipeline.root("by_name")));

    // Written out twice rather than held in a variable: a Catch2 matcher
    // expression keeps references to its operands, so a named one outlives
    // the temporaries it is made of and reading it is undefined.
    CHECK_THROWS_WITH(pipeline.interpreter().requireEvaluable(pipeline.root("inlined")),
                      ContainsSubstring("minecraft:shift_a") && ContainsSubstring("inline") &&
                          ContainsSubstring("vanilla server") && ContainsSubstring("SPEC §11"));
    // And on the sample too, not only on the up-front check: a caller that
    // skipped requireEvaluable must not get a number here.
    CHECK_THROWS_WITH(pipeline.at("inlined"), ContainsSubstring("minecraft:shift_a") &&
                                                  ContainsSubstring("inline") &&
                                                  ContainsSubstring("SPEC §11"));

    // UnbuildableError, not a plain EvalError, from both entry points. The
    // type is the machine-readable half of the sentence above — it is what
    // lets `stratum validate` report a pack that works nowhere as an error
    // and a node this build has not got to yet as a warning — so a caller
    // that stopped distinguishing them would be a regression the message
    // text could not catch.
    CHECK_THROWS_AS(pipeline.interpreter().requireEvaluable(pipeline.root("inlined")),
                    stratum::density::UnbuildableError);
    CHECK_THROWS_AS(pipeline.at("inlined"), stratum::density::UnbuildableError);

    // Conservative about the column, because it will not evaluate the node
    // at all — a caller asking "may I cache this on (x, z)?" gets no.
    CHECK_FALSE(pipeline.interpreter().isColumnInvariant(pipeline.root("inlined")));
    // Whereas shift_a with a named noise samples with y pinned, so it is.
    CHECK(pipeline.interpreter().isColumnInvariant(pipeline.root("by_name")));
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
        CHECK_NOTHROW(plain.requireEvaluable(blendedRoot));
        CHECK_NOTHROW(plain.requireEvaluable(weirdRoot));
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
        CHECK_NOTHROW(running.requireEvaluable(weirdRoot));
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

        CHECK_NOTHROW(running.requireEvaluable(blendedRoot));
    }

    SECTION("an empty set of substitutions is the same as none") {
        Interpreter running(graph, noises);
        running.substitute(stratum::density::UnsettledSubstitutions{});
        CHECK(stratum::density::UnsettledSubstitutions{}.empty());
        CHECK_NOTHROW(running.requireEvaluable(blendedRoot));
    }
}

// ---------------------------------------------------------------------------
// weird_scaled_sampler's rarity ladders (SPEC §11). Measured off the vanilla
// server, one dimension per input value, and the thresholds pinned by probing
// each one at the value itself and at plus and minus 1e-7.
//
// These are known-answer vectors, not a restatement of the implementation: the
// numbers came from the measurement, and the reason they are worth asserting
// at every step is that minecraft.wiki gives both ladders but its two pages
// assign them to type_1 and type_2 in OPPOSITE order. Getting the names the
// wrong way round would still produce caves — the wrong ones, everywhere.
// ---------------------------------------------------------------------------

TEST_CASE("type_1's rarity ladder", "[density][interpreter][rarity]") {
    // Below the first threshold, then one bucket at a time.
    CHECK(bits(rarityValueMapper("type_1", -100.0)) == bits(0.75));
    CHECK(bits(rarityValueMapper("type_1", -0.5000001)) == bits(0.75));
    // A value exactly on a threshold takes the rarity ABOVE it: the comparison
    // is a strict `<`. Probed at 1e-7 either side of every boundary.
    CHECK(bits(rarityValueMapper("type_1", -0.5)) == bits(1.0));
    CHECK(bits(rarityValueMapper("type_1", -0.0000001)) == bits(1.0));
    CHECK(bits(rarityValueMapper("type_1", 0.0)) == bits(1.5));
    CHECK(bits(rarityValueMapper("type_1", 0.4999999)) == bits(1.5));
    CHECK(bits(rarityValueMapper("type_1", 0.5)) == bits(2.0));
    CHECK(bits(rarityValueMapper("type_1", 100.0)) == bits(2.0));
}

TEST_CASE("type_2's rarity ladder", "[density][interpreter][rarity]") {
    CHECK(bits(rarityValueMapper("type_2", -100.0)) == bits(0.5));
    CHECK(bits(rarityValueMapper("type_2", -0.7500001)) == bits(0.5));
    CHECK(bits(rarityValueMapper("type_2", -0.75)) == bits(0.75));
    CHECK(bits(rarityValueMapper("type_2", -0.5000001)) == bits(0.75));
    CHECK(bits(rarityValueMapper("type_2", -0.5)) == bits(1.0));
    CHECK(bits(rarityValueMapper("type_2", 0.4999999)) == bits(1.0));
    CHECK(bits(rarityValueMapper("type_2", 0.5)) == bits(2.0));
    CHECK(bits(rarityValueMapper("type_2", 0.7499999)) == bits(2.0));
    CHECK(bits(rarityValueMapper("type_2", 0.75)) == bits(3.0));
    CHECK(bits(rarityValueMapper("type_2", 100.0)) == bits(3.0));
}

TEST_CASE("the two ladders are not each other", "[density][interpreter][rarity]") {
    // The one assertion that a swapped pair of names would fail. type_2 reaches
    // 0.5 and 3.0, which type_1 never does; type_1 reaches 1.5, which type_2
    // never does. Nothing else in this file would notice the swap.
    CHECK(bits(rarityValueMapper("type_2", -1.0)) == bits(0.5));
    CHECK(bits(rarityValueMapper("type_1", -1.0)) == bits(0.75));
    CHECK(bits(rarityValueMapper("type_1", 0.25)) == bits(1.5));
    CHECK(bits(rarityValueMapper("type_2", 0.25)) == bits(1.0));
    CHECK(bits(rarityValueMapper("type_2", 1.0)) == bits(3.0));
    CHECK(bits(rarityValueMapper("type_1", 1.0)) == bits(2.0));
}

TEST_CASE("an unknown rarity mapper is refused by name", "[density][interpreter][rarity]") {
    // The graph validates the selector against the schema, so this is
    // unreachable through a pack. It is asserted because a default here would
    // reshape every cave in the world and nothing would say so.
    CHECK_THROWS_WITH(rarityValueMapper("type_3", 0.0),
                      ContainsSubstring("type_3") && ContainsSubstring("type_1"));
}

TEST_CASE("weird_scaled_sampler scales the position and the value, and takes the modulus",
          "[density][interpreter][rarity]") {
    const TempTree tree;
    tree.defineNoise("test", R"({"firstOctave":-3,"amplitudes":[1.0]})");

    // type_2 maps an input below -0.75 to a rarity of 0.5, so the node must
    // equal abs(0.5 * noise(x/0.5, y/0.5, z/0.5)).
    //
    // A rarity of one half is chosen so the claim can be asserted BIT-EXACTLY:
    // dividing by 0.5 is multiplying by 2, both exact in binary, so the
    // equivalent can be written as a density function the pack itself carries —
    // `minecraft:noise` with xz_scale and y_scale of 2. At a rarity of 1.5 the
    // same equivalence holds only to rounding, because x/1.5 and x*(1/1.5) are
    // not the same double.
    tree.define("weird_half", R"({"type":"minecraft:weird_scaled_sampler",
        "rarity_value_mapper":"type_2","noise":"test","input":-1.0})");
    tree.define("formula_half", R"({"type":"minecraft:abs","argument":{
        "type":"minecraft:mul","argument1":0.5,"argument2":{
            "type":"minecraft:noise","noise":"test","xz_scale":2.0,"y_scale":2.0}}})");

    // And a rarity of 1.5, checked to rounding rather than to the bit.
    tree.define("weird_onehalf", R"({"type":"minecraft:weird_scaled_sampler",
        "rarity_value_mapper":"type_1","noise":"test","input":0.0})");
    tree.define("formula_onehalf", R"({"type":"minecraft:abs","argument":{
        "type":"minecraft:mul","argument1":1.5,"argument2":{
            "type":"minecraft:noise","noise":"test",
            "xz_scale":0.6666666666666666,"y_scale":0.6666666666666666}}})");

    const Pipeline pipeline(tree.pack(), 0);

    for (const Point at :
         {Point{.x = 0, .y = 0, .z = 0}, Point{.x = 17, .y = 33, .z = -49},
          Point{.x = -7, .y = -64, .z = 5}, Point{.x = 300, .y = 200, .z = -300}}) {
        CAPTURE(at.x, at.y, at.z);
        CHECK(bits(pipeline.at("weird_half", at)) == bits(pipeline.at("formula_half", at)));
        CHECK(pipeline.at("weird_onehalf", at) ==
              Catch::Approx(pipeline.at("formula_onehalf", at)).epsilon(1e-12));
    }

    // Never negative, which is what makes it a cave function: the tunnels are
    // where it is near zero, and that is both sides of the noise's own zero
    // crossing rather than one.
    for (int x = 0; x < 24; ++x) {
        CAPTURE(x);
        CHECK(pipeline.at("weird_half", Point{.x = x, .y = 40, .z = x * 3}) >= 0.0);
        CHECK(pipeline.at("weird_onehalf", Point{.x = x, .y = 40, .z = x * 3}) >= 0.0);
    }
}

// ---------------------------------------------------------------------------
// find_top_surface (SPEC §11). minecraft.wiki gives the semantics — "scans
// through a column of an input density and returns the topmost y-level that
// is above 0. If no such position exists within the bounds, the lower_bound
// is returned" — and leaves four things open. All four were measured off the
// vanilla server with a density whose zero crossing is placed by hand, so the
// answer is analytic and the staircase is read straight off.
//
// The density below is +1 under `t` and -1 above it, so "topmost lattice point
// above zero" is the largest lattice point strictly below t.
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] std::string crossingAt(int t) {
    return R"({"type":"minecraft:y_clamped_gradient","from_y":)" + std::to_string(t - 1) +
           R"(,"to_y":)" + std::to_string(t + 1) + R"(,"from_value":1.0,"to_value":-1.0})";
}

[[nodiscard]] std::string findTopSurface(const std::string& density, const std::string& upper,
                                         int lower, int cell) {
    return R"({"type":"minecraft:find_top_surface","density":)" + density + R"(,"upper_bound":)" +
           upper + R"(,"lower_bound":)" + std::to_string(lower) + R"(,"cell_height":)" +
           std::to_string(cell) + "}";
}

} // namespace

TEST_CASE("find_top_surface scans a lattice of absolute multiples of cell_height",
          "[density][interpreter][surface]") {
    const TempTree tree;
    tree.define("c8_104", findTopSurface(crossingAt(104), "320.0", -64, 8));
    tree.define("c8_105", findTopSurface(crossingAt(105), "320.0", -64, 8));
    tree.define("c8_112", findTopSurface(crossingAt(112), "320.0", -64, 8));
    tree.define("c8_113", findTopSurface(crossingAt(113), "320.0", -64, 8));
    tree.define("c4_110", findTopSurface(crossingAt(110), "320.0", -64, 4));
    const Pipeline pipeline(tree.pack(), 0);

    // At exactly t the density is zero, and zero is not "above 0" — the test
    // is a strict `>`. So a crossing at 104 gives 96, not 104.
    CHECK(bits(pipeline.at("c8_104")) == bits(96.0));
    CHECK(bits(pipeline.at("c8_105")) == bits(104.0));
    CHECK(bits(pipeline.at("c8_112")) == bits(104.0));
    CHECK(bits(pipeline.at("c8_113")) == bits(112.0));
    // A different cell_height moves the lattice with it.
    CHECK(bits(pipeline.at("c4_110")) == bits(108.0));
}

TEST_CASE("find_top_surface's lattice follows neither bound", "[density][interpreter][surface]") {
    // The measurement that pins this: an upper_bound of 317 and a lower_bound
    // of -60 are both off a lattice of eight, and vanilla still answers in
    // multiples of eight. Anchoring the scan to either bound — the obvious
    // reading, and the one a loop written from the wiki would produce — gives
    // answers congruent to 5 and to 4 instead.
    const TempTree tree;
    tree.define("upper317", findTopSurface(crossingAt(114), "317.0", -64, 8));
    tree.define("lower60", findTopSurface(crossingAt(114), "320.0", -60, 8));
    const Pipeline pipeline(tree.pack(), 0);
    CHECK(bits(pipeline.at("upper317")) == bits(112.0));
    CHECK(bits(pipeline.at("lower60")) == bits(112.0));
}

TEST_CASE("find_top_surface floors upper_bound onto the lattice, inclusively",
          "[density][interpreter][surface]") {
    // A density positive everywhere in range, so the answer is entirely about
    // where the scan starts. Vanilla's own upper_bound is a `clamp` and so is
    // fractional in general, which is why this is worth pinning: 319.9 starts
    // at 312 and 320.0 starts at 320, so it floors rather than rounds.
    const TempTree tree;
    const std::string always = crossingAt(400);
    tree.define("u319", findTopSurface(always, "319.0", -64, 8));
    tree.define("u319_9", findTopSurface(always, "319.9", -64, 8));
    tree.define("u320", findTopSurface(always, "320.0", -64, 8));
    tree.define("u320_5", findTopSurface(always, "320.5", -64, 8));
    const Pipeline pipeline(tree.pack(), 0);
    CHECK(bits(pipeline.at("u319")) == bits(312.0));
    CHECK(bits(pipeline.at("u319_9")) == bits(312.0));
    CHECK(bits(pipeline.at("u320")) == bits(320.0));
    CHECK(bits(pipeline.at("u320_5")) == bits(320.0));
}

TEST_CASE("find_top_surface returns lower_bound when there is no surface",
          "[density][interpreter][surface]") {
    const TempTree tree;
    // Crossing far below the range, so nothing scanned is above zero.
    tree.define("none64", findTopSurface(crossingAt(-200), "320.0", -64, 8));
    tree.define("none60", findTopSurface(crossingAt(-200), "320.0", -60, 8));
    const Pipeline pipeline(tree.pack(), 0);
    CHECK(bits(pipeline.at("none64")) == bits(-64.0));
    CHECK(bits(pipeline.at("none60")) == bits(-60.0));
}

TEST_CASE("find_top_surface refuses a cell_height that would never finish",
          "[density][interpreter][surface]") {
    // Not reachable through vanilla's data, but a datapack can write it, and
    // the alternative to an error here is a hang with no explanation. Refused
    // at LOAD now, by the schema's own range (cell_height int @ 1..), where
    // it used to be refused on the first evaluation.
    const TempTree tree;
    tree.define("zero", findTopSurface(crossingAt(100), "320.0", -64, 0));
    CHECK_THROWS_WITH(Graph::resolveAll(tree.pack()), ContainsSubstring("minecraft:zero") &&
                                                          ContainsSubstring("cell_height 0") &&
                                                          ContainsSubstring("at least 1"));
}

// --- inputs that would reach undefined behaviour (SPEC §8) -----------------
//
// Each of these is schema-legal, or a function's legal value, and each used
// to carry the interpreter into a conversion or an int operation C++ leaves
// undefined (found under UBSan). Each is now refused by name — at load where
// the input is a constant, at evaluation where it is a function's value —
// and the case beside it shows the in-range neighbour still evaluates.

TEST_CASE("find_top_surface refuses a lower_bound or cell_height outside the schema's range",
          "[density][interpreter][surface][ub]") {
    // vanilla-mcdoc: lower_bound int @ -4064..4062, cell_height int @ 1..
    // A lower_bound of 3e9 was an out-of-range float-to-int conversion.
    const TempTree tree;
    tree.define("low", R"({"type":"minecraft:find_top_surface","density":0.0,"upper_bound":0.0,
                           "lower_bound":3000000000,"cell_height":8})");
    CHECK_THROWS_WITH(Graph::resolveAll(tree.pack()), ContainsSubstring("lower_bound 3000000000") &&
                                                          ContainsSubstring("[-4064, 4062]"));

    const TempTree tall;
    tall.define("tall", R"({"type":"minecraft:find_top_surface","density":0.0,"upper_bound":0.0,
                            "lower_bound":-64,"cell_height":3000000000})");
    CHECK_THROWS_WITH(Graph::resolveAll(tall.pack()), ContainsSubstring("cell_height 3000000000"));

    // Both ends of the schema's range load.
    const TempTree edges;
    edges.define("bottom", findTopSurface(crossingAt(100), "320.0", -4064, 8));
    edges.define("top", findTopSurface(crossingAt(100), "320.0", 4062, 8));
    const Pipeline pipeline(edges.pack(), 0);
    CHECK(bits(pipeline.at("bottom")) == bits(96.0));
    CHECK(bits(pipeline.at("top")) == bits(4062.0));
}

TEST_CASE("find_top_surface refuses an upper_bound whose floor is no 32-bit integer",
          "[density][interpreter][surface][ub]") {
    // upper_bound is a function, so this is refused where it is evaluated.
    // mul(1e6, 1e6) is two schema-legal constants; its floor, 1e12, used to
    // be converted to int32 regardless.
    const TempTree tree;
    tree.define("huge", findTopSurface(crossingAt(100), R"({"type":"minecraft:mul",
        "argument1":1000000.0,"argument2":1000000.0})",
                                       -64, 8));
    // Within one cell_height of INT32_MIN the lattice point below the bound
    // is no int32 either: floorDiv(-2147483647, 3) * 3 is -2147483649.
    tree.define("bottom", findTopSurface(crossingAt(100), "-2147483647.0", -64, 3));
    // The largest int32 is a fine place to start a scan from, if a slow one.
    tree.define("ceiling", findTopSurface(crossingAt(100), "2147483647.0", -64, 1 << 30));
    const Pipeline pipeline(tree.pack(), 0);
    CHECK_THROWS_WITH(pipeline.at("huge"), ContainsSubstring("upper_bound 1000000000000") &&
                                               ContainsSubstring("not a 32-bit integer"));
    CHECK_THROWS_WITH(pipeline.at("bottom"), ContainsSubstring("-2147483649"));
    CHECK(bits(pipeline.at("ceiling")) == bits(0.0));
}

TEST_CASE("interpolated refuses a cell whose corners are not all 32-bit integers",
          "[density][interpreter][cells][ub]") {
    // A cell's corners are its lower corner plus a cell's width or height,
    // and within one cell of either end of int32 that sum left the range.
    // A pack reaches the top of y through find_top_surface, whose scan
    // starts at the lattice point under its upper_bound: a cell_height as
    // large as the bound starts it at the bound itself.
    const auto scanFrom = [](std::int32_t top) {
        return findTopSurface(R"({"type":"minecraft:interpolated","argument":-1.0})",
                              std::to_string(top) + ".0", -64, top);
    };
    const TempTree tree;
    // Cells 8 high: 2147483639's cell runs from 2147483632 to 2147483640,
    // and 2147483640's would run to 2^31.
    tree.define("last", scanFrom(2147483639));
    tree.define("past", scanFrom(2147483640));
    const Pipeline tall(tree.pack(), 0, CellGeometry{.width = 4, .height = 8});
    CHECK(bits(tall.at("last")) == bits(-64.0));
    CHECK_THROWS_WITH(tall.at("past"), ContainsSubstring("'minecraft:interpolated'") &&
                                           ContainsSubstring("(0, 2147483640, 0)") &&
                                           ContainsSubstring("32-bit"));

    // Through evaluate() itself every axis reaches either end. A cell 12
    // wide does not divide 2^31, so the cell holding INT32_MIN starts below
    // it: -2147483641's lower corner is -2147483652.
    const TempTree flat;
    flat.define("flat", R"({"type":"minecraft:interpolated","argument":0.75})");
    const Pipeline wide(flat.pack(), 0, CellGeometry{.width = 12, .height = 8});
    CHECK(bits(wide.at("flat", Point{.x = -2147483640, .y = 0, .z = 2147483639})) == bits(0.75));
    CHECK_THROWS_WITH(wide.at("flat", Point{.x = -2147483641, .y = 0, .z = 0}),
                      ContainsSubstring("(-2147483652, 0, 0)"));
    CHECK_THROWS_WITH(wide.at("flat", Point{.x = 0, .y = 0, .z = 2147483640}),
                      ContainsSubstring("(0, 0, 2147483640)"));
    // 8 divides 2^31, so the lowest cell on y starts at INT32_MIN itself.
    CHECK(bits(wide.at("flat",
                       Point{.x = 0, .y = std::numeric_limits<std::int32_t>::min(), .z = 0})) ==
          bits(0.75));
}

TEST_CASE("clamp refuses a min above its max", "[density][interpreter][ub]") {
    // std::clamp's precondition; reversed bounds were undefined behaviour,
    // and the schema bounds each without ordering them.
    const TempTree tree;
    tree.define("reversed", R"({"type":"minecraft:clamp","input":0.5,"min":1.0,"max":0.0})");
    CHECK_THROWS_WITH(Graph::resolveAll(tree.pack()), ContainsSubstring("minecraft:reversed") &&
                                                          ContainsSubstring("min 1") &&
                                                          ContainsSubstring("max 0"));

    const TempTree equal;
    equal.define("point", R"({"type":"minecraft:clamp","input":0.5,"min":0.25,"max":0.25})");
    const Pipeline pipeline(equal.pack(), 0);
    CHECK(bits(pipeline.at("point")) == bits(0.25));
}

TEST_CASE("old_blended_noise refuses a scale or factor outside the wiki's range",
          "[density][interpreter][ub]") {
    // minecraft.wiki: each of the four "between 0.001 and 1000.0 (both
    // inclusive)". A factor of 0 made the blend's coordinate infinite, which
    // reached the Perlin lattice as NaN.
    const TempTree tree;
    tree.define("flat", R"({"type":"minecraft:old_blended_noise","xz_scale":0.25,"y_scale":0.125,
                            "xz_factor":0.0,"y_factor":160.0,"smear_scale_multiplier":8.0})");
    CHECK_THROWS_WITH(Graph::resolveAll(tree.pack()),
                      ContainsSubstring("xz_factor 0") && ContainsSubstring("[0.001, 1000]"));

    const TempTree steep;
    steep.define("steep", R"({"type":"minecraft:old_blended_noise","xz_scale":1e300,
        "y_scale":0.125,"xz_factor":80.0,"y_factor":160.0,"smear_scale_multiplier":8.0})");
    CHECK_THROWS_WITH(Graph::resolveAll(steep.pack()),
                      ContainsSubstring("xz_scale 1") && ContainsSubstring("e+300"));
}

TEST_CASE("a noise refuses a coordinate past its 32-bit lattice", "[density][interpreter][ub]") {
    const TempTree tree;
    // A top octave at frequency 1/8, so the lattice reaches about 1.7e10.
    tree.defineNoise("test", R"({"firstOctave":-3,"amplitudes":[1.0]})");
    // A scale no schema bounds, from mcdoc's plain `float`.
    tree.define("scaled", R"({"type":"minecraft:noise","noise":"test","xz_scale":1e10,
                              "y_scale":0.0})");
    // A shift is any function's value.
    tree.define("shifted", R"({"type":"minecraft:shifted_noise","noise":"test","xz_scale":1.0,
        "y_scale":0.0,"shift_x":{"type":"minecraft:mul","argument1":1000000.0,
        "argument2":1000000.0},"shift_y":0.0,"shift_z":0.0})");
    tree.define("plain", R"({"type":"minecraft:noise","noise":"test","xz_scale":1.0,
                             "y_scale":0.0})");
    const Pipeline pipeline(tree.pack(), 0);

    // x = 0 is fine whatever the scale: 0 * 1e10 is 0.
    CHECK_NOTHROW(pipeline.at("scaled"));
    CHECK_THROWS_WITH(pipeline.at("scaled", Point{.x = 2, .y = 0, .z = 0}),
                      ContainsSubstring("samples noise 'minecraft:test'") &&
                          ContainsSubstring("for block (2, 0, 0)") && ContainsSubstring("32-bit"));
    CHECK_THROWS_WITH(pipeline.at("shifted"), ContainsSubstring("1000000000000"));

    // The block coordinate alone gets there at 1/8 only past int32, so an
    // ordinary noise at an ordinary scale is never refused here.
    CHECK_NOTHROW(pipeline.at("plain", Point{.x = 2147483647, .y = 0, .z = -2147483647 - 1}));
}

TEST_CASE("a node of each type's exact shape is evaluated without reading past it",
          "[density][interpreter][ub]") {
    // The other half of the shape check. density_graph_test.cpp shows a node
    // of any shape but its type's is refused; this evaluates a node of exactly
    // that shape — one argument per Function field, each a constant, one
    // parameter per Number field, a noise, a spline and a selector where the
    // type has one — for every type. Were the interpreter to read one more
    // argument or parameter than the schema declares, this would read past a
    // vector, which the sanitize preset's libstdc++ assertions abort on. A
    // type this build refuses to evaluate is refused by EvalError, by name.
    const ResourceLocation noiseId = ResourceLocation::parse("stratum:shaped");
    const std::map<ResourceLocation, NoiseParameters> parameters{
        {noiseId, NoiseParameters{.firstOctave = -4, .amplitudes = {1.0, 0.5}}}};
    const std::vector<ResourceLocation> wanted{noiseId};
    const NoiseRegistry noises =
        NoiseRegistry::create(parameters, wanted, 42, stratum::density::RandomSource::Xoroshiro);

    std::size_t evaluated = 0;
    std::size_t refused = 0;
    for (int raw = 0; stratum::density::nodeTypeName(static_cast<NodeType>(raw)) != "unknown";
         ++raw) {
        const auto type = static_cast<NodeType>(raw);
        CAPTURE(stratum::density::nodeTypeName(type));

        Graph::Assembler assembler;
        stratum::density::Node constant;
        constant.type = NodeType::Constant;
        constant.parameters = {0.25};
        static_cast<void>(assembler.addNode(constant));

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
                    node.noise = noiseId;
                    break;
                case stratum::density::FieldKind::Spline:
                    node.spline = 0;
                    break;
                case stratum::density::FieldKind::Selector:
                    node.selector = std::string(field.selectorValues.front());
                    break;
            }
        }
        const bool hasSpline = node.spline.has_value();
        const stratum::density::NodeIndex root = assembler.addNode(std::move(node));
        if (hasSpline) {
            stratum::density::SplineDefinition spline;
            spline.coordinate = 0;
            spline.points.push_back(stratum::density::SplinePoint{
                .location = 0.0, .derivative = 0.5, .value = 1.0, .nested = std::nullopt});
            static_cast<void>(assembler.addSpline(std::move(spline)));
        }
        assembler.addRoot(ResourceLocation::parse("stratum:root"), root);
        const Graph graph = assembler.release();

        const Interpreter interpreter(graph, noises, CellGeometry{.width = 4, .height = 8});
        try {
            const double value = interpreter.evaluate(root, Point{.x = 1, .y = 2, .z = 3});
            CHECK(!std::isnan(value));
            ++evaluated;
        } catch (const EvalError& error) {
            CHECK_THAT(error.what(),
                       ContainsSubstring(std::string(stratum::density::nodeTypeName(type))));
            ++refused;
        }
    }
    CHECK(evaluated + refused == 34U);
    CHECK(evaluated > refused);
}

TEST_CASE("a graph at kMaxDepth is evaluated, from a pack and from an assembler",
          "[density][interpreter][ub]") {
    // The other side of the bound (density_graph_test.cpp refuses one level
    // past it). At kMaxDepth the interpreter's recursion — through arguments
    // and through splines, and requireEvaluable's beside it — fits the stack
    // in every build, the sanitize preset's included.
    using stratum::density::kMaxDepth;
    REQUIRE(kMaxDepth == 256U);

    // 255 `abs` around -0.25, written in a pack: as deep as a graph may be.
    // Across two files, since one holds no more than data::kMaxJsonDepth:
    // `deep_a` is 127 of them around the constant, and `deep_b` 128 around
    // it. `deep_a` sorts first, so it is resolved first and `deep_b` meets it
    // memoised: resolved from `deep_b` down, the reference would be a level
    // of the resolver's own descent too, 257 in all, and refused.
    const auto nested = [](std::size_t levels, const std::string& innermost) {
        std::string text;
        for (std::size_t i = 0; i < levels; ++i) {
            text += R"({"type":"minecraft:abs","argument":)";
        }
        return text + innermost + std::string(levels, '}');
    };
    const TempTree tree;
    tree.define("deep_a", nested(127, "-0.25"));
    tree.define("deep_b", nested(128, R"("minecraft:deep_a")"));
    const Pipeline pipeline(tree.pack());
    CHECK_NOTHROW(pipeline.interpreter().requireEvaluable(pipeline.root("deep_b")));
    CHECK(bits(pipeline.at("deep_b")) == bits(0.25));

    // A constant, 254 splines each nesting the one before, and the node
    // holding them: 256 deep, assembled as freeze::read assembles a blob's.
    Graph::Assembler assembler;
    stratum::density::Node constant;
    constant.type = NodeType::Constant;
    constant.parameters = {0.25};
    static_cast<void>(assembler.addNode(constant));
    stratum::density::Node holder;
    holder.type = NodeType::Spline;
    holder.spline = static_cast<stratum::density::SplineIndex>(kMaxDepth - 3);
    const stratum::density::NodeIndex root = assembler.addNode(holder);
    for (std::size_t k = 0; k + 2 < kMaxDepth; ++k) {
        stratum::density::SplineDefinition spline;
        spline.coordinate = 0;
        spline.points.push_back(stratum::density::SplinePoint{
            .location = 0.0,
            .derivative = 0.0,
            .value = k == 0 ? std::optional<double>{1.0} : std::nullopt,
            .nested = k == 0 ? std::nullopt
                             : std::optional<stratum::density::SplineIndex>{
                                   static_cast<stratum::density::SplineIndex>(k - 1)}});
        static_cast<void>(assembler.addSpline(std::move(spline)));
    }
    const Graph graph = assembler.release();
    const NoiseRegistry noises = NoiseRegistry::create(
        std::map<ResourceLocation, NoiseParameters>{}, std::vector<ResourceLocation>{}, 0,
        stratum::density::RandomSource::Xoroshiro);
    const Interpreter interpreter(graph, noises);
    CHECK_NOTHROW(interpreter.requireEvaluable(root));
    // Every spline reads its one knot off the end, flat: 1.0 all the way up.
    CHECK(bits(interpreter.evaluate(root, Point{.x = 0, .y = 0, .z = 0})) == bits(1.0));
}

// ---------------------------------------------------------------------------
// The scope frames a CornerCache carries (Interpreter::ScopeArena). Every
// evaluation through one cache leases its per-point memo from frames that
// earlier evaluations wrote, and is told apart from them by an epoch rather
// than by zeroing. These are the claims that makes: nothing an earlier
// evaluation wrote is ever read by a later one — across the counter's wrap,
// which no chunk reaches and a test has to force, and across a refusal.
// ---------------------------------------------------------------------------

namespace {

/// A pack whose `mix` opens scopes at three levels: an interpolated cell's
/// corners, a flat_cache relocation inside each, and a find_top_surface
/// scan. `branch` takes `field` only at y >= 1 and `column` below, so the
/// nodes it reaches differ from one point to the next — which is what lets
/// a memo slot keep a stale stamp at all, since a slot every scope rewrites
/// can never be mistaken for anything.
void defineScratchPack(const TempTree& tree) {
    tree.defineNoise("test", R"({"firstOctave":-4,"amplitudes":[1.0]})");
    tree.define("field", R"({"type":"minecraft:noise","noise":"test",
        "xz_scale":1.0,"y_scale":1.0})");
    tree.define("column", R"({"type":"minecraft:flat_cache","argument":"field"})");
    tree.define("smooth", R"({"type":"minecraft:interpolated","argument":{
        "type":"minecraft:add","argument1":"field","argument2":"column"}})");
    tree.define("branch", R"({"type":"minecraft:range_choice",
        "input":{"type":"minecraft:y_clamped_gradient","from_y":0,"to_y":1,
            "from_value":0.0,"to_value":1.0},
        "min_inclusive":0.5,"max_exclusive":2.0,
        "when_in_range":{"type":"minecraft:mul","argument1":"field","argument2":3.0},
        "when_out_of_range":{"type":"minecraft:mul","argument1":"column","argument2":-2.0}})");
    tree.define("surface", R"({"type":"minecraft:find_top_surface",
        "density":{"type":"minecraft:add",
            "argument1":{"type":"minecraft:y_clamped_gradient","from_y":-24,"to_y":24,
                "from_value":1.0,"to_value":-1.0},
            "argument2":{"type":"minecraft:mul","argument1":"field","argument2":4.0}},
        "upper_bound":24.0,"lower_bound":-24,"cell_height":8})");
    tree.define("mix", R"({"type":"minecraft:add",
        "argument1":{"type":"minecraft:add","argument1":"smooth","argument2":"branch"},
        "argument2":{"type":"minecraft:mul","argument1":"surface","argument2":0.01}})");
}

/// The @p i-th point of the sweeps below: x and z wander over a few dozen
/// cells, and y is at or above 1 (`branch` reads `field`) when @p high, at
/// or below 0 (`column`) otherwise.
[[nodiscard]] Point scratchPoint(std::uint32_t i, bool high) {
    const auto x = static_cast<std::int32_t>((i * 37U) % 211U) - 105;
    const auto z = static_cast<std::int32_t>((i * 53U) % 199U) - 99;
    const auto lift = static_cast<std::int32_t>((i * 7U) % 40U);
    return Point{.x = x, .y = high ? 1 + lift : -lift, .z = z};
}

[[nodiscard]] std::string spell(const Point& at) {
    return "(" + std::to_string(at.x) + ", " + std::to_string(at.y) + ", " + std::to_string(at.z) +
           ")";
}

} // namespace

TEST_CASE("one corner cache reused for 10^5 evaluations, through the epoch's wrap, changes no "
          "value",
          "[density][interpreter][scratch]") {
    const TempTree tree;
    defineScratchPack(tree);
    const Pipeline pipeline(tree.pack(), 2026, kOverworldCells);
    const Interpreter& interpreter = pipeline.interpreter();
    const stratum::density::NodeIndex root = pipeline.root("mix");

    Interpreter::CornerCache reused(interpreter.cacheSize());
    std::uint32_t evaluations = 0;
    std::uint32_t differing = 0;
    std::string firstDiffering = "none";
    const auto evaluate = [&](bool high) {
        ++evaluations;
        const Point at = scratchPoint(evaluations, high);
        // Fresh scopes two ways: through a cache of its own, and with no
        // cache at all.
        Interpreter::CornerCache fresh(interpreter.cacheSize());
        const std::uint64_t own = bits(interpreter.evaluate(root, at, fresh));
        const std::uint64_t none = bits(interpreter.evaluate(root, at));
        const std::uint64_t shared = bits(interpreter.evaluate(root, at, reused));
        if (shared != own || shared != none) {
            if (differing == 0U) {
                firstDiffering = spell(at) + " at evaluation " + std::to_string(evaluations);
            }
            ++differing;
        }
    };

    // The schedule is counted in evaluations, which are epochs at the
    // outermost level: this cache opens exactly one scope there per call.
    //
    // 1. 64 evaluations at y >= 1, then the rest of 2 000 below it: the
    //    nodes only `field`'s branch reaches are left stamped 64, the last
    //    epoch that wrote them.
    constexpr std::uint32_t kHigh = 64;
    for (std::uint32_t i = 0; i < 2000; ++i) {
        evaluate(i < kHigh);
    }
    // 2. Every counter to 4 096 short of the wrap, and up to it below y = 1,
    //    so the outermost frame's last epoch before the wrap is 2^32 - 1.
    constexpr std::uint32_t kBeforeWrap = 4096;
    reused.advanceScopeEpochs(std::numeric_limits<std::uint32_t>::max() - kBeforeWrap);
    for (std::uint32_t i = 0; i < kBeforeWrap; ++i) {
        evaluate(false);
    }
    // The inner levels open several scopes per evaluation, so they are past
    // it already.
    REQUIRE(reused.scopeEpochWraps() >= 2U);
    // 3. Past the wrap the outermost epochs run 1, 2, ... again: 63 of them
    //    below y = 1, which leave `field`'s branch alone, and then epoch 64
    //    reads it at a new point. A wrap that kept the old stamps would hand
    //    that point the value step 1's 64th point stored there.
    for (std::uint32_t i = 1; i <= kHigh; ++i) {
        evaluate(i == kHigh);
    }
    // 4. The rest of 10^5, mixed.
    while (evaluations < 100000U) {
        evaluate(evaluations % 3U == 0U);
    }

    CAPTURE(firstDiffering);
    CHECK(differing == 0U);
    CHECK(evaluations == 100000U);
    // Every level that had a frame when the counters were moved wrapped:
    // the outermost; the corners and scan steps; the relocations inside the
    // corners.
    CHECK(reused.scopeEpochWraps() >= 3U);
}

TEST_CASE("a refusal leaves a corner cache's scope frames as good as new",
          "[density][interpreter][scratch]") {
    // A refusal thrown inside a find_top_surface step — an `interpolated`
    // whose cell there leaves int32 — unwinds through the step's scope and
    // the evaluation's. Each gives its frame back on the way, so the cache
    // that saw it evaluates the next point exactly as a fresh one does.
    const TempTree tree;
    defineScratchPack(tree);
    tree.define("past", findTopSurface(R"({"type":"minecraft:interpolated","argument":-1.0})",
                                       "2147483640.0", -64, 2147483640));
    const Pipeline pipeline(tree.pack(), 7, kOverworldCells);
    const Interpreter& interpreter = pipeline.interpreter();
    const stratum::density::NodeIndex mix = pipeline.root("mix");
    const stratum::density::NodeIndex past = pipeline.root("past");

    Interpreter::CornerCache reused(interpreter.cacheSize());
    for (std::uint32_t i = 1; i <= 200; ++i) {
        const Point at = scratchPoint(i, i % 2U == 0U);
        CAPTURE(spell(at));
        CHECK_THROWS_WITH(interpreter.evaluate(past, at, reused),
                          ContainsSubstring("'minecraft:interpolated'") &&
                              ContainsSubstring("32-bit"));
        CHECK(bits(interpreter.evaluate(mix, at, reused)) == bits(interpreter.evaluate(mix, at)));
    }
}

TEST_CASE("every form of evaluate refuses a root that names no node",
          "[density][interpreter][scratch]") {
    // The two forms with a cache used to index the scope's memo with the
    // root before anything had checked it.
    const TempTree tree;
    defineScratchPack(tree);
    const Pipeline pipeline(tree.pack(), 7, kOverworldCells);
    const Interpreter& interpreter = pipeline.interpreter();
    Interpreter::CornerCache cache(interpreter.cacheSize());
    const auto missing = static_cast<stratum::density::NodeIndex>(interpreter.cacheSize());
    const Point at{.x = 1, .y = 2, .z = 3};
    CHECK_THROWS_WITH(interpreter.evaluate(missing, at), ContainsSubstring("out of range"));
    CHECK_THROWS_WITH(interpreter.evaluate(missing, at, cache), ContainsSubstring("out of range"));
    CHECK_THROWS_WITH(
        interpreter.evaluate(missing, at, cache, stratum::density::FlatCacheWindow::none()),
        ContainsSubstring("out of range"));
}
