// Stratum — loading vanilla's own worldgen data.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The unit suite loads packs this repository wrote. This one loads the 952
// files vanilla ships, which is the first check that the loader copes with
// real identifiers, real nesting and the real spread of registries.
//
// Mojang-derived and never committed (SPEC §12); without the fixtures this
// SKIPs, naming the command that produces them.

#include <stratum/data/pack.hpp>
#include <stratum/data/registry.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/validate/pack_report.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace {

using stratum::data::Pack;
using stratum::data::Registry;
using stratum::data::ResourceLocation;

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

TEST_CASE("vanilla's worldgen data loads whole", "[conformance][data][pack]") {
    const std::filesystem::path tree = findWorldgenTree();
    if (tree.empty()) {
        SKIP("no extracted vanilla worldgen under "
             << STRATUM_FIXTURES_DIR
             << " — Mojang-derived and never committed (SPEC §12). Generate it with: "
                "tools/fetch-vanilla");
    }

    const Pack pack = Pack::openWorldgenTree(tree);

    // Every file is either executed or named. Nothing is quietly skipped,
    // which is the property SPEC §8 cares about most.
    std::size_t jsonFiles = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(tree)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            ++jsonFiles;
        }
    }
    CHECK(pack.size() + pack.rejected().size() == jsonFiles);
    CHECK(jsonFiles > 900);

    // The registries v1 executes, at the sizes 1.21.11 actually ships.
    CHECK(pack.entriesOf(Registry::DensityFunction).size() == 35U);
    CHECK(pack.entriesOf(Registry::Noise).size() == 60U);
    CHECK(pack.entriesOf(Registry::NoiseSettings).size() == 7U);
    CHECK(pack.entriesOf(Registry::Biome).size() == 65U);
    CHECK(pack.entriesOf(Registry::MultiNoiseBiomeSourceParameterList).size() == 2U);
    CHECK(pack.entriesOf(Registry::WorldPreset).size() == 6U);

    // And the ones it does not, which must have been reported rather than
    // silently ignored.
    std::map<Registry, std::size_t> rejectedByRegistry;
    for (const auto& rejected : pack.rejected()) {
        ++rejectedByRegistry[rejected.registry];
        CHECK_FALSE(stratum::data::isSupported(rejected.registry));
    }
    CHECK(rejectedByRegistry[Registry::PlacedFeature] == 258U);
    CHECK(rejectedByRegistry[Registry::ConfiguredFeature] == 224U);
    CHECK(rejectedByRegistry[Registry::TemplatePool] == 188U);
    CHECK(rejectedByRegistry[Registry::ConfiguredCarver] == 4U);

    for (const auto& entry : pack.entries()) {
        CAPTURE(entry.id.toString());
        CHECK(entry.id.namespaceName() == "minecraft");
        // A density function may be a bare number, the documented shorthand
        // for a constant, so "an entry is an object" is not true of all of
        // them — exactly one file in 1.21.11 takes that form, which only
        // real data would have told us.
        CHECK((entry.json.is_object() || entry.json.is_number()));
        // An identifier must survive being printed and read back, or an
        // error message could name something that does not resolve.
        CHECK(ResourceLocation::parse(entry.id.toString()) == entry.id);
        CHECK(pack.find(entry.registry, entry.id) == &entry);
    }
}

TEST_CASE("the settings the pipeline will need are all present and addressable",
          "[conformance][data][pack]") {
    const std::filesystem::path tree = findWorldgenTree();
    if (tree.empty()) {
        SKIP("no extracted vanilla worldgen under " << STRATUM_FIXTURES_DIR);
    }

    const Pack pack = Pack::openWorldgenTree(tree);

    // One noise_settings per dimension, each with the router M3 will walk.
    for (const std::string_view name : {"overworld", "nether", "end"}) {
        CAPTURE(name);
        const auto* settings =
            pack.find(Registry::NoiseSettings, ResourceLocation::parse(std::string(name)));
        REQUIRE(settings != nullptr);
        CHECK(settings->json.contains("noise_router"));
        CHECK(settings->json.contains("surface_rule"));
        CHECK(settings->json.at("noise").contains("min_y"));
    }

    // The constant shorthand, pinned because the evaluator must handle it:
    // density_function/zero.json is the number 0.0, not an object.
    const auto* zero = pack.find(Registry::DensityFunction, ResourceLocation::parse("zero"));
    REQUIRE(zero != nullptr);
    CHECK(zero->json.is_number());
    // Compared as bits: the project keeps -Wfloat-equal on, and this is an
    // exact-value check, not a tolerance.
    CHECK(std::bit_cast<std::uint64_t>(zero->json.get<double>()) == 0U);

    // The density function every dimension's terrain is built on.
    for (const std::string_view path :
         {"overworld/base_3d_noise", "nether/base_3d_noise", "end/base_3d_noise"}) {
        CAPTURE(path);
        const auto* function =
            pack.find(Registry::DensityFunction, ResourceLocation::parse(std::string(path)));
        REQUIRE(function != nullptr);
        CHECK(function->json.at("type") == "minecraft:old_blended_noise");
    }
}

