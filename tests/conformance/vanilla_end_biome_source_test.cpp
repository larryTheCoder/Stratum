// Stratum — the End's biome source, `minecraft:the_end`, against every End quart on disk.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// WHAT THIS SCORES. The SHIPPED path: world::CompiledDimension compiled from a
// thawed blob with world::BiomeSource::theEnd(), its fillBiomes against every
// quart the server stored, in every chunk of every End region there is:
//
//   * the eight golden `end/r.0.0` (six distinct worlds: java.util.Random
//     keeps 48 bits, so 0 and LONG_MIN are one End and -1 and LONG_MAX
//     another). All of r.0.0 is within 1024 blocks of the origin, so these
//     say "the_end, inside the ring" and nothing else;
//   * end-islands' r.-1.-1 and r.64.0 at seeds 0 and 42
//     (tools/analysis/end-islands-probe.sh): a negative quadrant inside the
//     ring, and 1024 chunks 32768 blocks out holding all four outer biomes;
//   * end-biomes' two regions at each of four more worlds (the same script,
//     --name end-biomes): a far region with all four outer biomes, signs
//     mixed, and one straddling the ring on a negative axis — chunks (-64, 0)
//     and (0, -64) sit exactly at 4096;
//   * end-biome-router-probe.sh's 23 dimensions, each the End's settings
//     with one router entry changed, scored the same way after compiling each
//     variant from a doctored copy of the pinned pack;
//   * and its `--set sample` pair: a dimension whose erosion differs from
//     block to block, and the world's own End again.
//
// The server stores sixteen sections, its dimension type's 256 blocks, and
// assigns every one a biome, where the End's NOISE height is 128. fillBiomes'
// noise-height form scores y 0..127 and its range form — the one ext's
// encoder fills PocketMine-MP's whole column with — y 128..255. On vanilla's
// End the column is one biome either way; the router probe's y-varying
// erosions are what makes the upper half say something.
//
// And every quart again through biome::Source::at at its own lowest block,
// which is what ChunkFiller's surface pass calls: fillBiomes is handed a
// chunk, so Source::at is the one shipped path where a position becomes its
// chunk by floorDiv, and only this pass holds that to the server.
//
// CONTROLS. A case that only ever agreed could not tell a rule from its
// neighbours, so each reading the rule takes is also scored WRONG, against
// the same server biomes, and must lose somewhere: the chunk's origin rather
// than its centre, and every other column of the chunk; truncation rather
// than floorDiv from quart to chunk; a strict ring; erosion read at
// 4qy + 1..3 or at qy rather than at 4qy; and the world's own island field
// rather than the router's erosion.
//
// SKIPS a corpus that is absent, naming the script that makes it; FAILS on a
// partial one, which is a broken corpus rather than a missing one. The
// fixtures are Mojang-derived and never committed (SPEC §12).
#include "support/temp_path.hpp"

#include <stratum/biome/source.hpp>
#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/freeze/pipeline.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/world/dimension.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using stratum::biome::EndRule;
using stratum::data::ResourceLocation;
using stratum::density::Point;
using stratum::settings::RouterEntry;
using stratum::world::BiomeSource;
using stratum::world::CompiledDimension;

namespace {

constexpr std::int32_t kRegionChunks = 32;
constexpr std::size_t kChunksPerRegion = std::size_t{kRegionChunks} * kRegionChunks;
/// Quart layers the server stores: its dimension type's 256 blocks.
constexpr std::int32_t kStoredLayers = 64;
/// Quart layers fillBiomes' noise-height form fills: the End's 128 blocks.
constexpr std::int32_t kNoiseLayers = 32;
constexpr std::size_t kQuartsPerLayer = 16;
constexpr std::size_t kQuartsPerChunk = static_cast<std::size_t>(kStoredLayers) * kQuartsPerLayer;
constexpr std::size_t kQuartsPerRegion = kChunksPerRegion * kQuartsPerChunk;

[[nodiscard]] std::filesystem::path fixtures() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11";
}

[[nodiscard]] bool havePack() {
    return std::filesystem::is_directory(fixtures() / "worldgen" / "noise_settings") &&
           std::filesystem::is_directory(fixtures() / "biome_parameters");
}

[[nodiscard]] stratum::freeze::Pipeline thaw(const stratum::data::Pack& pack) {
    return stratum::freeze::read(
        stratum::freeze::write(stratum::freeze::resolve(pack, fixtures() / "biome_parameters")));
}

/// Biome names as small integers, so a region's million quarts are a
/// million bytes and comparing two is comparing bytes.
class Biomes {
public:
    [[nodiscard]] std::uint8_t code(const std::string& name) {
        const auto [at, added] = codes_.try_emplace(name, static_cast<std::uint8_t>(names_.size()));
        if (added) {
            names_.push_back(name);
        }
        return at->second;
    }

