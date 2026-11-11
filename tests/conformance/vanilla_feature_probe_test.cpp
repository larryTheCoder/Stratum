// Stratum — the decoration framework, against vanilla's own marker features.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// tools/analysis/feature-probe.sh generates vanilla's own overworld with every
// biome's features replaced by a variant's MARKERS: placed features writing
// one block no terrain holds, behind the placement modifiers under test. Where
// a marker lands is what the framework drew — the decoration seed, the
// feature's index in its step, each modifier's draws and reads — so each case
// here decorates the same chunks from the same pack and requires every marker
// the server stored, where it stored it, and no other.
//
//   seed         the population and feature seeds: one marker a step
//   draws        the modifiers' and providers' draws, and their order
//   terrain      the modifiers that read the world: heightmaps, the surface
//                filters, block predicates, the scan, fixed positions
//   order        every shipped placed feature as a marker under its own name,
//                the biomes untouched: the order features run in, and which
//                features a chunk runs
//   biomefilter  the `biome` modifier, a marker per biome
//   misc         the origin's height, the two noise counts, a scan that
//                leaves the world (and `count_on_every_layer`, unrun)
//
// And the feature types, on the same footing (each case says its own):
//
//   oresizes, oresurface, oreexposed, orediscard
//                `minecraft:ore` as the probe configures it
//   ores, springs, disks
//                the shipped features of one type, alone in the shipped world
//
// A chunk is compared when the server stored it at status `minecraft:full`,
// which is when it ran its own features and had its neighbours' written in.
#include "support/temp_path.hpp"

#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/feature/feature.hpp>
#include <stratum/freeze/pipeline.hpp>
#include <stratum/javamath.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/terrain/filler.hpp>
#include <stratum/world/dimension.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using stratum::data::ResourceLocation;

namespace {

constexpr std::int64_t kSeed = 42;

using ChunkAt = std::pair<std::int32_t, std::int32_t>;

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

[[nodiscard]] std::shared_ptr<const stratum::world::CompiledDimension>
compileOverworld(const std::filesystem::path& worldgen) {
    const stratum::data::Pack pack = stratum::data::Pack::open(worldgen);
    const ResourceLocation overworld = ResourceLocation::parse("minecraft:overworld");
    return stratum::world::CompiledDimension::compile(
        stratum::freeze::read(stratum::freeze::write(stratum::freeze::resolve(
            pack, versionDir() / "biome_parameters", versionDir() / "tags" / "block"))),
        overworld, stratum::world::BiomeSource::multiNoise(overworld), kSeed);
}

/// One chunk before any feature: its blocks, the terrain's heightmaps, and
/// its biomes over the noise height.
struct BaseChunk {
    stratum::terrain::ChunkBuffer blocks;
    stratum::feature::TerrainHeightmaps heightmaps;
    std::vector<const ResourceLocation*> biomes;
};

/// The undecorated terrain every variant shares: the probes differ in their
/// features alone, so a chunk is filled once for all of them.
class Terrain {
public:
    [[nodiscard]] static Terrain& shared() {
        static Terrain terrain;
        return terrain;
    }

    [[nodiscard]] const BaseChunk& at(const ChunkAt chunk) {
        const auto found = chunks_.find(chunk);
        if (found != chunks_.end()) {
            return found->second;
        }
        const stratum::settings::NoiseGeometry& geometry = dimension_->geometry();
        BaseChunk base{.blocks = stratum::terrain::ChunkBuffer(geometry),
                       .heightmaps = {},
                       .biomes = std::vector<const ResourceLocation*>(
                           static_cast<std::size_t>(16 * (geometry.height / 4)))};
        dimension_->fillBlocks(chunk.first, chunk.second, base.blocks);
        dimension_->fillBiomes(chunk.first, chunk.second, base.biomes, base.blocks);
        base.heightmaps = stratum::feature::terrainHeightmaps(base.blocks);
        return chunks_.emplace(chunk, std::move(base)).first->second;
    }

private:
    Terrain() : dimension_(compileOverworld(versionDir() / "worldgen")) {}

