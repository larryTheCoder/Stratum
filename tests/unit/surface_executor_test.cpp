// Stratum — running a dimension's surface rules.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#include "support/temp_path.hpp"

#include <stratum/data/pack.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/executor.hpp>
#include <stratum/surface/rule_graph.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using stratum::surface::Context;
using stratum::surface::ExecutionError;
using stratum::surface::Executor;
using stratum::surface::RuleGraph;

namespace {

constexpr std::int64_t kSeed = 42;

[[nodiscard]] stratum::settings::NoiseGeometry overworldGeometry() {
    return stratum::settings::NoiseGeometry{
        .minY = -64, .height = 384, .sizeHorizontal = 1, .sizeVertical = 2};
}

[[nodiscard]] RuleGraph resolve(const nlohmann::json& json) {
    return RuleGraph::resolve(json, stratum::data::ResourceLocation::parse("stratum:test"));
}

[[nodiscard]] nlohmann::json block(const std::string& name) {
    return nlohmann::json{{"type", "minecraft:block"}, {"result_state", {{"Name", name}}}};
}

/// Spelled out rather than using a designated initialiser: the project set
/// builds with -Werror=missing-field-initializers, and a Context grows fields
/// as conditions are settled, so every call site would have to grow with it.
[[nodiscard]] Context at(std::int32_t x, std::int32_t y, std::int32_t z,
                         std::int32_t preliminarySurface = 0) {
    Context context;
    context.x = x;
    context.y = y;
    context.z = z;
    context.preliminarySurface = preliminarySurface;
    return context;
}

[[nodiscard]] nlohmann::json gradient(const std::string& randomName, int trueAt, int falseAt) {
    return nlohmann::json{{"type", "minecraft:vertical_gradient"},
                          {"random_name", randomName},
                          {"true_at_and_below", {{"absolute", trueAt}}},
                          {"false_at_and_above", {{"absolute", falseAt}}}};
}

/// A worldgen tree holding just `clay_bands_offset`, written on the fly —
/// `bandlands`' one registered noise (spec/bandlands-spec.md Q4.3).
class ClayBandsTree {
public:
    ClayBandsTree() : path_(std::filesystem::temp_directory_path() / uniqueName()) {
        const std::filesystem::path noiseDir = path_ / "noise";
        std::filesystem::create_directories(noiseDir);
        std::ofstream out(noiseDir / "clay_bands_offset.json");
        out << R"({"firstOctave": -8, "amplitudes": [1.0]})";
    }

    ClayBandsTree(const ClayBandsTree&) = delete;
    ClayBandsTree& operator=(const ClayBandsTree&) = delete;

    ~ClayBandsTree() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] stratum::data::Pack pack() const {
        return stratum::data::Pack::openWorldgenTree(path_);
    }

private:
    [[nodiscard]] static std::string uniqueName() {
        return stratum::test::tempName("stratum-bandlands-test");
    }

    std::filesystem::path path_;
};

/// A worldgen tree holding the two noises every surface DEPTH reads —
/// `minecraft:surface` and `minecraft:surface_secondary`. The parameters are
/// this test's own, not vanilla's: a unit case pins the SHAPE of the depth's
/// use, and the numbers vanilla's own parameters produce are pinned against
/// the server in tests/conformance instead.
class SurfaceNoiseTree {
public:
    /// @p amplitudes defaults to vanilla's own `minecraft:surface`. A data
    /// pack may write anything here, and one that writes larger amplitudes is
    /// the only way a surface depth reaches a negative INTEGER at all — see
    /// the truncation case below, which passes its own.
    explicit SurfaceNoiseTree(const char* amplitudes = "[1.0, 1.0, 1.0]")
        : path_(std::filesystem::temp_directory_path() / uniqueName()) {
        const std::filesystem::path noiseDir = path_ / "noise";
        std::filesystem::create_directories(noiseDir);
        std::ofstream surface(noiseDir / "surface.json");
        surface << R"({"firstOctave": -6, "amplitudes": )" << amplitudes << "}";
        std::ofstream secondary(noiseDir / "surface_secondary.json");
        secondary << R"({"firstOctave": -6, "amplitudes": [1.0, 1.0, 0.0, 1.0]})";
    }

    SurfaceNoiseTree(const SurfaceNoiseTree&) = delete;
    SurfaceNoiseTree& operator=(const SurfaceNoiseTree&) = delete;
    SurfaceNoiseTree(SurfaceNoiseTree&&) = delete;
    SurfaceNoiseTree& operator=(SurfaceNoiseTree&&) = delete;

    ~SurfaceNoiseTree() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] stratum::data::Pack pack() const {
        return stratum::data::Pack::openWorldgenTree(path_);
    }

private:
    [[nodiscard]] static std::string uniqueName() {
        return stratum::test::tempName("stratum-surface-noise-test");
    }

    std::filesystem::path path_;
};

[[nodiscard]] stratum::density::NoiseRegistry surfaceNoises(const SurfaceNoiseTree& tree,
                                                            std::int64_t seed) {
    const std::vector<stratum::data::ResourceLocation> wanted{
        stratum::data::ResourceLocation::parse("minecraft:surface"),
        stratum::data::ResourceLocation::parse("minecraft:surface_secondary")};
    return stratum::density::NoiseRegistry::create(tree.pack(), wanted, seed,
                                                   stratum::density::RandomSource::Xoroshiro);
}

[[nodiscard]] stratum::density::NoiseRegistry clayBandsOffsetNoise(const ClayBandsTree& tree,
                                                                   std::int64_t seed) {
    const std::vector<stratum::data::ResourceLocation> wanted{
        stratum::data::ResourceLocation::parse("minecraft:clay_bands_offset")};
    return stratum::density::NoiseRegistry::create(tree.pack(), wanted, seed,
                                                   stratum::density::RandomSource::Xoroshiro);
}

