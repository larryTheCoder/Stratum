// Stratum — the 2D density pipeline against vanilla's own data.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The unit suite evaluates graphs this repository wrote, against answers this
// repository computed. This one loads vanilla 1.21.11's density functions and
// noises out of the fixtures, evaluates them, and compares the results
// bit-exactly against cubiomes (MIT) — an independent reimplementation that
// knows nothing about our JSON, our resolver or our interpreter.
//
// That makes it the first end-to-end check in the project: seed derivation,
// noise parameter parsing, octave and normal noise, reference resolution,
// flat_cache relocation, shifted_noise, the arithmetic tree and the float
// spline all have to be simultaneously right for a single value to match.
//
// It is still not proof of parity with Mojang — cubiomes could be wrong in
// the same way we are. The goldens settle that in M3. What it does establish
// outright is that vanilla's shipped JSON describes the computation cubiomes
// hardcodes, which is the question the resolver could not answer.
//
// Mojang-derived fixtures are never committed (SPEC §12); without them this
// SKIPs, naming the command that produces them.

#include "climate_vectors.inc"

#include <stratum/data/pack.hpp>
#include <stratum/data/registry.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/graph.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using stratum::data::Pack;
using stratum::data::ResourceLocation;
using stratum::density::Graph;
using stratum::density::Interpreter;
using stratum::density::NoiseRegistry;
using stratum::density::Point;

/// Bits, not values: the project builds with -Wfloat-equal, and "close
/// enough" is not a category this comparison has.
[[nodiscard]] std::uint64_t bits(double value) noexcept {
    return std::bit_cast<std::uint64_t>(value);
}

[[nodiscard]] double fromBits(std::uint64_t value) noexcept {
    return std::bit_cast<double>(value);
}

// How far a value may sit from cubiomes' answer.
//
// Two bugs found against the vanilla SERVER in September 2026 put this build
// one ulp away from cubiomes on some noises, and it is cubiomes that is out
// (SPEC §11). Both are in how a NormalNoise is assembled:
//
//   * octave amplitude grouping — vanilla computes (sample * amplitude) *
//     persistence; folding the amplitude into the persistence at setup, as
//     cubiomes and this build both did, gives a different double whenever an
//     amplitude is not a power of two.
//   * the value factor — 1/(6 * 0.1 * (1 + 1/n)) is one ulp from the
//     algebraically equal (5n)/(3(n+1)) at effective octave counts
//     1, 2, 6, 9, 15, 16, 17 and 22.
//
// Each was settled by putting the two candidate doubles in as EXACT
// `noise_threshold` bounds and letting the server choose; it painted every
// position of one and none of the other, 40 positions per test, on three
// noises across two seeds.
//
// So these vectors can no longer be asserted bit-exact for every noise. They
// are still asserted bit-exact for every noise the corrections do not reach,
// and bounded for the two they do — which keeps the whole file's value as a
// regression net while recording, rather than hiding, a real divergence.
[[nodiscard]] std::int64_t ulpsApart(std::uint64_t left, std::uint64_t right) {
    const auto a = static_cast<std::int64_t>(left);
    const auto b = static_cast<std::int64_t>(right);
    return a > b ? a - b : b - a;
}

// How far each noise may be from cubiomes, by cause. Zero means bit-exact,
// which is still the answer for most of them. The bounds are the MEASURED
// maxima rather than comfortable round numbers, so that a regression which
// widened the gap would fail here.
//
//   * `temperature` has an amplitude of 1.5, so the grouping correction
//     reaches it: up to five ulps over these vectors, 34 of 42 still exact.
//   * `continentalness` (effective span 9) and `vegetation` (span 2) have
//     spans the value-factor correction reaches: up to two ulps.
//
// Every other noise here has power-of-two amplitudes AND an unaffected span,
// so neither correction can move it and it stays asserted bit-exact.
[[nodiscard]] std::int64_t allowedUlps(std::string_view noise) {
    if (noise == "minecraft:temperature") {
        return 5;
    }
    if (noise == "minecraft:continentalness" || noise == "minecraft:vegetation") {
        return 2;
    }
    return 0;
}

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

/// The whole 2D pipeline, assembled once per seed.
class Overworld {
public:
    // Neither copyable nor movable on purpose: the interpreter holds a
    // pointer to the graph beside it, so a moved-from copy would leave that
    // pointer aimed at a destroyed object. Callers rebuild in place with
    // std::optional::emplace instead.
    Overworld(const Overworld&) = delete;
    Overworld& operator=(const Overworld&) = delete;

    Overworld(const Pack& pack, std::int64_t seed)
        : graph_(Graph::resolveAll(pack)),
          noises_(NoiseRegistry::create(pack, graph_.referencedNoises(), seed,
                                        stratum::density::RandomSource::Xoroshiro)),
          interpreter_(graph_, noises_) {}

