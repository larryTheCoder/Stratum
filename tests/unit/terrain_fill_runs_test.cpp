// Stratum — the fill's runs: what a run of the block kernel alone decides.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// ChunkFiller::fill runs final_density (and vein_toggle, on the solid lanes
// inside the vein range) over a cell's blocks of one y at once, and places
// from the run's lanes every block that needs nothing more read: solid
// ground where no vein can be, and ground that is not solid where the
// aquifer is not asked — a whole run at once where every lane holds one
// state. The rest go block by block. This holds the fill to the rule it
// implements, written out block by block here from the interpreter's walk
// and the vein module: over runs that are all one state and runs that mix
// solid ground, open fluid and vein candidates, in the lava sea, the water
// and the air, and to the palette's order, which is the order blocks are
// first placed — in cells 4, 8, 12 and 16 blocks wide, whose runs are 16
// lanes in rows of 4, 8 and 16 and 12 lanes in one row of 12, with a
// vein_toggle the fill runs over a run's lanes and one it reads block by
// block.
#include "support/temp_path.hpp"

#include <stratum/aquifer/lattice.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/ore/vein.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/terrain/filler.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using stratum::data::ResourceLocation;
using stratum::settings::BlockState;
using stratum::settings::RouterEntry;
using stratum::terrain::ChunkBuffer;
using stratum::terrain::ChunkFiller;

namespace {

class RunsTree {
public:
    RunsTree() : path_(std::filesystem::temp_directory_path() / uniqueName()) {
        std::filesystem::remove_all(path_);
        // A worldgen tree is one with density_function/, here empty.
        std::filesystem::create_directories(path_ / "density_function");
        std::filesystem::create_directories(path_ / "noise");
        std::filesystem::create_directories(path_ / "noise_settings");
    }

    RunsTree(const RunsTree&) = delete;
    RunsTree& operator=(const RunsTree&) = delete;
    RunsTree(RunsTree&&) = delete;
    RunsTree& operator=(RunsTree&&) = delete;

    ~RunsTree() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    void defineNoise(std::string_view name, std::string_view json) const {
        std::ofstream out(path_ / "noise" / (std::string(name) + ".json"));
        out << json;
    }

    void defineSettings(std::string_view name, const nlohmann::json& json) const {
        std::ofstream out(path_ / "noise_settings" / (std::string(name) + ".json"));
        out << json.dump();
    }

    [[nodiscard]] stratum::data::Pack pack() const { return stratum::data::Pack::open(path_); }

private:
    [[nodiscard]] static std::string uniqueName() {
        return stratum::test::tempName("stratum-fill-runs-test");
    }

