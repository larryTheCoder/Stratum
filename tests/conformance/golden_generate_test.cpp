// Stratum — `stratum generate`'s regions against the regions the server wrote.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// WHAT THIS IS. Every other golden case calls the library and compares what
// it returns. This one runs the shipped binary — `stratum generate`, from the
// version root tools/fetch-vanilla left, into region files on disk — and
// reads those files back the way anything else would, through the region
// reader and the chunk decoder. So what it scores is the whole path a
// generated world takes to disk: the dimension the CLI compiles for a name,
// the column it stores (the dimension type's, not the noise height), the
// section layout, the palettes, the biomes over every stored section, the
// heightmaps, the fluid updates in PostProcessing, and the region container.
//
// HOW IT SCORES. Not with `stratum diff`, whose exit code has no tolerance:
// the server's fluids MOVE after generating (spec Q8.1; support/
// fluid_flow.hpp says how much and why), and Stratum runs no fluid ticks, so
// a world generated exactly right still differs from a saved one wherever
// water or lava flowed. Every block is put in one bucket:
//
//   exact         the same state, properties and all
//   level         the same fluid with another `level`: flowing, in the
//                 golden, where the aquifer placed a source
//   flowed        another category, attributed to post-generation flow by
//                 `explainedByFlow` — the five shapes flow leaves
//   material      solid in both, another block: surface MATERIAL near biome
//                 borders, the M4 residual golden_overworld_test.cpp pins on
//                 its own grid (flow replaces nothing solid)
//   unexplained   anything else, which must be nothing
//
// What the model alone decides is pinned exactly (material, and the count
// of everything that is not flow); what flow decides is bounded and its
// shape required, because CI regenerates these regions and the flow remnant
// differs from run to run (SPEC §7).
//
// The biomes are compared cell for cell over the whole stored column, and
// the ones that disagree are pinned by number: each is a TIE in the
// multi-noise search that vanilla's tree resolves to the earlier row and
// `ParameterList::find` to the later (SPEC §11's tie-break entry). The
// heightmaps must agree in every column whose blocks agree: a column where
// flow moved a fluid may legitimately disagree, and none other may.
//
// PostProcessing cannot be compared here: every golden chunk is at full
// status, and the server empties a chunk's lists as it promotes it, so the
// goldens hold none (vanilla_aquifer_fluid_update_test.cpp holds the lists
// to probe worlds' un-promoted chunks instead). Ours are written — the
// count of fluid updates is the model's and is pinned — and every mark a
// regenerated golden might ever keep is required among them.
//
// WHAT IT GENERATES, sized for the Debug build `ctest --preset conformance`
// runs: the End's whole r.0.0, all 1024 chunks (`--region 0,0`), and the
// overworld's low quarter of r.0.0, chunks 0..15 on each axis (`--chunks
// 0,0:15,15`), the overworld costing about twenty times the End a chunk.
//
// The fixtures are Mojang-derived and never committed (SPEC §12). Without
// them this skips; CI generates them (tools/probe-worlds' goldens shards).
#include "support/cli_process.hpp"
#include "support/fluid_flow.hpp"
#include "support/temp_path.hpp"

#include <stratum/biome/parameter_list.hpp>
#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/settings/noise_settings.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using stratum::chunk::Chunk;
using stratum::chunk::Heightmap;
using stratum::test::Category;
using stratum::test::categoryOf;
using stratum::test::explainedByFlow;
using stratum::test::GoldenRegion;
using stratum::test::isFluid;

/// One of the eight golden seeds: negative and using all 64 bits, which
/// the overworld's Xoroshiro source reads and the End's LCG folds to 48.
constexpr std::int64_t kSeed = -4172144997902289642LL;

/// The binary under test; empty in a build without the CLI, where the cases
/// skip saying so.
#ifdef STRATUM_CLI_PATH
constexpr std::string_view kCli = STRATUM_CLI_PATH;
#else
constexpr std::string_view kCli;
#endif

/// Two threads: what this box's shared-build rules allow, and the bytes do
/// not depend on it (cli_test.cpp holds that).
constexpr const char* kThreads = "2";