    [[nodiscard]] double at(std::string_view function, Point point) const {
        return interpreter_.evaluate(graph_.rootOf(ResourceLocation::parse(std::string(function))),
                                     point);
    }

    [[nodiscard]] const Graph& graph() const noexcept { return graph_; }

    [[nodiscard]] const Interpreter& interpreter() const noexcept { return interpreter_; }

private:
    Graph graph_;
    NoiseRegistry noises_;
    Interpreter interpreter_;
};

} // namespace

TEST_CASE("the noises a world seed derives match cubiomes", "[conformance][density][noise]") {
    const std::filesystem::path tree = findWorldgenTree();
    if (tree.empty()) {
        SKIP("no extracted vanilla worldgen under "
             << STRATUM_FIXTURES_DIR
             << " — Mojang-derived and never committed (SPEC §12). Generate it with: "
                "tools/fetch-vanilla");
    }

    const Pack pack = Pack::openWorldgenTree(tree);

    // The whole chain is under test here: two draws from the world seed, the
    // MD5 of each noise's own name XORed into them, the firstOctave and
    // amplitudes read out of worldgen/noise/<name>.json, and the octave and
    // normal-noise stacks built on top. One wrong step and none of these
    // match (CLAUDE.md: "one wrong salt/seed derivation shifts everything
    // downstream").
    std::int64_t currentSeed = 0;
    std::vector<ResourceLocation> wanted;
    for (const auto& vector : kClimateNoiseVectors) {
        wanted.push_back(ResourceLocation::parse(std::string(vector.noise)));
    }

    NoiseRegistry registry =
        NoiseRegistry::create(pack, wanted, currentSeed, stratum::density::RandomSource::Xoroshiro);
    for (const auto& vector : kClimateNoiseVectors) {
        if (vector.seed != currentSeed) {
            currentSeed = vector.seed;
            registry = NoiseRegistry::create(pack, wanted, currentSeed,
                                             stratum::density::RandomSource::Xoroshiro);
        }

        const auto id = ResourceLocation::parse(std::string(vector.noise));
        const double sampled =
            registry.get(id).sample(fromBits(vector.x), fromBits(vector.y), fromBits(vector.z));
        CAPTURE(vector.seed, vector.noise);

        CHECK(ulpsApart(bits(sampled), vector.value) <= allowedUlps(vector.noise));
    }
}

TEST_CASE("vanilla's overworld climate functions match cubiomes",
          "[conformance][density][interpreter]") {
    const std::filesystem::path tree = findWorldgenTree();
    if (tree.empty()) {
        SKIP("no extracted vanilla worldgen under " << STRATUM_FIXTURES_DIR);
    }

    const Pack pack = Pack::openWorldgenTree(tree);
    std::int64_t currentSeed = kClimateChainVectors.front().seed;
    std::optional<Overworld> overworld;
    overworld.emplace(pack, currentSeed);

    // How often cubiomes' own getSpline disagrees with the float-throughout
    // reading our interpreter implements. Reported rather than asserted: the
    // difference is cubiomes routing two interpolations through a double
    // lerp helper, which is a detail of that library and not a claim about
    // vanilla. See tools/vectors/climate_vectors.c.
    std::size_t splineRoundingDifferences = 0;

    for (const auto& vector : kClimateChainVectors) {
        if (vector.seed != currentSeed) {
            currentSeed = vector.seed;
            overworld.emplace(pack, currentSeed);
        }
        if (vector.splineStrict != vector.splineCubiomes) {
            ++splineRoundingDifferences;
        }

        // The corner of the 4x4 column, which is where vanilla's flat_cache
        // samples and the resolution cubiomes works in.
        const Point corner{.x = vector.quartX * 4, .y = 0, .z = vector.quartZ * 4};
        CAPTURE(vector.seed, vector.quartX, vector.quartZ);

        // shift_a and shift_b, through flat_cache and cache_2d.
        CHECK(bits(overworld->at("shift_x", corner)) == vector.shiftX);
        CHECK(bits(overworld->at("shift_z", corner)) == vector.shiftZ);

        // shifted_noise over those shifts, through flat_cache.
        // continents reaches minecraft:continentalness and carries its bound.
        CHECK(ulpsApart(bits(overworld->at("overworld/continents", corner)), vector.continents) <=
              allowedUlps("minecraft:continentalness"));
        CHECK(bits(overworld->at("overworld/erosion", corner)) == vector.erosion);
        CHECK(bits(overworld->at("overworld/ridges", corner)) == vector.ridges);

        // mul/add/abs over a reference.
        CHECK(bits(overworld->at("overworld/ridges_folded", corner)) == vector.ridgesFolded);

        // The offset spline: 47 knots over three coordinates, evaluated in
        // float, plus the blend_alpha/blend_offset arithmetic around it.
        CHECK(bits(overworld->at("overworld/offset", corner)) == vector.offset);
    }

    // Asserted, not merely noted: if this ever reached zero, every sample
    // would agree under both readings and the float-throughout handling that
    // makes them agree would be untested — which is when it would rot.
    INFO("cubiomes' getSpline and the float-throughout reading disagree on "
         << splineRoundingDifferences << " of " << kClimateChainVectors.size() << " samples");
    CHECK(splineRoundingDifferences > 0);
}