    [[nodiscard]] std::uint8_t code(const ResourceLocation* biome) {
        const auto found = pointers_.find(biome);
        if (found != pointers_.end()) {
            return found->second;
        }
        const std::uint8_t result = code(biome->toString());
        pointers_.emplace(biome, result);
        return result;
    }

    [[nodiscard]] const std::string& name(std::uint8_t code) const { return names_.at(code); }

private:
    std::map<std::string, std::uint8_t> codes_;
    std::map<const ResourceLocation*, std::uint8_t> pointers_;
    std::vector<std::string> names_;
};

struct RegionFile {
    std::int64_t seed = 0;
    std::filesystem::path path;
    std::int32_t regionX = 0;
    std::int32_t regionZ = 0;

    [[nodiscard]] std::int32_t chunkX(std::size_t chunk) const {
        return (regionX * kRegionChunks) + static_cast<std::int32_t>(chunk % kRegionChunks);
    }

    [[nodiscard]] std::int32_t chunkZ(std::size_t chunk) const {
        return (regionZ * kRegionChunks) + static_cast<std::int32_t>(chunk / kRegionChunks);
    }

    [[nodiscard]] std::string name() const {
        return "seed " + std::to_string(seed) + " r." + std::to_string(regionX) + "." +
               std::to_string(regionZ);
    }
};

/// SKIPs when none of @p files is on disk, and FAILs when only some are.
void requireCorpus(const std::vector<RegionFile>& files, const std::string& make) {
    std::vector<std::string> missing;
    for (const RegionFile& file : files) {
        if (!std::filesystem::is_regular_file(file.path)) {
            missing.push_back(file.path.string());
        }
    }
    if (missing.size() == files.size()) {
        SKIP("none of the " << files.size() << " region(s) of this corpus is on disk (first: "
                            << files.front().path << "); generate them with " << make);
    }
    if (!missing.empty()) {
        std::ostringstream list;
        for (const std::string& path : missing) {
            list << "\n  " << path;
        }
        FAIL("a partial corpus: " << missing.size() << " of " << files.size()
                                  << " region(s) missing, which is a broken corpus rather than "
                                     "an absent one; regenerate it with "
                                  << make << list.str());
    }
}

/// The server's biome at every stored quart of every chunk of @p region:
/// chunk by chunk (x fastest), then `(layer * 4 + qz) * 4 + qx` over all 64
/// stored layers.
[[nodiscard]] std::vector<std::uint8_t> storedBiomes(const RegionFile& region, Biomes& biomes) {
    const auto file = stratum::region::RegionFile::open(region.path);
    std::vector<std::uint8_t> stored(kQuartsPerRegion);
    for (std::size_t chunk = 0; chunk < kChunksPerRegion; ++chunk) {
        const std::int32_t chunkX = region.chunkX(chunk);
        const std::int32_t chunkZ = region.chunkZ(chunk);
        CAPTURE(chunkX, chunkZ);
        REQUIRE(file.hasChunk(chunkX, chunkZ));
        const auto decoded =
            stratum::chunk::Chunk::decode(stratum::nbt::read(file.readChunk(chunkX, chunkZ)).root);
        REQUIRE(decoded.status() == "minecraft:full");
        std::size_t filled = 0;
        for (const stratum::chunk::Section& section : decoded.sections()) {
            if (section.y < 0 || section.y >= kStoredLayers / 4) {
                continue;
            }
            REQUIRE(section.biomes.size() == stratum::chunk::kBiomesPerSection);
            for (std::size_t cell = 0; cell < stratum::chunk::kBiomesPerSection; ++cell) {
                // Stored as y, then z, then x within the section, four of each.
                const std::size_t layer = (static_cast<std::size_t>(section.y) * 4U) + (cell / 16U);
                stored[(chunk * kQuartsPerChunk) + (layer * kQuartsPerLayer) + (cell % 16U)] =
                    biomes.code(section.biomePalette.at(section.biomes[cell]));
                ++filled;
            }
        }
        REQUIRE(filled == kQuartsPerChunk);
    }
    return stored;
}

/// What a dimension under test needs beside its CompiledDimension: its
/// router read through an interpreter over the same pipeline, for
/// biome::Source::at and for the wrong readings the controls score.
struct Reader {
    stratum::freeze::Pipeline pipeline;
    const stratum::settings::NoiseSettings* settings;
    stratum::density::NoiseRegistry noises;
    stratum::density::Interpreter interpreter;

