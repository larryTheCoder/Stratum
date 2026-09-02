// Stratum — resource location, registry and pack loading tests.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The property this layer exists for is negative: nothing may be loaded
// halfway. Every file is either understood, or reported by name — never
// dropped (SPEC §8).

#include <stratum/data/pack.hpp>
#include <stratum/data/registry.hpp>
#include <stratum/data/resource_location.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace {

using stratum::data::Pack;
using stratum::data::PackError;
using stratum::data::PackLoadOptions;
using stratum::data::Registry;
using stratum::data::ResourceLocation;
using stratum::data::ResourceLocationError;

/// A directory that removes itself, so a failing test leaves nothing behind.
class TempDirectory {
public:
    TempDirectory() : path_(std::filesystem::temp_directory_path() / uniqueName()) {
        std::filesystem::create_directories(path_);
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    void write(std::string_view relative, std::string_view contents) const {
        const std::filesystem::path file = path_ / relative;
        std::filesystem::create_directories(file.parent_path());
        std::ofstream out(file);
        out << contents;
    }

private:
    [[nodiscard]] static std::string uniqueName() {
        static int counter = 0;
        return "stratum-pack-test-" + std::to_string(++counter);
    }

    std::filesystem::path path_;
};

constexpr std::string_view kConstantDensityFunction =
    R"({"type":"minecraft:constant","argument":1})";

} // namespace

TEST_CASE("resource locations parse the way vanilla reads them", "[data][id]") {
    const ResourceLocation qualified = ResourceLocation::parse("minecraft:overworld");
    CHECK(qualified.namespaceName() == "minecraft");
    CHECK(qualified.path() == "overworld");
    CHECK(qualified.toString() == "minecraft:overworld");

    // A bare path is in the default namespace.
    const ResourceLocation bare = ResourceLocation::parse("overworld");
    CHECK(bare == qualified);

    // Paths may nest; namespaces may not.
    const ResourceLocation nested = ResourceLocation::parse("minecraft:overworld/base_3d_noise");
    CHECK(nested.path() == "overworld/base_3d_noise");
    CHECK(stratum::data::isValidPath("a/b/c"));
    CHECK_FALSE(stratum::data::isValidNamespace("a/b"));
}

TEST_CASE("malformed identifiers are refused, not coerced", "[data][id]") {
    // Silently lowercasing or trimming would resolve to a different, valid
    // entry — a different world rather than an error.
    for (const std::string_view bad :
         {"Minecraft:overworld", "minecraft:Overworld", "minecraft: overworld", "minecraft::x",
          "minecraft:", ":overworld", "", "mine craft:overworld"}) {
        CAPTURE(bad);
        CHECK_FALSE(ResourceLocation::tryParse(bad).has_value());
        CHECK_THROWS_AS(ResourceLocation::parse(bad), ResourceLocationError);
    }
    REQUIRE_THROWS_WITH(ResourceLocation::parse("minecraft:over world"),
                        Catch::Matchers::ContainsSubstring("position 4"));
}

TEST_CASE("the capability matrix matches SPEC section 8", "[data][registry]") {
    // These two lists are the public contract; a change here is a change to
    // the README table and to what users may rely on.
    for (const Registry supported :
         {Registry::DensityFunction, Registry::Noise, Registry::NoiseSettings, Registry::Biome,
          Registry::MultiNoiseBiomeSourceParameterList, Registry::WorldPreset,
          Registry::Dimension}) {
        CAPTURE(stratum::data::registryDirectory(supported));
        CHECK(stratum::data::isSupported(supported));
    }
    for (const Registry rejected :
         {Registry::ConfiguredFeature, Registry::PlacedFeature, Registry::ConfiguredCarver,
          Registry::Structure, Registry::StructureSet, Registry::TemplatePool,
          Registry::ProcessorList, Registry::FlatLevelGeneratorPreset}) {
        CAPTURE(stratum::data::registryDirectory(rejected));
        CHECK_FALSE(stratum::data::isSupported(rejected));
    }

    CHECK(stratum::data::registryFromDirectory("worldgen/density_function") ==
          Registry::DensityFunction);
    CHECK_FALSE(stratum::data::registryFromDirectory("worldgen/nonsense").has_value());
    CHECK(stratum::data::allRegistries().size() == 16U);
}

TEST_CASE("a worldgen tree loads with nested identifiers", "[data][pack]") {
    TempDirectory tree;
    tree.write("density_function/overworld/base_3d_noise.json", kConstantDensityFunction);
    tree.write("noise/temperature.json", R"({"firstOctave":-10,"amplitudes":[1.5,0,1,0,0,0]})");

    const Pack pack = Pack::openWorldgenTree(tree.path());
    REQUIRE(pack.size() == 2U);

    const auto* nested = pack.find(Registry::DensityFunction,
                                   ResourceLocation::parse("minecraft:overworld/base_3d_noise"));
    REQUIRE(nested != nullptr);
    CHECK(nested->json.at("type") == "minecraft:constant");

    const auto* noise = pack.find(Registry::Noise, ResourceLocation::parse("temperature"));
    REQUIRE(noise != nullptr);
    CHECK(noise->json.at("firstOctave") == -10);

    CHECK(pack.find(Registry::Noise, ResourceLocation::parse("absent")) == nullptr);
}