/// The block one of the golden 192-entry tables names at @p index, from a
/// single-letter code: t=terracotta, o=orange, y=yellow, b=brown, r=red,
/// w=white, l=light_gray (all `_terracotta`). Read directly off the real
/// server via tools/analysis/bandlands-probe.sh and bandlands-dump.cpp.
[[nodiscard]] stratum::data::ResourceLocation clayBlockOf(char code) {
    switch (code) {
        case 't':
            return stratum::data::ResourceLocation::parse("minecraft:terracotta");
        case 'o':
            return stratum::data::ResourceLocation::parse("minecraft:orange_terracotta");
        case 'y':
            return stratum::data::ResourceLocation::parse("minecraft:yellow_terracotta");
        case 'b':
            return stratum::data::ResourceLocation::parse("minecraft:brown_terracotta");
        case 'r':
            return stratum::data::ResourceLocation::parse("minecraft:red_terracotta");
        case 'w':
            return stratum::data::ResourceLocation::parse("minecraft:white_terracotta");
        case 'l':
            return stratum::data::ResourceLocation::parse("minecraft:light_gray_terracotta");
        default:
            throw std::logic_error("bad clay code");
    }
}

[[nodiscard]] RuleGraph bandlandsGraph() {
    return resolve(nlohmann::json{{"type", "minecraft:bandlands"}});
}

} // namespace

// There is no longer a real example of "a tree with an unrunnable
// construct": `bandlands` was the last one (spec/bandlands-spec.md), and
// every rule type and condition type this schema defines now compiles.
// `Executor::compile`'s whole-tree refusal (any one unrunnable construct
// refuses everything, not just its own branch) is still real code — it
// exists for whatever a future data pack format adds that this build does
// not understand yet — but nothing in today's 1.21.11 schema can exercise it
// with real JSON any more, since RuleGraph::resolve() itself already rejects
// an unknown "type" string before compile() is ever reached.

TEST_CASE("a runnable tree places what its rules say", "[surface]") {
    const auto geometry = overworldGeometry();
    const RuleGraph graph =
        resolve(nlohmann::json{{"type", "minecraft:condition"},
                               {"if_true", gradient("minecraft:deepslate", -8, 8)},
                               {"then_run", block("minecraft:deepslate")}});
    const Executor executor = Executor::compile(graph, kSeed, geometry);

    // Below the lower anchor the gradient is certain, above the upper one it
    // is impossible — no draw is consulted at either end.
    CHECK(executor.apply(at(0, -8, 0)) != nullptr);
    CHECK(executor.apply(at(0, -64, 0)) != nullptr);
    CHECK(executor.apply(at(0, 8, 0)) == nullptr);
    CHECK(executor.apply(at(0, 100, 0)) == nullptr);

    const auto* placed = executor.apply(at(0, -8, 0));
    REQUIRE(placed != nullptr);
    CHECK(placed->name == stratum::data::ResourceLocation::parse("minecraft:deepslate"));
}

TEST_CASE("a sequence stops at the first rule that places something", "[surface]") {
    const auto geometry = overworldGeometry();
    const RuleGraph graph = resolve(
        nlohmann::json{{"type", "minecraft:sequence"},
                       {"sequence", {block("minecraft:granite"), block("minecraft:andesite")}}});
    const Executor executor = Executor::compile(graph, kSeed, geometry);

    const auto* placed = executor.apply(at(0, 0, 0));
    REQUIRE(placed != nullptr);
    CHECK(placed->name == stratum::data::ResourceLocation::parse("minecraft:granite"));
}

TEST_CASE("a condition that does not hold places nothing at all", "[surface]") {
    const auto geometry = overworldGeometry();
    // `not` over a gradient that is certain low down, so the whole thing is
    // false low down and true high up.
    const RuleGraph graph = resolve(nlohmann::json{
        {"type", "minecraft:condition"},
        {"if_true",
         {{"type", "minecraft:not"}, {"invert", gradient("minecraft:deepslate", -8, 8)}}},
        {"then_run", block("minecraft:stone")}});
    const Executor executor = Executor::compile(graph, kSeed, geometry);

    CHECK(executor.apply(at(0, -32, 0)) == nullptr);
    CHECK(executor.apply(at(0, 32, 0)) != nullptr);
}

TEST_CASE("above_preliminary_surface opens below the preliminary surface, not at it", "[surface]") {
    const auto geometry = overworldGeometry();
    const SurfaceNoiseTree tree;
    const auto noises = surfaceNoises(tree, kSeed);
    const RuleGraph graph =
        resolve(nlohmann::json{{"type", "minecraft:condition"},
                               {"if_true", {{"type", "minecraft:above_preliminary_surface"}}},
                               {"then_run", block("minecraft:stone")}});
    const Executor executor = Executor::compile(graph, kSeed, geometry, &noises);

    // MEASURED (SPEC §11, tools/analysis/aps-boundary-probe.sh): the boundary
    // is `preliminarySurface + surfaceDepth - 8`, so the condition reaches
    // `8 - surfaceDepth` blocks BELOW the level it is named after. The depth
    // is whatever this tree's own noises give — the point of the case is the
    // SHAPE, and the numbers are pinned against the server in
    // tests/conformance/vanilla_above_preliminary_surface_test.cpp.
    for (const std::int32_t level : {-64, -7, 0, 1, 64, 200}) {
        for (const auto& [x, z] : {std::pair{0, 0}, std::pair{37, -91}, std::pair{-512, 4096}}) {
            const std::int32_t boundary = level + executor.surfaceDepth(x, z) - 8;
            INFO("level " << level << " at (" << x << ", " << z << "), boundary " << boundary);
            CHECK(executor.apply(at(x, boundary, z, level)) != nullptr);
            CHECK(executor.apply(at(x, boundary - 1, z, level)) == nullptr);
            CHECK(executor.apply(at(x, boundary + 40, z, level)) != nullptr);
            CHECK(executor.apply(at(x, boundary - 40, z, level)) == nullptr);
        }
    }

    // The boundary genuinely moves with the column rather than sitting at a
    // fixed offset — otherwise the case above would pass on a constant.
    std::set<std::int32_t> depths;
    for (std::int32_t x = 0; x < 64; ++x) {
        depths.insert(executor.surfaceDepth(x, 0));
    }
    CHECK(depths.size() > 1);

    // `>=` against `>` — the strictness this project called unmeasured for
    // two milestones. It was never the open question: the condition is true
    // at the boundary and false one block under it on every column measured,
    // so `y >= C` and `y > C - 1` are the same predicate, and the pair of
    // CHECKs at `boundary` and `boundary - 1` above is the whole of it. What
    // was unknown was C, and C is not the preliminary surface.
}