    /// Kept: the biomes point into its parameter list.
    std::shared_ptr<const stratum::world::CompiledDimension> dimension_;
    std::map<ChunkAt, BaseChunk> chunks_;
};

struct Golden {
    std::map<ChunkAt, stratum::chunk::Chunk> full;
    /// The chunks around them, at whatever status: their biomes are stored.
    std::map<ChunkAt, stratum::chunk::Chunk> ring;
};

[[nodiscard]] Golden readGolden(const std::filesystem::path& corpus) {
    Golden golden;
    for (const auto& file : std::filesystem::directory_iterator(corpus / "overworld")) {
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
            auto chunk = stratum::chunk::Chunk::decode(
                stratum::nbt::read(region.readChunk(chunkX, chunkZ)).root);
            if (chunk.status() == "minecraft:full") {
                golden.full.emplace(ChunkAt{chunkX, chunkZ}, std::move(chunk));
            } else {
                golden.ring.emplace(ChunkAt{chunkX, chunkZ}, std::move(chunk));
            }
        }
    }
    return golden;
}

struct Score {
    long long chunks = 0;
    /// Marker blocks the server stored, and how many of them this build
    /// wrote at the same position with the same block.
    long long served = 0;
    long long matched = 0;
    /// A marker of ours where the server stored none or another, and the
    /// reverse; the first few of each, spelled.
    long long oursOnly = 0;
    long long serverOnly = 0;
    /// Positions of either kind within a feature's reach of a quart whose
    /// biome this build and the server disagree on: the open ties of the
    /// multi-noise search (SPEC §8), which the `biome` modifier then reads
    /// differently.
    long long biomeApart = 0;
    /// Positions where both sides hold a marker and not the same one: two
    /// features that each replace the other's block met there, and the later
    /// one's stands. Within a chunk the order is the features'; across chunks
    /// it is the order the chunks were decorated in, which the server does
    /// not fix.
    long long contested = 0;
    /// Blocks this build's decoration changed, and how many of them hold what
    /// the server holds there: what the features wrote, apart from terrain
    /// the two sides hold differently for another reason (fluid that flowed).
    long long written = 0;
    long long writtenMatched = 0;
    std::map<std::pair<std::string, std::string>, long long> contestedPairs;
    /// What is left, by (ours, server).
    std::map<std::pair<std::string, std::string>, long long> unexplained;
    std::string examples;
    std::size_t supported = 0;
    std::size_t unsupported = 0;
    /// Marker blocks the server stored, by name.
    std::map<std::string, long long> byName;
};