[[nodiscard]] std::filesystem::path versionDir() {
    return std::filesystem::path(STRATUM_FIXTURES_DIR) / "1.21.11";
}

/// A region file's coordinates; r.0.0 unless said.
struct RegionAt {
    std::int32_t x = 0;
    std::int32_t z = 0;

    [[nodiscard]] std::string fileName() const {
        return "r." + std::to_string(x) + "." + std::to_string(z) + ".mca";
    }
};

[[nodiscard]] std::filesystem::path goldenRegion(const char* dimension, std::int64_t seed = kSeed,
                                                 RegionAt region = {}) {
    return versionDir() / "regions" / ("seed-" + std::to_string(seed)) / dimension /
           region.fileName();
}

struct Window {
    std::int32_t minX = 0;
    std::int32_t minZ = 0;
    std::int32_t maxX = 0;
    std::int32_t maxZ = 0;
};

struct Score {
    std::size_t chunks = 0;
    /// Chunk-level disagreements: status, DataVersion, yPos, the sections
    /// stored. Each is named as it is found.
    std::size_t chunkFindings = 0;
    std::size_t blocks = 0;
    std::size_t exact = 0;
    std::size_t level = 0;
    std::size_t flowed = 0;
    std::size_t material = 0;
    std::size_t unexplained = 0;
    std::size_t biomeCells = 0;
    std::size_t biomesExact = 0;
    /// Per heightmap kind, columns compared and columns that agree.
    std::size_t columns = 0;
    std::array<std::size_t, 4> heightsExact{};
    /// A heightmap that disagrees in a column whose blocks all agree by name.
    std::size_t heightsUnexplained = 0;
    /// PostProcessing marks: ours, the golden's, and those in both.
    std::size_t marksOurs = 0;
    std::size_t marksGolden = 0;
    std::size_t marksShared = 0;

    /// The biome cells that disagree, by their lowest block, and the solid
    /// blocks of another material: what the preset cases attribute.
    struct CellApart {
        std::int32_t x = 0;
        std::int32_t y = 0;
        std::int32_t z = 0;
        std::string ours;
        std::string golden;
    };

    std::vector<CellApart> cellsApart;
    std::vector<std::array<std::int32_t, 3>> materialAt;
    /// The golden's highest WORLD_SURFACE entry, as stored (the first air
    /// above the top block, counted from the column's floor).
    std::int64_t highestSurface = 0;
};

constexpr std::array<Heightmap, 4> kHeightmaps = {Heightmap::WorldSurface, Heightmap::OceanFloor,
                                                  Heightmap::MotionBlocking,
                                                  Heightmap::MotionBlockingNoLeaves};

[[nodiscard]] Chunk decodeChunk(const stratum::region::RegionFile& file, std::int32_t chunkX,
                                std::int32_t chunkZ) {
    return Chunk::decode(stratum::nbt::read(file.readChunk(chunkX, chunkZ)).root);
}

/// Runs `stratum generate` for @p dimension over @p range into a fresh
/// directory, and returns the region file it wrote.
[[nodiscard]] std::filesystem::path generate(const char* dimension, const std::string& range,
                                             const std::filesystem::path& out,
                                             std::int64_t seed = kSeed, RegionAt region = {}) {
    std::filesystem::remove_all(out);
    const stratum::test::CliResult result = stratum::test::runCli(
        std::string(kCli), "generate \"" + versionDir().string() + "\" --dimension " + dimension +
                               " --seed " + std::to_string(seed) + " " + range + " --out \"" +
                               out.string() + "\" --threads " + kThreads + " --no-carvers");
    INFO(result.output);
    REQUIRE(result.exitCode == 0);
    // The timing line, for whoever reads the log: not a measurement of
    // record (Debug, a shared machine), but the same numbers the CLI prints.
    WARN(result.output);
    const std::filesystem::path written = out / region.fileName();
    REQUIRE(std::filesystem::is_regular_file(written));
    return written;
}