    Reader(stratum::freeze::Pipeline thawed, const ResourceLocation& id, std::int64_t seed)
        : pipeline(std::move(thawed)), settings(&pipeline.settings.at(id)),
          noises(stratum::density::NoiseRegistry::create(
              pipeline.noises,
              pipeline.graph.noisesReachableFrom(std::vector<stratum::density::NodeIndex>{
                  settings->router.entries.begin(), settings->router.entries.end()}),
              seed,
              settings->legacyRandomSource ? stratum::density::RandomSource::Legacy
                                           : stratum::density::RandomSource::Xoroshiro)),
          interpreter(pipeline.graph, noises,
                      stratum::density::CellGeometry{.width = settings->geometry.cellWidth(),
                                                     .height = settings->geometry.cellHeight()}) {}

    /// The End's rule with each of its choices open: the chunk, where in it
    /// the sample sits, at what y, and whether the ring is inclusive.
    [[nodiscard]] const ResourceLocation&
    endBiome(std::int32_t chunkX, std::int32_t chunkZ, Point sample, bool strictRing,
             stratum::density::Interpreter::CornerCache& cache) const {
        const std::int64_t squared =
            (std::int64_t{chunkX} * chunkX) + (std::int64_t{chunkZ} * chunkZ);
        if (strictRing ? squared < EndRule::kCentralRadiusSquared
                       : squared <= EndRule::kCentralRadiusSquared) {
            return EndRule::central();
        }
        return EndRule::outer(
            interpreter.evaluate(settings->router.at(RouterEntry::Erosion), sample, cache));
    }
};

struct Score {
    /// Quarts scored, and those exact, through CompiledDimension::fillBiomes.
    std::size_t quarts = 0;
    std::size_t exact = 0;
    /// The same quarts through biome::Source::at, at each one's own corner.
    std::size_t exactAt = 0;
    std::map<std::string, std::size_t> chunksByBiome;
    std::string firstMiss;
};

/// Every quart of @p stored, scored twice, both times on shipped code:
///
///   * through @p dimension's fillBiomes — its noise-height form for layers
///     0..31 and its range form for 32..63, the half of the server's column
///     above the End's noise height (the form ext's encoder uses);
///   * through biome::Source::at at the quart's own lowest block, (16 chunkX
///     + 4 qx, 4 layer, 16 chunkZ + 4 qz) — the lookup ChunkFiller's
///     surface pass makes, and the one place a position becomes its chunk
///     by floorDiv rather than arriving as one. fillBiomes is handed the
///     chunk, so without this pass truncation there would go unscored.
[[nodiscard]] Score score(const CompiledDimension& dimension, const Reader& reader,
                          const RegionFile& region, const std::vector<std::uint8_t>& stored,
                          Biomes& biomes) {
    const stratum::biome::Source source = stratum::biome::Source::theEnd();
    stratum::density::Interpreter::CornerCache cache(reader.interpreter.cacheSize());
    std::vector<const ResourceLocation*> low(static_cast<std::size_t>(kNoiseLayers) *
                                             kQuartsPerLayer);
    std::vector<const ResourceLocation*> high(
        static_cast<std::size_t>(kStoredLayers - kNoiseLayers) * kQuartsPerLayer);
    Score result;
    const auto miss = [&](std::int32_t chunkX, std::int32_t chunkZ, std::size_t quart,
                          std::uint8_t server, const ResourceLocation& ours, const char* path) {
        if (result.firstMiss.empty()) {
            result.firstMiss = std::string(path) + ": chunk (" + std::to_string(chunkX) + ", " +
                               std::to_string(chunkZ) + ") quart " + std::to_string(quart) +
                               ": server " + biomes.name(server) + ", ours " + ours.toString();
        }
    };
    for (std::size_t chunk = 0; chunk < kChunksPerRegion; ++chunk) {
        const std::int32_t chunkX = region.chunkX(chunk);
        const std::int32_t chunkZ = region.chunkZ(chunk);
        dimension.fillBiomes(chunkX, chunkZ, low);
        dimension.fillBiomes(chunkX, chunkZ, kNoiseLayers, high);
        const std::size_t base = chunk * kQuartsPerChunk;
        ++result.chunksByBiome[biomes.name(stored[base])];
        for (std::size_t i = 0; i < kQuartsPerChunk; ++i) {
            const ResourceLocation* ours = i < low.size() ? low[i] : high[i - low.size()];
            ++result.quarts;
            if (biomes.code(ours) == stored[base + i]) {
                ++result.exact;
            } else {
                miss(chunkX, chunkZ, i, stored[base + i], *ours, "fillBiomes");
            }
        }
        for (std::int32_t layer = 0; layer < kStoredLayers; ++layer) {
            for (std::int32_t qz = 0; qz < 4; ++qz) {
                for (std::int32_t qx = 0; qx < 4; ++qx) {
                    const ResourceLocation& ours =
                        source.at(reader.interpreter, reader.settings->router,
                                  Point{.x = (chunkX * 16) + (qx * 4),
                                        .y = layer * 4,
                                        .z = (chunkZ * 16) + (qz * 4)},
                                  cache);
                    const std::size_t i = (static_cast<std::size_t>(layer) * kQuartsPerLayer) +
                                          static_cast<std::size_t>((qz * 4) + qx);
                    if (biomes.code(&ours) == stored[base + i]) {
                        ++result.exactAt;
                    } else {
                        miss(chunkX, chunkZ, i, stored[base + i], ours, "Source::at");
                    }
                }
            }
        }
    }
    return result;
}