TEST_CASE("the surface depth is its raw value truncated toward zero, never floored or clamped",
          "[surface]") {
    // Three separate readings of one cast, and this is where they are kept
    // apart. `surfaceDepthRaw` is the value before it; `surfaceDepth` is
    // `(int)` of that, which TRUNCATES — so a raw of -0.4 is 0, where a floor
    // would give -1 and a bottom clamp would give 0 as well.
    //
    // Which is exactly why `depth` and `max(0, depth)` cannot be separated by
    // a raw in (-1, 0): both are 0 there. Only a raw at or below -1 tells
    // them apart. Vanilla's own `minecraft:surface` does reach that, about
    // one column in 87.7 million, and a clamp AT 0 is now REFUTED against
    // the server at 49 such columns across three seeds — see
    // tests/conformance/vanilla_above_preliminary_surface_test.cpp. Only at
    // 0: all 49 are at depth exactly -1, so a clamp at -1 or lower predicts
    // the same thing as none and is untouched by that reading.
    //
    // The 4x amplitudes below are a deliberately generous choice so that both
    // halves of the split fire densely in a unit test, NOT a threshold: 1x
    // reaches -1.048 somewhere, it is simply far too rare to write a test
    // around.
    const SurfaceNoiseTree tree{"[4.0, 4.0, 4.0]"};
    const auto noises = surfaceNoises(tree, kSeed);
    const RuleGraph graph =
        resolve(nlohmann::json{{"type", "minecraft:condition"},
                               {"if_true", {{"type", "minecraft:above_preliminary_surface"}}},
                               {"then_run", block("minecraft:stone")}});
    const Executor executor = Executor::compile(graph, kSeed, overworldGeometry(), &noises);

    int sawAboveMinusOne = 0;
    int sawBelowMinusOne = 0;
    for (std::int32_t x = -512; x < 512; x += 7) {
        for (std::int32_t z = -64; z < 64; z += 11) {
            const double raw = executor.surfaceDepthRaw(x, z);
            const std::int32_t depth = executor.surfaceDepth(x, z);
            INFO("(" << x << ", " << z << ") raw " << raw);
            CHECK(depth == static_cast<std::int32_t>(raw));
            if (raw >= 0.0) {
                continue;
            }
            if (raw > -1.0) {
                // Truncation, not a floor: a negative raw above -1 is 0, and
                // so is indistinguishable from a clamped depth.
                ++sawAboveMinusOne;
                CHECK(depth == 0);
                CHECK(static_cast<std::int32_t>(std::floor(raw)) == -1);
            } else {
                // At or below -1 the clamp WOULD show: this build returns the
                // negative value, and `max(0, depth)` would not.
                ++sawBelowMinusOne;
                CHECK(depth < 0);
            }
        }
    }
    // Both halves of the split are actually exercised — otherwise the case
    // above would assert nothing.
    CHECK(sawAboveMinusOne > 0);
    CHECK(sawBelowMinusOne > 0);
}

TEST_CASE("a tree naming above_preliminary_surface is refused without the surface noises",
          "[surface]") {
    // The condition reads a surface depth, so it needs `minecraft:surface`
    // and `minecraft:surface_secondary` even though nothing in the tree
    // mentions a depth. Before the boundary was measured this compiled, and
    // then threw at the first block.
    const RuleGraph graph =
        resolve(nlohmann::json{{"type", "minecraft:condition"},
                               {"if_true", {{"type", "minecraft:above_preliminary_surface"}}},
                               {"then_run", block("minecraft:stone")}});
    CHECK_THROWS_MATCHES(Executor::compile(graph, kSeed, overworldGeometry()), ExecutionError,
                         Catch::Matchers::MessageMatches(ContainsSubstring("noise")));
}

TEST_CASE("stone_depth counts from a run's own edge, not from the world's", "[surface]") {
    const auto geometry = overworldGeometry();
    const auto rule = [](const char* type, int offset) {
        return nlohmann::json{{"type", "minecraft:condition"},
                              {"if_true", nlohmann::json{{"type", "minecraft:stone_depth"},
                                                         {"offset", offset},
                                                         {"add_surface_depth", false},
                                                         {"secondary_depth_range", 0},
                                                         {"surface_type", type}}},
                              {"then_run", block("minecraft:stone")}};
    };

    // offset 0 paints one block, offset 2 paints three: the comparison is
    // depth <= offset on a 0-BASED depth, where the stored counter is 1 at a
    // run's top. A strict test, or a 1-based depth, moves every surface layer.
    const RuleGraph topGraph = resolve(rule("floor", 0));
    const Executor top = Executor::compile(topGraph, kSeed, geometry);
    Context inRun = at(0, 100, 0);
    inRun.stoneDepthAbove = 1;
    CHECK(top.apply(inRun) != nullptr);
    inRun.stoneDepthAbove = 2;
    CHECK(top.apply(inRun) == nullptr);

    const RuleGraph threeGraph = resolve(rule("floor", 2));
    const Executor three = Executor::compile(threeGraph, kSeed, geometry);
    for (std::int32_t depth = 1; depth <= 3; ++depth) {
        Context ctx = at(0, 100, 0);
        ctx.stoneDepthAbove = depth;
        CHECK(three.apply(ctx) != nullptr);
    }
    Context past = at(0, 100, 0);
    past.stoneDepthAbove = 4;
    CHECK(three.apply(past) == nullptr);

    // "ceiling" reads the other counter, so a block deep under a run's top can
    // still be at its bottom.
    const RuleGraph bottomGraph = resolve(rule("ceiling", 0));
    const Executor bottom = Executor::compile(bottomGraph, kSeed, geometry);
    Context deep = at(0, 100, 0);
    deep.stoneDepthAbove = 40;
    deep.stoneDepthBelow = 1;
    CHECK(bottom.apply(deep) != nullptr);
}

