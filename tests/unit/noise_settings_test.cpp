// Stratum — noise settings loading tests.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Noise settings are where a dimension's shape is decided, so almost every
// mistake here is one the cell sampler would inherit and then hide: a height
// that is not a whole number of cells becomes a half cell at the top of the
// world, a router entry read from the wrong key becomes terrain generated
// from the wrong function. The loader refuses all of them, and this is where
// that is checked.
//
// The field names are generated from mcdoc rather than written here, so the
// tests do not restate them either — where a name appears below it is
// because that particular field's *handling* is under test.

#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/graph.hpp>
#include <stratum/settings/noise_settings.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace {

using Catch::Matchers::ContainsSubstring;
using stratum::data::Pack;
using stratum::data::ResourceLocation;
using stratum::settings::LoadedSettings;
using stratum::settings::NoiseSettings;
using stratum::settings::RouterEntry;
using stratum::settings::SettingsError;

class TempTree {
public:
    TempTree() : path_(std::filesystem::temp_directory_path() / uniqueName()) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_ / "density_function");
        std::filesystem::create_directories(path_ / "noise_settings");
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

    const TempTree& defineSettings(std::string_view name, const nlohmann::json& json) const {
        return write("noise_settings", name, json.dump());
    }

    [[nodiscard]] Pack pack() const { return Pack::open(path_); }

    [[nodiscard]] LoadedSettings load() const { return stratum::settings::loadAll(pack()); }

private:
    const TempTree& write(std::string_view registry, std::string_view name,
                          std::string_view json) const {
        std::ofstream out(path_ / registry / (std::string(name) + ".json"));
        out << json;
        return *this;
    }

    [[nodiscard]] static std::string uniqueName() {
        static int counter = 0;
        return "stratum-settings-test-" + std::to_string(++counter);
    }

    std::filesystem::path path_;
};

/// A settings file with nothing wrong with it. Built through the generated
/// router field list rather than by writing fifteen names out, so that a
/// schema change moves the tests with it instead of leaving them stale.
[[nodiscard]] nlohmann::json workingSettings() {
    nlohmann::json router = nlohmann::json::object();
    for (std::size_t i = 0; i < stratum::settings::kRouterEntryCount; ++i) {
        router[std::string(stratum::settings::routerEntryName(static_cast<RouterEntry>(i)))] = 0.0;
    }

    return nlohmann::json{
        {"default_block", {{"Name", "minecraft:stone"}}},
        {"default_fluid", {{"Name", "minecraft:water"}, {"Properties", {{"level", "0"}}}}},
        {"sea_level", 63},
        {"disable_mob_generation", false},
        {"aquifers_enabled", true},
        {"ore_veins_enabled", true},
        {"legacy_random_source", false},
        {"noise", {{"min_y", -64}, {"height", 384}, {"size_horizontal", 1}, {"size_vertical", 2}}},
        {"noise_router", router},
        {"spawn_target", nlohmann::json::array()},
        {"surface_rule", {{"type", "minecraft:bandlands"}}},
    };
}

} // namespace

TEST_CASE("a settings file with nothing wrong with it loads whole", "[settings]") {
    const TempTree tree;
    tree.defineSettings("overworld", workingSettings());

    const LoadedSettings loaded = tree.load();
    REQUIRE(loaded.settings.size() == 1U);
    const NoiseSettings& settings = loaded.settings.begin()->second;

    CHECK(settings.id == ResourceLocation::parse("minecraft:overworld"));
    CHECK(settings.defaultBlock.name == ResourceLocation::parse("minecraft:stone"));
    CHECK(settings.defaultFluid.name == ResourceLocation::parse("minecraft:water"));
    REQUIRE(settings.defaultFluid.properties.size() == 1U);
    CHECK(settings.defaultFluid.properties.at("level") == "0");
    CHECK(settings.seaLevel == 63);
    CHECK_FALSE(settings.disableMobGeneration);
    CHECK(settings.aquifersEnabled);
    CHECK(settings.oreVeinsEnabled);
    CHECK_FALSE(settings.legacyRandomSource);

    // The cell shape the sampler will work in: size 1 and 2 mean 4 blocks
    // wide and 8 tall, which is the only unit vanilla's schema allows.
    CHECK(settings.geometry.minY == -64);
    CHECK(settings.geometry.height == 384);
    CHECK(settings.geometry.cellWidth() == 4);
    CHECK(settings.geometry.cellHeight() == 8);
    CHECK(settings.geometry.maxY() == 320);
    CHECK(settings.geometry.cellCountY() == 48);

    // Kept, not interpreted (M4), and kept verbatim so a stored pipeline can
    // round-trip them.
    CHECK(settings.surfaceRule.at("type") == "minecraft:bandlands");
    CHECK(settings.spawnTarget.is_array());
}