void scoreChunk(const Chunk& ours, const Chunk& golden, GoldenRegion& flowReader, Score& score) {
    ++score.chunks;
    const auto finding = [&](const std::string& what) {
        ++score.chunkFindings;
        UNSCOPED_INFO("chunk (" << golden.x() << ", " << golden.z() << "): " << what);
    };
    if (ours.x() != golden.x() || ours.z() != golden.z()) {
        finding("stored as (" + std::to_string(ours.x()) + ", " + std::to_string(ours.z()) + ")");
        return;
    }
    if (ours.status() != golden.status()) {
        finding("status " + ours.status() + ", the server's " + golden.status());
    }
    if (ours.dataVersion() != golden.dataVersion()) {
        finding("DataVersion " + std::to_string(ours.dataVersion()) + ", the server's " +
                std::to_string(golden.dataVersion()));
    }
    if (ours.lowestSection() != golden.lowestSection()) {
        finding("yPos " + std::to_string(ours.lowestSection()) + ", the server's " +
                std::to_string(golden.lowestSection()));
    }
    const auto& ourSections = ours.sections();
    const auto& theirSections = golden.sections();
    const bool sameSections =
        std::ranges::equal(ourSections, theirSections,
                           [](const auto& lhs, const auto& rhs) { return lhs.y == rhs.y; });
    if (!sameSections) {
        finding("stores sections " + std::to_string(ours.minY()) + ".." +
                std::to_string(ours.maxY()) + ", the server " + std::to_string(golden.minY()) +
                ".." + std::to_string(golden.maxY()));
        return;
    }

    // Which columns hold a block whose NAME differs: the heightmaps are a
    // function of names, so only those columns may disagree on one.
    std::array<bool, 256> columnDiffers{};
    for (std::size_t s = 0; s < ourSections.size(); ++s) {
        const stratum::chunk::Section& mine = ourSections[s];
        const stratum::chunk::Section& theirs = theirSections[s];
        for (std::size_t i = 0; i < stratum::chunk::kBlocksPerSection; ++i) {
            const stratum::chunk::BlockState& ourState = mine.palette[mine.blocks[i]];
            const stratum::chunk::BlockState& theirState = theirs.palette[theirs.blocks[i]];
            ++score.blocks;
            if (ourState == theirState) {
                ++score.exact;
                continue;
            }
            const int x = static_cast<int>(i % 16U);
            const int z = static_cast<int>((i / 16U) % 16U);
            const std::int32_t y = (mine.y * 16) + static_cast<std::int32_t>(i / 256U);
            const Category ourCategory = categoryOf(ourState.name);
            const Category theirCategory = categoryOf(theirState.name);
            const std::int32_t worldX = (golden.x() * 16) + x;
            const std::int32_t worldZ = (golden.z() * 16) + z;
            if (ourState.name == theirState.name) {
                // The same fluid, flowing in the golden: a level only flow
                // gives (the aquifer and the sea place sources).
                if (isFluid(theirCategory) && stratum::test::fluidLevel(&theirState) > 0) {
                    ++score.level;
                } else {
                    ++score.unexplained;
                    UNSCOPED_INFO("unexplained: ours " << ourState.toString() << ", golden "
                                                       << theirState.toString() << " at " << worldX
                                                       << " " << y << " " << worldZ);
                }
                continue;
            }
            columnDiffers[(static_cast<std::size_t>(z) * 16U) + static_cast<std::size_t>(x)] = true;
            if (ourCategory != theirCategory) {
                if (explainedByFlow(flowReader, worldX, y, worldZ, theirCategory, ourCategory)) {
                    ++score.flowed;
                } else {
                    ++score.unexplained;
                    UNSCOPED_INFO("unexplained: ours " << ourState.toString() << ", golden "
                                                       << theirState.toString() << " at " << worldX
                                                       << " " << y << " " << worldZ);
                }
            } else if (ourCategory == Category::Solid) {
                ++score.material;
                score.materialAt.push_back({worldX, y, worldZ});
            } else {
                ++score.unexplained;
                UNSCOPED_INFO("unexplained: ours " << ourState.toString() << ", golden "
                                                   << theirState.toString() << " at " << worldX
                                                   << " " << y << " " << worldZ);
            }
        }

        for (std::size_t cell = 0; cell < stratum::chunk::kBiomesPerSection; ++cell) {
            ++score.biomeCells;
            const bool stored = !mine.biomes.empty() && !theirs.biomes.empty();
            if (stored &&
                mine.biomePalette[mine.biomes[cell]] == theirs.biomePalette[theirs.biomes[cell]]) {
                ++score.biomesExact;
            } else {
                score.cellsApart.push_back(
                    {.x = (golden.x() * 16) + static_cast<int>((cell % 4U) * 4U),
                     .y = (mine.y * 16) + static_cast<int>((cell / 16U) * 4U),
                     .z = (golden.z() * 16) + static_cast<int>(((cell / 4U) % 4U) * 4U),
                     .ours = mine.biomes.empty() ? "<none>" : mine.biomePalette[mine.biomes[cell]],
                     .golden = theirs.biomes.empty() ? "<none>"
                                                     : theirs.biomePalette[theirs.biomes[cell]]});
                // Named by the cell's lowest block, as `stratum diff` does.
                UNSCOPED_INFO(
                    "biome cell at "
                    << (golden.x() * 16) + static_cast<int>((cell % 4U) * 4U) << " "
                    << (mine.y * 16) + static_cast<int>((cell / 16U) * 4U) << " "
                    << (golden.z() * 16) + static_cast<int>(((cell / 4U) % 4U) * 4U) << ": ours "
                    << (mine.biomes.empty() ? "<none>" : mine.biomePalette[mine.biomes[cell]])
                    << ", golden "
                    << (theirs.biomes.empty() ? "<none>"
                                              : theirs.biomePalette[theirs.biomes[cell]]));
            }
        }
    }

    score.columns += 256U;
    for (std::size_t kind = 0; kind < kHeightmaps.size(); ++kind) {
        const auto mine = ours.heightmap(kHeightmaps[kind]);
        const auto theirs = golden.heightmap(kHeightmaps[kind]);
        if (!mine.has_value() || !theirs.has_value()) {
            finding("has no " + std::string(stratum::chunk::heightmapName(kHeightmaps[kind])));
            continue;
        }
        for (std::size_t column = 0; column < 256U; ++column) {
            if (kind == 0) {
                score.highestSurface = std::max(
                    score.highestSurface, static_cast<std::int64_t>((*theirs)[column].value_or(0)));
            }
            if ((*mine)[column] == (*theirs)[column]) {
                ++score.heightsExact[kind];
            } else if (!columnDiffers[column]) {
                ++score.heightsUnexplained;
            }
        }
    }

    std::vector<std::tuple<std::int32_t, int, int>> mine;
    std::vector<std::tuple<std::int32_t, int, int>> theirs;
    for (const stratum::chunk::PostProcessingMark& mark : ours.postProcessing()) {
        mine.emplace_back(mark.y, mark.localX, mark.localZ);
    }
    for (const stratum::chunk::PostProcessingMark& mark : golden.postProcessing()) {
        theirs.emplace_back(mark.y, mark.localX, mark.localZ);
    }
    score.marksOurs += mine.size();
    score.marksGolden += theirs.size();
    std::ranges::sort(mine);
    std::ranges::sort(theirs);
    std::vector<std::tuple<std::int32_t, int, int>> shared;
    std::ranges::set_intersection(mine, theirs, std::back_inserter(shared));
    score.marksShared += shared.size();
}