[[nodiscard]] std::string census(const Score& score) {
    std::ostringstream out;
    for (const auto& [biome, chunks] : score.chunksByBiome) {
        out << " " << biome.substr(biome.find(':') + 1) << " " << chunks;
    }
    return out.str();
}

/// Every quart of a region exact, by both paths.
void checkExact(const std::string& name, const Score& result) {
    WARN(name << ": " << result.exact << " of " << result.quarts
              << " quarts exact through fillBiomes, " << result.exactAt
              << " through Source::at; chunks:" << census(result));
    CHECK(result.firstMiss.empty());
    CHECK(result.quarts == kQuartsPerRegion);
    CHECK(result.exact == result.quarts);
    CHECK(result.exactAt == result.quarts);
}

/// One dimension compiled through the public path, and a Reader over the
/// same pipeline.
struct Compiled {
    std::unique_ptr<CompiledDimension> dimension;
    Reader reader;

    Compiled(const stratum::freeze::Pipeline& thawed, const ResourceLocation& id, std::int64_t seed)
        : dimension(CompiledDimension::compile(thawed, id, BiomeSource::theEnd(), seed)),
          reader(thawed, id, seed) {}
};

[[nodiscard]] RegionFile golden(std::int64_t seed) {
    return RegionFile{.seed = seed,
                      .path = fixtures() / "regions" / ("seed-" + std::to_string(seed)) / "end" /
                              "r.0.0.mca",
                      .regionX = 0,
                      .regionZ = 0};
}

[[nodiscard]] RegionFile probe(const std::string& corpus, std::int64_t seed, std::int32_t regionX,
                               std::int32_t regionZ) {
    return RegionFile{.seed = seed,
                      .path =
                          fixtures() / "probes" / corpus / ("seed-" + std::to_string(seed)) /
                          ("r." + std::to_string(regionX) + "." + std::to_string(regionZ) + ".mca"),
                      .regionX = regionX,
                      .regionZ = regionZ};
}

using Also = std::function<void(const RegionFile&, const Compiled&, const Score&,
                                const std::vector<std::uint8_t>&, Biomes&)>;

/// Scores vanilla's End, compiled at each region's own seed, against every
/// region of @p files; @p also runs per region on what was scored.
void scoreEnd(const std::vector<RegionFile>& files, const Also& also = nullptr) {
    const stratum::freeze::Pipeline thawed =
        thaw(stratum::data::Pack::open(fixtures() / "worldgen"));
    const auto endId = ResourceLocation::parse("minecraft:end");
    Biomes biomes;
    for (const RegionFile& region : files) {
        CAPTURE(region.name());
        const Compiled end(thawed, endId, region.seed);
        const std::vector<std::uint8_t> stored = storedBiomes(region, biomes);
        const Score result = score(*end.dimension, end.reader, region, stored, biomes);
        checkExact(region.name(), result);
        if (also) {
            also(region, end, result, stored, biomes);
        }
    }
}

[[nodiscard]] std::int32_t centre(std::int32_t chunk) {
    return (chunk * 16) + 8;
}

/// A private copy of the pinned tree, removed on every exit.
struct TempTree {
    std::filesystem::path dir;

    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;
    TempTree(TempTree&&) = delete;
    TempTree& operator=(TempTree&&) = delete;

    explicit TempTree(const std::string& stem) : dir(stratum::test::tempPath(stem)) {
        std::filesystem::copy(fixtures() / "worldgen", dir,
                              std::filesystem::copy_options::recursive);
    }

    ~TempTree() {
        std::error_code ignored;
        std::filesystem::remove_all(dir, ignored);
    }
};

