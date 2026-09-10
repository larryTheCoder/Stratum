// Stratum — running a dimension's surface rules.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#include <stratum/data/pack.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/executor.hpp>
#include <stratum/surface/rule_graph.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
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
        static int counter = 0;
        return "stratum-bandlands-test-" + std::to_string(++counter);
    }

    std::filesystem::path path_;
};

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

TEST_CASE("above_preliminary_surface reads the column's own level", "[surface]") {
    const auto geometry = overworldGeometry();
    const RuleGraph graph =
        resolve(nlohmann::json{{"type", "minecraft:condition"},
                               {"if_true", {{"type", "minecraft:above_preliminary_surface"}}},
                               {"then_run", block("minecraft:stone")}});
    const Executor executor = Executor::compile(graph, kSeed, geometry);

    CHECK(executor.apply(at(0, 70, 0, 64)) != nullptr);
    CHECK(executor.apply(at(0, 60, 0, 64)) == nullptr);

    // The boundary itself is the DOCUMENTED reading and not a measured one:
    // the only probe that reached this condition found it true everywhere,
    // which cannot separate >= from > (SPEC §11). If that is ever settled the
    // other way, this is the assertion that has to change.
    CHECK(executor.apply(at(0, 64, 0, 64)) != nullptr);
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
    for (std::int32_t x = 0; x < 48; ++x)
        for (std::int32_t z = 0; z < 48; ++z) {
            const auto at = [&](float t) {
                for (std::int32_t y = -64; y <= 319; ++y)
                    if (executor.freezing(x, y, z, t))
                        return y;
                return 1 << 20;
            };
            total += static_cast<double>(at(0.31F) - at(0.29F));
            ++counted;
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