[[nodiscard]] Score scoreRegion(const std::filesystem::path& ours,
                                const std::filesystem::path& golden, const Window& window,
                                RegionAt region = {}) {
    const auto ourFile = stratum::region::RegionFile::open(ours);
    const auto goldenFile = stratum::region::RegionFile::open(golden);
    GoldenRegion flowReader(golden, region.x, region.z);
    // Exactly the window, and nothing beside it: a chunk outside the range
    // asked for is one the CLI was not asked to write.
    const auto expected = static_cast<std::size_t>(window.maxX - window.minX + 1) *
                          static_cast<std::size_t>(window.maxZ - window.minZ + 1);
    CHECK(ourFile.chunkCount() == expected);
    Score score;
    for (std::int32_t chunkZ = window.minZ; chunkZ <= window.maxZ; ++chunkZ) {
        for (std::int32_t chunkX = window.minX; chunkX <= window.maxX; ++chunkX) {
            REQUIRE(ourFile.hasChunk(chunkX, chunkZ));
            REQUIRE(goldenFile.hasChunk(chunkX, chunkZ));
            scoreChunk(decodeChunk(ourFile, chunkX, chunkZ),
                       decodeChunk(goldenFile, chunkX, chunkZ), flowReader, score);
        }
    }
    return score;
}

