// Stratum — the carving stage under a pack's redefinitions, against vanilla.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Two goldens of tools/analysis/carver-golden-probe.sh's variants, each the
// carving stage of vanilla_carver_golden_test.cpp (chunks stored at status
// `minecraft:carvers`, compared on their stored block arrays) with a few
// registry entries redefined. The probe keeps its redefinitions beside its
// regions, and each case here lays them over the shipped worldgen tree, so
// this build and the server read the same pack.
//
//   debug        spec/carvers-a-spec.md Q13.4 (IMPLEMENTER-RULING-01, carvers
//                P2): every shipped carver with `debug_mode` true. Compared
//                state for state, properties included, since a debug state's
//                `waterlogged` is part of the claim; and mark for mark.
//   noise-edges  spec/legacy-named-noise-spec.md Q3.3 (legacy named noise
//                P2): surface noises with an empty `amplitudes` list, an
//                all-zero one, and positive first octaves.
#include "support/temp_path.hpp"

#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/freeze/pipeline.hpp>
#include <stratum/javamath.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/terrain/filler.hpp>
#include <stratum/world/dimension.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

using stratum::data::ResourceLocation;

namespace {

constexpr std::int64_t kSeed = 42;

[[nodiscard]] std::filesystem::path versionDir() {
    return std::filesystem::path(STRATUM_FIXTURES_DIR) / "1.21.11";
}

/// The shipped worldgen tree with a probe's `overrides/` laid over it.
class OverlaidTree {
public:
    OverlaidTree(const std::filesystem::path& overrides, std::string_view stem)
        : path_(stratum::test::tempPath(stem)) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
        const auto options = std::filesystem::copy_options::recursive |
                             std::filesystem::copy_options::overwrite_existing;
        std::filesystem::copy(versionDir() / "worldgen", path_ / "worldgen", options);
        std::filesystem::copy(overrides, path_ / "worldgen", options);
    }

    OverlaidTree(const OverlaidTree&) = delete;
    OverlaidTree& operator=(const OverlaidTree&) = delete;
    OverlaidTree(OverlaidTree&&) = delete;
    OverlaidTree& operator=(OverlaidTree&&) = delete;

    ~OverlaidTree() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] std::filesystem::path worldgen() const { return path_ / "worldgen"; }

private:
    std::filesystem::path path_;
};

/// "name[key=value,...]", keys ascending: chunk::BlockState::toString's
/// spelling, for a state of this build's.
[[nodiscard]] std::string spell(const stratum::settings::BlockState& state) {
    std::string text = state.name.toString();
    if (!state.properties.empty()) {
        text += '[';
        bool first = true;
        for (const auto& [key, value] : state.properties) {
            text += first ? "" : ",";
            text += key;
            text += '=';
            text += value;
            first = false;
        }
        text += ']';
    }
    return text;
}

struct Tally {
    long long chunks = 0;
    long long blocks = 0;
    /// Cells whose block name differs, by (ours, server).
    std::map<std::pair<std::string, std::string>, long long> names;
    /// Cells of one name whose properties differ, by (ours, server).
    std::map<std::pair<std::string, std::string>, long long> states;
    /// How many cells the server stores of each block name.
    std::map<std::string, long long> served;
    long long marksServer = 0;
    long long marksOurs = 0;
    /// Chunks whose mark lists differ in content, order or repeats.
    long long listsDiffering = 0;

    [[nodiscard]] long long count(const std::string& name) const {
        const auto found = served.find(name);
        return found == served.end() ? 0 : found->second;
    }

    [[nodiscard]] static std::string
    listed(const std::map<std::pair<std::string, std::string>, long long>& confusions) {
        std::string text;
        for (const auto& [pair, count] : confusions) {
            text += "\n  ours " + pair.first + " / server " + pair.second + ": " +
                    std::to_string(count);
        }
        return text;
    }
};

