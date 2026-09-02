// Stratum — pack validation tests.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Validation exists to be believed, so the thing worth testing is that it
// does not lie in either direction: it must not call a working pack broken,
// and — the failure that matters — it must not call a broken pack clean.
// Most of what follows is a pack with one deliberate fault, checked to see
// that exactly that fault is named.
//
// The other half is the distinction the report is built around. A registry
// this engine does not execute and a density function this build cannot yet
// evaluate are both "no", and reporting them as the same kind of no would
// tell whoever holds the pack to go and fix something that is not their
// problem.

#include <stratum/data/pack.hpp>
#include <stratum/data/registry.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/validate/pack_report.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace {

using Catch::Matchers::ContainsSubstring;
using stratum::data::Pack;
using stratum::data::Registry;
using stratum::validate::Finding;
using stratum::validate::Report;
using stratum::validate::Severity;

class TempTree {
public:
    TempTree() : path_(std::filesystem::temp_directory_path() / uniqueName()) {
        std::filesystem::remove_all(path_);
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

    /// Writes into any registry directory, including one this build does not
    /// execute.
    const TempTree& defineIn(std::string_view registry, std::string_view name,
                             std::string_view json) const {
        std::filesystem::create_directories(path_ / registry);
        return write(registry, name, json);
    }

    [[nodiscard]] Pack pack() const { return Pack::open(path_); }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    const TempTree& write(std::string_view registry, std::string_view name,
                          std::string_view json) const {
        const std::filesystem::path file = path_ / registry / (std::string(name) + ".json");
        std::ofstream out(file);
        out << json;
        return *this;
    }

    [[nodiscard]] static std::string uniqueName() {
        static int counter = 0;
        return "stratum-validate-test-" + std::to_string(++counter);
    }

    std::filesystem::path path_;
};

/// A pack with nothing wrong with it: one noise, one function over it.
void defineWorkingPack(const TempTree& tree) {
    tree.defineNoise("test", R"({"firstOctave":-4,"amplitudes":[1.0,1.0]})");
    tree.define("field", R"({"type":"minecraft:noise","noise":"test",
        "xz_scale":1.0,"y_scale":0.0})");
}

/// A valid settings file whose `final_density` points at @p finalDensity.
void defineSettings(const TempTree& tree, std::string_view name, std::string_view finalDensity,
                    bool legacyRandomSource = false) {
    nlohmann::json router = nlohmann::json::object();
    for (std::size_t i = 0; i < stratum::settings::kRouterEntryCount; ++i) {
        router[std::string(stratum::settings::routerEntryName(
            static_cast<stratum::settings::RouterEntry>(i)))] = 0.0;
    }
    router["final_density"] = std::string(finalDensity);

    const nlohmann::json json{
        {"default_block", {{"Name", "minecraft:stone"}}},
        {"default_fluid", {{"Name", "minecraft:water"}}},
        {"sea_level", 63},
        {"disable_mob_generation", false},
        {"aquifers_enabled", true},
        {"ore_veins_enabled", true},
        {"legacy_random_source", legacyRandomSource},
        {"noise", {{"min_y", -64}, {"height", 384}, {"size_horizontal", 1}, {"size_vertical", 2}}},
        {"noise_router", router},
        {"spawn_target", nlohmann::json::array()},
        {"surface_rule", {{"type", "minecraft:bandlands"}}},
    };
    tree.defineIn("noise_settings", name, json.dump());
}

[[nodiscard]] const Finding* findingAbout(const Report& report, std::string_view subject) {
    const auto found = std::ranges::find_if(
        report.findings, [subject](const Finding& finding) { return finding.subject == subject; });
    return found == report.findings.end() ? nullptr : &*found;
}

} // namespace

TEST_CASE("a pack with nothing wrong with it reports nothing", "[validate]") {
    const TempTree tree;
    defineWorkingPack(tree);

    const Report report = stratum::validate::validatePack(tree.pack());

    CHECK(report.clean());
    CHECK(report.findings.empty());
    CHECK(report.resolved);
    CHECK(report.densityFunctions == 1U);
    CHECK(report.evaluable == 1U);
    CHECK(report.noisesReferenced == 1U);
    CHECK(report.nodes == 1U);
    CHECK(report.splines == 0U);
}