TEST_CASE("a field the schema does not declare is refused by name", "[settings]") {
    const TempTree tree;
    nlohmann::json json = workingSettings();
    // The name this field had before 1.21.9. A pack written for an older
    // version must be told, not half-loaded.
    json["initial_density_without_jaggedness"] = 0.0;
    tree.defineSettings("overworld", json);

    CHECK_THROWS_WITH(tree.load(), ContainsSubstring("initial_density_without_jaggedness") &&
                                       ContainsSubstring("1.21.11"));
}

TEST_CASE("a missing field is refused by name", "[settings]") {
    const TempTree tree;
    nlohmann::json json = workingSettings();
    json.erase("sea_level");
    tree.defineSettings("overworld", json);

    CHECK_THROWS_WITH(tree.load(), ContainsSubstring("sea_level") && ContainsSubstring("missing"));
}

TEST_CASE("the router must carry exactly the entries the schema declares", "[settings]") {
    SECTION("a missing entry") {
        const TempTree tree;
        nlohmann::json json = workingSettings();
        json["noise_router"].erase("final_density");
        tree.defineSettings("overworld", json);

        CHECK_THROWS_WITH(tree.load(),
                          ContainsSubstring("final_density") && ContainsSubstring("missing"));
    }

    SECTION("an entry that is not one") {
        const TempTree tree;
        nlohmann::json json = workingSettings();
        json["noise_router"]["initial_density_without_jaggedness"] = 0.0;
        tree.defineSettings("overworld", json);

        CHECK_THROWS_WITH(tree.load(), ContainsSubstring("initial_density_without_jaggedness"));
    }
}

TEST_CASE("the geometry is checked against the schema's own ranges", "[settings]") {
    const auto loadWith = [](const nlohmann::json& noise) {
        const TempTree tree;
        nlohmann::json json = workingSettings();
        json["noise"] = noise;
        tree.defineSettings("overworld", json);
        return tree.load();
    };

    // size_horizontal is `int @ 1..4` in mcdoc; the bound is generated, not
    // written here, so this also checks the range survived the generator.
    CHECK_THROWS_WITH(
        loadWith({{"min_y", -64}, {"height", 384}, {"size_horizontal", 5}, {"size_vertical", 2}}),
        ContainsSubstring("size_horizontal") && ContainsSubstring("[1, 4]"));

    CHECK_THROWS_WITH(
        loadWith({{"min_y", -64}, {"height", 384}, {"size_horizontal", 0}, {"size_vertical", 2}}),
        ContainsSubstring("size_horizontal"));

    CHECK_THROWS_WITH(
        loadWith(
            {{"min_y", -100000}, {"height", 384}, {"size_horizontal", 1}, {"size_vertical", 2}}),
        ContainsSubstring("min_y"));
}

TEST_CASE("a world that is not a whole number of cells is refused", "[settings]") {
    const auto loadWith = [](int minY, int height, int sizeVertical) {
        const TempTree tree;
        nlohmann::json json = workingSettings();
        json["noise"] = {{"min_y", minY},
                         {"height", height},
                         {"size_horizontal", 1},
                         {"size_vertical", sizeVertical}};
        tree.defineSettings("overworld", json);
        return tree.load();
    };

    // The cell-height check has exactly one way to bite, and it is worth
    // being precise about it: cell height is size_vertical * 4, so 4, 8, 12
    // or 16 blocks, and a height that is a whole number of 16-block sections
    // is automatically a whole number of 4, 8 and 16. Twelve is the only
    // divisor that can fail — which is why a naive test with size_vertical 2
    // passes whatever height it is given, and why this one uses 3.
    CHECK_THROWS_WITH(loadWith(-64, 400, 3), ContainsSubstring("12-block cells"));
    CHECK_NOTHROW(loadWith(-64, 384, 3));

    CHECK_THROWS_WITH(loadWith(-64, 200, 2), ContainsSubstring("sections"));
    CHECK_THROWS_WITH(loadWith(-60, 384, 2), ContainsSubstring("min_y"));
    // A cell height that divides the world exactly is fine.
    CHECK_NOTHROW(loadWith(-64, 384, 4));
}