/// Every chunk of @p corpus's @p folder stored at the carving stage, filled
/// and carved by @p compiled and compared with what the server stored.
[[nodiscard]] Tally compare(const std::filesystem::path& corpus, const char* folder,
                            const stratum::world::CompiledDimension& compiled) {
    const stratum::settings::NoiseGeometry& geometry = compiled.geometry();
    Tally tally;
    for (const auto& file : std::filesystem::directory_iterator(corpus / folder)) {
        const auto region = stratum::region::RegionFile::open(file.path());
        int regionX = 0;
        int regionZ = 0;
        REQUIRE(std::sscanf(file.path().filename().string().c_str(), "r.%d.%d.mca", &regionX,
                            &regionZ) == 2);
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
            compiled.fillBlocks(chunkX, chunkZ, buffer);

            // The marks as lists, section by section, in the order listed.
            using Mark = std::tuple<std::int32_t, int, int>;
            std::map<std::int32_t, std::vector<Mark>> ours;
            for (const auto& update : buffer.fluidUpdates()) {
                ours[stratum::javamath::floorDiv(update.y, 16)].emplace_back(
                    update.y, update.localX, update.localZ);
            }
            std::map<std::int32_t, std::vector<Mark>> theirs;
            for (const auto& mark : golden.postProcessing()) {
                theirs[stratum::javamath::floorDiv(mark.y, 16)].emplace_back(mark.y, mark.localX,
                                                                             mark.localZ);
            }
            tally.marksOurs += static_cast<long long>(buffer.fluidUpdates().size());
            tally.marksServer += static_cast<long long>(golden.postProcessing().size());
            tally.listsDiffering += ours == theirs ? 0 : 1;

            for (int z = 0; z < 16; ++z) {
                for (int x = 0; x < 16; ++x) {
                    for (std::int32_t y = geometry.minY; y < geometry.maxY(); ++y) {
                        ++tally.blocks;
                        const stratum::settings::BlockState& mine = buffer.at(x, y, z);
                        const auto* stored = golden.blockAt(x, y, z);
                        const std::string servedName =
                            stored != nullptr ? stored->name : "minecraft:air";
                        ++tally.served[servedName];
                        if (mine.name.toString() != servedName) {
                            ++tally.names[{mine.name.toString(), servedName}];
                        } else if (stored != nullptr && spell(mine) != stored->toString()) {
                            ++tally.states[{spell(mine), stored->toString()}];
                        }
                    }
                }
            }
        }
    }
    return tally;
}

[[nodiscard]] std::vector<std::byte> freezeOverlaid(const OverlaidTree& tree) {
    const stratum::data::Pack pack = stratum::data::Pack::open(tree.worldgen());
    return stratum::freeze::write(stratum::freeze::resolve(pack, versionDir() / "biome_parameters",
                                                           versionDir() / "tags" / "block"));
}

[[nodiscard]] std::shared_ptr<const stratum::world::CompiledDimension>
compileDimension(const std::vector<std::byte>& blob, const char* id) {
    return stratum::world::CompiledDimension::compile(
        stratum::freeze::read(blob), ResourceLocation::parse(id),
        stratum::world::BiomeSource::multiNoise(ResourceLocation::parse(id)), kSeed);
}

[[nodiscard]] bool fixturesPresent() {
    return std::filesystem::is_directory(versionDir() / "tags" / "block") &&
           std::filesystem::is_directory(versionDir() / "biome_parameters");
}

} // namespace