TEST_CASE("stone_depth is false off the top of any run, not depth -1", "[surface]") {
    // A run counter of 0 means "reset by air" (Context's own doc), not "one
    // past the top of a solid run" — the naive `0 - 1 = -1` reading trivially
    // satisfies almost any non-negative offset, which is exactly what let a
    // biome-composition sequence fire at every position above real terrain
    // instead of only its top: measured against a real probe world, 292005
    // of 393216 blocks in four chunks (golden_fill_test.cpp), grass painting
    // the whole sky and deepslate painting over open water.
    const auto geometry = overworldGeometry();
    const auto rule = [](const char* type, int offset) {
        return nlohmann::json{{"type", "minecraft:condition"},
                              {"if_true", nlohmann::json{{"type", "minecraft:stone_depth"},
                                                         {"offset", offset},
                                                         {"add_surface_depth", false},
                                                         {"secondary_depth_range", 0},
                                                         {"surface_type", type}}},
                              {"then_run", block("minecraft:stone")}};
    };

    // offset 5 is generous enough that the old `-1 <= 5` reading would fire;
    // a position off the top of any run must still refuse it.
    const RuleGraph floorGraph = resolve(rule("floor", 5));
    const Executor floor = Executor::compile(floorGraph, kSeed, geometry);
    Context notInARun = at(0, 100, 0);
    notInARun.stoneDepthAbove = 0;
    CHECK(floor.apply(notInARun) == nullptr);

    const RuleGraph ceilingGraph = resolve(rule("ceiling", 5));
    const Executor ceiling = Executor::compile(ceilingGraph, kSeed, geometry);
    Context notInARunEither = at(0, 100, 0);
    notInARunEither.stoneDepthBelow = 0;
    CHECK(ceiling.apply(notInARunEither) == nullptr);
}

TEST_CASE("water is unconditionally true in a column with no fluid at all", "[surface]") {
    const auto geometry = overworldGeometry();
    const RuleGraph graph =
        resolve(nlohmann::json{{"type", "minecraft:condition"},
                               {"if_true", nlohmann::json{{"type", "minecraft:water"},
                                                          {"offset", -20},
                                                          {"surface_depth_multiplier", 0},
                                                          {"add_stone_depth", false}}},
                               {"then_run", block("minecraft:stone")}});
    const Executor executor = Executor::compile(graph, kSeed, geometry);

    // No fluid: true everywhere, at any y, whatever the offset. `sea_level`
    // alone does not make a water height — only real fluid blocks do.
    CHECK(executor.apply(at(0, -60, 0)) != nullptr);
    CHECK(executor.apply(at(0, 300, 0)) != nullptr);

    // With a fluid band topping out at y = 1, the latched height is 2 and the
    // boundary is 2 + (-20) = -18.
    Context wet = at(0, -18, 0);
    wet.waterHeight = 2;
    CHECK(executor.apply(wet) != nullptr);
    Context below = at(0, -19, 0);
    below.waterHeight = 2;
    CHECK(executor.apply(below) == nullptr);
}

TEST_CASE("steep is asymmetric, and that is not a bug to tidy away", "[surface]") {
    const auto geometry = overworldGeometry();
    const RuleGraph graph = resolve(nlohmann::json{{"type", "minecraft:condition"},
                                                   {"if_true", {{"type", "minecraft:steep"}}},
                                                   {"then_run", block("minecraft:stone")}});
    const Executor executor = Executor::compile(graph, kSeed, geometry);

    const auto fires = [&](std::int32_t w, std::int32_t e, std::int32_t n, std::int32_t s) {
        Context ctx = at(8, 100, 8);
        ctx.heightWest = w;
        ctx.heightEast = e;
        ctx.heightNorth = n;
        ctx.heightSouth = s;
        return executor.apply(ctx) != nullptr;
    };

    // West minus east, south minus north. The opposite signs must NOT fire —
    // 17375 columns of measured evidence say so, and an abs() would break them.
    CHECK(fires(10, 6, 0, 0));
    CHECK_FALSE(fires(6, 10, 0, 0));
    CHECK(fires(0, 0, 6, 10));
    CHECK_FALSE(fires(0, 0, 10, 6));

    // The threshold is >= 4 on integers: three is not steep, four is.
    CHECK_FALSE(fires(9, 6, 0, 0));
    CHECK(fires(10, 6, 0, 0));

    // OR, not AND.
    CHECK(fires(10, 6, 10, 6));
}

TEST_CASE("the steep neighbours are clamped inside the block's own chunk", "[surface]") {
    // At a chunk edge the clamp repeats the edge column rather than reading
    // the neighbouring chunk — which is what keeps steep free of a cross-chunk
    // dependency in the hot path.
    std::vector<std::pair<std::int32_t, std::int32_t>> asked;
    Context edge = at(0, 100, 5);
    stratum::surface::fillSteepNeighbours(edge, [&](std::int32_t x, std::int32_t z) {
        asked.emplace_back(x, z);
        return 0;
    });
    for (const auto& [x, z] : asked) {
        CHECK(x >= 0);
        CHECK(x <= 15);
    }

    // Deep inside a chunk it reads the true neighbours.
    asked.clear();
    Context middle = at(8, 100, 8);
    stratum::surface::fillSteepNeighbours(middle, [&](std::int32_t x, std::int32_t z) {
        asked.emplace_back(x, z);
        return 0;
    });
    CHECK(asked[0].first == 7);
    CHECK(asked[1].first == 9);
}

