// Stratum — what one PocketMine-MP generation task costs per chunk: the fill,
// the biomes and the chunkutils2 encode, single-threaded, stage by stage and
// end to end.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
//   stratum_chunk_cost_bench <fixtures-version-dir> [options]
//   stratum_chunk_cost_bench --compare <dump-dir-a> <dump-dir-b>
//
//     --dimensions a,b,...   overworld, amplified, large_biomes, end (default all four)
//     --seeds a,b,...        world seeds (default 0,42,-4172144997902289642)
//     --spiral cx,cz,r       every chunk within r of (cx, cz), ring by ring from the
//                            centre (default -64,64,5: 121 chunks, the square
//                            tools/analysis/vanilla-chunk-cost.py forceloads)
//     --scattered on|off     the fixed scattered set below, negative coordinates and
//                            far chunks included (default on, 16 chunks)
//     --warmup n             untimed passes over the set first (default 1). The
//                            first pass, timed or not, also checks every chunk's
//                            staged encode against encodeChunk and writes the dump
//     --repeat n             timed passes (default 3); a chunk's time for a stage is
//                            its median over these
//     --json <file>          the summary as JSON (what tools/perf-baseline reads)
//     --dump <dir>           every chunk's output, one file per dimension and seed
//
// WHAT IT TIMES is exactly what the zend module runs for one
// `Stratum\Dimension::encodeChunk` (ext/zend/php_stratum.cpp), on one thread,
// from a pipeline frozen and thawed the way `Stratum\freezePipeline` and
// `Dimension::open` do it:
//
//   fill     CompiledDimension::fillBlocks into a fresh ChunkBuffer — the noise
//            fill, aquifers, ore veins and the surface rules
//   biomes   CompiledDimension::fillBiomes over PocketMine-MP's 96 quart layers
//            (y -64..319), the range form encodeChunk calls
//   encode   everything else encodeChunk does: Java state ids per palette entry,
//            Bedrock biome ids, and the 24 sub-chunks packed into
//            PalettedBlockArray::fromData's arguments
//   e2e      stratum::pmmp::encodeChunk itself, on its own sweep of each timed
//            pass
//
// encodeChunk is one call, so the three stages run here as a copy of its
// steps (encodeStages below) and e2e runs the real thing. The copy is held to
// the real thing: on the first pass every chunk's staged result is compared
// with encodeChunk's, sub-chunk for sub-chunk, and any difference stops the
// run naming the chunk. "sum" is the three stages per chunk; set against e2e
// it shows what timing them separately costs or saves, which should be noise.
//
// THE CLOCKS. Wall time (steady_clock) and this thread's CPU time
// (CLOCK_THREAD_CPUTIME_ID; "-" where the platform has none). On a shared
// machine wall time includes being descheduled and CPU time does not; on a
// quiet one they agree, and the quiet one is where a number of record comes
// from. Each chunk's time for a stage is its median over the timed passes, so
// one pass a load spike hit costs that pass, not the chunk; the figures below
// are then taken over chunks: median, p10, p90 and mean ms/chunk, and
// chunks/s as 1000 / mean — a rate is what the mean, not the median, decides.
//
// ONLY AN OPTIMISED BUILD MEASURES ANYTHING. The release preset
// (RelWithDebInfo) is the one tools/perf-baseline builds; a Debug build runs
// this — the conformance test does, to keep the tool itself honest — but its
// header and JSON say "unoptimised", and its numbers are not a measurement.
//
// THE DUMP is how a performance change is shown to move no output, the same
// idea as aquifer-cost-bench's. One file per dimension and seed, every chunk
// in set order: its blocks as Java state ids, its biomes over PocketMine-MP's
// column, the 24 encoded sub-chunks exactly as encodeChunk returned them, and
// its fluid updates. Two builds whose dumps `--compare` calls identical
// produced the same chunks; the compare names the first chunk that differs.
// Dumps are host-endian: compare dumps made on one machine.
//
// DUMPS ARE VANILLA-DERIVED (SPEC §12) and must never be committed: the
// blocks and biomes are vanilla's worldgen executed. Write them outside the
// repository (on the development box, under $HOME/stratum-work/scratch); each
// dump directory also gets a `.gitignore` of `*`, so a stray `git add` passes
// it over. Nothing this reads is committed either: the worldgen tree and the
// biome parameter lists are tools/fetch-vanilla's.
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/freeze/pipeline.hpp>
#include <stratum/javamath.hpp>
#include <stratum/mapping/biome.hpp>
#include <stratum/mapping/block_state.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/terrain/filler.hpp>
#include <stratum/version.hpp>
#include <stratum/world/dimension.hpp>

#include <nlohmann/json.hpp>
#include <stratum_pmmp/chunk_encoder.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace stratum;