/// The probe's variants.json under @p corpus, each variant written into
/// @p tree beside the End's own settings as `minecraft:<prefix><name>` —
/// built as end-biome-router-probe.sh built its datapack: end.json, each key
/// a router entry where the router has one and a top-level field otherwise.
/// Returns the variants, for the case to check against its own list.
[[nodiscard]] nlohmann::json writeVariants(const std::filesystem::path& corpus,
                                           const TempTree& tree, const std::string& prefix) {
    REQUIRE(std::filesystem::is_regular_file(corpus / "variants.json"));
    nlohmann::json variants;
    {
        std::ifstream in(corpus / "variants.json");
        in >> variants;
    }
    nlohmann::json end;
    {
        std::ifstream in(tree.dir / "noise_settings" / "end.json");
        in >> end;
    }
    for (auto variant = variants.begin(); variant != variants.end(); ++variant) {
        nlohmann::json settings = end;
        for (auto entry = variant.value().begin(); entry != variant.value().end(); ++entry) {
            if (settings.at("noise_router").contains(entry.key())) {
                settings["noise_router"][entry.key()] = entry.value();
            } else {
                REQUIRE(settings.contains(entry.key()));
                settings[entry.key()] = entry.value();
            }
        }
        std::ofstream out(tree.dir / "noise_settings" / (prefix + variant.key() + ".json"));
        out << settings.dump();
    }
    return variants;
}

} // namespace

TEST_CASE("the End's biome source stores what the server stored, on every golden End region",
          "[conformance][biome][end]") {
    if (!havePack()) {
        SKIP("no worldgen or biome_parameters fixtures under " << STRATUM_FIXTURES_DIR
                                                               << "; run tools/fetch-vanilla");
    }
    std::vector<RegionFile> files;
    for (const std::int64_t seed :
         {std::int64_t{0}, std::int64_t{1}, std::int64_t{-1}, std::int64_t{42},
          std::int64_t{2891948927356891}, std::int64_t{-4172144997902289642},
          std::int64_t{9223372036854775807}, std::int64_t{-9223372036854775807} - 1}) {
        files.push_back(golden(seed));
    }
    requireCorpus(files, "tools/fetch-vanilla --generate-regions --accept-eula");
    // r.0.0 is blocks 0..511: every chunk is inside the ring, so this is the
    // central test alone, at six distinct worlds.
    scoreEnd(files, [](const RegionFile&, const Compiled&, const Score& result,
                       const std::vector<std::uint8_t>&, Biomes&) {
        CHECK(result.chunksByBiome.size() == 1U);
        CHECK(result.chunksByBiome.contains("minecraft:the_end"));
    });
}

TEST_CASE("the End's biome source past the ring and in the negative quadrant, at seeds 0 and 42",
          "[conformance][biome][end][islands]") {
    if (!havePack()) {
        SKIP("no worldgen or biome_parameters fixtures under " << STRATUM_FIXTURES_DIR
                                                               << "; run tools/fetch-vanilla");
    }
    const std::vector<RegionFile> files = {
        probe("end-islands", 0, -1, -1), probe("end-islands", 0, 64, 0),
        probe("end-islands", 42, -1, -1), probe("end-islands", 42, 64, 0)};
    requireCorpus(files, "tools/analysis/end-islands-probe.sh --accept-eula");
    scoreEnd(files, [](const RegionFile& region, const Compiled& end, const Score& result,
                       const std::vector<std::uint8_t>& stored, Biomes& biomes) {
        if (region.regionX != 64) {
            CHECK(result.chunksByBiome.size() == 1U);
            CHECK(result.chunksByBiome.contains("minecraft:the_end"));
            return;
        }
        // All four outer biomes, so every band of the rule is in play.
        CHECK(result.chunksByBiome.size() == 4U);
        CHECK_FALSE(result.chunksByBiome.contains("minecraft:the_end"));
        // The CONTROL: the chunk's origin, not its centre. The island field
        // is constant over each 8-block cell, and the origin's cell is the
        // other half of the chunk.
        stratum::density::Interpreter::CornerCache cache(end.reader.interpreter.cacheSize());
        std::size_t originWrong = 0;
        for (std::size_t chunk = 0; chunk < kChunksPerRegion; ++chunk) {
            const std::int32_t chunkX = region.chunkX(chunk);
            const std::int32_t chunkZ = region.chunkZ(chunk);
            const ResourceLocation& origin = end.reader.endBiome(
                chunkX, chunkZ, Point{.x = chunkX * 16, .y = 0, .z = chunkZ * 16}, false, cache);
            originWrong += biomes.code(&origin) == stored[chunk * kQuartsPerChunk] ? 0U : 1U;
        }
        WARN(region.name() << ": the chunk's origin would miss " << originWrong
                           << " of 1024 chunks");
        CHECK(originWrong > 0U);
    });
}