void report(const char* dimension, const Score& score) {
    WARN(dimension << ": " << score.chunks << " chunks, " << score.blocks << " blocks: "
                   << score.exact << " exact, " << score.level << " level only, " << score.flowed
                   << " flowed, " << score.material << " material, " << score.unexplained
                   << " unexplained; biomes " << score.biomesExact << " of " << score.biomeCells
                   << "; heightmaps WORLD_SURFACE " << score.heightsExact[0] << ", OCEAN_FLOOR "
                   << score.heightsExact[1] << ", MOTION_BLOCKING " << score.heightsExact[2]
                   << ", MOTION_BLOCKING_NO_LEAVES " << score.heightsExact[3] << " of "
                   << score.columns << " (" << score.heightsUnexplained
                   << " unexplained); PostProcessing marks " << score.marksOurs << " ours, "
                   << score.marksGolden << " the server's, " << score.marksShared << " shared");
}

/// A scratch output directory, removed however the case ends.
class ScratchDir {
public:
    explicit ScratchDir(std::string_view stem) : path_(stratum::test::tempPath(stem)) {}

    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;

    ~ScratchDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

/// The fixtures a case reads, or the reason it cannot run.
[[nodiscard]] std::string missingInputs(const char* dimension, std::int64_t seed = kSeed,
                                        RegionAt region = {}) {
    if (kCli.empty()) {
        return "this build has no stratum binary (STRATUM_BUILD_CLI=OFF)";
    }
    if (!std::filesystem::is_directory(versionDir() / "worldgen" / "noise_settings") ||
        !std::filesystem::is_directory(versionDir() / "biome_parameters")) {
        return "no worldgen or biome_parameters fixtures under " + versionDir().string() +
               "; run tools/fetch-vanilla";
    }
    if (!std::filesystem::is_regular_file(goldenRegion(dimension, seed, region))) {
        return "no golden region at " + goldenRegion(dimension, seed, region).string() +
               "; generate it with tools/fetch-vanilla --generate-regions";
    }
    return {};
}

} // namespace

TEST_CASE("stratum generate writes the End the server wrote, every block and every biome of r.0.0",
          "[conformance][cli][end]") {
    const std::string missing = missingInputs("end");
    if (!missing.empty()) {
        SKIP(missing);
    }
    const ScratchDir out("stratum-generate-end");
    const Score score =
        scoreRegion(generate("end", "--region 0,0", out.path()), goldenRegion("end"),
                    Window{.minX = 0, .minZ = 0, .maxX = 31, .maxZ = 31});
    report("end", score);

    CHECK(score.chunks == 1024U);
    CHECK(score.chunkFindings == 0U);
    // The End's dimension type stores sections 0..15: twice its noise
    // height, the upper half air.
    CHECK(score.blocks == std::size_t{1024} * 16U * 4096U);
    CHECK(score.exact == score.blocks);
    CHECK(score.biomesExact == score.biomeCells);
    CHECK(score.biomeCells == std::size_t{1024} * 16U * 64U);
    for (const std::size_t exact : score.heightsExact) {
        CHECK(exact == score.columns);
    }
    // No aquifers in the End, so no fluid updates on either side.
    CHECK(score.marksOurs == 0U);
    CHECK(score.marksGolden == 0U);
}

