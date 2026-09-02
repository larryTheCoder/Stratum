// Stratum — vanilla's noise settings, loaded whole.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Seven files, and between them every shape the loader has to cope with: a
// sea level below the bottom of the world, a dimension whose default fluid
// is air, cells that are wider than they are tall and cells that are taller
// than they are wide. The unit suite checks the refusals; this checks that
// nothing vanilla actually ships is refused, and that what comes out matches
// the file rather than a plausible default.
//
// Mojang-derived fixtures are never committed (SPEC §12); without them this
// SKIPs, naming the command that produces them.

#include <stratum/data/pack.hpp>
#include <stratum/data/registry.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/graph.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/settings/noise_settings.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <string>
#include <string_view>

namespace {

using stratum::data::Pack;
using stratum::data::ResourceLocation;
using stratum::settings::LoadedSettings;
using stratum::settings::NoiseSettings;
using stratum::settings::RouterEntry;

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

[[nodiscard]] const NoiseSettings& get(const LoadedSettings& loaded, std::string_view id) {
    return loaded.settings.at(ResourceLocation::parse(std::string(id)));
}

} // namespace

TEST_CASE("every vanilla noise settings entry loads", "[conformance][settings]") {
    const std::filesystem::path tree = findWorldgenTree();
    if (tree.empty()) {
        SKIP("no extracted vanilla worldgen under "
             << STRATUM_FIXTURES_DIR
             << " — Mojang-derived and never committed (SPEC §12). Generate it with: "
                "tools/fetch-vanilla");
    }

    const Pack pack = Pack::open(tree);
    const LoadedSettings loaded = stratum::settings::loadAll(pack);

    // All seven, and nothing skipped for being unreadable.
    REQUIRE(loaded.settings.size() ==
            pack.entriesOf(stratum::data::Registry::NoiseSettings).size());
    CHECK(loaded.settings.size() == 7U);

    // The overworld, checked field by field against the file rather than
    // against what a reasonable default would be — a loader that dropped
    // every value and returned zeroes would still "work".
    const NoiseSettings& overworld = get(loaded, "minecraft:overworld");
    CHECK(overworld.geometry.minY == -64);
    CHECK(overworld.geometry.height == 384);
    CHECK(overworld.geometry.maxY() == 320);
    CHECK(overworld.geometry.cellWidth() == 4);
    CHECK(overworld.geometry.cellHeight() == 8);
    CHECK(overworld.seaLevel == 63);
    CHECK(overworld.aquifersEnabled);
    CHECK(overworld.oreVeinsEnabled);
    CHECK_FALSE(overworld.legacyRandomSource);
    CHECK_FALSE(overworld.disableMobGeneration);
    CHECK(overworld.defaultBlock.name == ResourceLocation::parse("minecraft:stone"));
    CHECK(overworld.defaultFluid.name == ResourceLocation::parse("minecraft:water"));
    CHECK(overworld.defaultFluid.properties.at("level") == "0");

    // The nether is a different shape in every way that matters, which is
    // what makes it worth checking rather than assuming the overworld's
    // values were read from the overworld's file.
    const NoiseSettings& nether = get(loaded, "minecraft:nether");
    CHECK(nether.geometry.minY == 0);
    CHECK(nether.geometry.height == 128);
    CHECK(nether.seaLevel == 32);
    CHECK(nether.legacyRandomSource);
    CHECK_FALSE(nether.aquifersEnabled);
    CHECK(nether.defaultBlock.name == ResourceLocation::parse("minecraft:netherrack"));
    CHECK(nether.defaultFluid.name == ResourceLocation::parse("minecraft:lava"));

    // The End's cells are wider than they are tall, the reverse of the
    // overworld's, and its default fluid is air with no properties at all.
    const NoiseSettings& end = get(loaded, "minecraft:end");
    CHECK(end.geometry.cellWidth() == 8);
    CHECK(end.geometry.cellHeight() == 4);
    CHECK(end.defaultBlock.name == ResourceLocation::parse("minecraft:end_stone"));
    CHECK(end.defaultFluid.name == ResourceLocation::parse("minecraft:air"));
    CHECK(end.defaultFluid.properties.empty());

    // Floating islands puts its sea level below the bottom of its own world.
    // That is why sea_level is not range-checked against the geometry: the
    // obvious sanity check would refuse a file vanilla ships.
    const NoiseSettings& islands = get(loaded, "minecraft:floating_islands");
    CHECK(islands.seaLevel == -64);
    CHECK(islands.geometry.minY == 0);
    CHECK(islands.seaLevel < islands.geometry.minY);
}