TEST_CASE("the End's biome source on the ring and far out, at four more End worlds",
          "[conformance][biome][end][islands]") {
    if (!havePack()) {
        SKIP("no worldgen or biome_parameters fixtures under " << STRATUM_FIXTURES_DIR
                                                               << "; run tools/fetch-vanilla");
    }
    // A far region and a ring region at each, the ring on a negative axis.
    const std::vector<RegionFile> files = {probe("end-biomes", 31337, -65, 3),
                                           probe("end-biomes", 31337, -2, 0),
                                           probe("end-biomes", 8675309, 40, -50),
                                           probe("end-biomes", 8675309, 0, -2),
                                           probe("end-biomes", 2891948927356891, -30, -45),
                                           probe("end-biomes", 2891948927356891, -2, -2),
                                           probe("end-biomes", -4172144997902289642, 70, 20),
                                           probe("end-biomes", -4172144997902289642, -2, -1)};
    requireCorpus(files, "tools/probe-worlds generate --accept-eula --shard end (its "
                         "end-biomes_s<seed> units: tools/analysis/end-islands-probe.sh "
                         "--name end-biomes)");
    std::size_t truncationWrong = 0;
    std::size_t strictWrong = 0;
    std::size_t originWrong = 0;
    std::size_t chunksAt4096 = 0;
    scoreEnd(files, [&](const RegionFile& region, const Compiled& end, const Score& result,
                        const std::vector<std::uint8_t>& stored, Biomes& biomes) {
        const bool ring = region.regionX >= -2 && region.regionX <= 0 && region.regionZ >= -2 &&
                          region.regionZ <= 0;
        if (ring) {
            CHECK(result.chunksByBiome.contains("minecraft:the_end"));
            CHECK(result.chunksByBiome.size() > 1U);
        } else {
            CHECK(result.chunksByBiome.size() == 4U);
            CHECK_FALSE(result.chunksByBiome.contains("minecraft:the_end"));
        }
        // THE CONTROLS, scored against the server's own biomes. Truncation
        // from quart to chunk puts quart columns 1..3 of a negative chunk in
        // the chunk toward zero; a strict ring calls (-64, 0) and (0, -64)
        // outer; the origin reads the other 8-block cell of the island field.
        stratum::density::Interpreter::CornerCache cache(end.reader.interpreter.cacheSize());
        for (std::size_t chunk = 0; chunk < kChunksPerRegion; ++chunk) {
            const std::int32_t chunkX = region.chunkX(chunk);
            const std::int32_t chunkZ = region.chunkZ(chunk);
            const std::size_t base = chunk * kQuartsPerChunk;
            if ((std::int64_t{chunkX} * chunkX) + (std::int64_t{chunkZ} * chunkZ) ==
                EndRule::kCentralRadiusSquared) {
                ++chunksAt4096;
            }
            const ResourceLocation& strict = end.reader.endBiome(
                chunkX, chunkZ, Point{.x = centre(chunkX), .y = 0, .z = centre(chunkZ)}, true,
                cache);
            strictWrong += biomes.code(&strict) == stored[base] ? 0U : 1U;
            const ResourceLocation& origin = end.reader.endBiome(
                chunkX, chunkZ, Point{.x = chunkX * 16, .y = 0, .z = chunkZ * 16}, false, cache);
            originWrong += biomes.code(&origin) == stored[base] ? 0U : 1U;
            for (std::int32_t qz = 0; qz < 4; ++qz) {
                for (std::int32_t qx = 0; qx < 4; ++qx) {
                    // Java's `/`, which truncates toward zero.
                    const std::int32_t truncX = ((chunkX * 4) + qx) / 4;
                    const std::int32_t truncZ = ((chunkZ * 4) + qz) / 4;
                    const ResourceLocation& truncated = end.reader.endBiome(
                        truncX, truncZ, Point{.x = centre(truncX), .y = 0, .z = centre(truncZ)},
                        false, cache);
                    truncationWrong +=
                        biomes.code(&truncated) ==
                                stored[base + static_cast<std::size_t>((qz * 4) + qx)]
                            ? 0U
                            : 1U;
                }
            }
        }
    });
    WARN("controls over the eight regions: truncation misses "
         << truncationWrong << " quart columns, a strict ring " << strictWrong << " chunks (of the "
         << chunksAt4096 << " at exactly 4096), the chunk's origin " << originWrong << " chunks");
    CHECK(chunksAt4096 == 2U);
    CHECK(strictWrong == chunksAt4096);
    CHECK(truncationWrong > 0U);
    CHECK(originWrong > 0U);
}