namespace {

constexpr int kChunkWidth = 16;
constexpr std::int32_t kSubChunkEdge = 16;
constexpr std::int32_t kQuartWidth = 4;
constexpr std::size_t kQuartsPerLayer = 16;
/// PocketMine-MP's column in quart layers, from y -64: what encodeChunk asks
/// fillBiomes for.
constexpr std::int32_t kMinQuartY = pmmp::kMinSubChunk * kSubChunkEdge / kQuartWidth;
constexpr auto kQuartLayers = static_cast<std::size_t>(
    (pmmp::kMaxSubChunk + 1 - pmmp::kMinSubChunk) * kSubChunkEdge / kQuartWidth);

constexpr std::uint64_t kFnvOffset = 0xcbf29ce484222325ULL;
constexpr std::uint64_t kFnvPrime = 0x100000001b3ULL;
constexpr std::string_view kDumpMagic = "STRATUMCHUNKDUMP";
constexpr std::uint32_t kDumpFormat = 1;
constexpr std::string_view kDumpSuffix = ".dump";

constexpr double kMillis = 1000.0;

using ChunkPos = std::pair<std::int32_t, std::int32_t>;

/// The scattered set: near and far, every quadrant, out to 100 000 chunks
/// (1.6 million blocks, inside vanilla's world border). Fixed, so two runs and
/// two builds see the same chunks.
constexpr std::array<ChunkPos, 16> kScattered = {{{-1, -1},
                                                  {3, -17},
                                                  {-25, 9},
                                                  {40, 40},
                                                  {-64, -64},
                                                  {117, -230},
                                                  {-301, 77},
                                                  {512, -512},
                                                  {-1000, 1000},
                                                  {1874, -2229},
                                                  {-3125, -4096},
                                                  {4999, 6001},
                                                  {-10000, -10000},
                                                  {12345, -6789},
                                                  {-31250, 31250},
                                                  {100000, -100000}}};

/// One of the dimensions this bench knows, by the name its options use.
struct DimensionSpec {
    std::string name;
    data::ResourceLocation noiseSettings;
    world::BiomeSource biomeSource;
};

[[nodiscard]] DimensionSpec dimensionNamed(const std::string_view name) {
    const auto overworld = data::ResourceLocation::parse("minecraft:overworld");
    if (name == "overworld") {
        return {std::string(name), overworld, world::BiomeSource::multiNoise(overworld)};
    }
    // vanilla's two presets search the overworld's own biome list
    if (name == "amplified" || name == "large_biomes") {
        return {std::string(name), data::ResourceLocation::parse("minecraft:" + std::string(name)),
                world::BiomeSource::multiNoise(overworld)};
    }
    if (name == "end") {
        return {std::string(name), data::ResourceLocation::parse("minecraft:end"),
                world::BiomeSource::theEnd()};
    }
    throw std::invalid_argument("unknown dimension '" + std::string(name) +
                                "' (overworld, amplified, large_biomes or end)");
}

struct Options {
    std::filesystem::path versionDir;
    std::vector<std::string> dimensions{"overworld", "amplified", "large_biomes", "end"};
    std::vector<std::int64_t> seeds{0, 42, -4172144997902289642LL};
    std::int32_t spiralX = -64;
    std::int32_t spiralZ = 64;
    std::int32_t spiralRadius = 5;
    bool scattered = true;
    int warmup = 1;
    int repeat = 3;
    std::filesystem::path json;
    std::filesystem::path dump;
};

constexpr std::string_view kUsage =
    "usage: stratum_chunk_cost_bench <fixtures-version-dir> [--dimensions a,b] [--seeds a,b]\n"
    "           [--spiral cx,cz,r] [--scattered on|off] [--warmup n] [--repeat n]\n"
    "           [--json file] [--dump dir]\n"
    "       stratum_chunk_cost_bench --compare <dump-dir-a> <dump-dir-b>\n"
    "Times one PocketMine-MP generation task per chunk (fill, biomes, encode, and\n"
    "stratum::pmmp::encodeChunk end to end) on one thread. Build it optimised (the\n"
    "release preset) for numbers. Dumps are vanilla-derived (SPEC 12): NEVER commit\n"
    "them; write them outside the repository, e.g. under $HOME/stratum-work/scratch.";

[[nodiscard]] std::vector<std::string> splitCommas(const std::string_view text) {
    std::vector<std::string> out;
    std::string current;
    for (const char c : text) {
        if (c == ',') {
            out.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    out.push_back(current);
    return out;
}

[[nodiscard]] bool onOff(const std::string_view flag, const std::string_view value) {
    if (value == "on") {
        return true;
    }
    if (value == "off") {
        return false;
    }
    throw std::invalid_argument(std::string(flag) + " takes on or off, not '" + std::string(value) +
                                "'");
}

[[nodiscard]] int count(const std::string_view flag, const std::string_view value,
                        const int minimum) {
    const int parsed = std::stoi(std::string(value));
    if (parsed < minimum) {
        throw std::invalid_argument(std::string(flag) + " needs at least " +
                                    std::to_string(minimum));
    }
    return parsed;
}

[[nodiscard]] Options parse(const std::vector<std::string_view>& args) {
    if (args.size() < 2U) {
        throw std::invalid_argument(std::string(kUsage));
    }
    Options options;
    options.versionDir = std::filesystem::path(std::string(args[1]));
    for (std::size_t i = 2; i < args.size(); ++i) {
        const std::string_view flag = args[i];
        if (i + 1 >= args.size()) {
            throw std::invalid_argument(std::string(flag) + " needs a value");
        }
        const std::string_view value = args[++i];
        if (flag == "--dimensions") {
            options.dimensions = splitCommas(value);
            for (const std::string& name : options.dimensions) {
                (void)dimensionNamed(name);
            }
        } else if (flag == "--seeds") {
            options.seeds.clear();
            for (const std::string& seed : splitCommas(value)) {
                options.seeds.push_back(std::stoll(seed));
            }
        } else if (flag == "--spiral") {
            const std::vector<std::string> parts = splitCommas(value);
            if (parts.size() != 3U) {
                throw std::invalid_argument("--spiral takes cx,cz,r");
            }
            options.spiralX = std::stoi(parts[0]);
            options.spiralZ = std::stoi(parts[1]);
            options.spiralRadius = count("--spiral's radius", parts[2], -1);
        } else if (flag == "--scattered") {
            options.scattered = onOff(flag, value);
        } else if (flag == "--warmup") {
            options.warmup = count(flag, value, 0);
        } else if (flag == "--repeat") {
            options.repeat = count(flag, value, 1);
        } else if (flag == "--json") {
            options.json = std::filesystem::path(std::string(value));
        } else if (flag == "--dump") {
            options.dump = std::filesystem::path(std::string(value));
        } else {
            throw std::invalid_argument("unknown option '" + std::string(flag) + "'\n" +
                                        std::string(kUsage));
        }
    }
    return options;
}

/// A chunk and the set it came from.
struct BenchChunk {
    ChunkPos pos;
    bool spiral = false;
};

/// The spiral ring by ring from its centre — ring r is the square of chunks r
/// away, walked from its lowest corner along +x, +z, -x, -z — then the
/// scattered set. A radius of -1 is no spiral at all.
[[nodiscard]] std::vector<BenchChunk> chunkSet(const Options& options) {
    std::vector<BenchChunk> out;
    std::set<ChunkPos> seen;
    const auto add = [&](const ChunkPos pos, const bool spiral) {
        if (seen.insert(pos).second) {
            out.push_back({pos, spiral});
        }
    };
    for (std::int32_t ring = 0; ring <= options.spiralRadius; ++ring) {
        const std::int32_t x0 = options.spiralX - ring;
        const std::int32_t z0 = options.spiralZ - ring;
        const std::int32_t side = (2 * ring) + 1;
        if (ring == 0) {
            add({x0, z0}, true);
            continue;
        }
        for (std::int32_t i = 0; i < side - 1; ++i) {
            add({x0 + i, z0}, true);
        }
        for (std::int32_t i = 0; i < side - 1; ++i) {
            add({x0 + side - 1, z0 + i}, true);
        }
        for (std::int32_t i = 0; i < side - 1; ++i) {
            add({x0 + side - 1 - i, z0 + side - 1}, true);
        }
        for (std::int32_t i = 0; i < side - 1; ++i) {
            add({x0, z0 + side - 1 - i}, true);
        }
    }
    if (options.scattered) {
        for (const ChunkPos& pos : kScattered) {
            add(pos, false);
        }
    }
    if (out.empty()) {
        throw std::invalid_argument("no chunks: --spiral's radius is -1 and --scattered is off");
    }
    return out;
}

// --- clocks -------------------------------------------------------------------

[[nodiscard]] double wallSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// This thread's CPU time, or NaN where the platform has no per-thread clock.
[[nodiscard]] double threadCpuSeconds() {
#if defined(CLOCK_THREAD_CPUTIME_ID)
    constexpr double kNanos = 1e-9;
    timespec now{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) != 0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return static_cast<double>(now.tv_sec) + (static_cast<double>(now.tv_nsec) * kNanos);
#else
    return std::numeric_limits<double>::quiet_NaN();
#endif
}

struct Stopwatch {
    double wall = wallSeconds();
    double cpu = threadCpuSeconds();

    /// Seconds since construction or the last lap, and restarts.
    [[nodiscard]] std::pair<double, double> lap() {
        const double nowWall = wallSeconds();
        const double nowCpu = threadCpuSeconds();
        const std::pair<double, double> elapsed{nowWall - wall, nowCpu - cpu};
        wall = nowWall;
        cpu = nowCpu;
        return elapsed;
    }
};

// --- the machine --------------------------------------------------------------

[[nodiscard]] std::string firstLine(const std::filesystem::path& path) {
    std::ifstream file(path);
    std::string line;
    if (!file || !std::getline(file, line)) {
        return "unknown";
    }
    return line;
}

[[nodiscard]] std::string cpuModel() {
    std::ifstream file("/proc/cpuinfo");
    std::string line;
    while (file && std::getline(file, line)) {
        const std::size_t colon = line.find(':');
        if (line.starts_with("model name") && colon != std::string::npos) {
            const std::size_t text = line.find_first_not_of(' ', colon + 1);
            return text == std::string::npos ? "unknown" : line.substr(text);
        }
    }
    return "unknown";
}

[[nodiscard]] std::string loadAverage() {
    std::string line = firstLine("/proc/loadavg");
    if (line == "unknown") {
        return line;
    }
    std::istringstream fields(line);
    std::string one;
    std::string five;
    std::string fifteen;
    fields >> one >> five >> fifteen;
    return one + " " + five + " " + fifteen;
}

[[nodiscard]] bool optimisedBuild() {
#if defined(__OPTIMIZE__) || (defined(_MSC_VER) && defined(NDEBUG))
    return true;
#else
    return false;
#endif
}

[[nodiscard]] std::string compilerName() {
#if defined(__clang__)
    return std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
    return std::string("gcc ") + __VERSION__;
#elif defined(_MSC_VER)
    return "msvc " + std::to_string(_MSC_FULL_VER);
#else
    return "unknown";
#endif
}

[[nodiscard]] nlohmann::json machine() {
    nlohmann::json out;
    out["cpu"] = cpuModel();
    out["hardware_threads"] = std::thread::hardware_concurrency();
    out["governor"] = firstLine("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
    out["boost"] = firstLine("/sys/devices/system/cpu/cpufreq/boost");
    out["build_config"] = STRATUM_BENCH_CONFIG;
    out["optimised"] = optimisedBuild();
    out["compiler"] = compilerName();
    out["engine_version"] = kPipelineEngineVersion;
    out["minecraft_version"] = std::string(kMinecraftVersion);
    return out;
}

// --- the stages -----------------------------------------------------------------

/// encodeChunk's steps after its two fills, on their output: the encode stage.
/// A copy of ext/src/chunk_encoder.cpp's, held to it by verifyAgainst() on
/// every chunk of the first pass.
[[nodiscard]] pmmp::EncodedChunk
encodeStages(const settings::NoiseGeometry& geometry, const terrain::ChunkBuffer& buffer,
             const std::vector<const data::ResourceLocation*>& javaBiomes) {
    std::vector<std::uint32_t> javaIds;
    javaIds.reserve(buffer.palette().size());
    for (const settings::BlockState& state : buffer.palette()) {
        javaIds.push_back(mapping::javaBlockStateId(state));
    }
    std::vector<std::uint32_t> bedrockBiomes;
    bedrockBiomes.reserve(javaBiomes.size());
    std::unordered_map<const data::ResourceLocation*, std::uint32_t> biomeIdOf;
    for (const data::ResourceLocation* biome : javaBiomes) {
        auto found = biomeIdOf.find(biome);
        if (found == biomeIdOf.end()) {
            const std::optional<std::int32_t> id = mapping::bedrockBiomeId(*biome);
            if (!id.has_value()) {
                throw std::runtime_error("biome '" + biome->toString() + "' has no Bedrock id");
            }
            found = biomeIdOf.emplace(biome, static_cast<std::uint32_t>(*id)).first;
        }
        bedrockBiomes.push_back(found->second);
    }

    pmmp::EncodedChunk encoded;
    encoded.subChunks.reserve(
        static_cast<std::size_t>(pmmp::kMaxSubChunk + 1 - pmmp::kMinSubChunk));
    std::array<std::uint32_t, pmmp::kSubChunkVolume> blocks{};
    std::array<std::uint32_t, pmmp::kSubChunkVolume> biomes{};
    for (std::int32_t index = pmmp::kMinSubChunk; index <= pmmp::kMaxSubChunk; ++index) {
        const std::int32_t sectionMinY = index * kSubChunkEdge;
        const bool generated =
            sectionMinY >= geometry.minY && sectionMinY + kSubChunkEdge <= geometry.maxY();
        bool allAir = true;
        for (int x = 0; x < kChunkWidth; ++x) {
            for (int z = 0; z < kChunkWidth; ++z) {
                for (int y = 0; y < kSubChunkEdge; ++y) {
                    const std::size_t at = pmmp::subChunkIndex(x, y, z);
                    if (generated) {
                        const std::uint16_t paletteIndex =
                            buffer.paletteIndexAt(x, sectionMinY + y, z);
                        allAir = allAir && paletteIndex == 0;
                        blocks[at] = javaIds[paletteIndex];
                    }
                    const auto quartY = static_cast<std::size_t>(
                        javamath::floorDiv(sectionMinY + y, kQuartWidth) - kMinQuartY);
                    const std::size_t quart =
                        ((quartY * 4U) + static_cast<std::size_t>(z / kQuartWidth)) * 4U +
                        static_cast<std::size_t>(x / kQuartWidth);
                    biomes[at] = bedrockBiomes[quart];
                }
            }
        }
        pmmp::EncodedSubChunk sub;
        sub.index = index;
        if (!allAir) {
            sub.blocks = pmmp::encodeLayer(blocks);
        }
        sub.biomes = pmmp::encodeLayer(biomes);
        encoded.subChunks.push_back(std::move(sub));
    }
    encoded.fluidUpdates = buffer.fluidUpdates();
    return encoded;
}

void verifyAgainst(const pmmp::EncodedChunk& staged, const pmmp::EncodedChunk& real,
                   const std::string& where) {
    const auto fail = [&](const std::string& what) {
        throw std::runtime_error(
            "the encode stage no longer reproduces stratum::pmmp::encodeChunk at " + where + ": " +
            what +
            ". ext/src/chunk_encoder.cpp changed; bring encodeStages in this file back "
            "in step with it before trusting its stage figures");
    };
    if (staged.subChunks.size() != real.subChunks.size()) {
        fail("sub-chunk counts differ");
    }
    for (std::size_t i = 0; i < real.subChunks.size(); ++i) {
        const pmmp::EncodedSubChunk& a = staged.subChunks[i];
        const pmmp::EncodedSubChunk& b = real.subChunks[i];
        if (a.index != b.index || a.blocks != b.blocks || !(a.biomes == b.biomes)) {
            fail("sub-chunk " + std::to_string(b.index) + " differs");
        }
    }
    if (staged.fluidUpdates != real.fluidUpdates) {
        fail("fluid updates differ");
    }
}

// --- the dump -------------------------------------------------------------------

/// One dump file: a header, a length-prefixed record per chunk, and the biome
/// names the records index, at the end.
class DumpFile {
public:
    DumpFile(const std::filesystem::path& path, const std::string& dimension,
             const std::int64_t seed, const std::uint32_t chunks)
        : path_(path), out_(path, std::ios::binary | std::ios::trunc) {
        if (!out_) {
            throw std::runtime_error("cannot write " + path.string());
        }
        std::vector<unsigned char> header;
        append(header, kDumpMagic.data(), kDumpMagic.size());
        put(header, kDumpFormat);
        put(header, kPipelineEngineVersion);
        put(header, static_cast<std::uint32_t>(dimension.size()));
        append(header, dimension.data(), dimension.size());
        put(header, seed);
        put(header, chunks);
        write(header);
    }

    void chunk(const ChunkPos pos, const terrain::ChunkBuffer& buffer,
               const std::vector<const data::ResourceLocation*>& javaBiomes,
               const pmmp::EncodedChunk& encoded) {
        std::vector<unsigned char> record;
        put(record, pos.first);
        put(record, pos.second);

        std::vector<std::uint32_t> javaIds;
        javaIds.reserve(buffer.palette().size());
        for (const settings::BlockState& state : buffer.palette()) {
            javaIds.push_back(mapping::javaBlockStateId(state));
        }
        put(record, static_cast<std::uint32_t>(buffer.height()) *
                        static_cast<std::uint32_t>(kChunkWidth * kChunkWidth));
        for (std::int32_t y = buffer.minY(); y < buffer.minY() + buffer.height(); ++y) {
            for (int z = 0; z < kChunkWidth; ++z) {
                for (int x = 0; x < kChunkWidth; ++x) {
                    put(record, javaIds.at(buffer.paletteIndexAt(x, y, z)));
                }
            }
        }

        put(record, static_cast<std::uint32_t>(javaBiomes.size()));
        for (const data::ResourceLocation* biome : javaBiomes) {
            put(record, biomeIndex(*biome));
        }

        put(record, static_cast<std::uint32_t>(encoded.subChunks.size()));
        for (const pmmp::EncodedSubChunk& sub : encoded.subChunks) {
            put(record, sub.index);
            put(record, static_cast<std::uint8_t>(sub.blocks.has_value() ? 1U : 0U));
            if (sub.blocks.has_value()) {
                layer(record, *sub.blocks);
            }
            layer(record, sub.biomes);
        }

        put(record, static_cast<std::uint32_t>(encoded.fluidUpdates.size()));
        for (const terrain::ChunkBuffer::FluidUpdate& update : encoded.fluidUpdates) {
            put(record, update.localX);
            put(record, update.y);
            put(record, update.localZ);
        }

        std::vector<unsigned char> length;
        put(length, static_cast<std::uint64_t>(record.size()));
        write(length);
        write(record);
    }

    /// Writes the biome names and returns the file's FNV-1a.
    [[nodiscard]] std::uint64_t finish() {
        std::vector<unsigned char> trailer;
        put(trailer, static_cast<std::uint32_t>(biomeNames_.size()));
        for (const std::string& name : biomeNames_) {
            put(trailer, static_cast<std::uint32_t>(name.size()));
            append(trailer, name.data(), name.size());
        }
        write(trailer);
        out_.flush();
        if (!out_) {
            throw std::runtime_error("writing " + path_.string() + " failed");
        }
        return hash_;
    }

private:
    static void append(std::vector<unsigned char>& into, const void* data, const std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        into.insert(into.end(), bytes, std::next(bytes, static_cast<std::ptrdiff_t>(size)));
    }

    template<typename T>
    static void put(std::vector<unsigned char>& into, const T value) {
        std::array<unsigned char, sizeof(T)> bytes{};
        std::memcpy(bytes.data(), &value, sizeof(T));
        into.insert(into.end(), bytes.begin(), bytes.end());
    }

    static void layer(std::vector<unsigned char>& into, const pmmp::PalettedLayer& paletted) {
        put(into, paletted.bitsPerBlock);
        put(into, static_cast<std::uint32_t>(paletted.words.size()));
        for (const std::uint32_t word : paletted.words) {
            put(into, word);
        }
        put(into, static_cast<std::uint32_t>(paletted.palette.size()));
        for (const std::uint32_t entry : paletted.palette) {
            put(into, entry);
        }
    }

    [[nodiscard]] std::uint16_t biomeIndex(const data::ResourceLocation& biome) {
        const std::string name = biome.toString();
        const auto found = biomeIndexOf_.find(name);
        if (found != biomeIndexOf_.end()) {
            return found->second;
        }
        const auto index = static_cast<std::uint16_t>(biomeNames_.size());
        biomeIndexOf_.emplace(name, index);
        biomeNames_.push_back(name);
        return index;
    }

    void write(const std::vector<unsigned char>& bytes) {
        for (const unsigned char byte : bytes) {
            hash_ = (hash_ ^ byte) * kFnvPrime;
        }
        out_.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }

    std::filesystem::path path_;
    std::ofstream out_;
    std::uint64_t hash_ = kFnvOffset;
    std::map<std::string, std::uint16_t> biomeIndexOf_;
    std::vector<std::string> biomeNames_;
};

/// Makes @p dir a dump directory: created, and holding a `.gitignore` of `*`
/// so that nothing in it is ever staged by accident (SPEC §12).
void prepareDumpDir(const std::filesystem::path& dir) {
    std::filesystem::create_directories(dir);
    std::ofstream ignore(dir / ".gitignore", std::ios::trunc);
    ignore << "# stratum_chunk_cost_bench dumps are vanilla-derived (SPEC 12): never commit them.\n"
              "*\n";
    if (!ignore) {
        throw std::runtime_error("cannot write " + (dir / ".gitignore").string());
    }
}

// --- --compare ------------------------------------------------------------------

[[nodiscard]] std::vector<std::string> dumpFiles(const std::filesystem::path& dir) {
    if (!std::filesystem::is_directory(dir)) {
        throw std::runtime_error(dir.string() + " is not a directory");
    }
    std::vector<std::string> names;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(dir)) {
        const std::string name = entry.path().filename().string();
        if (entry.is_regular_file() && name.size() > kDumpSuffix.size() &&
            name.compare(name.size() - kDumpSuffix.size(), kDumpSuffix.size(), kDumpSuffix) == 0) {
            names.push_back(name);
        }
    }
    std::ranges::sort(names);
    return names;
}

template<typename T>
[[nodiscard]] bool readValue(std::istream& in, T& value) {
    std::array<char, sizeof(T)> bytes{};
    if (!in.read(bytes.data(), static_cast<std::streamsize>(sizeof(T)))) {
        return false;
    }
    std::memcpy(&value, bytes.data(), sizeof(T));
    return true;
}

/// Which chunk record of dump @p path holds byte @p offset, read off the
/// header and the records' length prefixes.
[[nodiscard]] std::string placeOffset(const std::filesystem::path& path,
                                      const std::uint64_t offset) {
    std::ifstream in(path, std::ios::binary);
    std::string magic(kDumpMagic.size(), '\0');
    std::uint32_t format = 0;
    std::uint32_t engine = 0;
    std::uint32_t nameLength = 0;
    if (!in.read(magic.data(), static_cast<std::streamsize>(magic.size())) || magic != kDumpMagic ||
        !readValue(in, format) || format != kDumpFormat || !readValue(in, engine) ||
        !readValue(in, nameLength)) {
        return "in a header this build does not read";
    }
    in.seekg(static_cast<std::streamoff>(nameLength), std::ios::cur);
    std::int64_t seed = 0;
    std::uint32_t chunks = 0;
    if (!readValue(in, seed) || !readValue(in, chunks)) {
        return "in the header";
    }
    auto position = static_cast<std::uint64_t>(in.tellg());
    if (offset < position) {
        return "in the header (engine, dimension, seed or chunk count)";
    }
    for (std::uint32_t i = 0; i < chunks; ++i) {
        std::uint64_t length = 0;
        std::int32_t x = 0;
        std::int32_t z = 0;
        if (!readValue(in, length) || !readValue(in, x) || !readValue(in, z)) {
            return "past a record this build cannot read";
        }
        const std::uint64_t end = position + sizeof(length) + length;
        if (offset < end) {
            return "in chunk (" + std::to_string(x) + ", " + std::to_string(z) + "), record " +
                   std::to_string(i) + ", byte " + std::to_string(offset - position) +
                   " of the record";
        }
        position = end;
        in.seekg(static_cast<std::streamoff>(end), std::ios::beg);
    }
    return "in the biome name table after the last chunk";
}

/// The first byte at which @p a and @p b differ, or nullopt when they are the
/// same bytes.
[[nodiscard]] std::optional<std::uint64_t> firstDifference(const std::filesystem::path& a,
                                                           const std::filesystem::path& b) {
    std::ifstream inA(a, std::ios::binary);
    std::ifstream inB(b, std::ios::binary);
    if (!inA || !inB) {
        throw std::runtime_error("cannot read " + (inA ? b : a).string());
    }
    constexpr std::size_t kBlock = 1U << 16U;
    std::vector<char> bufferA(kBlock);
    std::vector<char> bufferB(kBlock);
    std::uint64_t offset = 0;
    while (true) {
        inA.read(bufferA.data(), static_cast<std::streamsize>(kBlock));
        inB.read(bufferB.data(), static_cast<std::streamsize>(kBlock));
        const auto gotA = static_cast<std::size_t>(inA.gcount());
        const auto gotB = static_cast<std::size_t>(inB.gcount());
        const std::size_t common = std::min(gotA, gotB);
        for (std::size_t i = 0; i < common; ++i) {
            if (bufferA[i] != bufferB[i]) {
                return offset + i;
            }
        }
        if (gotA != gotB) {
            return offset + common;
        }
        if (gotA == 0) {
            return std::nullopt;
        }
        offset += gotA;
    }
}

/// 0 when every dump file in the two directories is byte-identical, 1 when
/// any differs or exists on one side only.
int compare(const std::filesystem::path& a, const std::filesystem::path& b) {
    const std::vector<std::string> namesA = dumpFiles(a);
    const std::vector<std::string> namesB = dumpFiles(b);
    std::set<std::string> all(namesA.begin(), namesA.end());
    all.insert(namesB.begin(), namesB.end());
    if (all.empty()) {
        throw std::runtime_error("neither " + a.string() + " nor " + b.string() +
                                 " holds a *.dump file");
    }
    int differing = 0;
    std::uint64_t bytes = 0;
    for (const std::string& name : all) {
        const bool inA = std::ranges::binary_search(namesA, name);
        const bool inB = std::ranges::binary_search(namesB, name);
        if (!inA || !inB) {
            std::printf("DIFFERENT %s: only in %s\n", name.c_str(), (inA ? a : b).string().c_str());
            ++differing;
            continue;
        }
        const std::optional<std::uint64_t> at = firstDifference(a / name, b / name);
        if (at.has_value()) {
            std::printf("DIFFERENT %s: first at byte %llu, %s\n", name.c_str(),
                        static_cast<unsigned long long>(*at), placeOffset(a / name, *at).c_str());
            ++differing;
        } else {
            bytes += std::filesystem::file_size(a / name);
        }
    }
    if (differing > 0) {
        std::printf("dumps differ: %d of %zu file(s)\n", differing, all.size());
        return 1;
    }
    std::printf("identical: %zu file(s), %llu bytes\n", all.size(),
                static_cast<unsigned long long>(bytes));
    return 0;
}

// --- statistics -----------------------------------------------------------------

constexpr std::array<std::string_view, 5> kStages = {"fill", "biomes", "encode", "sum", "e2e"};
/// fill, biomes, encode and sum are timed on the stage sweep; e2e on its own.
constexpr std::size_t kE2e = 4;

/// Per chunk, per stage, per timed pass: wall and CPU seconds.
struct Samples {
    std::vector<std::array<std::vector<double>, kStages.size()>> wall;
    std::vector<std::array<std::vector<double>, kStages.size()>> cpu;

    explicit Samples(const std::size_t chunks) : wall(chunks), cpu(chunks) {}
};

[[nodiscard]] double median(std::vector<double> values) {
    std::ranges::sort(values);
    const std::size_t n = values.size();
    return n % 2U == 1U ? values[n / 2U] : (values[(n / 2U) - 1U] + values[n / 2U]) / 2.0;
}

/// Linear interpolation between closest ranks, on sorted @p values.
[[nodiscard]] double percentile(const std::vector<double>& sorted, const double p) {
    const double rank = p * static_cast<double>(sorted.size() - 1U);
    const auto low = static_cast<std::size_t>(std::floor(rank));
    const std::size_t high = std::min(low + 1U, sorted.size() - 1U);
    return sorted[low] + ((sorted[high] - sorted[low]) * (rank - static_cast<double>(low)));
}

/// ms/chunk over chunks: each value already one chunk's median over passes.
[[nodiscard]] nlohmann::json summarise(std::vector<double> perChunkSeconds) {
    nlohmann::json out;
    out["chunks"] = perChunkSeconds.size();
    if (perChunkSeconds.empty() ||
        std::ranges::any_of(perChunkSeconds, [](const double v) { return std::isnan(v); })) {
        out["available"] = false;
        return out;
    }
    std::ranges::sort(perChunkSeconds);
    double total = 0.0;
    for (const double v : perChunkSeconds) {
        total += v;
    }
    const double mean = total / static_cast<double>(perChunkSeconds.size());
    out["available"] = true;
    out["median_ms"] = percentile(perChunkSeconds, 0.5) * kMillis;
    out["p10_ms"] = percentile(perChunkSeconds, 0.1) * kMillis;
    out["p90_ms"] = percentile(perChunkSeconds, 0.9) * kMillis;
    out["mean_ms"] = mean * kMillis;
    out["chunks_per_s"] = mean > 0.0 ? 1.0 / mean : 0.0;
    return out;
}

/// Every stage's figures over the chunks @p include picks.
template<typename Pick>
[[nodiscard]] nlohmann::json stageTable(const std::vector<const Samples*>& samples,
                                        const std::vector<BenchChunk>& chunks,
                                        const Pick& include) {
    nlohmann::json out;
    for (std::size_t stage = 0; stage < kStages.size(); ++stage) {
        std::vector<double> wall;
        std::vector<double> cpu;
        for (const Samples* run : samples) {
            for (std::size_t i = 0; i < chunks.size(); ++i) {
                if (include(chunks[i])) {
                    wall.push_back(median(run->wall[i][stage]));
                    cpu.push_back(median(run->cpu[i][stage]));
                }
            }
        }
        out[std::string(kStages.at(stage))] = {{"wall", summarise(wall)}, {"cpu", summarise(cpu)}};
    }
    return out;
}

void printFigure(const std::string_view label, const nlohmann::json& figure) {
    if (!figure.at("available").get<bool>()) {
        std::printf("  %-7s %s\n", std::string(label).c_str(), "-");
        return;
    }
    std::printf("  %-7s median %8.2f  p10 %8.2f  p90 %8.2f  mean %8.2f ms/chunk  %7.2f chunks/s\n",
                std::string(label).c_str(), figure.at("median_ms").get<double>(),
                figure.at("p10_ms").get<double>(), figure.at("p90_ms").get<double>(),
                figure.at("mean_ms").get<double>(), figure.at("chunks_per_s").get<double>());
}

void printTable(const nlohmann::json& table, const std::string_view clock) {
    for (const std::string_view stage : kStages) {
        printFigure(stage, table.at(std::string(stage)).at(std::string(clock)));
    }
}

// --- the run --------------------------------------------------------------------

[[nodiscard]] std::string dumpName(const std::string& dimension, const std::int64_t seed) {
    return dimension + "_seed" + std::to_string(seed) + std::string(kDumpSuffix);
}

/// One dimension at one seed: compile, warm up, time, and (first pass) verify
/// and dump.
[[nodiscard]] Samples benchOne(const Options& options, const DimensionSpec& spec,
                               const std::vector<std::byte>& blob, const std::int64_t seed,
                               const std::vector<BenchChunk>& chunks, nlohmann::json& record) {
    Stopwatch compileClock;
    const std::unique_ptr<world::CompiledDimension> dimension = world::CompiledDimension::compile(
        freeze::read(blob), spec.noiseSettings, spec.biomeSource, seed);
    record["compile_s"] = compileClock.lap().first;
    const settings::NoiseGeometry& geometry = dimension->geometry();

    std::unique_ptr<DumpFile> dump;
    if (!options.dump.empty()) {
        dump = std::make_unique<DumpFile>(options.dump / dumpName(spec.name, seed), spec.name, seed,
                                          static_cast<std::uint32_t>(chunks.size()));
    }

    Samples samples(chunks.size());
    std::vector<const data::ResourceLocation*> javaBiomes(kQuartsPerLayer * kQuartLayers);
    const int passes = options.warmup + options.repeat;
    for (int pass = 0; pass < passes; ++pass) {
        const bool timed = pass >= options.warmup;
        // The three stages, chunk by chunk, as a generation task runs them.
        for (std::size_t i = 0; i < chunks.size(); ++i) {
            const auto [chunkX, chunkZ] = chunks[i].pos;
            Stopwatch clock;
            terrain::ChunkBuffer buffer(geometry);
            dimension->fillBlocks(chunkX, chunkZ, buffer);
            const auto fill = clock.lap();
            dimension->fillBiomes(chunkX, chunkZ, kMinQuartY, javaBiomes);
            const auto biomes = clock.lap();
            const pmmp::EncodedChunk staged = encodeStages(geometry, buffer, javaBiomes);
            const auto encode = clock.lap();
            if (timed) {
                const std::array<std::pair<double, double>, 4> laps = {
                    fill, biomes, encode,
                    std::pair{fill.first + biomes.first + encode.first,
                              fill.second + biomes.second + encode.second}};
                for (std::size_t stage = 0; stage < laps.size(); ++stage) {
                    samples.wall[i][stage].push_back(laps.at(stage).first);
                    samples.cpu[i][stage].push_back(laps.at(stage).second);
                }
            }
            if (pass == 0) {
                const pmmp::EncodedChunk real = pmmp::encodeChunk(*dimension, chunkX, chunkZ);
                verifyAgainst(staged, real,
                              spec.name + " seed " + std::to_string(seed) + " chunk (" +
                                  std::to_string(chunkX) + ", " + std::to_string(chunkZ) + ")");
                if (dump) {
                    dump->chunk(chunks[i].pos, buffer, javaBiomes, real);
                }
            }
        }
        // encodeChunk itself, on its own sweep: timed passes only, since the
        // first pass has already run it on every chunk to verify the stages.
        if (!timed) {
            continue;
        }
        for (std::size_t i = 0; i < chunks.size(); ++i) {
            const auto [chunkX, chunkZ] = chunks[i].pos;
            Stopwatch clock;
            const pmmp::EncodedChunk encoded = pmmp::encodeChunk(*dimension, chunkX, chunkZ);
            const auto e2e = clock.lap();
            if (encoded.subChunks.empty()) {
                throw std::runtime_error("encodeChunk returned no sub-chunks");
            }
            samples.wall[i][kE2e].push_back(e2e.first);
            samples.cpu[i][kE2e].push_back(e2e.second);
        }
    }
    if (dump) {
        std::array<char, 17> hash{};
        std::snprintf(hash.data(), hash.size(), "%016llx",
                      static_cast<unsigned long long>(dump->finish()));
        record["dump"] = (options.dump / dumpName(spec.name, seed)).string();
        record["dump_fnv1a"] = std::string(hash.data());
    }
    return samples;
}

int run(const Options& options) {
    const std::vector<BenchChunk> chunks = chunkSet(options);
    const auto spiralCount = static_cast<std::size_t>(
        std::ranges::count_if(chunks, [](const BenchChunk& c) { return c.spiral; }));

    nlohmann::json report;
    report["tool"] = "stratum_chunk_cost_bench";
    report["machine"] = machine();
    report["load_average_start"] = loadAverage();
    report["warmup_passes"] = options.warmup;
    report["timed_passes"] = options.repeat;
    report["spiral"] = {{"centre", {options.spiralX, options.spiralZ}},
                        {"radius", options.spiralRadius},
                        {"chunks", spiralCount}};
    report["scattered_chunks"] = chunks.size() - spiralCount;
    report["seeds"] = options.seeds;

    const nlohmann::json& host = report["machine"];
    std::printf("stratum_chunk_cost_bench: %s, %s build (%s), %s\n",
                host["cpu"].get<std::string>().c_str(),
                host["build_config"].get<std::string>().c_str(),
                optimisedBuild() ? "optimised" : "UNOPTIMISED: these numbers are not a measurement",
                host["compiler"].get<std::string>().c_str());
    std::printf("  governor %s, boost %s, %u hardware threads, engine v%u, one thread\n",
                host["governor"].get<std::string>().c_str(),
                host["boost"].get<std::string>().c_str(), std::thread::hardware_concurrency(),
                kPipelineEngineVersion);
    std::printf("  load average at start: %s\n",
                report["load_average_start"].get<std::string>().c_str());
    std::printf("  chunks: spiral of %zu around (%d, %d), radius %d, and %zu scattered; %d "
                "warm-up pass(es), %d timed\n",
                spiralCount, options.spiralX, options.spiralZ, options.spiralRadius,
                chunks.size() - spiralCount, options.warmup, options.repeat);
    std::fflush(stdout);

    if (!options.dump.empty()) {
        prepareDumpDir(options.dump);
    }

    // Frozen and thawed once, as `Stratum\freezePipeline` writes it at world
    // creation and every `Dimension::open` reads it back.
    const Stopwatch freezeClock;
    const std::vector<std::byte> blob =
        freeze::write(freeze::resolve(data::Pack::open(options.versionDir / "worldgen"),
                                      options.versionDir / "biome_parameters"));
    report["freeze_s"] = wallSeconds() - freezeClock.wall;

    nlohmann::json& dimensions = report["dimensions"];
    for (const std::string& name : options.dimensions) {
        const DimensionSpec spec = dimensionNamed(name);
        std::vector<Samples> runs;
        nlohmann::json perSeed = nlohmann::json::array();
        for (const std::int64_t seed : options.seeds) {
            nlohmann::json record;
            record["seed"] = seed;
            record["load_average_before"] = loadAverage();
            runs.push_back(benchOne(options, spec, blob, seed, chunks, record));
            record["load_average_after"] = loadAverage();
            const std::vector<const Samples*> one = {&runs.back()};
            record["all"] = stageTable(one, chunks, [](const BenchChunk&) { return true; });
            const nlohmann::json& e2e = record["all"]["e2e"]["wall"];
            std::printf("%s seed %lld: compile %.3f s; e2e median %.2f ms/chunk, %.2f chunks/s "
                        "(load %s)\n",
                        name.c_str(), static_cast<long long>(seed),
                        record["compile_s"].get<double>(), e2e["median_ms"].get<double>(),
                        e2e["chunks_per_s"].get<double>(),
                        record["load_average_after"].get<std::string>().c_str());
            if (record.contains("dump_fnv1a")) {
                std::printf("  dump %s fnv1a %s\n", record["dump"].get<std::string>().c_str(),
                            record["dump_fnv1a"].get<std::string>().c_str());
            }
            std::fflush(stdout);
            perSeed.push_back(record);
        }
        std::vector<const Samples*> all;
        all.reserve(runs.size());
        for (const Samples& samples : runs) {
            all.push_back(&samples);
        }
        nlohmann::json entry;
        entry["noise_settings"] = spec.noiseSettings.toString();
        entry["biome_source"] = spec.biomeSource.toString();
        entry["seeds"] = perSeed;
        entry["all"] = stageTable(all, chunks, [](const BenchChunk&) { return true; });
        entry["spiral"] = stageTable(all, chunks, [](const BenchChunk& c) { return c.spiral; });
        entry["scattered"] = stageTable(all, chunks, [](const BenchChunk& c) { return !c.spiral; });

        std::printf("%s, every seed, %zu chunk(s) each — wall clock:\n", name.c_str(),
                    chunks.size());
        printTable(entry["all"], "wall");
        std::printf("  thread CPU:\n");
        printTable(entry["all"], "cpu");
        std::printf("  e2e by set (wall):\n");
        printFigure("spiral", entry["spiral"]["e2e"]["wall"]);
        printFigure("scatter", entry["scattered"]["e2e"]["wall"]);
        std::fflush(stdout);
        dimensions[name] = entry;
    }
    report["load_average_end"] = loadAverage();
    std::printf("load average at end: %s\n", report["load_average_end"].get<std::string>().c_str());

    if (!options.json.empty()) {
        std::ofstream out(options.json, std::ios::trunc);
        out << report.dump(2) << '\n';
        if (!out) {
            throw std::runtime_error("cannot write " + options.json.string());
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::vector<std::string_view> args(argv, argv + static_cast<std::size_t>(argc));
        if (args.size() >= 2U && (args[1] == "--help" || args[1] == "-h")) {
            std::printf("%s\n", std::string(kUsage).c_str());
            return 0;
        }
        if (args.size() >= 2U && args[1] == "--compare") {
            if (args.size() != 4U) {
                throw std::invalid_argument("--compare takes two dump directories");
            }
            return compare(std::filesystem::path(std::string(args[2])),
                           std::filesystem::path(std::string(args[3])));
        }
        return run(parse(args));
    } catch (const std::exception& error) {
        std::fprintf(stderr, "stratum_chunk_cost_bench: %s\n", error.what());
        return 2;
    }
}