TEST_CASE("temperature compares a height-adjusted value, not the biome's own", "[surface]") {
    // sea_level 63 puts the origin at 80, which is vanilla's overworld.
    const auto geometry = overworldGeometry();
    const RuleGraph graph =
        resolve(nlohmann::json{{"type", "minecraft:condition"},
                               {"if_true", nlohmann::json{{"type", "minecraft:temperature"}}},
                               {"then_run", block("minecraft:stone")}});
    const Executor executor = Executor::compile(graph, kSeed, geometry, nullptr, 63);

    // Below the origin no adjustment happens at all, so a warm biome never
    // freezes however deep you go.
    CHECK_FALSE(executor.freezing(0, -60, 0, 0.8F));
    CHECK_FALSE(executor.freezing(0, 80, 0, 0.8F));

    // A biome already below the threshold freezes everywhere, including under
    // the origin where the adjustment is suppressed.
    CHECK(executor.freezing(0, -60, 0, 0.1F));
    CHECK(executor.freezing(0, 80, 0, 0.1F));

    // Above the origin it gets colder with height, so each column has ONE
    // boundary and everything above it freezes.
    const auto boundary = [&](float temperature) {
        for (std::int32_t y = 81; y <= 319; ++y) {
            if (executor.freezing(0, y, 0, temperature)) {
                return y;
            }
        }
        return 1 << 20;
    };
    const std::int32_t cold = boundary(0.29F);
    const std::int32_t mid = boundary(0.30F);
    const std::int32_t warm = boundary(0.31F);
    CHECK(cold < mid);
    CHECK(mid < warm);

    // Eight blocks per 0.01 of biome temperature — the slope that refuted the
    // flat threshold this project carried for two milestones. At any ONE
    // column the integer boundary lands 8 or 9 apart depending where the noise
    // puts the fractional part, so the exact figure is the mean over columns.
    CHECK(mid - cold >= 8);
    CHECK(mid - cold <= 9);
    CHECK(warm - mid >= 8);
    CHECK(warm - mid <= 9);

    double total = 0.0;
    int counted = 0;
    for (std::int32_t x = 0; x < 48; ++x) {
        for (std::int32_t z = 0; z < 48; ++z) {
            const auto at = [&](float t) {
                for (std::int32_t y = -64; y <= 319; ++y) {
                    if (executor.freezing(x, y, z, t)) {
                        return y;
                    }
                }
                return 1 << 20;
            };
            total += static_cast<double>(at(0.31F) - at(0.29F));
            ++counted;
        }
    }
    // Two steps of 0.01, so sixteen blocks across 2304 columns.
    CHECK(total / counted > 15.9);
    CHECK(total / counted < 16.1);

    // Once a column freezes it stays frozen all the way up.
    for (std::int32_t y = mid; y <= 319; ++y) {
        CHECK(executor.freezing(0, y, 0, 0.30F));
    }

    // And the field is seedless: built from the constant 1234, so a different
    // world seed gives the identical boundary.
    const Executor other = Executor::compile(graph, 12345, geometry, nullptr, 63);
    CHECK(other.freezing(0, mid, 0, 0.30F));
    CHECK_FALSE(other.freezing(0, mid - 1, 0, 0.30F));
}

TEST_CASE("the temperature origin follows sea level, not the world floor", "[surface]") {
    const auto geometry = overworldGeometry();
    const RuleGraph graph =
        resolve(nlohmann::json{{"type", "minecraft:condition"},
                               {"if_true", nlohmann::json{{"type", "minecraft:temperature"}}},
                               {"then_run", block("minecraft:stone")}});

    const auto boundaryAt = [&](std::int32_t seaLevel) {
        const Executor executor = Executor::compile(graph, kSeed, geometry, nullptr, seaLevel);
        for (std::int32_t y = -64; y <= 319; ++y) {
            if (executor.freezing(0, y, 0, 0.30F)) {
                return y;
            }
        }
        return 1 << 20;
    };

    // Raising the sea by one raises the whole thing by one: the origin is
    // sea_level + 17 and nothing else moves.
    CHECK(boundaryAt(64) - boundaryAt(63) == 1);
    CHECK(boundaryAt(0) < boundaryAt(63));
}

// bandlands (spec/bandlands-spec.md). The golden 192-entry tables below are
// not a derivation checked against itself: they are what tools/analysis/
// bandlands-probe.sh and bandlands-dump.cpp read directly off the real,
// unmodified 1.21.11 server at seeds 42 and -1, over 49152 sampled columns
// with zero exceptions (SPEC §11). Reproducing them exactly is this build's
// own independent confirmation of the clean-room spec, not a restatement of
// it.

TEST_CASE("bandlands' table matches the server's own, seed 42", "[surface][bandlands]") {
    const ClayBandsTree tree;
    const auto noises = clayBandsOffsetNoise(tree, 42);
    const RuleGraph graph = bandlandsGraph();
    const Executor executor =
        Executor::compile(graph, 42, overworldGeometry(), &noises, /*seaLevel=*/63);

    constexpr std::string_view kGolden =
        "wttobwlottblwltttotbbrotttotttlwttottotrototttorrrwotttototttott"
        "tttlwlttottotottottylwoytttotwrrtyttwtttttotttotlwttotttototowlt"
        "ottolwlolwlbbbbbbbottorottlwtbrrbtttotttyyytttbbbtototrrtottotot";
    REQUIRE(kGolden.size() == stratum::surface::kClayBandsSize);
    for (std::size_t i = 0; i < kGolden.size(); ++i) {
        CAPTURE(i);
        CHECK(executor.clayBandAt(i).name == clayBlockOf(kGolden[i]));
    }
}

TEST_CASE("bandlands' table matches the server's own, seed -1", "[surface][bandlands]") {
    const ClayBandsTree tree;
    const auto noises = clayBandsOffsetNoise(tree, -1);
    const RuleGraph graph = bandlandsGraph();
    const Executor executor =
        Executor::compile(graph, -1, overworldGeometry(), &noises, /*seaLevel=*/63);

    constexpr std::string_view kGolden =
        "wrrrottotttttotttlwlttottyttotttlwlrrrtbbbbotlwbbbbbbttotrblwttt"
        "lwltlwltbbtoytrrrottolwtttotttolwttototoyyyttttowttttottttbbttlw"
        "ltttyotttotottytttttottbbbbbyottttotttttottbbbttttotybbotttrrrto";
    REQUIRE(kGolden.size() == stratum::surface::kClayBandsSize);
    for (std::size_t i = 0; i < kGolden.size(); ++i) {
        CAPTURE(i);
        CHECK(executor.clayBandAt(i).name == clayBlockOf(kGolden[i]));
    }
}

