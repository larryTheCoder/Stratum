// Stratum — the carvers, against vanilla's own chunks at the carving stage.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The golden confirmation (IMPLEMENTER-RULING-01 §2) of spec/carvers-a-spec.md
// and spec/carvers-b-spec.md. tools/analysis/carver-golden-probe.sh writes a
// vanilla world's regions, every shipped carver included, and the chunks
// stored at status `minecraft:carvers` are the carving stage itself: carvers
// run, features and promotion not (carvers P1: compare at the carving stage,
// on STORED block arrays — Chunk::decode reads the stored sections, so a
// section of cave air is cave air here, A-Q12.7). Each such chunk is filled
// and carved by this build, through the path every world takes (a pipeline
// resolved with block tags, frozen, thawed, compiled as a dimension), and
// compared block name for block name.

#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/freeze/pipeline.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/terrain/filler.hpp>
#include <stratum/world/dimension.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using stratum::data::ResourceLocation;

namespace {

[[nodiscard]] std::filesystem::path versionDir() {
    return std::filesystem::path(STRATUM_FIXTURES_DIR) / "1.21.11";
}

constexpr std::array<std::int64_t, 2> kSeeds = {42, 31337};

struct Dimension {
    const char* folder;
    const char* settings;
    const char* biomes;
};

constexpr std::array<Dimension, 2> kDimensions{{
    {"overworld", "minecraft:overworld", "minecraft:overworld"},
    {"nether", "minecraft:nether", "minecraft:nether"},
}};

struct Tally {
    long long chunks = 0;
    long long blocks = 0;
    long long exact = 0;
    std::map<std::pair<std::string, std::string>, long long> confusions;
    /// Post-processing marks (A-Q12.4): the server's, ours, and both.
    long long marksServer = 0;
    long long marksOurs = 0;
    long long marksShared = 0;
};

} // namespace

TEST_CASE("the carvers carve what vanilla's carving stage stored", "[conformance][carver]") {
    if (!std::filesystem::is_directory(versionDir() / "tags" / "block") ||
        !std::filesystem::is_directory(versionDir() / "biome_parameters")) {
        SKIP("no block tags or biome parameters under " << versionDir()
                                                        << "; run tools/fetch-vanilla");
    }
    // The path every world takes: the pack resolved with its block tags,
    // frozen to a blob and thawed, and compiled as a dimension, which carves
    // because the pipeline carries carvers.
    const stratum::data::Pack pack = stratum::data::Pack::open(versionDir() / "worldgen");
    const std::vector<std::byte> blob = stratum::freeze::write(stratum::freeze::resolve(
        pack, versionDir() / "biome_parameters", versionDir() / "tags" / "block"));

    std::size_t corpora = 0;
    for (const std::int64_t seed : kSeeds) {
        const std::filesystem::path corpus =
            versionDir() / "probes" / ("carvers_s" + std::to_string(seed));
        if (!std::filesystem::is_regular_file(corpus / "manifest.json")) {
            SKIP("no carving-stage goldens at " << corpus
                                                << "; generate them with "
                                                   "tools/analysis/carver-golden-probe.sh");
        }
        ++corpora;
        for (const Dimension& dimension : kDimensions) {
            const ResourceLocation settingsId = ResourceLocation::parse(dimension.settings);
            const auto compiled = stratum::world::CompiledDimension::compile(
                stratum::freeze::read(blob), settingsId,
                stratum::world::BiomeSource::multiNoise(ResourceLocation::parse(dimension.biomes)),
                seed);
            const stratum::settings::NoiseGeometry& geometry = compiled->geometry();

            Tally tally;
            for (const auto& file :
                 std::filesystem::directory_iterator(corpus / dimension.folder)) {
                const auto region = stratum::region::RegionFile::open(file.path());
                int regionX = 0;
                int regionZ = 0;
                REQUIRE(std::sscanf(file.path().filename().string().c_str(), "r.%d.%d.mca",
                                    &regionX, &regionZ) == 2);
                for (int index = 0; index < 1024; ++index) {
                    const std::int32_t chunkX = (regionX * 32) + (index % 32);
                    const std::int32_t chunkZ = (regionZ * 32) + (index / 32);
                    if (!region.hasChunk(chunkX, chunkZ)) {
                        continue;
                    }
                    const auto golden = stratum::chunk::Chunk::decode(
                        stratum::nbt::read(region.readChunk(chunkX, chunkZ)).root);
                    if (golden.status() != "minecraft:carvers") {
                        continue;
                    }
                    ++tally.chunks;
                    stratum::terrain::ChunkBuffer buffer(geometry);
                    compiled->fillBlocks(chunkX, chunkZ, buffer);
                    std::set<std::tuple<std::int32_t, int, int>> ourMarks;
                    for (const auto& update : buffer.fluidUpdates()) {
                        ourMarks.emplace(update.y, update.localX, update.localZ);
                    }
                    std::set<std::tuple<std::int32_t, int, int>> serverMarks;
                    for (const auto& mark : golden.postProcessing()) {
                        serverMarks.emplace(mark.y, mark.localX, mark.localZ);
                    }
                    tally.marksOurs += static_cast<long long>(ourMarks.size());
                    tally.marksServer += static_cast<long long>(serverMarks.size());
                    for (const auto& mark : ourMarks) {
                        tally.marksShared += serverMarks.contains(mark) ? 1 : 0;
                    }
                    for (int z = 0; z < 16; ++z) {
                        for (int x = 0; x < 16; ++x) {
                            for (std::int32_t y = geometry.minY; y < geometry.maxY(); ++y) {
                                ++tally.blocks;
                                const std::string ours = buffer.at(x, y, z).name.toString();
                                const auto* theirs = golden.blockAt(x, y, z);
                                const std::string served =
                                    theirs != nullptr ? theirs->name : "minecraft:air";
                                if (ours == served) {
                                    ++tally.exact;
                                } else {
                                    ++tally.confusions[{ours, served}];
                                }
                            }
                        }
                    }
                }
            }
            std::string listed;
            for (const auto& [pair, count] : tally.confusions) {
                listed += "\n  ours " + pair.first + " / server " + pair.second + ": " +
                          std::to_string(count);
            }
            WARN("seed " << seed << " " << dimension.folder << ": " << tally.chunks
                         << " carving-stage chunks, " << tally.exact << " of " << tally.blocks
                         << " blocks exact; marks: server " << tally.marksServer << ", ours "
                         << tally.marksOurs << ", shared " << tally.marksShared << listed);
            CHECK(tally.chunks > 0);
            CHECK(tally.exact == tally.blocks);
            // Every mark this build makes is the server's.
            CHECK(tally.marksShared == tally.marksOurs);
            // And the server makes none this build does not — except, in the
            // Nether, marks on lava-sea cells no carver touched: 233 at seed
            // 31337, in four chunks, none at seed 42. A run that marked every
            // lava cell the Nether carver writes shared none of them, so they
            // are not carver output (spec/carvers-a-spec.md Q12.6 holds), and
            // no spec says what makes them. Reported to the owner
            // (IMPLEMENTER-RULING-01: a golden disagreement is reported, not
            // reconciled), and pinned so a change in them shows.
            const long long unexplained =
                (seed == 31337 && std::string(dimension.folder) == "nether") ? 233 : 0;
            CHECK(tally.marksServer - tally.marksShared == unexplained);
        }
    }
    CHECK(corpora == kSeeds.size());
}