TEST_CASE("flat_cache relocates the sample to its column corner",
          "[conformance][density][interpreter]") {
    const std::filesystem::path tree = findWorldgenTree();
    if (tree.empty()) {
        SKIP("no extracted vanilla worldgen under " << STRATUM_FIXTURES_DIR);
    }

    const Pack pack = Pack::openWorldgenTree(tree);
    std::int64_t currentSeed = kClimateChainVectors.front().seed;
    std::optional<Overworld> overworld;
    overworld.emplace(pack, currentSeed);

    // Every 2D climate function vanilla ships is wrapped in flat_cache, which
    // means its value is not sampled where you ask but at the corner of the
    // 4x4 column, with y pinned to zero. That is the difference between a
    // cache and a relocation, and it is why CLAUDE.md calls these nodes
    // parity-critical rather than optimisations: getting it wrong shifts the
    // climate of every block that is not on a corner.
    for (const auto& vector : kClimateChainVectors) {
        if (vector.seed != currentSeed) {
            currentSeed = vector.seed;
            overworld.emplace(pack, currentSeed);
        }

        for (const Point inside :
             {Point{.x = (vector.quartX * 4) + 3, .y = 0, .z = (vector.quartZ * 4) + 1},
              Point{.x = (vector.quartX * 4) + 1, .y = 200, .z = (vector.quartZ * 4) + 3},
              Point{.x = (vector.quartX * 4) + 2, .y = -60, .z = (vector.quartZ * 4) + 2}}) {
            CAPTURE(vector.seed, vector.quartX, vector.quartZ, inside.x, inside.y, inside.z);
            CHECK(ulpsApart(bits(overworld->at("overworld/continents", inside)),
                            vector.continents) <= allowedUlps("minecraft:continentalness"));
            CHECK(bits(overworld->at("overworld/erosion", inside)) == vector.erosion);
            CHECK(bits(overworld->at("overworld/offset", inside)) == vector.offset);
        }
    }
}

TEST_CASE("the interpreter's account of vanilla's graph is honest",
          "[conformance][density][interpreter]") {
    const std::filesystem::path tree = findWorldgenTree();
    if (tree.empty()) {
        SKIP("no extracted vanilla worldgen under " << STRATUM_FIXTURES_DIR);
    }

    const Pack pack = Pack::openWorldgenTree(tree);
    const Overworld overworld(pack, 42);
    const Graph& graph = overworld.graph();
    const Interpreter& interpreter = overworld.interpreter();

    const auto root = [&graph](std::string_view path) {
        return graph.rootOf(ResourceLocation::parse(std::string(path)));
    };

    // What this build can evaluate today, stated as a fact about vanilla's
    // own data rather than a list of intentions. When M3 adds the cell
    // sampler these move from one group to the other, and this test is where
    // that has to be admitted.
    for (const std::string_view path :
         {"shift_x", "shift_z", "y", "zero", "overworld/continents", "overworld/erosion",
          "overworld/ridges", "overworld/ridges_folded", "overworld/offset", "overworld/depth",
          "overworld/factor", "overworld/jaggedness",
          // Terrain shape, since old_blended_noise was settled: base_3d_noise
          // is that node, and sloped_cheese is the function built on it.
          "overworld/base_3d_noise", "overworld/sloped_cheese",
          // The cave functions, since weird_scaled_sampler was settled.
          "overworld/caves/entrances", "overworld/caves/spaghetti_2d"}) {
        CAPTURE(path);
        CHECK_NOTHROW(interpreter.requireEvaluable(root(path)));
    }

    // What is left waits on weird_scaled_sampler and end_islands, not on
    // anything about the blended noise.
    for (const std::string_view path : {"end/sloped_cheese", "overworld/caves/noodle"}) {
        CAPTURE(path);
        CHECK_THROWS_AS(interpreter.requireEvaluable(root(path)), stratum::density::EvalError);
    }

    // Column invariance is what makes vanilla's cache_2d wrapping sound, and
    // the interpreter refuses to evaluate a cache_2d over anything else. If
    // that analysis ever went wrong in the permissive direction it would go
    // unnoticed, so the classification is asserted directly.
    CHECK(interpreter.isColumnInvariant(root("overworld/continents")));
    CHECK(interpreter.isColumnInvariant(root("overworld/offset")));
    CHECK(interpreter.isColumnInvariant(root("shift_x")));
    CHECK_FALSE(interpreter.isColumnInvariant(root("y")));
    CHECK_FALSE(interpreter.isColumnInvariant(root("overworld/depth")));
}