TEST_CASE("bandlands reads through apply(), not just the raw table", "[surface][bandlands]") {
    // The same seed-42 table as above, reached the ordinary way: a sequence
    // like vanilla's own, `condition -> bandlands`, walked by apply().
    const ClayBandsTree tree;
    const auto noises = clayBandsOffsetNoise(tree, 42);
    const RuleGraph graph = resolve(nlohmann::json{{"type", "minecraft:bandlands"}});
    const Executor executor =
        Executor::compile(graph, 42, overworldGeometry(), &noises, /*seaLevel=*/63);

    const auto* placed = executor.apply(at(0, -64, 0));
    REQUIRE(placed != nullptr);
    // index(0, -64, 0) = floorMod(-64 + shift, 192); table[k] was built from
    // y = k - g0 at (0,0), so this is the same read path bandlandsAt() takes
    // directly, exercised through apply() and runRule() instead.
    CHECK(placed->name == executor.bandlandsAt(0, -64, 0).name);
}

TEST_CASE("bandlands' index reproduces vanilla's own reachable crash, not a clamp",
          "[surface][bandlands]") {
    // spec/bandlands-spec.md Q5.1: vanilla's own index arithmetic is not
    // safe for extreme y, and the real server was confirmed to throw
    // ArrayIndexOutOfBoundsException reaching it — not a hypothetical this
    // build invented. A dimension whose geometry reaches far enough down
    // hits it here too, deliberately, rather than silently clamping into a
    // colour vanilla never placed.
    const ClayBandsTree tree;
    const auto noises = clayBandsOffsetNoise(tree, 42);
    const RuleGraph graph = bandlandsGraph();
    const auto deepGeometry = stratum::settings::NoiseGeometry{
        .minY = -2048, .height = 2048 + 320, .sizeHorizontal = 1, .sizeVertical = 2};
    const Executor executor = Executor::compile(graph, 42, deepGeometry, &noises, 63);

    // Ordinary y still reads fine.
    CHECK_NOTHROW(executor.bandlandsAt(0, -64, 0));
    // Far enough down, vanilla's own single '+192'-then-'%' goes negative.
    CHECK_THROWS_WITH(executor.bandlandsAt(0, -2032, 0), ContainsSubstring("outside"));
}

TEST_CASE("clayBandAt refuses a tree that never named bandlands", "[surface][bandlands]") {
    const auto graph = resolve(nlohmann::json{{"type", "minecraft:block"},
                                              {"result_state", {{"Name", "minecraft:stone"}}}});
    const Executor executor = Executor::compile(graph, kSeed, overworldGeometry());
    CHECK_THROWS_WITH(executor.clayBandAt(0), ContainsSubstring("bandlands"));
}

// --- inputs that would reach undefined behaviour (SPEC §8) -----------------
//
// The rules' arithmetic is 32-bit, and every int the schema allows is an
// int: an anchor, an offset or a sea level near 2^31 overflowed it, and a
// datapack's noise amplitudes put a surface depth past any int. Each is now
// refused by name — at compile where it is a constant, at the block where it
// is not — and the in-range neighbour still runs.

namespace {

/// A worldgen tree holding exactly the noises a case writes, each in the
/// `minecraft` namespace under the path it is given.
class NoiseTree {
public:
    NoiseTree(std::initializer_list<std::pair<std::string_view, std::string_view>> noises)
        : path_(std::filesystem::temp_directory_path() /
                stratum::test::tempName("stratum-surface-ub-test")) {
        const std::filesystem::path noiseDir = path_ / "noise";
        std::filesystem::create_directories(noiseDir);
        for (const auto& [name, json] : noises) {
            std::ofstream out(noiseDir / (std::string(name) + ".json"));
            out << json;
            names_.push_back(stratum::data::ResourceLocation::parse(std::string(name)));
        }
    }

    NoiseTree(const NoiseTree&) = delete;
    NoiseTree& operator=(const NoiseTree&) = delete;
    NoiseTree(NoiseTree&&) = delete;
    NoiseTree& operator=(NoiseTree&&) = delete;

    ~NoiseTree() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] stratum::density::NoiseRegistry registry() const {
        return stratum::density::NoiseRegistry::create(stratum::data::Pack::openWorldgenTree(path_),
                                                       names_, kSeed,
                                                       stratum::density::RandomSource::Xoroshiro);
    }

private:
    std::filesystem::path path_;
    std::vector<stratum::data::ResourceLocation> names_;
};

constexpr std::string_view kTameSurface = R"({"firstOctave": -6, "amplitudes": [1.0, 1.0, 1.0]})";
constexpr std::string_view kTameSecondary =
    R"({"firstOctave": -6, "amplitudes": [1.0, 1.0, 0.0, 1.0]})";
constexpr std::string_view kHugeAmplitude = R"({"firstOctave": -6, "amplitudes": [1e12]})";

[[nodiscard]] nlohmann::json when(const nlohmann::json& condition) {
    return nlohmann::json{{"type", "minecraft:condition"},
                          {"if_true", condition},
                          {"then_run", block("minecraft:stone")}};
}

/// A column whose surface depth (seed 42, the tame surface noise) is at least
/// one, so that adding it to INT32_MAX overflows.
constexpr std::int32_t kDeepColumn = 0;

} // namespace