/// Decorates every full chunk of the corpus and its neighbours with the
/// corpus's own pack, and compares the markers — the blocks the features
/// this build runs write — in every full chunk.
[[nodiscard]] Score score(const std::string& corpusName) {
    const std::filesystem::path corpus = versionDir() / "probes" / corpusName;
    const Golden golden = readGolden(corpus);
    const OverlaidTree tree(corpus / "overrides", "stratum-" + corpusName);
    const auto dimension = compileOverworld(tree.worldgen());
    const stratum::feature::FeatureSet* features = dimension->features();
    REQUIRE(features != nullptr);
    Score result;
    result.supported = features->supportedCount();
    result.unsupported = features->unsupported().size();
    std::set<std::string> markers;
    for (const stratum::settings::BlockState& state : features->writtenStates()) {
        markers.insert(state.name.toString());
    }

    // Every chunk a compared chunk's blocks can come from is decorated; every
    // chunk a decorated chunk reads is filled.
    std::set<ChunkAt> decorated;
    for (const auto& [at, chunk] : golden.full) {
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                decorated.insert({at.first + dx, at.second + dz});
            }
        }
    }
    Terrain& terrain = Terrain::shared();
    std::map<ChunkAt, stratum::terrain::ChunkBuffer> blocks;
    const auto bufferOf = [&](const ChunkAt at) -> stratum::terrain::ChunkBuffer& {
        const auto found = blocks.find(at);
        return found != blocks.end() ? found->second
                                     : blocks.emplace(at, terrain.at(at).blocks).first->second;
    };
    for (const ChunkAt& at : decorated) {
        std::array<stratum::feature::RegionChunk, 9> region{};
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                const ChunkAt neighbour{at.first + dx, at.second + dz};
                const BaseChunk& base = terrain.at(neighbour);
                region.at((static_cast<std::size_t>(dz + 1) * 3U) +
                          static_cast<std::size_t>(dx + 1)) = {
                    .blocks = &bufferOf(neighbour),
                    .heightmaps = &base.heightmaps,
                    .biomes = base.biomes,
                    .minQuartY = dimension->geometry().minY / 4,
                };
            }
        }
        dimension->decorate(at.first, at.second, region);
    }

    const stratum::settings::NoiseGeometry& geometry = dimension->geometry();

    // Every quart of a decorated chunk whose biome the two sides disagree on.
    struct Quart {
        std::int32_t x;
        std::int32_t y;
        std::int32_t z;
    };

    std::vector<Quart> apart;
    for (const ChunkAt& at : decorated) {
        const auto inFull = golden.full.find(at);
        const auto inRing = golden.ring.find(at);
        const stratum::chunk::Chunk* theirs = nullptr;
        if (inFull != golden.full.end()) {
            theirs = &inFull->second;
        } else if (inRing != golden.ring.end()) {
            theirs = &inRing->second;
        }
        if (theirs == nullptr) {
            continue;
        }
        const std::vector<const ResourceLocation*>& ours = terrain.at(at).biomes;
        for (std::size_t cell = 0; cell < ours.size(); ++cell) {
            const auto qx = static_cast<std::int32_t>(cell % 4);
            const auto qz = static_cast<std::int32_t>((cell / 4) % 4);
            const std::int32_t qy = (geometry.minY / 4) + static_cast<std::int32_t>(cell / 16);
            const std::string* stored = theirs->biomeAt(qx * 4, qy * 4, qz * 4);
            if (stored != nullptr && *stored != ours[cell]->toString()) {
                apart.push_back({(at.first * 4) + qx, qy, (at.second * 4) + qz});
            }
        }
    }
    // A feature's blocks lie within sixteen blocks of its position, and its
    // position's biome is read from a quart within a quart of it.
    const auto nearBiomesApart = [&](const std::int32_t x, const std::int32_t y,
                                     const std::int32_t z) {
        return std::ranges::any_of(apart, [&](const Quart& quart) {
            return std::abs((quart.x * 4) - x) <= 24 && std::abs((quart.z * 4) - z) <= 24 &&
                   std::abs((quart.y * 4) - y) <= 16;
        });
    };
    int listed = 0;
    for (const auto& [at, chunk] : golden.full) {
        ++result.chunks;
        const stratum::terrain::ChunkBuffer& mine = blocks.at(at);
        const stratum::terrain::ChunkBuffer& undecorated = terrain.at(at).blocks;
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                for (std::int32_t y = geometry.minY; y < geometry.maxY(); ++y) {
                    const std::string ours = mine.at(x, y, z).name.toString();
                    const bool wrote = mine.at(x, y, z) != undecorated.at(x, y, z);
                    const auto* stored = chunk.blockAt(x, y, z);
                    const std::string theirs = stored != nullptr ? stored->name : "minecraft:air";
                    const bool oursMarker = markers.contains(ours);
                    const bool theirsMarker = markers.contains(theirs);
                    result.written += wrote ? 1 : 0;
                    result.writtenMatched += wrote && ours == theirs ? 1 : 0;
                    if (theirsMarker) {
                        ++result.served;
                        ++result.byName[theirs];
                    }
                    if (oursMarker && theirsMarker && ours == theirs) {
                        ++result.matched;
                        continue;
                    }
                    if (!oursMarker && !theirsMarker) {
                        continue;
                    }
                    result.oursOnly += oursMarker ? 1 : 0;
                    result.serverOnly += theirsMarker ? 1 : 0;
                    if (oursMarker && theirsMarker) {
                        ++result.contested;
                        ++result.contestedPairs[{ours, theirs}];
                        continue;
                    }
                    if (nearBiomesApart((at.first * 16) + x, y, (at.second * 16) + z)) {
                        ++result.biomeApart;
                        continue;
                    }
                    ++result.unexplained[{ours, theirs}];
                    if (listed++ < 12) {
                        result.examples += "\n  (";
                        result.examples += std::to_string((at.first * 16) + x) + ", ";
                        result.examples += std::to_string(y) + ", ";
                        result.examples += std::to_string((at.second * 16) + z) + "): ours ";
                        result.examples += ours;
                        result.examples += ", server ";
                        result.examples += theirs;
                    }
                }
            }
        }
    }
    return result;
}