    std::filesystem::path path_;
};

[[nodiscard]] nlohmann::json noiseNode(std::string_view noise, double scale) {
    return nlohmann::json{{"type", "minecraft:noise"},
                          {"noise", std::string("minecraft:") + std::string(noise)},
                          {"xz_scale", scale},
                          {"y_scale", scale}};
}

[[nodiscard]] nlohmann::json mul(double factor, const nlohmann::json& argument) {
    return nlohmann::json{
        {"type", "minecraft:mul"}, {"argument1", factor}, {"argument2", argument}};
}

/// 128 blocks from y = -64, cells @p sizeHorizontal * 4 blocks wide and 4
/// high, veins on and aquifers off. The ground is a plane at y = 0
/// roughened by a noise strong enough to open caves at the floor (lava
/// under -54, water under the sea at 8) and to carry the surface across
/// cells; vein_toggle is a noise wide enough to clear the richness gate on
/// some lanes of a run and not on others — @p toggleInterpolated, as
/// vanilla's is, so that it has a block kernel and the fill runs it over a
/// run's solid lanes in a vein range, or bare, so that it has none and
/// every one of those lanes takes the block-by-block body.
[[nodiscard]] nlohmann::json runsSettings(const std::int32_t sizeHorizontal,
                                          const bool toggleInterpolated) {
    nlohmann::json router = nlohmann::json::object();
    for (std::size_t i = 0; i < stratum::settings::kRouterEntryCount; ++i) {
        router[std::string(stratum::settings::routerEntryName(static_cast<RouterEntry>(i)))] = 0.0;
    }
    const nlohmann::json plane{{"type", "minecraft:y_clamped_gradient"},
                               {"from_y", -64},
                               {"to_y", 64},
                               {"from_value", 0.6},
                               {"to_value", -0.6}};
    router["final_density"] = nlohmann::json{{"type", "minecraft:interpolated"},
                                             {"argument",
                                              {{"type", "minecraft:add"},
                                               {"argument1", plane},
                                               {"argument2", mul(1.2, noiseNode("field", 1.0))}}}};
    const nlohmann::json toggle = mul(1.5, noiseNode("toggle", 1.0));
    router["vein_toggle"] = toggleInterpolated ? nlohmann::json{{"type", "minecraft:interpolated"},
                                                                {"argument", toggle}}
                                               : toggle;
    router["vein_ridged"] = noiseNode("ridged", 1.0);
    router["vein_gap"] = noiseNode("gap", 1.0);
    return nlohmann::json{
        {"default_block", {{"Name", "minecraft:stone"}}},
        {"default_fluid", {{"Name", "minecraft:water"}}},
        {"sea_level", 8},
        {"disable_mob_generation", false},
        {"aquifers_enabled", false},
        {"ore_veins_enabled", true},
        {"legacy_random_source", false},
        {"noise",
         {{"min_y", -64},
          {"height", 128},
          {"size_horizontal", sizeHorizontal},
          {"size_vertical", 1}}},
        {"noise_router", router},
        {"spawn_target", nlohmann::json::array()},
        {"surface_rule", {{"type", "minecraft:bandlands"}}},
    };
}

/// runsSettings at every cell width, as minecraft:runs_<n> for
/// size_horizontal n with the toggle interpolated and runs_<n>_bare with it
/// bare, and the four noises they read.
void defineRuns(const RunsTree& tree) {
    tree.defineNoise("field", R"({"firstOctave": -4, "amplitudes": [1.0, 0.5]})");
    tree.defineNoise("toggle", R"({"firstOctave": -3, "amplitudes": [1.0]})");
    tree.defineNoise("ridged", R"({"firstOctave": -2, "amplitudes": [1.0]})");
    tree.defineNoise("gap", R"({"firstOctave": -2, "amplitudes": [1.0]})");
    for (std::int32_t size = 1; size <= 4; ++size) {
        tree.defineSettings("runs_" + std::to_string(size), runsSettings(size, true));
        tree.defineSettings("runs_" + std::to_string(size) + "_bare", runsSettings(size, false));
    }
}

[[nodiscard]] BlockState named(std::string_view name) {
    return BlockState{.name = ResourceLocation::parse(name), .properties = {}};
}

/// What the runs of one width met, among those the block kernel stood for
/// (the only runs the fill places from lanes), by what decides them.
struct RunCounts {
    /// Runs the kernel stood for.
    std::size_t ran = 0;
    /// The run alone decides every block, and every lane holds one state
    /// that is not air: placed a row at a time (writeRun's rows of one
    /// state, over blocks the buffer's clear() left air). Those of them in
    /// a vein range.
    std::size_t whole = 0;
    std::size_t wholeInVeins = 0;
    /// Of those in a vein range, the ones solid on every lane: no lane's
    /// toggle, from the toggle's own run, clears the gate.
    std::size_t solidInVeins = 0;
    /// Solid lanes and lanes that are not in one run: the run alone decides
    /// every block (writeRun, lane by lane), or a vein candidate is left to
    /// the block-by-block body and every other lane is placed from the run
    /// there. Those in a vein range, either way.
    std::size_t mixedWhole = 0;
    std::size_t mixedBody = 0;
    std::size_t mixedInVeins = 0;
};

/// What filling some chunks of one dimension found, block by block.
struct FillCheck {
    std::size_t mismatched = 0;
    std::size_t palettesDiffering = 0;
    std::size_t veinPlaced = 0;
    std::size_t lavaBlocks = 0;
    std::size_t waterBlocks = 0;
    /// By the run's width: its cell's, or less at the chunk's edge.
    std::map<std::int32_t, RunCounts> runs;
};

using Chunks = std::vector<std::pair<std::int32_t, std::int32_t>>;

/// Fills each of @p chunks of minecraft:<dimension> and holds every block to
/// the rule the fill implements, written out block by block from the
/// interpreter's walk and the vein module, and each chunk's palette to the
/// order its blocks are first placed in. The runs counted are the fill's
/// own — a cell's blocks of one y, as many of its rows along x as kLanes
/// holds — by what decides each.
[[nodiscard]] FillCheck fillAndCheck(const stratum::settings::LoadedSettings& loaded,
                                     const stratum::density::NoiseRegistry& noises,
                                     const std::string& dimension, const Chunks& chunks) {
    using stratum::density::Interpreter;
    const auto& settings = loaded.settings.at(ResourceLocation::parse("minecraft:" + dimension));
    const auto& geometry = settings.geometry;
    const std::int32_t cellWidth = geometry.cellWidth();
    const ChunkFiller filler = ChunkFiller::compile(loaded.graph, noises, settings);
    // The fill runs its kernels only under the corner memo.
    REQUIRE(filler.cornerSharing() == Interpreter::CornerCache::Sharing::Lattice);

    const Interpreter interpreter(
        loaded.graph, noises,
        stratum::density::CellGeometry{.width = cellWidth, .height = geometry.cellHeight()});
    const stratum::ore::VeinSource veins(noises.worldSeed(), noises.source());
    const auto entry = [&settings](const RouterEntry e) { return settings.router.at(e); };
    // final_density's kernel, run here over each of the fill's runs only to
    // tell which of them the fill places from lanes: those it stands for.
    const std::optional<Interpreter::BlockKernel> kernel =
        interpreter.blockKernel(entry(RouterEntry::FinalDensity));
    REQUIRE(kernel.has_value());
    // Where vein_toggle has a kernel the fill runs it over a run's solid
    // lanes in a vein range, and only those whose toggle clears the gate are
    // left to the body; where it has none, every one of them is.
    const bool toggleRuns = interpreter.blockKernel(entry(RouterEntry::VeinToggle)).has_value();
    const auto lanes = std::make_unique<Interpreter::BlockKernel::Lanes>();
    constexpr auto kRunLanes = static_cast<std::int32_t>(Interpreter::BlockKernel::kLanes);

    // The six vein blocks, by metal and then filler, ore, raw block.
    const std::array<BlockState, 6> veinBlocks{named("minecraft:granite"),
                                               named("minecraft:copper_ore"),
                                               named("minecraft:raw_copper_block"),
                                               named("minecraft:tuff"),
                                               named("minecraft:deepslate_iron_ore"),
                                               named("minecraft:raw_iron_block")};
    const BlockState stone = named("minecraft:stone");
    const BlockState water = named("minecraft:water");
    const BlockState air = named("minecraft:air");
    const BlockState lava{.name = ResourceLocation::parse("minecraft:lava"),
                          .properties = {{"level", "0"}}};

    FillCheck check;
    for (const auto& [chunkX, chunkZ] : chunks) {
        CAPTURE(dimension, chunkX, chunkZ);
        ChunkBuffer buffer(geometry);
        filler.fill(chunkX, chunkZ, buffer);

        Interpreter::CornerCache cache(interpreter.cacheSize());
        Interpreter::CornerCache kernelCache(interpreter.cacheSize());
        std::vector<BlockState> palette{air};
        const std::int32_t topY = geometry.minY + geometry.height;
        // The fill's own order — cell by cell, then y, then z, then x — so
        // that the palette is rebuilt in the order blocks are first placed.
        for (std::int32_t cellZ = 0; cellZ < 16; cellZ += cellWidth) {
            for (std::int32_t cellX = 0; cellX < 16; cellX += cellWidth) {
                const std::int32_t cellEndX = std::min(cellX + cellWidth, std::int32_t{16});
                const std::int32_t cellEndZ = std::min(cellZ + cellWidth, std::int32_t{16});
                const std::int32_t runWidth = cellEndX - cellX;
                const std::int32_t runRows = std::max(std::int32_t{1}, kRunLanes / runWidth);
                RunCounts& counts = check.runs[runWidth];
                for (std::int32_t y = geometry.minY; y < topY; ++y) {
                    const bool inVeins = stratum::ore::inAnyVeinRange(y);
                    const stratum::ore::RichnessGate gate(y);
                    for (std::int32_t runZ = cellZ; runZ < cellEndZ; runZ += runRows) {
                        const std::int32_t runDepth = std::min(runRows, cellEndZ - runZ);
                        const bool ran = interpreter.evaluate(
                            *kernel,
                            Interpreter::BlockKernel::Blocks{
                                .first = stratum::density::Point{.x = (chunkX * 16) + cellX,
                                                                 .y = y,
                                                                 .z = (chunkZ * 16) + runZ},
                                .width = runWidth,
                                .depth = runDepth,
                                .lanes = ~0U},
                            *lanes, kernelCache);
                        std::int32_t solidInRun = 0;
                        bool candidateInRun = false;
                        bool oneState = true;
                        BlockState first = air;
                        for (std::int32_t z = runZ; z < runZ + runDepth; ++z) {
                            for (std::int32_t x = cellX; x < cellEndX; ++x) {
                                const stratum::density::Point at{
                                    .x = (chunkX * 16) + x, .y = y, .z = (chunkZ * 16) + z};
                                const double density = interpreter.evaluate(
                                    entry(RouterEntry::FinalDensity), at, cache);
                                BlockState expected = air;
                                if (density > 0.0) {
                                    ++solidInRun;
                                    expected = stone;
                                    if (inVeins) {
                                        const double toggle = interpreter.evaluate(
                                            entry(RouterEntry::VeinToggle), at, cache);
                                        // A lane the body reads more for.
                                        candidateInRun =
                                            candidateInRun || !toggleRuns || gate.clears(toggle);
                                        const stratum::ore::Vein vein = veins.at(
                                            at.x, y, at.z,
                                            stratum::ore::VeinInputs{
                                                .toggle = toggle,
                                                .ridged = interpreter.evaluate(
                                                    entry(RouterEntry::VeinRidged), at, cache),
                                                .gap = interpreter.evaluate(
                                                    entry(RouterEntry::VeinGap), at, cache)});
                                        if (vein.placed()) {
                                            ++check.veinPlaced;
                                            const std::size_t metal =
                                                vein.type == stratum::ore::VeinType::Copper ? 0U
                                                                                            : 3U;
                                            std::size_t kind = 2U;
                                            if (vein.block == stratum::ore::VeinBlock::Filler) {
                                                kind = 0U;
                                            } else if (vein.block == stratum::ore::VeinBlock::Ore) {
                                                kind = 1U;
                                            }
                                            expected = veinBlocks[metal + kind];
                                        }
                                    }
                                } else if (y < stratum::aquifer::lambdaLevel(settings.seaLevel)) {
                                    expected = lava;
                                    ++check.lavaBlocks;
                                } else if (y < settings.seaLevel) {
                                    expected = water;
                                    ++check.waterBlocks;
                                }
                                if (z == runZ && x == cellX) {
                                    first = expected;
                                }
                                oneState = oneState && expected == first;
                                if (std::ranges::find(palette, expected) == palette.end()) {
                                    palette.push_back(expected);
                                }
                                if (!(buffer.at(x, y, z) == expected)) {
                                    ++check.mismatched;
                                }
                            }
                        }
                        if (!ran) {
                            continue;
                        }
                        ++counts.ran;
                        if (solidInRun != 0 && solidInRun != runWidth * runDepth) {
                            ++(candidateInRun ? counts.mixedBody : counts.mixedWhole);
                            counts.mixedInVeins += inVeins ? 1U : 0U;
                        } else if (!candidateInRun && oneState && !(first == air)) {
                            ++counts.whole;
                            counts.wholeInVeins += inVeins ? 1U : 0U;
                            counts.solidInVeins += inVeins && solidInRun != 0 ? 1U : 0U;
                        }
                    }
                }
            }
        }
        check.palettesDiffering += buffer.palette() == palette ? 0U : 1U;
    }
    return check;
}

/// Every block and palette as the rule has them, every case the runs must
/// tell apart present — veins, lava, water — and for each of @p widths, the
/// widths of run the dimension's cells make, every way a run is decided,
/// each in a vein range and out of one: whole and mixed, mixed by the run
/// alone and with the body. Where the toggle runs (@p toggleRuns), whole
/// runs of solid ground in a vein range too, which only its run decides.
void expectEveryRunShape(const FillCheck& check, const std::vector<std::int32_t>& widths,
                         const bool toggleRuns) {
    CHECK(check.mismatched == 0U);
    CHECK(check.palettesDiffering == 0U);
    CHECK(check.veinPlaced > 0U);
    CHECK(check.lavaBlocks > 0U);
    CHECK(check.waterBlocks > 0U);
    REQUIRE(check.runs.size() == widths.size());
    for (const std::int32_t width : widths) {
        CAPTURE(width);
        const RunCounts& runs = check.runs.at(width);
        CHECK(runs.ran > 0U);
        CHECK(runs.whole > 0U);
        CHECK(runs.wholeInVeins > 0U);
        CHECK(runs.wholeInVeins < runs.whole);
        CHECK((runs.solidInVeins > 0U) == toggleRuns);
        CHECK(runs.mixedWhole > 0U);
        CHECK(runs.mixedBody > 0U);
        CHECK(runs.mixedInVeins > 0U);
        CHECK(runs.mixedInVeins < runs.mixedWhole + runs.mixedBody);
    }
}

} // namespace