TEST_CASE("an anchor whose y is not a 32-bit integer is refused at compile", "[surface][ub]") {
    // `below_top: -2147483648` puts the y at (top block) + 2^31.
    const RuleGraph below =
        resolve(when(nlohmann::json{{"type", "minecraft:y_above"},
                                    {"anchor", {{"below_top", -2147483647 - 1}}},
                                    {"surface_depth_multiplier", 0},
                                    {"add_stone_depth", false}}));
    CHECK_THROWS_WITH(Executor::compile(below, kSeed, overworldGeometry()),
                      ContainsSubstring("'minecraft:y_above' condition's anchor") &&
                          ContainsSubstring("32-bit"));
    const RuleGraph above = resolve(when(gradient("stratum:g", 0, 0)));
    CHECK_NOTHROW(Executor::compile(above, kSeed, overworldGeometry()));
    nlohmann::json far = gradient("stratum:g", 0, 0);
    // `above_bottom: -2147483648` puts it at min_y - 2^31.
    far["false_at_and_above"] = {{"above_bottom", -2147483647 - 1}};
    const RuleGraph farGraph = resolve(when(far));
    CHECK_THROWS_WITH(Executor::compile(farGraph, kSeed, overworldGeometry()),
                      ContainsSubstring("false_at_and_above (-2147483648)"));
    // The largest absolute anchor is an int32 as written.
    const RuleGraph absolute = resolve(when(nlohmann::json{{"type", "minecraft:y_above"},
                                                           {"anchor", {{"absolute", 2147483647}}},
                                                           {"surface_depth_multiplier", 0},
                                                           {"add_stone_depth", false}}));
    const Executor executor = Executor::compile(absolute, kSeed, overworldGeometry());
    CHECK(executor.apply(at(0, 64, 0)) == nullptr);
}

TEST_CASE("a temperature condition refuses a sea level it cannot measure from", "[surface][ub]") {
    // The freezing test's height runs from sea_level + 17.
    const RuleGraph graph = resolve(when(nlohmann::json{{"type", "minecraft:temperature"}}));
    constexpr std::int32_t kHighest = std::numeric_limits<std::int32_t>::max();
    CHECK_THROWS_WITH(Executor::compile(graph, kSeed, overworldGeometry(), nullptr, kHighest),
                      ContainsSubstring("sea_level 2147483647"));
    CHECK_NOTHROW(Executor::compile(graph, kSeed, overworldGeometry(), nullptr, kHighest - 17));
    // A tree without one takes the same sea level: nothing adds to it.
    const RuleGraph plain = resolve(block("minecraft:stone"));
    CHECK_NOTHROW(Executor::compile(plain, kSeed, overworldGeometry(), nullptr, kHighest));
}

TEST_CASE("the conditions refuse 32-bit arithmetic that would overflow", "[surface][ub]") {
    const NoiseTree tree{{"surface", kTameSurface}, {"surface_secondary", kTameSecondary}};
    const auto noises = tree.registry();
    const auto geometry = overworldGeometry();
    constexpr std::int32_t kHighest = std::numeric_limits<std::int32_t>::max();

    const RuleGraph yAbove = resolve(when(nlohmann::json{{"type", "minecraft:y_above"},
                                                         {"anchor", {{"absolute", kHighest}}},
                                                         {"surface_depth_multiplier", 1},
                                                         {"add_stone_depth", false}}));
    const Executor yAboveExecutor = Executor::compile(yAbove, kSeed, geometry, &noises);
    REQUIRE(yAboveExecutor.surfaceDepth(kDeepColumn, 0) >= 1);
    CHECK_THROWS_WITH(yAboveExecutor.apply(at(kDeepColumn, 64, 0)),
                      ContainsSubstring("y_above's anchor plus") && ContainsSubstring("32-bit"));

    const RuleGraph water = resolve(when(nlohmann::json{{"type", "minecraft:water"},
                                                        {"offset", kHighest},
                                                        {"surface_depth_multiplier", 0},
                                                        {"add_stone_depth", false}}));
    const Executor waterExecutor = Executor::compile(water, kSeed, geometry, &noises);
    Context wet = at(0, 10, 0);
    wet.waterHeight = 63;
    CHECK_THROWS_WITH(waterExecutor.apply(wet),
                      ContainsSubstring("water's water height plus its offset"));
    wet.waterHeight = 0;
    CHECK(waterExecutor.apply(wet) == nullptr);

    const RuleGraph stone = resolve(when(nlohmann::json{{"type", "minecraft:stone_depth"},
                                                        {"offset", kHighest},
                                                        {"surface_type", "floor"},
                                                        {"add_surface_depth", true},
                                                        {"secondary_depth_range", 0}}));
    const Executor stoneExecutor = Executor::compile(stone, kSeed, geometry, &noises);
    CHECK_THROWS_WITH(stoneExecutor.apply(at(kDeepColumn, 64, 0)),
                      ContainsSubstring("stone_depth's offset plus the surface depth"));

    const RuleGraph aps =
        resolve(when(nlohmann::json{{"type", "minecraft:above_preliminary_surface"}}));
    const Executor apsExecutor = Executor::compile(aps, kSeed, geometry, &noises);
    CHECK_THROWS_WITH(apsExecutor.apply(at(kDeepColumn, 64, 0, kHighest)),
                      ContainsSubstring("the preliminary surface plus the surface depth"));
    CHECK_NOTHROW(apsExecutor.apply(at(kDeepColumn, 64, 0, 63)));
}

