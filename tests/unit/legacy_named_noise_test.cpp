// Stratum — named noises under `legacy_random_source`, against the
// clean-room specification's own vectors.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// spec/legacy-named-noise-constants.json carries a "golden" table: for six
// world seeds (among them 2^48, which must reproduce seed 0, and the value
// that zeroes the seeding state), the dimension's factory seed F(W), each of
// the thirteen surface-rule noises at four positions, and the surface depth
// at four columns, one of them the truncation case of Q4.6. They were
// computed from the unmodified 1.21.11 jar and committed with the owner's
// approval (2026-11-09). This file reads that table where it lives rather
// than copying it, and holds the shipped registry and the surface executor
// to it bit for bit.

#include <stratum/data/resource_location.hpp>
#include <stratum/density/noise_parameters.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/noise/perlin.hpp>
#include <stratum/rng/java_random.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/executor.hpp>
#include <stratum/surface/rule_graph.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using stratum::data::ResourceLocation;
using stratum::density::NoiseParameters;
using stratum::density::NoiseRegistry;
using stratum::density::RandomSource;

namespace {

[[nodiscard]] const nlohmann::json& constants() {
    static const nlohmann::json kConstants = [] {
        const std::filesystem::path path =
            std::filesystem::path(STRATUM_SPEC_DIR) / "legacy-named-noise-constants.json";
        std::ifstream in(path);
        REQUIRE(in.good());
        return nlohmann::json::parse(in);
    }();
    return kConstants;
}

/// The thirteen shipped parameter sets, as the spec lists them (Q2.1).
[[nodiscard]] std::map<ResourceLocation, NoiseParameters> shippedParameters() {
    std::map<ResourceLocation, NoiseParameters> parameters;
    for (const auto& [key, entry] : constants().at("per_noise_vanilla").items()) {
        NoiseParameters parsed;
        parsed.firstOctave = entry.at("firstOctave").get<int>();
        parsed.amplitudes = entry.at("amplitudes").get<std::vector<double>>();
        parameters.emplace(ResourceLocation::parse(key), std::move(parsed));
    }
    return parameters;
}

[[nodiscard]] std::uint64_t bits(const double value) {
    return std::bit_cast<std::uint64_t>(value);
}

[[nodiscard]] stratum::settings::NoiseGeometry netherGeometry() {
    return stratum::settings::NoiseGeometry{
        .minY = 0, .height = 128, .sizeHorizontal = 1, .sizeVertical = 2};
}

} // namespace

TEST_CASE("legacy named noises match the clean-room spec's vectors bit for bit",
          "[noise][legacy][spec]") {
    const nlohmann::json& golden = constants().at("golden");
    const auto parameters = shippedParameters();
    REQUIRE(parameters.size() == 13);

    std::vector<ResourceLocation> wanted;
    wanted.reserve(parameters.size());
    for (const auto& [id, unused] : parameters) {
        wanted.push_back(id);
    }

    const auto& positions = golden.at("positions");
    REQUIRE(positions.size() == 4);

    std::size_t compared = 0;
    for (const auto& [seedText, values] : golden.at("values").items()) {
        const std::int64_t seed = std::stoll(seedText);
        INFO("world seed " << seed);

        // Q1.2: F(W) is the first nextLong of java.util.Random(W).
        stratum::rng::JavaRandom world{seed};
        CHECK(world.nextLong() == values.at("dimension_factory_seed").get<std::int64_t>());

        // Built through the registry, as a dimension builds them, and all
        // thirteen at once, in the registry's own order (Q1.4 says order
        // cannot matter, and the reverse-order case below checks it).
        const NoiseRegistry registry =
            NoiseRegistry::create(parameters, wanted, seed, RandomSource::Legacy);
        for (const ResourceLocation& id : wanted) {
            INFO("noise " << id.toString());
            const auto& expected = values.at(id.toString());
            REQUIRE(expected.size() == positions.size());
            for (std::size_t p = 0; p < positions.size(); ++p) {
                const double x = positions[p][0].get<double>();
                const double y = positions[p][1].get<double>();
                const double z = positions[p][2].get<double>();
                INFO("at (" << x << ", " << y << ", " << z << ")");
                CHECK(bits(registry.get(id).sample(x, y, z)) == bits(expected[p].get<double>()));
                ++compared;
            }
        }
    }
    CHECK(compared == std::size_t{6} * 13U * 4U);
}