[[nodiscard]] bool present(const std::string& corpus) {
    return std::filesystem::is_directory(versionDir() / "tags" / "block") &&
           std::filesystem::is_directory(versionDir() / "biome_parameters") &&
           std::filesystem::is_regular_file(versionDir() / "probes" / corpus / "manifest.json");
}

/// The case every variant is: all of the server's markers, and no others —
/// but @p biomeApart positions, where the two sides read different biomes.
void requireExact(const std::string& corpus, const long long served, const std::size_t unsupported,
                  const long long biomeApart = 0) {
    if (!present(corpus)) {
        SKIP("no feature probe at " << (versionDir() / "probes" / corpus)
                                    << "; generate it with tools/probe-worlds generate --only "
                                    << corpus << " --accept-eula");
    }
    const Score result = score(corpus);
    std::string names;
    for (const auto& [name, count] : result.byName) {
        names += "\n  " + name + ": " + std::to_string(count);
    }
    WARN(corpus << ": " << result.chunks << " full chunks; features run " << result.supported
                << ", unrun " << result.unsupported << "; markers the server stored "
                << result.served << ", matched " << result.matched << ", ours alone "
                << result.oursOnly << ", the server's alone " << result.serverOnly
                << ", of which where the biomes differ " << result.biomeApart << result.examples
                << "\n the server's markers:" << names);
    CHECK(result.chunks == 288);
    CHECK(result.unsupported == unsupported);
    CHECK(result.served == served);
    CHECK(result.biomeApart == biomeApart);
    CHECK(result.oursOnly + result.serverOnly == result.biomeApart);
    CHECK(result.matched + result.serverOnly == result.served);
}

} // namespace

TEST_CASE("the decoration seed places one marker a step where the server's are",
          "[conformance][feature]") {
    requireExact("featseed_s42", 3456, 0);
}

TEST_CASE("the placement modifiers and providers draw what the server's draw",
          "[conformance][feature]") {
    requireExact("featdraws_s42", 10491, 0);
}

TEST_CASE("the placement modifiers that read the world read what the server's read",
          "[conformance][feature]") {
    requireExact("featterrain_s42", 7133, 0);
}

TEST_CASE("every shipped placed feature runs at the server's index, in the server's chunks",
          "[conformance][feature]") {
    requireExact("featorder_s42", 15520, 0);
}

TEST_CASE("the biome modifier keeps a feature to the blocks of its biome",
          "[conformance][feature]") {
    requireExact("featbiomefilter_s42", 11558, 0, 13);
}

TEST_CASE("a feature's origin is the lowest layer, and the noise counts count as the server's",
          "[conformance][feature]") {
    requireExact("featmisc_s42", 20242, 1);
}