TEST_CASE("a surface depth no int holds is refused, not converted", "[surface][ub]") {
    // A datapack may give `minecraft:surface` any amplitudes; at 1e12 the
    // depth's raw value is far past int32, where the truncating conversion is
    // undefined.
    const NoiseTree tree{{"surface", kHugeAmplitude}, {"surface_secondary", kHugeAmplitude}};
    const auto noises = tree.registry();
    const RuleGraph hole = resolve(when(nlohmann::json{{"type", "minecraft:hole"}}));
    const Executor executor = Executor::compile(hole, kSeed, overworldGeometry(), &noises);
    bool refused = false;
    for (std::int32_t x = 0; x < 64 && !refused; ++x) {
        const double raw = executor.surfaceDepthRaw(x, 0);
        if (raw > 2147483648.0 || raw < -2147483649.0) {
            CHECK_THROWS_WITH(executor.surfaceDepth(x, 0),
                              ContainsSubstring("the surface depth (minecraft:surface)") &&
                                  ContainsSubstring("no 32-bit integer"));
            refused = true;
        }
    }
    CHECK(refused);

    // The secondary depth, likewise.
    const RuleGraph secondary = resolve(when(nlohmann::json{{"type", "minecraft:stone_depth"},
                                                            {"offset", 0},
                                                            {"surface_type", "floor"},
                                                            {"add_surface_depth", false},
                                                            {"secondary_depth_range", 30}}));
    const Executor secondaryExecutor =
        Executor::compile(secondary, kSeed, overworldGeometry(), &noises);
    bool secondaryRefused = false;
    for (std::int32_t x = 0; x < 64 && !secondaryRefused; ++x) {
        try {
            static_cast<void>(secondaryExecutor.apply(at(x, 64, 0)));
        } catch (const ExecutionError& error) {
            CHECK_THAT(error.what(), ContainsSubstring("minecraft:surface_secondary"));
            secondaryRefused = true;
        }
    }
    CHECK(secondaryRefused);
}

TEST_CASE("the secondary depth and bandlands' index refuse their own overflowing steps",
          "[surface][bandlands][ub]") {
    // The two `narrowed` steps the cases above do not reach: stone_depth's
    // threshold plus its secondary depth, and bandlands' y plus its offset,
    // then plus 192. Each is driven at INT32_MAX with tame noises, so the
    // step itself is what leaves int32, not a conversion before it.
    constexpr std::int32_t kHighest = std::numeric_limits<std::int32_t>::max();
    const NoiseTree tree{{"surface", kTameSurface},
                         {"surface_secondary", kTameSecondary},
                         {"clay_bands_offset", kTameSurface}};
    const auto noises = tree.registry();
    const auto geometry = overworldGeometry();

    // The secondary depth is (secondary + 1) / 2 * 30, truncated: at least 1
    // in almost every column, which INT32_MAX cannot take.
    const auto secondary = [](std::int32_t offset) {
        return resolve(when(nlohmann::json{{"type", "minecraft:stone_depth"},
                                           {"offset", offset},
                                           {"surface_type", "floor"},
                                           {"add_surface_depth", false},
                                           {"secondary_depth_range", 30}}));
    };
    const RuleGraph highGraph = secondary(kHighest);
    const Executor high = Executor::compile(highGraph, kSeed, geometry, &noises);
    bool thresholdRefused = false;
    for (std::int32_t x = 0; x < 64 && !thresholdRefused; ++x) {
        try {
            static_cast<void>(high.apply(at(x, 64, 0)));
        } catch (const ExecutionError& error) {
            CHECK_THAT(error.what(), ContainsSubstring("stone_depth's threshold plus its "
                                                       "secondary depth") &&
                                         ContainsSubstring("32-bit"));
            thresholdRefused = true;
        }
    }
    CHECK(thresholdRefused);
    const RuleGraph plainGraph = secondary(0);
    const Executor plain = Executor::compile(plainGraph, kSeed, geometry, &noises);
    for (std::int32_t x = 0; x < 64; ++x) {
        CHECK_NOTHROW(plain.apply(at(x, 64, 0)));
    }

    // bandlands' offset from a tame noise is a few blocks either way. At
    // INT32_MAX - 100 adding it stays in range and the + 192 does not; at
    // INT32_MAX a positive offset is itself the overflow.
    const RuleGraph bands = resolve(nlohmann::json{{"type", "minecraft:bandlands"}});
    const Executor bandlands = Executor::compile(bands, kSeed, geometry, &noises, 63);
    CHECK_THROWS_WITH(bandlands.bandlandsAt(0, kHighest - 100, 0),
                      ContainsSubstring("bandlands' y plus its offset plus 192 comes to"));
    bool offsetRefused = false;
    for (std::int32_t x = 0; x < 64 && !offsetRefused; ++x) {
        try {
            static_cast<void>(bandlands.bandlandsAt(x, kHighest, 0));
        } catch (const ExecutionError& error) {
            const std::string message = error.what();
            if (message.find("bandlands' y plus its offset comes to") != std::string::npos) {
                offsetRefused = true;
            }
        }
    }
    CHECK(offsetRefused);
    CHECK_NOTHROW(bandlands.bandlandsAt(0, 64, 0));
}

TEST_CASE("bandlands refuses an offset no int holds", "[surface][bandlands][ub]") {
    const NoiseTree tree{{"clay_bands_offset", kHugeAmplitude}};
    const auto noises = tree.registry();
    const RuleGraph graph = resolve(nlohmann::json{{"type", "minecraft:bandlands"}});
    const Executor executor = Executor::compile(graph, kSeed, overworldGeometry(), &noises, 63);
    bool refused = false;
    for (std::int32_t x = 0; x < 64 && !refused; ++x) {
        try {
            static_cast<void>(executor.bandlandsAt(x, 64, 0));
        } catch (const ExecutionError& error) {
            CHECK_THAT(error.what(), ContainsSubstring("minecraft:clay_bands_offset"));
            refused = true;
        }
    }
    CHECK(refused);
}

TEST_CASE("a noise_threshold refuses a noise past its 32-bit lattice", "[surface][ub]") {
    // firstOctave 64 is inside the loader's bound; at frequency 2^64 no
    // column but the origin's has a lattice cell an int32 holds.
    const NoiseTree tree{{"steep", R"({"firstOctave": 64, "amplitudes": [1.0]})"}};
    const auto noises = tree.registry();
    const RuleGraph graph = resolve(when(nlohmann::json{{"type", "minecraft:noise_threshold"},
                                                        {"noise", "minecraft:steep"},
                                                        {"min_threshold", -10.0},
                                                        {"max_threshold", 10.0}}));
    const Executor executor = Executor::compile(graph, kSeed, overworldGeometry(), &noises);
    CHECK_NOTHROW(executor.apply(at(0, 64, 0)));
    CHECK_THROWS_WITH(
        executor.apply(at(1, 64, 0)),
        ContainsSubstring("noise 'minecraft:steep' cannot be sampled at column (1, 0)"));
}