TEST_CASE("stratum generate writes the overworld the server wrote, but for fluid that moved",
          "[conformance][cli][world]") {
    const std::string missing = missingInputs("overworld");
    if (!missing.empty()) {
        SKIP(missing);
    }
    const ScratchDir out("stratum-generate-overworld");
    const Score score = scoreRegion(generate("overworld", "--chunks 0,0:15,15", out.path()),
                                    goldenRegion("overworld"),
                                    Window{.minX = 0, .minZ = 0, .maxX = 15, .maxZ = 15});
    report("overworld", score);

    CHECK(score.chunks == 256U);
    CHECK(score.chunkFindings == 0U);
    // Sections -4..19, the overworld's whole stored column.
    CHECK(score.blocks == std::size_t{256} * 24U * 4096U);

    // Every block is exact, flow, or surface material; nothing else.
    CHECK(score.unexplained == 0U);
    CHECK(score.exact + score.level + score.flowed + score.material == score.blocks);
    // The material residual is the model's alone, so it is pinned. It was
    // 863 blocks of surface material under water higher up the column,
    // where the server has dirt or grass_block; the water height is cleared
    // by air (pipeline engine v18, SPEC §11 "The water height is the fluid
    // under the last air"), and none are left, as on
    // golden_overworld_test.cpp's own grid.
    CHECK(score.material == 0U);
    // Flow is the server's and run-dependent: bounded, as the other golden
    // cases bound it, under 1 in 100 000. 65 blocks on the first set.
    CHECK((score.flowed + score.level) * 100000U < score.blocks);

    // Every biome cell but two ties, both cave cells at the same seed: at
    // (96, -16, 16) and (92, -36, 48) lush_caves (row 7591 of the 7593-row
    // list) and deep_lukewarm_ocean (rows 14 and 15) are at EQUAL fitness,
    // 18309841 and 19775809, and the server stored deep_lukewarm_ocean, the
    // earlier row, as at the five columns SPEC §11 records. The library's
    // to fix, not the CLI's: the cells are read where the server reads them,
    // or there would be thousands.
    CHECK(score.biomeCells == std::size_t{256} * 24U * 64U);
    CHECK(score.biomeCells - score.biomesExact == 2U);

    // Flow moved nothing at a column's top on this set, but a regeneration's
    // remnant may: only a column whose blocks differ may disagree.
    CHECK(score.heightsUnexplained == 0U);

    // The aquifer's fluid updates, every one written (the model's count),
    // and every mark a golden keeps among them.
    CHECK(score.marksOurs == 25936U);
    CHECK(score.marksShared == score.marksGolden);
}