TEST_CASE("an ore of every shipped size is the server's blob, block for block",
          "[conformance][feature][ore]") {
    // Twelve sizes, two ores a chunk each, replacing whatever is there.
    requireExact("featoresizes_s42", 496482, 0);
}

TEST_CASE("an ore attempt in the open is given up where the server gives it up",
          "[conformance][feature][ore]") {
    // Sizes 9 and 33 between y 50 and 140: under the terrain, across its
    // surface, and above it.
    requireExact("featoresurface_s42", 57927, 0);
}

namespace {

/// The cases whose ores draw as they are placed: where two chunks' ores meet,
/// the server's own two runs differ (SPEC §11, "The ore feature, measured"),
/// so these are held to a share, not to the block.
void requireNearly(const std::string& corpus, const std::size_t supported,
                   const double sharePerMille) {
    if (!present(corpus)) {
        SKIP("no feature probe at " << (versionDir() / "probes" / corpus)
                                    << "; generate it with tools/probe-worlds generate --only "
                                    << corpus << " --accept-eula");
    }
    const Score result = score(corpus);
    std::string left;
    long long unexplained = 0;
    for (const auto& [pair, count] : result.unexplained) {
        left += "\n  ours " + pair.first + ", server " + pair.second + ": " + std::to_string(count);
        unexplained += count;
    }
    WARN(corpus << ": " << result.chunks << " full chunks; features run " << result.supported
                << ", unrun " << result.unsupported << "; markers the server stored "
                << result.served << ", matched " << result.matched << ", ours alone "
                << result.oursOnly << ", the server's alone " << result.serverOnly
                << ", of which near biomes that differ " << result.biomeApart
                << " and contested between two markers " << result.contested << "; left over "
                << unexplained << ":" << left);
    CHECK(result.chunks == 288);
    CHECK(result.supported == supported);
    CHECK(result.unsupported == 0);
    CHECK(result.served > 50000);
    // Out of every thousand of the server's markers.
    CHECK(static_cast<double>(result.served - result.matched) * 1000.0 <=
          sharePerMille * static_cast<double>(result.served));
    CHECK(static_cast<double>(unexplained) * 1000.0 <=
          sharePerMille * static_cast<double>(result.served));
    // What is left over is a drawing ore's, never another's: each pair holds
    // the block of a shipped ore that discards, or one of the probe's own.
    for (const auto& [pair, count] : result.unexplained) {
        INFO("ours " << pair.first << ", server " << pair.second << ": " << count);
        const auto draws = [](const std::string& name) {
            return name.ends_with("coal_ore") || name.ends_with("gold_ore") ||
                   name.ends_with("diamond_ore") || name.ends_with("_concrete");
        };
        CHECK((draws(pair.first) || draws(pair.second)));
    }
}

} // namespace

TEST_CASE("an ore beside air discards the blocks the server discards",
          "[conformance][feature][ore]") {
    // One ore a feature, so none is put out of step by another: exact on the
    // corpus this was measured on (168 694 blocks).
    requireNearly("featoreexposed_s42", 32, 1.0);
    // Sixteen a feature, at chances of a half, one and none.
    requireNearly("featorediscard_s42", 3, 2.0);
}

TEST_CASE("the shipped ores, alone in the shipped world, are the server's",
          "[conformance][feature][ore]") {
    // Every biome's `minecraft:ore` features and nothing else: thirty
    // features, all run. 1 749 597 blocks stored; 99.6 in 100 the same, and
    // the rest contested between two ores, beside a biome the two sides read
    // differently, or a drawing ore's.
    requireNearly("featores_s42", 30, 5.0);
}