TEST_CASE("a registry this engine does not execute is a warning, not an error", "[validate]") {
    const TempTree tree;
    defineWorkingPack(tree);
    tree.defineIn("placed_feature", "a", "{}");
    tree.defineIn("placed_feature", "b", "{}");
    tree.defineIn("structure", "c", "{}");

    const Report report = stratum::validate::validatePack(tree.pack());

    // The pack is not wrong. This engine is not going to run that part of
    // it, by design (SPEC §8), and saying "error" would send someone off to
    // fix something that is not broken.
    CHECK(report.count(Severity::Error) == 0U);
    CHECK(report.resolved);
    CHECK(report.evaluable == 1U);

    const Finding* features = findingAbout(report, "worldgen/placed_feature");
    REQUIRE(features != nullptr);
    CHECK(features->severity == Severity::Warning);
    // The count, not just the fact: two entries and two hundred are
    // different situations.
    CHECK_THAT(features->message, ContainsSubstring("2 entries"));

    const Finding* structures = findingAbout(report, "worldgen/structure");
    REQUIRE(structures != nullptr);
    CHECK_THAT(structures->message, ContainsSubstring("1 entry"));
}

TEST_CASE("a function this build cannot evaluate is named with its reason", "[validate]") {
    const TempTree tree;
    defineWorkingPack(tree);
    tree.define("blended", R"({"type":"minecraft:old_blended_noise","xz_scale":1.0,
        "y_scale":1.0,"xz_factor":80.0,"y_factor":160.0,"smear_scale_multiplier":8.0})");

    const Report report = stratum::validate::validatePack(tree.pack());

    CHECK(report.count(Severity::Error) == 0U);
    CHECK(report.densityFunctions == 2U);
    // Counted, so a caller can say "3 of 35" rather than only listing the
    // failures.
    CHECK(report.evaluable == 1U);

    const Finding* finding = findingAbout(report, "minecraft:blended");
    REQUIRE(finding != nullptr);
    CHECK(finding->severity == Severity::Warning);
    CHECK_THAT(finding->message, ContainsSubstring("minecraft:old_blended_noise"));
}

TEST_CASE("a registry that loads but is not interpreted yet says so", "[validate]") {
    const TempTree tree;
    defineWorkingPack(tree);
    tree.defineIn("biome", "plains", "{}");

    const Report report = stratum::validate::validatePack(tree.pack());

    // This is the honest half of a clean report. Biomes load and nothing
    // reads them, so a pack whose biomes are nonsense passes today — and
    // silence about that would read as approval.
    const Finding* finding = findingAbout(report, "worldgen/biome");
    REQUIRE(finding != nullptr);
    CHECK(finding->severity == Severity::Note);
    CHECK_THAT(finding->message, ContainsSubstring("not"));

    // A note is not a problem: the pack is still clean.
    CHECK(report.clean());

    const auto biomes =
        std::ranges::find_if(report.registries, [](const stratum::validate::RegistryCount& count) {
            return count.registry == Registry::Biome;
        });
    REQUIRE(biomes != report.registries.end());
    CHECK(biomes->supported);
    CHECK_FALSE(biomes->interpreted);
}

TEST_CASE("a graph that will not resolve is an error, and says it stopped early", "[validate]") {
    const TempTree tree;
    tree.define("a", R"({"type":"minecraft:abs","argument":"b"})");
    tree.define("b", R"({"type":"minecraft:abs","argument":"a"})");

    const Report report = stratum::validate::validatePack(tree.pack());

    CHECK_FALSE(report.resolved);
    CHECK_FALSE(report.clean());
    REQUIRE(report.count(Severity::Error) == 1U);
    CHECK_THAT(report.findings.front().message, ContainsSubstring("cycle"));
    // Resolution is all-or-nothing, so a caller who fixes this and finds
    // another problem should not conclude the report was lying.
    CHECK_THAT(report.findings.front().message, ContainsSubstring("stops at the first problem"));
}