// The two other overworld presets (PROGRESS, M4: "Full-biome goldens for
// amplified and large_biomes"). Their probes fix the biome, so until these
// regions neither preset's biome assignment — large_biomes' own
// `temperature_large` and `vegetation_large`, each preset's own `depth`,
// through the search — nor its surface over its real biomes had been read
// off the server. tools/fetch-vanilla --world-preset generates them: the
// same frozen, terrain-only world as the other goldens, under
// server.properties' level-type.
namespace {

struct PresetCase {
    const char* preset;
    std::int64_t seed;
    /// The region, and the 16x16 chunks of it scored, by their lowest chunk.
    RegionAt region{};
    std::int32_t chunkX = 0;
    std::int32_t chunkZ = 0;
    /// The model's own residual, pinned: biome cells apart (every one a tie
    /// the server resolved to the earlier row), the surface material that
    /// follows those cells' columns, and the fluid updates written.
    std::size_t cellsApart = 0;
    std::size_t material = 0;
    std::size_t marks = 0;
    /// What the golden's highest stored WORLD_SURFACE must reach.
    std::int64_t surfaceAtLeast = 0;
};

struct Attribution {
    std::size_t tied = 0;
    std::size_t earlier = 0;
    std::size_t columns = 0;
    std::size_t materialByCell = 0;
    std::string pairs;
};

/// Why each biome cell is apart: the preset's own climate router at the
/// cell, quantised, against every row of the overworld's table — a TIE when
/// the server's biome is on a row at the minimum fitness beside ours.
[[nodiscard]] Attribution attribute(const PresetCase& which, const Score& score) {
    using stratum::data::ResourceLocation;
    using stratum::settings::RouterEntry;
    Attribution result;
    const stratum::data::Pack pack = stratum::data::Pack::open(versionDir() / "worldgen");
    const std::ifstream file(versionDir() / "biome_parameters" / "minecraft" / "overworld.json");
    std::stringstream contents;
    contents << file.rdbuf();
    const auto list = stratum::biome::ParameterList::fromJson(
        nlohmann::json::parse(contents.str()), ResourceLocation::parse("minecraft:overworld"));
    const stratum::settings::LoadedSettings loaded = stratum::settings::loadAll(pack);
    const auto& settings =
        loaded.settings.at(ResourceLocation::parse(std::string("minecraft:") + which.preset));
    const auto noises =
        stratum::density::NoiseRegistry::create(pack, loaded.graph.referencedNoises(), which.seed,
                                                stratum::density::RandomSource::Xoroshiro);
    const stratum::density::Interpreter interpreter(
        loaded.graph, noises,
        stratum::density::CellGeometry{.width = settings.geometry.cellWidth(),
                                       .height = settings.geometry.cellHeight()});
    std::set<std::pair<std::int32_t, std::int32_t>> columns;
    std::set<std::string> pairs;
    for (const Score::CellApart& cell : score.cellsApart) {
        columns.emplace(cell.x, cell.z);
        const stratum::density::Point at{.x = cell.x, .y = cell.y, .z = cell.z};
        const auto axis = [&](RouterEntry entry) {
            return interpreter.evaluate(settings.router.at(entry), at);
        };
        const auto sample = stratum::biome::QuantizedSample::of(
            stratum::biome::ClimateSample{.temperature = axis(RouterEntry::Temperature),
                                          .humidity = axis(RouterEntry::Vegetation),
                                          .continentalness = axis(RouterEntry::Continents),
                                          .erosion = axis(RouterEntry::Erosion),
                                          .depth = axis(RouterEntry::Depth),
                                          .weirdness = axis(RouterEntry::Ridges)});
        std::int64_t best = 0;
        std::vector<std::size_t> tie;
        std::size_t row = 0;
        for (const auto& entry : list.entries()) {
            const std::int64_t fitness =
                stratum::biome::QuantizedPoint::of(entry.parameters).fitness(sample);
            if (tie.empty() || fitness < best) {
                best = fitness;
                tie.assign(1, row);
            } else if (fitness == best) {
                tie.push_back(row);
            }
            ++row;
        }
        std::size_t oursRow = list.size();
        std::size_t theirsRow = list.size();
        for (const std::size_t tied : tie) {
            const std::string biome = list.entries()[tied].biome.toString();
            if (biome == cell.ours) {
                oursRow = tied;
            }
            if (biome == cell.golden && theirsRow == list.size()) {
                theirsRow = tied;
            }
        }
        if (oursRow != list.size() && theirsRow != list.size()) {
            ++result.tied;
            if (theirsRow < oursRow) {
                ++result.earlier;
            }
            pairs.insert(" " + cell.golden + " (row " + std::to_string(theirsRow) + ") over " +
                         cell.ours + " (row " + std::to_string(oursRow) + ")");
        } else {
            UNSCOPED_INFO("not a tie: biome cell at " << cell.x << " " << cell.y << " " << cell.z
                                                      << ": ours " << cell.ours << ", golden "
                                                      << cell.golden);
        }
    }
    result.columns = columns.size();
    for (const std::string& pair : pairs) {
        result.pairs += pair + ";";
    }
    for (const auto& block : score.materialAt) {
        const bool near = std::ranges::any_of(columns, [&](const auto& column) {
            return block[0] >= column.first - 8 && block[0] < column.first + 12 &&
                   block[2] >= column.second - 8 && block[2] < column.second + 12;
        });
        if (near) {
            ++result.materialByCell;
        } else {
            UNSCOPED_INFO("material block away from any cell apart: " << block[0] << " " << block[1]
                                                                      << " " << block[2]);
        }
    }
    return result;
}

void scorePreset(const PresetCase& which) {
    const std::string missing = missingInputs(which.preset, which.seed, which.region);
    if (!missing.empty()) {
        SKIP(missing << " --world-preset " << which.preset);
    }
    const ScratchDir out(std::string("stratum-generate-") + which.preset);
    const Score score = scoreRegion(generate(which.preset,
                                             "--chunks " + std::to_string(which.chunkX) + "," +
                                                 std::to_string(which.chunkZ) + ":" +
                                                 std::to_string(which.chunkX + 15) + "," +
                                                 std::to_string(which.chunkZ + 15),
                                             out.path(), which.seed, which.region),
                                    goldenRegion(which.preset, which.seed, which.region),
                                    Window{.minX = which.chunkX,
                                           .minZ = which.chunkZ,
                                           .maxX = which.chunkX + 15,
                                           .maxZ = which.chunkZ + 15},
                                    which.region);
    report(which.preset, score);
    const Attribution found = attribute(which, score);
    WARN(which.preset << " at seed " << which.seed << ": " << score.cellsApart.size()
                      << " biome cells apart in " << found.columns << " column(s), " << found.tied
                      << " of them ties holding the server's biome (" << found.earlier
                      << " where the server took the earlier row), pairs:" << found.pairs << "; "
                      << found.materialByCell << " of " << score.material
                      << " material blocks within 8 blocks of such a cell's column; highest "
                         "stored WORLD_SURFACE "
                      << score.highestSurface);
    CHECK(score.highestSurface >= which.surfaceAtLeast);
    CHECK(score.chunks == 256U);
    CHECK(score.chunkFindings == 0U);
    CHECK(score.blocks == std::size_t{256} * 24U * 4096U);
    CHECK(score.biomeCells == std::size_t{256} * 24U * 64U);

    // Every block is exact, flow, or surface material; nothing else. Flow is
    // the server's and run-dependent, so bounded: under 1 in 50 000, twice
    // the overworld's bound, for amplified's tall terrain sheds more of it
    // (131, 45, 1 and 94 blocks on the first set).
    CHECK(score.unexplained == 0U);
    CHECK(score.exact + score.level + score.flowed + score.material == score.blocks);
    CHECK((score.flowed + score.level) * 50000U < score.blocks);
    CHECK(score.heightsUnexplained == 0U);

    // The biomes: every cell but ties in the multi-noise search, each
    // holding the server's biome on a row at the minimum fitness and each
    // resolved by the server to the EARLIER row — the open tie-break of SPEC
    // §11, on the preset's own router, and nothing of the preset's. Whole
    // columns of them where the tie is on an axis that does not vary down
    // the column. Surface material follows a column's biome, so any
    // material block must lie by such a column (none does on this set).
    CHECK(score.cellsApart.size() == which.cellsApart);
    CHECK(found.tied == score.cellsApart.size());
    CHECK(found.earlier == found.tied);
    CHECK(score.material == which.material);
    CHECK(found.materialByCell == score.material);

    // The aquifer's fluid updates, the model's count; a full-status golden
    // keeps none (the overworld case above says why).
    CHECK(score.marksOurs == which.marks);
    CHECK(score.marksShared == score.marksGolden);
}

} // namespace