TEST_CASE("a carver's debug mode writes the debug states the server writes",
          "[conformance][carver]") {
    const std::filesystem::path corpus = versionDir() / "probes" / "carverdebug_s42";
    if (!fixturesPresent() || !std::filesystem::is_regular_file(corpus / "manifest.json")) {
        SKIP("no debug carving-stage goldens at "
             << corpus
             << "; generate them with tools/probe-worlds generate --only carverdebug_s42 "
                "--accept-eula");
    }
    const OverlaidTree tree(corpus / "overrides", "stratum-carver-debug");
    const std::vector<std::byte> blob = freezeOverlaid(tree);

    const Tally overworld =
        compare(corpus, "overworld", *compileDimension(blob, "minecraft:overworld"));
    WARN("overworld: " << overworld.chunks << " carving-stage chunks, " << overworld.blocks
                       << " blocks; marks: server " << overworld.marksServer << ", ours "
                       << overworld.marksOurs << "; chunks whose lists differ "
                       << overworld.listsDiffering
                       << "; names differing:" << Tally::listed(overworld.names)
                       << "\n states differing:" << Tally::listed(overworld.states));
    std::string seen;
    for (const auto& [name, count] : overworld.served) {
        seen += "\n  " + name + ": " + std::to_string(count);
    }
    WARN("overworld, what the server stored:" << seen);
    CHECK(overworld.chunks > 0);
    CHECK(overworld.names.empty());
    CHECK(overworld.marksOurs == overworld.marksServer);
    CHECK(overworld.listsDiffering == 0);
    // Every debug state the probe's three carvers name was written, so each
    // answer and each shape of state is in the comparison — but for the
    // canyon's lava state (magenta wool), which this corpus never reaches:
    // the aquifer answers lava above the lava height in 101 cells, all of
    // them the cave's.
    for (const char* name :
         {"minecraft:crimson_button", "minecraft:candle", "minecraft:orange_stained_glass",
          "minecraft:glass", "minecraft:acacia_button", "minecraft:blue_wool", "minecraft:red_wool",
          "minecraft:red_stained_glass", "minecraft:oak_slab"}) {
        INFO(name);
        CHECK(overworld.count(name) > 0);
    }
    // Properties, on the debug states (elsewhere this build's states carry
    // none the fill does not write, which is not this case's subject).
    for (const auto& [pair, count] : overworld.states) {
        INFO("ours " << pair.first << " / server " << pair.second << ": " << count);
        const std::string name = pair.first.substr(0, pair.first.find('['));
        CHECK_FALSE((name == "minecraft:crimson_button" || name == "minecraft:candle" ||
                     name == "minecraft:acacia_button" || name == "minecraft:oak_slab"));
    }

    // The Nether: debug lifts only the mask, which nothing can observe.
    const Tally nether = compare(corpus, "nether", *compileDimension(blob, "minecraft:nether"));
    WARN("nether: " << nether.chunks
                    << " carving-stage chunks; names differing:" << Tally::listed(nether.names));
    CHECK(nether.chunks > 0);
    CHECK(nether.names.empty());
    CHECK(nether.marksOurs == nether.marksServer);
    CHECK(nether.listsDiffering == 0);
}

TEST_CASE("edge-shaped noise definitions sample as the server's do", "[conformance][noise]") {
    const std::filesystem::path corpus = versionDir() / "probes" / "noiseedges_s42";
    if (!fixturesPresent() || !std::filesystem::is_regular_file(corpus / "manifest.json")) {
        SKIP("no edge-shaped noise goldens at "
             << corpus
             << "; generate them with tools/probe-worlds generate --only noiseedges_s42 "
                "--accept-eula");
    }
    const OverlaidTree tree(corpus / "overrides", "stratum-noise-edges");
    const std::vector<std::byte> blob = freezeOverlaid(tree);

    // The legacy source: empty `patch`, all-zero `soul_sand_layer`, and
    // positive first octaves for `nether_wart`, `netherrack` and
    // `nether_state_selector`.
    const Tally nether = compare(corpus, "nether", *compileDimension(blob, "minecraft:nether"));
    std::string seen;
    for (const auto& [name, count] : nether.served) {
        seen += "\n  " + name + ": " + std::to_string(count);
    }
    WARN("nether: " << nether.chunks << " carving-stage chunks, " << nether.blocks
                    << " blocks; names differing:" << Tally::listed(nether.names)
                    << "\n what the server stored:" << seen);
    CHECK(nether.chunks > 0);
    CHECK(nether.names.empty());
    CHECK(nether.listsDiffering == 0);

    // The Xoroshiro source: an empty `minecraft:surface`, which every
    // overworld column's surface depth reads.
    const Tally overworld =
        compare(corpus, "overworld", *compileDimension(blob, "minecraft:overworld"));
    WARN("overworld: " << overworld.chunks << " carving-stage chunks, " << overworld.blocks
                       << " blocks; names differing:" << Tally::listed(overworld.names));
    CHECK(overworld.chunks > 0);
    CHECK(overworld.names.empty());
    CHECK(overworld.listsDiffering == 0);
}