TEST_CASE("a dangling reference is an error", "[validate]") {
    const TempTree tree;
    tree.define("a", R"({"type":"minecraft:abs","argument":"nowhere"})");

    const Report report = stratum::validate::validatePack(tree.pack());

    CHECK_FALSE(report.resolved);
    REQUIRE(report.count(Severity::Error) == 1U);
    CHECK_THAT(report.findings.front().message, ContainsSubstring("minecraft:nowhere"));
}

TEST_CASE("every missing noise is reported, not just the first", "[validate]") {
    const TempTree tree;
    tree.define("one", R"({"type":"minecraft:noise","noise":"missing_a",
        "xz_scale":1.0,"y_scale":0.0})");
    tree.define("two", R"({"type":"minecraft:noise","noise":"missing_b",
        "xz_scale":1.0,"y_scale":0.0})");
    tree.defineNoise("broken", R"({"firstOctave":"nonsense","amplitudes":[1.0]})");
    tree.define("three", R"({"type":"minecraft:noise","noise":"broken",
        "xz_scale":1.0,"y_scale":0.0})");

    const Report report = stratum::validate::validatePack(tree.pack());

    // The graph resolves — a noise is only a name until something builds it
    // — and then all three problems come out at once. Stopping at the first
    // would mean three runs to learn what one should have said.
    CHECK(report.resolved);
    CHECK(report.count(Severity::Error) == 3U);

    REQUIRE(findingAbout(report, "minecraft:missing_a") != nullptr);
    REQUIRE(findingAbout(report, "minecraft:missing_b") != nullptr);
    const Finding* broken = findingAbout(report, "minecraft:broken");
    REQUIRE(broken != nullptr);
    CHECK_THAT(broken->message, ContainsSubstring("firstOctave"));
}

TEST_CASE("findings come worst first", "[validate]") {
    const TempTree tree;
    defineWorkingPack(tree);
    tree.defineIn("biome", "plains", "{}");
    tree.defineIn("placed_feature", "a", "{}");
    tree.define("blended", R"({"type":"minecraft:old_blended_noise","xz_scale":1.0,
        "y_scale":1.0,"xz_factor":80.0,"y_factor":160.0,"smear_scale_multiplier":8.0})");

    const Report report = stratum::validate::validatePack(tree.pack());

    REQUIRE(report.findings.size() >= 3U);
    CHECK(std::ranges::is_sorted(report.findings, [](const Finding& left, const Finding& right) {
        return left.severity > right.severity;
    }));
    // A long report should open with what stops the pack working, not with
    // notes about registries nobody asked about.
    CHECK(report.findings.front().severity == Severity::Warning);
    CHECK(report.findings.back().severity == Severity::Note);
}

TEST_CASE("validation is deterministic", "[validate]") {
    const TempTree tree;
    defineWorkingPack(tree);
    tree.defineIn("placed_feature", "a", "{}");
    tree.define("blended", R"({"type":"minecraft:old_blended_noise","xz_scale":1.0,
        "y_scale":1.0,"xz_factor":80.0,"y_factor":160.0,"smear_scale_multiplier":8.0})");

    const Pack pack = tree.pack();
    const Report first = stratum::validate::validatePack(pack);
    const Report second = stratum::validate::validatePack(pack);

    REQUIRE(first.findings.size() == second.findings.size());
    for (std::size_t i = 0; i < first.findings.size(); ++i) {
        CAPTURE(i);
        CHECK(first.findings[i].severity == second.findings[i].severity);
        CHECK(first.findings[i].subject == second.findings[i].subject);
        CHECK(first.findings[i].message == second.findings[i].message);
    }
}