TEST_CASE("a legacy named noise does not depend on which others are built, or in what order",
          "[noise][legacy][spec]") {
    const auto parameters = shippedParameters();
    std::vector<ResourceLocation> forward;
    forward.reserve(parameters.size());
    for (const auto& [id, unused] : parameters) {
        forward.push_back(id);
    }
    const std::vector<ResourceLocation> reverse(forward.rbegin(), forward.rend());
    for (const std::int64_t seed : {std::int64_t{0}, std::int64_t{-1}, std::int64_t{7}}) {
        const NoiseRegistry all =
            NoiseRegistry::create(parameters, forward, seed, RandomSource::Legacy);
        const NoiseRegistry backwards =
            NoiseRegistry::create(parameters, reverse, seed, RandomSource::Legacy);
        for (const ResourceLocation& id : forward) {
            const NoiseRegistry alone = NoiseRegistry::create(
                parameters, std::vector<ResourceLocation>{id}, seed, RandomSource::Legacy);
            for (const double at : {0.0, 123.5, -40000.25}) {
                CHECK(bits(all.get(id).sample(at, 0.0, -at)) ==
                      bits(alone.get(id).sample(at, 0.0, -at)));
                CHECK(bits(all.get(id).sample(at, 0.0, -at)) ==
                      bits(backwards.get(id).sample(at, 0.0, -at)));
            }
        }
    }
}

TEST_CASE("the legacy surface depth matches the clean-room spec's vectors",
          "[surface][legacy][spec]") {
    // Q4.5: d(x, z) = trunc(N_surface(x, 0, z) * 2.75 + 3.0 + J(x, z) * 0.25),
    // J the first nextDouble of java.util.Random(F(W) ^ positionSeed(x, 0, z)).
    // Read through the executor a dimension runs, with a tree that reads a
    // depth so compile() binds `minecraft:surface` and the legacy jitter.
    const nlohmann::json& golden = constants().at("golden");
    auto parameters = shippedParameters();
    // A depth-reading tree also binds `minecraft:surface_secondary`
    // (surface::requiredNoises). With secondary_depth_range 0 below it is
    // never sampled, so a placeholder shape stands in for vanilla's.
    parameters.emplace(ResourceLocation::parse("minecraft:surface_secondary"),
                       NoiseParameters{.firstOctave = -1, .amplitudes = {1.0}});
    const stratum::surface::RuleGraph graph = stratum::surface::RuleGraph::resolve(
        nlohmann::json{
            {"type", "minecraft:condition"},
            {"if_true",
             {{"type", "minecraft:stone_depth"},
              {"offset", 0},
              {"add_surface_depth", true},
              {"secondary_depth_range", 0},
              {"surface_type", "floor"}}},
            {"then_run",
             {{"type", "minecraft:block"}, {"result_state", {{"Name", "minecraft:stone"}}}}}},
        ResourceLocation::parse("stratum:test"));
    const auto wanted = stratum::surface::requiredNoises(graph);
    const auto geometry = netherGeometry();

    const auto& columns = golden.at("surface_depth_columns");
    std::size_t compared = 0;
    for (const auto& [seedText, values] : golden.at("values").items()) {
        const std::int64_t seed = std::stoll(seedText);
        INFO("world seed " << seed);
        const NoiseRegistry registry =
            NoiseRegistry::create(parameters, wanted, seed, RandomSource::Legacy);
        const auto executor =
            stratum::surface::Executor::compile(graph, seed, geometry, &registry, 32);
        const auto& depths = values.at("surface_depth");
        REQUIRE(depths.size() == columns.size());
        for (std::size_t c = 0; c < columns.size(); ++c) {
            const auto x = columns[c][0].get<std::int32_t>();
            const auto z = columns[c][1].get<std::int32_t>();
            INFO("column (" << x << ", " << z << ")");
            CHECK(executor.surfaceDepth(x, z) == depths[c].get<std::int32_t>());
            ++compared;
        }
    }
    CHECK(compared == std::size_t{6} * 4U);
}