TEST_CASE("vanilla's own data validates, with an exact account of what is left",
          "[conformance][validate]") {
    const std::filesystem::path tree = findWorldgenTree();
    if (tree.empty()) {
        SKIP("no extracted vanilla worldgen under "
             << STRATUM_FIXTURES_DIR
             << " — Mojang-derived and never committed (SPEC §12). Generate it with: "
                "tools/fetch-vanilla");
    }

    const stratum::data::Pack pack = stratum::data::Pack::open(tree);
    CHECK(pack.layout() == stratum::data::Pack::Layout::WorldgenTree);

    const stratum::validate::Report report = stratum::validate::validatePack(pack);

    // Vanilla's own data must never fail to load. That is the line SPEC §8's
    // open question turns on: a reading of "hard error" that makes Mojang's
    // shipped data unloadable is the wrong reading.
    CHECK(report.count(stratum::validate::Severity::Error) == 0U);
    CHECK(report.resolved);

    // Numbers, not "some": these move when the data moves or when a
    // milestone lands, and both are things a person should have to look at
    // rather than have slide past.
    CHECK(report.densityFunctions == 35U);
    CHECK(report.evaluable == 33U);
    // The routers reach fourteen noises no named density function does —
    // the aquifer and ore-vein ones — so this is larger than the 25 the
    // density functions alone reference.
    CHECK(report.noisesReferenced == 39U);
    // 275 named nodes plus what the seven routers add inline.
    CHECK(report.nodes == 730U);
    CHECK(report.splines == 354U);

    // The routers are resolved into the same graph, so the settings are
    // checked here too — but only the dimensions whose noises this build can
    // actually seed. Four of vanilla's seven declare `legacy_random_source`,
    // and their entries are left unchecked rather than counted as failures:
    // "we did not look" is not "we looked and it does not work".
    CHECK(report.noiseSettings == 7U);
    CHECK(report.dimensionsChecked == 3U);
    CHECK(report.routerEntries == 45U);
    CHECK(report.routerEntriesEvaluable == 45U);

    // The six are preliminary_surface_level and final_density in each of the
    // three, waiting on find_top_surface and old_blended_noise respectively.
    std::vector<std::string> unseeded;
    for (const stratum::validate::Finding& finding : report.findings) {
        // Match the phrase the *dimension* refusal opens with, not the bare
        // registry name: old_blended_noise's refusal names
        // `legacy_random_source` too, now that the seeding is the only thing
        // left unsettled about it, and a looser test silently counted every
        // final_density here as an unseedable dimension.
        if (finding.message.find("this dimension declares legacy_random_source") !=
            std::string::npos) {
            unseeded.push_back(finding.subject);
        }
    }
    std::ranges::sort(unseeded);
    CHECK(unseeded == std::vector<std::string>{"minecraft:caves", "minecraft:end",
                                               "minecraft:floating_islands", "minecraft:nether"});

    // The ten that are left are exactly the ones SPEC §11 accounts for, and
    // no others. A refusal that spread to a function nobody expected would
    // otherwise just look like a bigger number. Router entries carry a space
    // in their subject — "minecraft:overworld final_density" — and are
    // counted above rather than listed here.
    std::vector<std::string> unevaluable;
    for (const stratum::validate::Finding& finding : report.findings) {
        // Selected on the message, not the subject: a legacy dimension's
        // warning is also about a "minecraft:" subject with no space in it,
        // and it says something entirely different.
        if (finding.severity == stratum::validate::Severity::Warning &&
            finding.subject.find(' ') == std::string::npos &&
            finding.message.find("cannot be evaluated by this build") != std::string::npos) {
            unevaluable.push_back(finding.subject);
        }
    }
    std::ranges::sort(unevaluable);
    const std::vector<std::string> expected{
        "minecraft:end/sloped_cheese",
        "minecraft:overworld/caves/noodle",
    };
    CHECK(unevaluable == expected);

    // Features and their neighbours are reported by registry, never dropped.
    const auto features =
        std::ranges::find_if(report.findings, [](const stratum::validate::Finding& finding) {
            return finding.subject == "worldgen/placed_feature";
        });
    REQUIRE(features != report.findings.end());
    CHECK(features->severity == stratum::validate::Severity::Warning);
}