TEST_CASE("Pack::open finds either layout, and names all three when it finds none",
          "[validate][pack]") {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "stratum-open-layout-test";
    std::filesystem::remove_all(root);

    // A worldgen tree: density_function/ at the root.
    std::filesystem::create_directories(root / "tree" / "density_function");
    {
        std::ofstream out(root / "tree" / "density_function" / "a.json");
        out << "1.0";
    }
    CHECK(Pack::open(root / "tree").layout() == Pack::Layout::WorldgenTree);

    // The shape tools/fetch-vanilla leaves: one level down under worldgen/.
    std::filesystem::create_directories(root / "nested" / "worldgen" / "density_function");
    {
        std::ofstream out(root / "nested" / "worldgen" / "density_function" / "a.json");
        out << "1.0";
    }
    CHECK(Pack::open(root / "nested").layout() == Pack::Layout::WorldgenTree);

    // A data pack: pack.mcmeta beside data/.
    std::filesystem::create_directories(root / "pack" / "data" / "test" / "worldgen" /
                                        "density_function");
    {
        std::ofstream out(root / "pack" / "pack.mcmeta");
        out << R"({"pack":{"description":"x","min_format":94,"max_format":95}})";
    }
    {
        std::ofstream out(root / "pack" / "data" / "test" / "worldgen" / "density_function" /
                          "a.json");
        out << "1.0";
    }
    CHECK(Pack::open(root / "pack").layout() == Pack::Layout::DataPack);

    // Nothing at all: the refusal names every layout it looked for, because
    // "not a pack" is not something anyone can act on.
    std::filesystem::create_directories(root / "empty");
    CHECK_THROWS_WITH(Pack::open(root / "empty"),
                      ContainsSubstring("pack.mcmeta") && ContainsSubstring("density_function"));

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_CASE("noise settings that will not load are an error", "[validate]") {
    const TempTree tree;
    defineWorkingPack(tree);
    tree.defineIn("noise_settings", "overworld", "{}");

    const Report report = stratum::validate::validatePack(tree.pack());

    // Settings are interpreted as of M3's first slice, so an empty one is no
    // longer a file nobody looks at — it is a dimension that cannot be built.
    CHECK_FALSE(report.clean());
    REQUIRE(report.count(Severity::Error) == 1U);
    CHECK_THAT(report.findings.front().message,
               ContainsSubstring("minecraft:overworld") && ContainsSubstring("default_block"));
}

TEST_CASE("a router entry this build cannot evaluate is named with its dimension", "[validate]") {
    const TempTree tree;
    defineWorkingPack(tree);
    tree.define("blended", R"({"type":"minecraft:old_blended_noise","xz_scale":1.0,
        "y_scale":1.0,"xz_factor":80.0,"y_factor":160.0,"smear_scale_multiplier":8.0})");
    defineSettings(tree, "overworld", "blended");

    const Report report = stratum::validate::validatePack(tree.pack());

    CHECK(report.count(Severity::Error) == 0U);
    CHECK(report.routerEntries == stratum::settings::kRouterEntryCount);
    // Fourteen of fifteen: only the entry that was pointed at the
    // unevaluable function fails, and the report says which dimension it
    // belongs to — a pack with seven of them needs that.
    CHECK(report.routerEntriesEvaluable == stratum::settings::kRouterEntryCount - 1U);

    const Finding* finding = findingAbout(report, "minecraft:overworld final_density");
    REQUIRE(finding != nullptr);
    CHECK(finding->severity == Severity::Warning);
    CHECK_THAT(finding->message, ContainsSubstring("minecraft:old_blended_noise"));
}

TEST_CASE("a dimension this build cannot seed is a warning, and is left unchecked", "[validate]") {
    const TempTree tree;
    defineWorkingPack(tree);
    defineSettings(tree, "overworld", "field", /*legacyRandomSource=*/false);
    defineSettings(tree, "nether", "field", /*legacyRandomSource=*/true);

    const Report report = stratum::validate::validatePack(tree.pack());

    // Not an error: the pack is fine, this build cannot seed that dimension.
    CHECK(report.count(Severity::Error) == 0U);
    CHECK(report.noiseSettings == 2U);

    // Left out of the counts entirely rather than counted as failures.
    // Reporting fifteen unevaluable entries would say we looked and they did
    // not work, and we did not look.
    CHECK(report.dimensionsChecked == 1U);
    CHECK(report.routerEntries == stratum::settings::kRouterEntryCount);
    CHECK(report.routerEntriesEvaluable == stratum::settings::kRouterEntryCount);

    const Finding* finding = findingAbout(report, "minecraft:nether");
    REQUIRE(finding != nullptr);
    CHECK(finding->severity == Severity::Warning);
    CHECK_THAT(finding->message, ContainsSubstring("legacy_random_source"));

    // And nothing was said about the dimension that is fine.
    CHECK(findingAbout(report, "minecraft:overworld") == nullptr);
}