TEST_CASE("block states are read strictly", "[settings]") {
    const auto loadWith = [](const nlohmann::json& block) {
        const TempTree tree;
        nlohmann::json json = workingSettings();
        json["default_block"] = block;
        tree.defineSettings("overworld", json);
        return tree.load();
    };

    CHECK_THROWS_WITH(loadWith(nlohmann::json::object()), ContainsSubstring("Name"));
    CHECK_THROWS_WITH(loadWith({{"Name", 5}}), ContainsSubstring("Name"));
    CHECK_THROWS_WITH(loadWith({{"Name", "Minecraft:Stone"}}), ContainsSubstring("default_block"));
    // Vanilla writes every property value as a string, even the numeric
    // ones. Accepting a bare number would make two spellings of one state.
    CHECK_THROWS_WITH(loadWith({{"Name", "minecraft:water"}, {"Properties", {{"level", 0}}}}),
                      ContainsSubstring("level"));
}

TEST_CASE("a router entry that will not resolve names the entry", "[settings]") {
    const TempTree tree;
    nlohmann::json json = workingSettings();
    json["noise_router"]["final_density"] = "nowhere";
    tree.defineSettings("overworld", json);

    // The resolver knows the chain it walked but not which router entry the
    // chain began at, because an inline function has no identifier of its
    // own. Without the entry name this error would be unactionable.
    CHECK_THROWS_WITH(tree.load(),
                      ContainsSubstring("final_density") && ContainsSubstring("minecraft:nowhere"));
}

TEST_CASE("a router shares one graph with the pack's named functions", "[settings]") {
    const TempTree tree;
    tree.define("shared", R"({"type":"minecraft:abs","argument":-2.0})");
    nlohmann::json json = workingSettings();
    json["noise_router"]["final_density"] = "shared";
    json["noise_router"]["depth"] = "shared";
    tree.defineSettings("overworld", json);

    const LoadedSettings loaded = tree.load();
    const NoiseSettings& settings = loaded.settings.begin()->second;

    // Not a copy: a router that resolved its references into a second graph
    // would sample the same noise twice per point and give one pipeline two
    // node numbering schemes.
    const auto shared = loaded.graph.rootOf(ResourceLocation::parse("minecraft:shared"));
    CHECK(settings.router.at(RouterEntry::FinalDensity) == shared);
    CHECK(settings.router.at(RouterEntry::Depth) == shared);
}

TEST_CASE("router entries are named and counted from the schema", "[settings]") {
    // Fifteen at 1.21.11. The number is asserted because it changes between
    // versions and should not change quietly.
    CHECK(stratum::settings::kRouterEntryCount == 15U);
    CHECK(stratum::settings::routerEntryName(RouterEntry::FinalDensity) == "final_density");
    CHECK(stratum::settings::routerEntryName(RouterEntry::Barrier) == "barrier");
    // The 1.21.9 rename, which is the reason this list is generated at all.
    CHECK(stratum::settings::routerEntryName(RouterEntry::PreliminarySurfaceLevel) ==
          "preliminary_surface_level");

    for (std::size_t i = 0; i < stratum::settings::kRouterEntryCount; ++i) {
        CAPTURE(i);
        CHECK_FALSE(stratum::settings::routerEntryName(static_cast<RouterEntry>(i)).empty());
    }
}

TEST_CASE("a pack with no noise settings loads a graph and no settings", "[settings]") {
    const TempTree tree;
    tree.define("lonely", "1.0");

    const LoadedSettings loaded = tree.load();
    CHECK(loaded.settings.empty());
    CHECK(loaded.graph.nodeCount() == 1U);
}