TEST_CASE("a legacy surface rule and the router get different noises under a climate key",
          "[surface][legacy][spec]") {
    // spec/legacy-named-noise-spec.md Q5.1c and Q5.2, and
    // IMPLEMENTER-RULING-01: under the legacy source the router reads
    // `minecraft:vegetation` as the built-in climate, while a surface rule
    // reading the same key gets the named construction. The two are held
    // apart, never one noise per key.
    const NoiseParameters shape{.firstOctave = -7, .amplitudes = {1.0, 1.0}};
    const std::map<ResourceLocation, NoiseParameters> parameters{
        {ResourceLocation::parse("minecraft:vegetation"), shape}};
    const std::vector<ResourceLocation> wanted{ResourceLocation::parse("minecraft:vegetation")};
    const ResourceLocation& vegetation = wanted.front();
    for (const std::int64_t seed : {std::int64_t{0}, std::int64_t{42}, std::int64_t{-7}}) {
        const NoiseRegistry legacy =
            NoiseRegistry::create(parameters, wanted, seed, RandomSource::Legacy);
        stratum::rng::JavaRandom world{seed};
        const auto expectedNamed = stratum::noise::NormalNoise::createNamedLegacy(
            world.nextLong(), "minecraft:vegetation", shape.firstOctave, shape.amplitudes);
        stratum::rng::JavaRandom climate{seed + 1};
        const auto expectedRouter = stratum::noise::NormalNoise::createLegacy(climate, -7, 2);
        bool differ = false;
        for (const double at : {0.0, 17.5, -2048.25, 99999.0}) {
            CHECK(bits(legacy.getNamed(vegetation).sample(at, 0.0, -at)) ==
                  bits(expectedNamed.sample(at, 0.0, -at)));
            CHECK(bits(legacy.get(vegetation).sample(at, 0.0, -at)) ==
                  bits(expectedRouter.sample(at, 0.0, -at)));
            differ = differ || bits(legacy.get(vegetation).sample(at, 0.0, -at)) !=
                                   bits(legacy.getNamed(vegetation).sample(at, 0.0, -at));
        }
        CHECK(differ);
    }

    // And a surface rule naming it compiles, reading the named path.
    const stratum::surface::RuleGraph graph = stratum::surface::RuleGraph::resolve(
        nlohmann::json{
            {"type", "minecraft:condition"},
            {"if_true",
             {{"type", "minecraft:noise_threshold"},
              {"noise", "minecraft:vegetation"},
              {"min_threshold", 0.0},
              {"max_threshold", 1.0}}},
            {"then_run",
             {{"type", "minecraft:block"}, {"result_state", {{"Name", "minecraft:stone"}}}}}},
        ResourceLocation::parse("stratum:test"));
    const NoiseRegistry legacy = NoiseRegistry::create(parameters, wanted, 0, RandomSource::Legacy);
    CHECK_NOTHROW(stratum::surface::Executor::compile(graph, 0, netherGeometry(), &legacy, 32));

    // Without the pack's parameters for the key, the named path has no noise
    // to give, and says which.
    const std::map<ResourceLocation, NoiseParameters> none;
    const NoiseRegistry bare = NoiseRegistry::create(none, wanted, 0, RandomSource::Legacy);
    CHECK(bare.findNamed(vegetation) == nullptr);
    CHECK_THROWS_WITH(bare.getNamed(vegetation), ContainsSubstring("minecraft:vegetation"));
}
