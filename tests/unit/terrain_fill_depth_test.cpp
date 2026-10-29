// Stratum — the chunk filler at the depth bound.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// density::kMaxDepth is sized by the stack (density/graph.hpp): a graph at
// the bound must resolve, compile and evaluate in every build, the sanitize
// preset's included. density_interpreter_test.cpp holds the interpreter to
// it on `abs` and spline chains; this holds the whole fill to it on the
// deepest frame per level, `interpolated`, reached the way a chunk reaches
// it — ChunkFiller::fill's block kernel, its corner reads and the walk
// beneath them — and the interpreter's own walk beside it.
#include "support/temp_path.hpp"

#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/graph.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/terrain/filler.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

using Catch::Matchers::ContainsSubstring;
using stratum::data::ResourceLocation;
using stratum::settings::RouterEntry;
using stratum::terrain::ChunkBuffer;
using stratum::terrain::ChunkFiller;

namespace {

class DepthTree {
public:
    DepthTree() : path_(std::filesystem::temp_directory_path() / uniqueName()) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_ / "density_function");
        std::filesystem::create_directories(path_ / "noise_settings");
    }

    DepthTree(const DepthTree&) = delete;
    DepthTree& operator=(const DepthTree&) = delete;
    DepthTree(DepthTree&&) = delete;
    DepthTree& operator=(DepthTree&&) = delete;

    ~DepthTree() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    void defineFunction(std::string_view name, const std::string& json) const {
        std::ofstream out(path_ / "density_function" / (std::string(name) + ".json"));
        out << json;
    }

    void defineSettings(std::string_view name, const nlohmann::json& json) const {
        std::ofstream out(path_ / "noise_settings" / (std::string(name) + ".json"));
        out << json.dump();
    }

    [[nodiscard]] stratum::data::Pack pack() const { return stratum::data::Pack::open(path_); }

private:
    [[nodiscard]] static std::string uniqueName() {
        return stratum::test::tempName("stratum-fill-depth-test");
    }

    std::filesystem::path path_;
};

/// @p levels `interpolated`, each the argument of the one around it, around
/// @p innermost.
[[nodiscard]] std::string interpolatedChain(const std::size_t levels,
                                            const std::string& innermost) {
    std::string text;
    for (std::size_t i = 0; i < levels; ++i) {
        text += R"({"type":"minecraft:interpolated","argument":)";
    }
    return text + innermost + std::string(levels, '}');
}

/// A plane: 1 at y = -1 and below, -1 at y = 1 and above.
constexpr std::string_view kPlane =
    R"({"type":"minecraft:y_clamped_gradient","from_y":-1,"to_y":1,"from_value":1.0,)"
    R"("to_value":-1.0})";

/// A small dimension, aquifers and veins off, whose final_density is
/// @p finalDensity: 48 blocks from y = -16, cells 4 wide and 4 high.
[[nodiscard]] nlohmann::json settingsWith(const std::string& finalDensity) {
    nlohmann::json router = nlohmann::json::object();
    for (std::size_t i = 0; i < stratum::settings::kRouterEntryCount; ++i) {
        router[std::string(stratum::settings::routerEntryName(static_cast<RouterEntry>(i)))] = 0.0;
    }
    router["final_density"] = finalDensity;
    return nlohmann::json{
        {"default_block", {{"Name", "minecraft:stone"}}},
        {"default_fluid", {{"Name", "minecraft:water"}}},
        {"sea_level", 8},
        {"disable_mob_generation", false},
        {"aquifers_enabled", false},
        {"ore_veins_enabled", false},
        {"legacy_random_source", false},
        {"noise", {{"min_y", -16}, {"height", 48}, {"size_horizontal", 1}, {"size_vertical", 1}}},
        {"noise_router", router},
        {"spawn_target", nlohmann::json::array()},
        {"surface_rule", {{"type", "minecraft:bandlands"}}},
    };
}

} // namespace

TEST_CASE("a final_density of interpolated kMaxDepth deep is filled and evaluated to its leaf",
          "[terrain][filler][density][ub]") {
    using stratum::density::kMaxDepth;
    REQUIRE(kMaxDepth == 256U);

    // 254 `interpolated` around a `find_top_surface` of two constants: 256
    // levels, as deep as a graph may be, and the deepest frame per level the
    // interpreter has. Across two files, since one holds no more than
    // data::kMaxJsonDepth: `deep_a` is 126 of them around the leaf, and
    // `deep_b` 128 around it (`deep_a` sorts first, so `deep_b` meets it
    // resolved — density_interpreter_test.cpp's "a graph at kMaxDepth is
    // evaluated" says why that matters).
    //
    // The leaf refuses where it is evaluated: its upper_bound floors to no
    // int32. So the walk goes all the way down, and only once. A chain of
    // `interpolated` that evaluates instead costs what its cells' corners
    // do, and each level reads its argument at every corner of a cell one
    // corner wider than the level around it — eight levels are below;
    // 254 would not finish.
    const std::string leaf =
        R"({"type":"minecraft:find_top_surface","density":0.0,"upper_bound":1e300,)"
        R"("lower_bound":-16,"cell_height":4})";
    const DepthTree tree;
    tree.defineFunction("deep_a", interpolatedChain(126, leaf));
    tree.defineFunction("deep_b", interpolatedChain(128, R"("minecraft:deep_a")"));
    tree.defineSettings("deep", settingsWith("minecraft:deep_b"));

    const stratum::data::Pack pack = tree.pack();
    const stratum::settings::LoadedSettings loaded = stratum::settings::loadAll(pack);
    const stratum::density::NoiseRegistry noises = stratum::density::NoiseRegistry::create(
        pack, loaded.graph.referencedNoises(), 0, stratum::density::RandomSource::Xoroshiro);
    const auto& deep = loaded.settings.at(ResourceLocation::parse("minecraft:deep"));

    // Compiled whole — requireEvaluable and the block kernel at the bound —
    // then filled, which reaches the leaf through the run's kernel and then
    // block by block, and refuses by its name.
    const ChunkFiller filler = ChunkFiller::compile(loaded.graph, noises, deep);
    ChunkBuffer blocks(deep.geometry);
    // The chunk's first block, the first the fill evaluates.
    CHECK_THROWS_WITH(filler.fill(0, 0, blocks),
                      ContainsSubstring("'minecraft:find_top_surface' reads upper_bound") &&
                          ContainsSubstring("at block (0, -16, 0)"));

    // And the interpreter's walk, with a cache and without.
    const stratum::density::Interpreter interpreter(
        loaded.graph, noises,
        stratum::density::CellGeometry{.width = deep.geometry.cellWidth(),
                                       .height = deep.geometry.cellHeight()});
    const stratum::density::NodeIndex root = deep.router.at(RouterEntry::FinalDensity);
    CHECK_NOTHROW(interpreter.requireEvaluable(root));
    stratum::density::Interpreter::CornerCache cache(interpreter.cacheSize());
    const stratum::density::Point at{.x = 1, .y = 2, .z = 3};
    CHECK_THROWS_WITH(static_cast<void>(interpreter.evaluate(root, at, cache)),
                      ContainsSubstring("find_top_surface"));
    CHECK_THROWS_WITH(static_cast<void>(interpreter.evaluate(root, at)),
                      ContainsSubstring("find_top_surface"));
}