TEST_CASE("the End's biome source reads the router's erosion, at y = 4 * quartY, and nothing "
          "else of it",
          "[conformance][biome][end][router]") {
    if (!havePack()) {
        SKIP("no worldgen or biome_parameters fixtures under " << STRATUM_FIXTURES_DIR
                                                               << "; run tools/fetch-vanilla");
    }
    const std::filesystem::path corpus = fixtures() / "probes" / "end-biome-router" / "seed-42";
    // end-biome-router-probe.sh's dimensions, in its order, then the world's
    // own End.
    const std::vector<std::string> names = {"ctl",
                                            "erosion_m0_5",
                                            "erosion_m0_21876",
                                            "erosion_m0_2187500001",
                                            "erosion_m0_21875",
                                            "erosion_m0_1",
                                            "erosion_m0_06251",
                                            "erosion_m0_0625000001",
                                            "erosion_m0_0625",
                                            "erosion_0",
                                            "erosion_p0_1",
                                            "erosion_p0_25",
                                            "erosion_p0_2500000001",
                                            "erosion_p0_25001",
                                            "erosion_p0_5",
                                            "erosion_ygrad",
                                            "erosion_ystep_m1",
                                            "erosion_ystep_0",
                                            "erosion_ystep_3",
                                            "erosion_ystep_4",
                                            "climate",
                                            "terrain",
                                            "vanilla"};
    std::vector<RegionFile> files;
    files.reserve(names.size());
    for (const std::string& name : names) {
        files.push_back(RegionFile{
            .seed = 42, .path = corpus / name / "r.-2.-2.mca", .regionX = -2, .regionZ = -2});
    }
    requireCorpus(files, "tools/analysis/end-biome-router-probe.sh --accept-eula");
    const TempTree tree("stratum-end-router");
    const nlohmann::json variants = writeVariants(corpus, tree, "end_router_");
    REQUIRE(variants.size() == names.size() - 1U);
    const stratum::freeze::Pipeline thawed = thaw(stratum::data::Pack::open(tree.dir));

    Biomes biomes;
    std::map<std::string, std::vector<std::uint8_t>> stored;
    std::map<std::string, Score> scores;
    std::map<std::int32_t, std::size_t> wrongY;
    for (std::size_t i = 0; i < names.size(); ++i) {
        const std::string& name = names[i];
        CAPTURE(name);
        const ResourceLocation id = name == "vanilla"
                                        ? ResourceLocation::parse("minecraft:end")
                                        : ResourceLocation("minecraft", "end_router_" + name);
        const Compiled dimension(thawed, id, 42);
        stored[name] = storedBiomes(files[i], biomes);
        const Score result =
            score(*dimension.dimension, dimension.reader, files[i], stored[name], biomes);
        checkExact(name, result);
        scores.emplace(name, result);

        // WRONG READINGS of y, on the variants whose erosion varies with it:
        // 4qy + 1, + 2, + 3, and the bare quart index (-1 below). Each step
        // separates some of them and not others — the step at 3 puts y 1..3
        // with y 0, the one at 4 puts them with y 4 — so what must hold is
        // that each loses on one variant or another.
        if (name.starts_with("erosion_y")) {
            stratum::density::Interpreter::CornerCache cache(
                dimension.reader.interpreter.cacheSize());
            for (const std::int32_t offset : {1, 2, 3, -1}) {
                std::size_t& wrong = wrongY[offset];
                for (std::size_t chunk = 0; chunk < kChunksPerRegion; ++chunk) {
                    const std::int32_t chunkX = files[i].chunkX(chunk);
                    const std::int32_t chunkZ = files[i].chunkZ(chunk);
                    for (std::int32_t layer = 0; layer < kStoredLayers; ++layer) {
                        const std::int32_t y = offset < 0 ? layer : (layer * 4) + offset;
                        const ResourceLocation& elsewhere = dimension.reader.endBiome(
                            chunkX, chunkZ, Point{.x = centre(chunkX), .y = y, .z = centre(chunkZ)},
                            false, cache);
                        const std::uint8_t code = biomes.code(&elsewhere);
                        // Counted per QUART, all sixteen of the layer: one
                        // sample serves them, so a wrong one is wrong sixteen
                        // times over.
                        const std::size_t first =
                            (chunk * kQuartsPerChunk) +
                            (static_cast<std::size_t>(layer) * kQuartsPerLayer);
                        for (std::size_t column = 0; column < kQuartsPerLayer; ++column) {
                            wrong += code == stored[name][first + column] ? 0U : 1U;
                        }
                    }
                }
            }
        }
    }
    for (const auto& [offset, wrong] : wrongY) {
        WARN("reading y at " << (offset < 0 ? std::string("the quart index")
                                            : "4 * quartY + " + std::to_string(offset))
                             << " would miss " << wrong << " quarts over the y-varying variants");
        CAPTURE(offset);
        CHECK(wrong > 0U);
    }
    CHECK(wrongY.size() == 4U);

    // The source reads erosion AND NOTHING ELSE of the router: the server's
    // own biomes do not move when the other climate entries or final_density
    // change. And it reads THE ROUTER, not an island field of its own: every
    // erosion constant makes the outer chunks one biome, which the world's
    // own island field — what a hard-wired source would read — does not.
    for (const char* unchanged : {"ctl", "climate", "terrain"}) {
        CAPTURE(unchanged);
        CHECK(stored[unchanged] == stored["vanilla"]);
    }
    for (const auto& [name, result] : scores) {
        if (!name.starts_with("erosion_") || name.starts_with("erosion_y")) {
            continue;
        }
        CAPTURE(name);
        // 299 chunks of r.-2.-2 inside the ring, 725 outside.
        CHECK(result.chunksByBiome.at("minecraft:the_end") == 299U);
        CHECK(result.chunksByBiome.size() == 2U);
        CHECK(stored[name] != stored["vanilla"]);
    }
}