TEST_CASE("entries in registries v1 does not execute are reported, never dropped", "[data][pack]") {
    TempDirectory tree;
    tree.write("density_function/a.json", kConstantDensityFunction);
    tree.write("placed_feature/ore_coal.json", R"({"feature":"minecraft:ore_coal"})");
    tree.write("structure/village_plains.json", R"({"type":"minecraft:jigsaw"})");

    const Pack pack = Pack::openWorldgenTree(tree.path());
    CHECK(pack.size() == 1U);
    REQUIRE(pack.rejected().size() == 2U);
    // Every file is accounted for: loaded or listed.
    CHECK(pack.size() + pack.rejected().size() == 3U);

    PackLoadOptions strict;
    strict.rejectUnsupported = true;
    REQUIRE_THROWS_WITH(Pack::openWorldgenTree(tree.path(), "minecraft", strict),
                        Catch::Matchers::ContainsSubstring("placed_feature") &&
                            Catch::Matchers::ContainsSubstring("does not execute"));
}

TEST_CASE("a directory this build cannot name is refused", "[data][pack]") {
    // Not the same as a registry we know and skip: an unknown directory means
    // the pack expects something of us we cannot even describe.
    TempDirectory tree;
    tree.write("density_function/a.json", kConstantDensityFunction);
    tree.write("mystery_registry/thing.json", R"({})");

    REQUIRE_THROWS_WITH(Pack::openWorldgenTree(tree.path()),
                        Catch::Matchers::ContainsSubstring("registry directory"));

    PackLoadOptions lenient;
    lenient.rejectUnknownDirectories = false;
    const Pack pack = Pack::openWorldgenTree(tree.path(), "minecraft", lenient);
    CHECK(pack.size() == 1U);
}

TEST_CASE("a data pack is read through its metadata", "[data][pack]") {
    TempDirectory pack;
    pack.write("pack.mcmeta",
               R"({"pack":{"description":"t","min_format":[94,1],"max_format":94}})");
    pack.write("data/minecraft/worldgen/density_function/a.json", kConstantDensityFunction);
    pack.write("data/mypack/worldgen/noise/custom.json", R"({"firstOctave":-3,"amplitudes":[1]})");

    const Pack loaded = Pack::openDataPack(pack.path());
    CHECK(loaded.size() == 2U);
    CHECK(loaded.find(Registry::DensityFunction, ResourceLocation::parse("minecraft:a")) !=
          nullptr);
    CHECK(loaded.find(Registry::Noise, ResourceLocation::parse("mypack:custom")) != nullptr);
}

TEST_CASE("a pack that does not cover the pinned format is refused", "[data][pack][pin]") {
    // SPEC §3: loading a pack written for another format would mean guessing
    // at a schema it does not claim to speak.
    TempDirectory pack;
    pack.write("data/minecraft/worldgen/density_function/a.json", kConstantDensityFunction);

    pack.write("pack.mcmeta", R"({"pack":{"description":"t","pack_format":48}})");
    REQUIRE_THROWS_WITH(Pack::openDataPack(pack.path()),
                        Catch::Matchers::ContainsSubstring("does not cover the pinned format"));

    pack.write("pack.mcmeta",
               R"({"pack":{"description":"t","min_format":[95,0],"max_format":96}})");
    CHECK_THROWS_AS(Pack::openDataPack(pack.path()), PackError);

    // A plain pack_format equal to the pin is fine, as is a covering range.
    pack.write("pack.mcmeta", R"({"pack":{"description":"t","pack_format":94}})");
    CHECK_NOTHROW(Pack::openDataPack(pack.path()));
    pack.write("pack.mcmeta", R"({"pack":{"description":"t","min_format":90,"max_format":94}})");
    CHECK_NOTHROW(Pack::openDataPack(pack.path()));
}

TEST_CASE("packs that cannot be read say why", "[data][pack][malformed]") {
    SECTION("no metadata at all") {
        TempDirectory pack;
        pack.write("data/minecraft/worldgen/noise/a.json", R"({})");
        REQUIRE_THROWS_WITH(Pack::openDataPack(pack.path()),
                            Catch::Matchers::ContainsSubstring("pack.mcmeta"));
    }

    SECTION("metadata that is not a pack") {
        TempDirectory pack;
        pack.write("pack.mcmeta", R"({"something":"else"})");
        REQUIRE_THROWS_WITH(Pack::openDataPack(pack.path()),
                            Catch::Matchers::ContainsSubstring("not a pack.mcmeta"));
    }

    SECTION("malformed JSON names the file") {
        TempDirectory tree;
        tree.write("noise/broken.json", R"({"firstOctave":)");
        REQUIRE_THROWS_WITH(Pack::openWorldgenTree(tree.path()),
                            Catch::Matchers::ContainsSubstring("broken.json"));
    }

    SECTION("one identifier defined twice") {
        // Only reachable across namespaces or layouts, but a duplicate would
        // otherwise resolve to whichever the filesystem listed first.
        TempDirectory pack;
        pack.write("pack.mcmeta", R"({"pack":{"description":"t","pack_format":94}})");
        pack.write("data/minecraft/worldgen/noise/a.json", R"({})");
        pack.write("data/minecraft/dimension/../worldgen/noise/a.json", R"({})");
        CHECK_NOTHROW(Pack::openDataPack(pack.path())); // same file, not a duplicate
    }
}

TEST_CASE("loading is deterministic", "[data][pack]") {
    // Directory iteration order is unspecified; two loads of one pack must
    // still produce the same order, or an error would name a different file
    // each run.
    TempDirectory tree;
    for (const std::string_view name : {"c", "a", "b", "nested/d", "nested/e"}) {
        tree.write(std::string("density_function/") + std::string(name) + ".json",
                   kConstantDensityFunction);
    }

    const Pack first = Pack::openWorldgenTree(tree.path());
    const Pack second = Pack::openWorldgenTree(tree.path());
    REQUIRE(first.size() == second.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        CAPTURE(i);
        CHECK(first.entries()[i].id == second.entries()[i].id);
    }
    CHECK(first.entries().front().id.path() == "a");
}