TEST_CASE("the fill places from a run's lanes exactly what each block's own reads place",
          "[terrain][filler][ore]") {
    const RunsTree tree;
    defineRuns(tree);
    const stratum::data::Pack pack = tree.pack();
    const stratum::settings::LoadedSettings loaded = stratum::settings::loadAll(pack);
    const stratum::density::NoiseRegistry noises = stratum::density::NoiseRegistry::create(
        pack, loaded.graph.referencedNoises(), 0, stratum::density::RandomSource::Xoroshiro);

    const bool toggleInterpolated = GENERATE(true, false);
    CAPTURE(toggleInterpolated);
    const FillCheck check =
        fillAndCheck(loaded, noises, toggleInterpolated ? "runs_1" : "runs_1_bare",
                     Chunks{{0, 0}, {-2, 3}, {5, -7}});
    expectEveryRunShape(check, {4}, toggleInterpolated);
    CHECK(check.runs.at(4).mixedWhole + check.runs.at(4).mixedBody > 100U);
}

TEST_CASE("the fill places from the lanes of runs in cells 8, 12 and 16 blocks wide what each "
          "block's own reads place",
          "[terrain][filler][ore]") {
    // A cell 8 or 16 wide makes runs of 16 lanes, in two rows of 8 or one of
    // 16; a cell 12 wide makes runs of one row of 12 lanes, and leaves the
    // chunk's last 4 blocks along x to cells 4 wide, whose runs are 16
    // lanes in rows of 4 again (along z the last 4 are rows of 12, one row
    // a run). A run stands only for blocks in one cell of the world's
    // lattice, which a chunk's cells 12 wide are at chunk coordinates that
    // are multiples of 3; (1, 2) is not, and there the runs 12 wide are left
    // to the blocks' own evaluations while its 4 wide ones still run.
    const RunsTree tree;
    defineRuns(tree);
    const stratum::data::Pack pack = tree.pack();
    const stratum::settings::LoadedSettings loaded = stratum::settings::loadAll(pack);
    const stratum::density::NoiseRegistry noises = stratum::density::NoiseRegistry::create(
        pack, loaded.graph.referencedNoises(), 0, stratum::density::RandomSource::Xoroshiro);

    const std::int32_t sizeHorizontal = GENERATE(2, 3, 4);
    const bool toggleInterpolated = GENERATE(true, false);
    CAPTURE(sizeHorizontal, toggleInterpolated);
    const std::string dimension =
        "runs_" + std::to_string(sizeHorizontal) + (toggleInterpolated ? "" : "_bare");
    if (sizeHorizontal == 3) {
        const FillCheck check =
            fillAndCheck(loaded, noises, dimension, Chunks{{0, 0}, {-3, 3}, {6, -9}, {1, 2}});
        expectEveryRunShape(check, {4, 12}, toggleInterpolated);
        // The 12 wide ran in the three chunks at multiples of 3, all 16 of
        // each's runs a y (a row of 12 a run: twelve in the cell 12 deep,
        // four in the one 4 deep), and in (1, 2) none; the 4 wide ran in all
        // four chunks, all 4 of each's runs a y (three of four rows in the
        // cell 12 deep, one in the one 4 deep).
        CHECK(check.runs.at(12).ran == std::size_t{3} * 16U * 128U);
        CHECK(check.runs.at(4).ran == std::size_t{4} * 4U * 128U);
    } else {
        const FillCheck check =
            fillAndCheck(loaded, noises, dimension, Chunks{{0, 0}, {-2, 3}, {5, -7}});
        expectEveryRunShape(check, {sizeHorizontal * 4}, toggleInterpolated);
    }
}