TEST_CASE("a chain of interpolated nodes fills as the one interpolated around its argument",
          "[terrain][filler][density]") {
    // At a corner an `interpolated` reads its argument where it is — the
    // corner of a cell it blends with a fraction of 0 on every axis — so a
    // chain of them is the one around the plane, bit for bit: the same
    // blocks through the fill's kernel, and the same value through the walk
    // at every one of them. Eight levels, which already read the plane one
    // corner wider than the chunk's own lattice at each.
    const DepthTree tree;
    tree.defineFunction("chain", interpolatedChain(8, std::string(kPlane)));
    tree.defineFunction("single", interpolatedChain(1, std::string(kPlane)));
    tree.defineSettings("chain", settingsWith("minecraft:chain"));
    tree.defineSettings("single", settingsWith("minecraft:single"));

    const stratum::data::Pack pack = tree.pack();
    const stratum::settings::LoadedSettings loaded = stratum::settings::loadAll(pack);
    const stratum::density::NoiseRegistry noises = stratum::density::NoiseRegistry::create(
        pack, loaded.graph.referencedNoises(), 0, stratum::density::RandomSource::Xoroshiro);
    const auto& chain = loaded.settings.at(ResourceLocation::parse("minecraft:chain"));
    const auto& single = loaded.settings.at(ResourceLocation::parse("minecraft:single"));

    const ChunkFiller chainFiller = ChunkFiller::compile(loaded.graph, noises, chain);
    const ChunkFiller singleFiller = ChunkFiller::compile(loaded.graph, noises, single);
    const stratum::density::Interpreter interpreter(
        loaded.graph, noises,
        stratum::density::CellGeometry{.width = chain.geometry.cellWidth(),
                                       .height = chain.geometry.cellHeight()});
    const stratum::density::NodeIndex chainRoot = chain.router.at(RouterEntry::FinalDensity);
    const stratum::density::NodeIndex singleRoot = single.router.at(RouterEntry::FinalDensity);

    // A chunk on either side of the origin, so negative cells are crossed.
    for (const std::pair<std::int32_t, std::int32_t> chunk : {std::pair{0, 0}, std::pair{-1, -1}}) {
        const std::int32_t chunkX = chunk.first;
        const std::int32_t chunkZ = chunk.second;
        CAPTURE(chunkX, chunkZ);
        ChunkBuffer chainBlocks(chain.geometry);
        ChunkBuffer singleBlocks(single.geometry);
        chainFiller.fill(chunkX, chunkZ, chainBlocks);
        singleFiller.fill(chunkX, chunkZ, singleBlocks);

        stratum::density::Interpreter::CornerCache chainCache(interpreter.cacheSize());
        stratum::density::Interpreter::CornerCache singleCache(interpreter.cacheSize());
        std::size_t stone = 0;
        std::size_t mismatched = 0;
        for (int localZ = 0; localZ < 16; ++localZ) {
            for (int localX = 0; localX < 16; ++localX) {
                for (std::int32_t y = chain.geometry.minY;
                     y < chain.geometry.minY + chain.geometry.height; ++y) {
                    const stratum::density::Point at{
                        .x = (chunkX * 16) + localX, .y = y, .z = (chunkZ * 16) + localZ};
                    const double chainValue = interpreter.evaluate(chainRoot, at, chainCache);
                    const double singleValue = interpreter.evaluate(singleRoot, at, singleCache);
                    const auto& placed = chainBlocks.at(localX, y, localZ);
                    const bool agrees =
                        std::bit_cast<std::uint64_t>(chainValue) ==
                            std::bit_cast<std::uint64_t>(singleValue) &&
                        placed == singleBlocks.at(localX, y, localZ) &&
                        (placed.name == ResourceLocation::parse("minecraft:stone")) ==
                            (chainValue > 0.0) &&
                        (chainValue > 0.0) == (y < 0);
                    if (!agrees) {
                        ++mismatched;
                    }
                    if (chainValue > 0.0) {
                        ++stone;
                    }
                }
            }
        }
        CHECK(mismatched == 0U);
        CHECK(stone == std::size_t{16} * 16 * 16);
    }
}