TEST_CASE("stratum generate writes the amplified overworld the server wrote",
          "[conformance][cli][world][presets]") {
    // Not r.0.0, which is ocean at both seeds, and amplified leaves ocean
    // as it is: the regions, and the quarter of each, where this engine's
    // amplified differs most from its default preset among the sixteen
    // regions round the origin — 2.3 million blocks of each quarter another
    // block, terrain past y 300, and every coordinate negative.
    scorePreset({.preset = "amplified",
                 .seed = 42,
                 .region = {.x = -1, .z = -1},
                 .chunkX = -16,
                 .chunkZ = -32,
                 .cellsApart = 173U,
                 .material = 0U,
                 .marks = 6689U,
                 .surfaceAtLeast = 300});
    scorePreset({.preset = "amplified",
                 .seed = kSeed,
                 .region = {.x = -2, .z = -2},
                 .chunkX = -48,
                 .chunkZ = -48,
                 .cellsApart = 96U,
                 .material = 0U,
                 .marks = 7359U,
                 .surfaceAtLeast = 300});
}

TEST_CASE("stratum generate writes the large_biomes overworld the server wrote",
          "[conformance][cli][world][presets]") {
    // The low quarter of each. Over the whole region 99 in 100 biome cells
    // are another biome than the default preset's at the same seed.
    scorePreset(
        {.preset = "large_biomes", .seed = 42, .cellsApart = 13U, .material = 0U, .marks = 590U});
    scorePreset({.preset = "large_biomes",
                 .seed = kSeed,
                 .cellsApart = 22U,
                 .material = 0U,
                 .marks = 14359U});
}