TEST_CASE("vanilla's routers resolve into the graph the pack already had",
          "[conformance][settings]") {
    const std::filesystem::path tree = findWorldgenTree();
    if (tree.empty()) {
        SKIP("no extracted vanilla worldgen under " << STRATUM_FIXTURES_DIR);
    }

    const Pack pack = Pack::open(tree);
    const LoadedSettings loaded = stratum::settings::loadAll(pack);

    // The router's named entries must be the very nodes the pack's own
    // density functions resolved to, not copies. If they were copies, one
    // point would sample continentalness twice and the compiled program M3
    // builds would carry the same subtree several times over.
    const NoiseSettings& overworld = get(loaded, "minecraft:overworld");
    const auto root = [&loaded](std::string_view path) {
        return loaded.graph.rootOf(ResourceLocation::parse(std::string(path)));
    };
    CHECK(overworld.router.at(RouterEntry::Continents) == root("overworld/continents"));
    CHECK(overworld.router.at(RouterEntry::Erosion) == root("overworld/erosion"));
    CHECK(overworld.router.at(RouterEntry::Ridges) == root("overworld/ridges"));
    CHECK(overworld.router.at(RouterEntry::Depth) == root("overworld/depth"));

    // large_biomes shares those same four with the overworld: it differs in
    // its noises, not in the shape of its router.
    const NoiseSettings& large = get(loaded, "minecraft:large_biomes");
    CHECK(large.router.at(RouterEntry::Depth) == root("overworld_large_biomes/depth"));

    // Every entry of every router points somewhere real.
    for (const auto& [id, settings] : loaded.settings) {
        CAPTURE(id.toString());
        for (std::size_t i = 0; i < stratum::settings::kRouterEntryCount; ++i) {
            CAPTURE(stratum::settings::routerEntryName(static_cast<RouterEntry>(i)));
            CHECK(settings.router.entries[i] < loaded.graph.nodeCount());
        }
    }

    // The named functions keep the node indices they would have had on their
    // own: the settings are resolved after them, so nothing they add can
    // shift what came before.
    const stratum::density::Graph alone = stratum::density::Graph::resolveAll(pack);
    CHECK(alone.nodeCount() == 275U);
    CHECK(loaded.graph.nodeCount() > alone.nodeCount());
    for (const auto& [id, index] : alone.roots()) {
        CAPTURE(id.toString());
        CHECK(loaded.graph.rootOf(id) == index);
    }
}

TEST_CASE("vanilla's routers gain the cell-structured entries once there is a lattice",
          "[conformance][settings][cells]") {
    const std::filesystem::path tree = findWorldgenTree();
    if (tree.empty()) {
        SKIP("no extracted vanilla worldgen under " << STRATUM_FIXTURES_DIR);
    }

    const Pack pack = Pack::open(tree);
    const LoadedSettings loaded = stratum::settings::loadAll(pack);
    const stratum::density::NoiseRegistry noises =
        stratum::density::NoiseRegistry::create(pack, loaded.graph.referencedNoises(), 42);

    const NoiseSettings& overworld = get(loaded, "minecraft:overworld");
    const stratum::density::Interpreter withoutCells(loaded.graph, noises);
    const stratum::density::Interpreter withCells(
        loaded.graph, noises,
        stratum::density::CellGeometry{.width = overworld.geometry.cellWidth(),
                                       .height = overworld.geometry.cellHeight()});

    // vanilla's noodle caves are wrapped in `interpolated`, and nothing else
    // about them is out of reach. They are the whole difference the cell
    // sampler makes to this dimension.
    const auto noodle =
        loaded.graph.rootOf(ResourceLocation::parse("minecraft:overworld/caves/noodle"));
    CHECK_THROWS_AS(withoutCells.requireEvaluable(noodle), stratum::density::EvalError);
    CHECK_NOTHROW(withCells.requireEvaluable(noodle));

    // The lattice comes from the dimension: 4x8 in the overworld, 8x4 in the
    // End, and the same function sampled on both is two different functions.
    CHECK(withCells.cells()->width == 4);
    CHECK(withCells.cells()->height == 8);
    const NoiseSettings& end = get(loaded, "minecraft:end");
    CHECK(end.geometry.cellWidth() == 8);
    CHECK(end.geometry.cellHeight() == 4);

    // final_density is still out of reach, for a reason that has nothing to
    // do with cells: `old_blended_noise`. The noise itself is implemented and
    // checked against cubiomes; what is unsettled is where
    // smear_scale_multiplier enters it and how a modern dimension seeds it
    // (SPEC §11). `blend_density`, which used to be the first refusal met on
    // this path, is transparent now — so this is the single remaining thing
    // between the pipeline and a block of terrain.
    CHECK_THROWS_WITH(withCells.requireEvaluable(overworld.router.at(RouterEntry::FinalDensity)),
                      Catch::Matchers::ContainsSubstring("minecraft:old_blended_noise"));
    CHECK_THROWS_WITH(withCells.requireEvaluable(loaded.graph.rootOf(
                          ResourceLocation::parse("minecraft:overworld/base_3d_noise"))),
                      Catch::Matchers::ContainsSubstring("minecraft:old_blended_noise"));
}