TEST_CASE("the shipped springs are placed where the server places them",
          "[conformance][feature][spring]") {
    const std::string corpus = "featsprings_s42";
    if (!present(corpus)) {
        SKIP("no feature probe at " << (versionDir() / "probes" / corpus)
                                    << "; generate it with tools/probe-worlds generate --only "
                                    << corpus << " --accept-eula");
    }
    // A spring's block is water or lava, as the terrain's own is, so the
    // markers here are every fluid block of 288 chunks. What this build's
    // springs wrote is told apart by what the terrain held before them.
    const Score result = score(corpus);
    WARN(corpus << ": features run " << result.supported << "; springs written " << result.written
                << ", of which the server's " << result.writtenMatched
                << "; fluid the server stored " << result.served << ", ours alone "
                << result.oursOnly << ", the server's alone " << result.serverOnly);
    CHECK(result.chunks == 288);
    // spring_water, spring_lava and spring_lava_frozen.
    CHECK(result.supported == 3);
    CHECK(result.unsupported == 0);
    CHECK(result.written >= 40);
    CHECK(result.writtenMatched == result.written);
    CHECK(result.oursOnly == 0);
    // The server's alone: fluid that flowed after generation, which no
    // spring did (a spring's update is still pending in a frozen world).
    CHECK(result.serverOnly * 5000 <= result.served);
}

TEST_CASE("the shipped disks are the server's, but where two met in another order",
          "[conformance][feature][disk]") {
    const std::string corpus = "featdisks_s42";
    if (!present(corpus)) {
        SKIP("no feature probe at " << (versionDir() / "probes" / corpus)
                                    << "; generate it with tools/probe-worlds generate --only "
                                    << corpus << " --accept-eula");
    }
    const Score result = score(corpus);
    std::string pairs;
    for (const auto& [pair, count] : result.contestedPairs) {
        pairs +=
            "\n  ours " + pair.first + ", server " + pair.second + ": " + std::to_string(count);
    }
    WARN(corpus << ": features run " << result.supported << ", unrun " << result.unsupported
                << "; written " << result.written << ", of which the server's "
                << result.writtenMatched << "; contested between two disks " << result.contested
                << ":" << pairs);
    CHECK(result.chunks == 288);
    // disk_sand, disk_clay, disk_gravel and their copies; disk_grass waits
    // on the `solid` predicate.
    CHECK(result.supported == 4);
    CHECK(result.unsupported == 1);
    CHECK(result.written >= 10000);
    CHECK(result.unexplained.empty());
    CHECK(result.oursOnly == result.contested);
    CHECK(result.serverOnly == result.contested);
    // One disk's block for another's, where a chunk's disk reached a block
    // before its neighbour's on one side and after it on the other.
    CHECK(result.contested * 50 <= result.written);
}

// For the bench, not the suite: any corpus of feature-probe.sh, by name.
//   STRATUM_FEATURE_CORPUS=featore9_s42 stratum_conformance_tests "[.featurelab]"
TEST_CASE("a feature probe named in the environment, scored", "[.featurelab]") {
    const char* corpus = std::getenv("STRATUM_FEATURE_CORPUS"); // NOLINT(concurrency-mt-unsafe)
    REQUIRE(corpus != nullptr);
    REQUIRE(present(corpus));
    const Score result = score(corpus);
    WARN(corpus << ": " << result.chunks << " full chunks; features run " << result.supported
                << ", unrun " << result.unsupported << "; markers the server stored "
                << result.served << ", matched " << result.matched << ", ours alone "
                << result.oursOnly << ", the server's alone " << result.serverOnly
                << ", of which where the biomes differ " << result.biomeApart
                << " and contested between two markers " << result.contested << "; written "
                << result.written << ", of which the server's " << result.writtenMatched
                << result.examples);
    std::string left;
    for (const auto& [pair, count] : result.unexplained) {
        left += "\n  ours " + pair.first + ", server " + pair.second + ": " + std::to_string(count);
    }
    std::string pairs;
    for (const auto& [pair, count] : result.contestedPairs) {
        pairs +=
            "\n  ours " + pair.first + ", server " + pair.second + ": " + std::to_string(count);
    }
    WARN("unexplained:" << left << "\ncontested:" << pairs);
}