TEST_CASE("the End's biome source reads erosion at the chunk's centre, block 16c + 8",
          "[conformance][biome][end][sample]") {
    // The island field is constant over 8-block cells, so the End itself
    // pins the sample only to blocks 16c + 8 .. 16c + 15 (the controls above
    // refute the origin's cell). end-biome-router-probe.sh --set sample's
    // `xz` routes a noise there that differs from block to block —
    // noise(minecraft:ridge, xz_scale 64, y_scale 0), under the Xoroshiro
    // source, no terrain — so each of a chunk's 256 columns is a reading the
    // server's biomes can refute.
    if (!havePack()) {
        SKIP("no worldgen or biome_parameters fixtures under " << STRATUM_FIXTURES_DIR
                                                               << "; run tools/fetch-vanilla");
    }
    const std::filesystem::path corpus = fixtures() / "probes" / "end-biome-sample" / "seed-42";
    const std::vector<std::string> names = {"xz", "vanilla"};
    std::vector<RegionFile> files;
    files.reserve(names.size());
    for (const std::string& name : names) {
        files.push_back(RegionFile{
            .seed = 42, .path = corpus / name / "r.-2.-2.mca", .regionX = -2, .regionZ = -2});
    }
    requireCorpus(files, "tools/analysis/end-biome-router-probe.sh --accept-eula --set sample");
    const TempTree tree("stratum-end-sample");
    const nlohmann::json variants = writeVariants(corpus, tree, "end_sample_");
    REQUIRE(variants.size() == 1U);
    REQUIRE(variants.contains("xz"));
    const stratum::freeze::Pipeline thawed = thaw(stratum::data::Pack::open(tree.dir));

    Biomes biomes;
    for (std::size_t i = 0; i < names.size(); ++i) {
        const std::string& name = names[i];
        CAPTURE(name);
        const ResourceLocation id = name == "vanilla"
                                        ? ResourceLocation::parse("minecraft:end")
                                        : ResourceLocation("minecraft", "end_sample_" + name);
        const Compiled dimension(thawed, id, 42);
        const std::vector<std::uint8_t> stored = storedBiomes(files[i], biomes);
        const Score result =
            score(*dimension.dimension, dimension.reader, files[i], stored, biomes);
        checkExact(name, result);
        if (name != "xz") {
            continue;
        }

        // Every column of every outer chunk as the sample, scored against
        // the server's biome for that chunk. Only one may read them all.
        // The variant's top-level change took: its noise is Xoroshiro's.
        REQUIRE_FALSE(dimension.reader.settings->legacyRandomSource);
        stratum::density::Interpreter::CornerCache cache(dimension.reader.interpreter.cacheSize());
        std::array<std::size_t, 256> agree{};
        std::size_t outer = 0;
        for (std::size_t chunk = 0; chunk < kChunksPerRegion; ++chunk) {
            const std::int32_t chunkX = files[i].chunkX(chunk);
            const std::int32_t chunkZ = files[i].chunkZ(chunk);
            if (EndRule::isCentral(chunkX, chunkZ)) {
                continue;
            }
            ++outer;
            const std::uint8_t server = stored[chunk * kQuartsPerChunk];
            for (std::int32_t dz = 0; dz < 16; ++dz) {
                for (std::int32_t dx = 0; dx < 16; ++dx) {
                    const ResourceLocation& candidate = dimension.reader.endBiome(
                        chunkX, chunkZ,
                        Point{.x = (chunkX * 16) + dx, .y = 0, .z = (chunkZ * 16) + dz}, false,
                        cache);
                    agree.at((static_cast<std::size_t>(dz) * 16U) + static_cast<std::size_t>(dx)) +=
                        biomes.code(&candidate) == server ? 1U : 0U;
                }
            }
        }
        std::size_t bestOther = 0;
        std::size_t bestOtherAt = 0;
        for (std::size_t at = 0; at < agree.size(); ++at) {
            if (at != (8U * 16U) + 8U && agree.at(at) > bestOther) {
                bestOther = agree.at(at);
                bestOtherAt = at;
            }
        }
        WARN("xz: the centre (8, 8) reads " << agree.at((8U * 16U) + 8U) << " of " << outer
                                            << " outer chunks; the best other column, ("
                                            << bestOtherAt % 16U << ", " << bestOtherAt / 16U
                                            << "), " << bestOther);
        CHECK(outer == 725U);
        CHECK(agree.at((8U * 16U) + 8U) == outer);
        CHECK(bestOther < outer);
    }
}
