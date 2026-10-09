// Stratum — what the aquifer costs a vanilla overworld chunk, and a byte dump
// of what it wrote.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
//   stratum_aquifer_cost_bench <fixtures-version-dir> [options]
//
//     --seeds a,b,...        world seeds (default 0)
//     --chunks x,z,nx,nz     the chunk window, lowest corner first (default 0,0,4,4)
//     --aquifers on|off      the preset's own aquifers_enabled, or forced off
//     --veins on|off         its ore_veins_enabled, or forced off
//     --surface on|off       run the 287-rule surface tree, or the first pass alone
//     --settings <id>        the noise settings (default minecraft:overworld; the
//                            amplified and large_biomes presets take the
//                            overworld's biome list, as vanilla's presets do)
//     --repeat n             fill the whole window n times, sum each chunk's fastest
//     --dump <file>          write every block state and fluid-update mark
//
// WHY THIS EXISTS. The aquifer's per-block cost (SPEC §11, "What the aquifer
// costs") had no figure of its own: the only speed on record was the whole
// overworld's ms/chunk, with no split between the density field, the surface
// pass and the aquifer. This fills vanilla's own overworld from the fetched
// pack exactly as `world::CompiledDimension` does (the same graph, registry,
// biome search and surface tree; tests/conformance/
// vanilla_compiled_dimension_test.cpp asserts the two agree block for block)
// with each of the three toggles independent, so the aquifer's share is a
// difference of two runs that differ in that flag alone. Ore veins are gated on
// aquifers by the engine itself (`ore::veinsPlaceBlocks`), so the aquifer's
// share is `veins off` against `aquifers off`, never `on` against `off`.
//
// THE TIME. CPU time per fill() (`std::clock`, process CPU time on POSIX),
// which a loaded machine moves far less than wall time; wall time is printed
// beside it. Compiling the dimension is timed separately and is not in
// ms/chunk. With --repeat the whole window is filled again, and each chunk's
// fastest fill is what is summed.
//
// THE DUMP is what makes an optimisation provably output-neutral rather than
// "the tests still pass": for every chunk, in window order, its seed and
// coordinates, every block's state id in (y, z, x) order, and every fluid-update
// mark in the order the first pass recorded it; the state names follow at the
// end. Two builds that write the same dump for the same arguments wrote the
// same blocks and the same marks — `cmp` is the whole comparison. The FNV-1a
// of the dump is printed too, so a run can be compared at a glance.
//
// Nothing it reads is committed: the worldgen tree is Mojang-derived
// (SPEC §12), and the dump is generated output.
#include <stratum/biome/parameter_list.hpp>
#include <stratum/biome/temperature_table.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/rule_graph.hpp>
#include <stratum/terrain/filler.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace stratum;

namespace {

constexpr int kChunkWidth = 16;
constexpr std::uint64_t kFnvOffset = 0xcbf29ce484222325ULL;
constexpr std::uint64_t kFnvPrime = 0x100000001b3ULL;

struct Options {
    std::filesystem::path versionDir;
    std::vector<std::int64_t> seeds{0};
    std::int32_t chunkMinX = 0;
    std::int32_t chunkMinZ = 0;
    std::int32_t chunkCountX = 4;
    std::int32_t chunkCountZ = 4;
    bool aquifers = true;
    bool veins = true;
    bool surface = true;
    data::ResourceLocation preset{"minecraft", "overworld"};
    int repeat = 1;
    std::filesystem::path dump;
};

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

[[nodiscard]] Options parse(const std::vector<std::string_view>& args) {
    if (args.size() < 2U) {
        throw std::invalid_argument("usage: stratum_aquifer_cost_bench <fixtures-version-dir> "
                                    "[--seeds a,b] [--chunks x,z,nx,nz] [--aquifers on|off] "
                                    "[--veins on|off] [--surface on|off] [--settings id] "
                                    "[--repeat n] [--dump file]");
    }
    Options options;
    options.versionDir = std::filesystem::path(std::string(args[1]));
    for (std::size_t i = 2; i < args.size(); ++i) {
        const std::string_view flag = args[i];
        if (i + 1 >= args.size()) {
            throw std::invalid_argument(std::string(flag) + " needs a value");
        }
        const std::string_view value = args[++i];
        if (flag == "--seeds") {
            options.seeds.clear();
            for (const std::string& seed : splitCommas(value)) {
                options.seeds.push_back(std::stoll(seed));
            }
        } else if (flag == "--chunks") {
            const std::vector<std::string> parts = splitCommas(value);
            if (parts.size() != 4U) {
                throw std::invalid_argument("--chunks takes x,z,nx,nz");
            }
            options.chunkMinX = std::stoi(parts[0]);
            options.chunkMinZ = std::stoi(parts[1]);
            options.chunkCountX = std::stoi(parts[2]);
            options.chunkCountZ = std::stoi(parts[3]);
            if (options.chunkCountX <= 0 || options.chunkCountZ <= 0) {
                throw std::invalid_argument("--chunks needs a window of at least one chunk");
            }
        } else if (flag == "--aquifers") {
            options.aquifers = onOff(flag, value);
        } else if (flag == "--veins") {
            options.veins = onOff(flag, value);
        } else if (flag == "--surface") {
            options.surface = onOff(flag, value);
        } else if (flag == "--settings") {
            options.preset = data::ResourceLocation::parse(std::string(value));
        } else if (flag == "--repeat") {
            options.repeat = std::stoi(std::string(value));
            if (options.repeat <= 0) {
                throw std::invalid_argument("--repeat needs at least 1");
            }
        } else if (flag == "--dump") {
            options.dump = std::filesystem::path(std::string(value));
        } else {
            throw std::invalid_argument("unknown option '" + std::string(flag) + "'");
        }
    }
    return options;
}

/// Seconds of this process's CPU time — `std::clock`, which is CPU time on
/// every POSIX system (the process is single-threaded here) and wall time on
/// Windows, where the wall column then says the same thing twice.
[[nodiscard]] double cpuSeconds() {
    const std::clock_t now = std::clock();
    if (now == static_cast<std::clock_t>(-1)) {
        throw std::runtime_error("std::clock() is unavailable");
    }
    return static_cast<double>(now) / static_cast<double>(CLOCKS_PER_SEC);
}

/// One seed's overworld, built the way vanilla_compiled_dimension_test.cpp's
/// reference builds it, with the three flags applied to a copy of the
/// settings. Every member is referenced by the filler, so none may move.
struct World {
    data::Pack pack;
    settings::LoadedSettings loaded;
    settings::NoiseSettings settings;
    biome::ParameterList parameters;
    biome::TemperatureTable temperatures;
    surface::RuleGraph rules;
    density::NoiseRegistry noises;
    terrain::ChunkFiller filler;

    World(const Options& options, const std::int64_t seed)
        : pack(data::Pack::open(options.versionDir / "worldgen")), loaded(settings::loadAll(pack)),
          settings(adjusted(loaded.settings.at(options.preset), options)),
          parameters(readParameters(options.versionDir)),
          temperatures(biome::TemperatureTable::fromPack(pack)),
          rules(surface::RuleGraph::resolve(settings.surfaceRule, options.preset)),
          noises(density::NoiseRegistry::create(pack, wantedNoises(loaded, rules), seed,
                                                density::RandomSource::Xoroshiro)),
          filler(terrain::ChunkFiller::compile(
              loaded.graph, noises, settings, options.surface ? &rules : nullptr,
              options.surface ? &parameters : nullptr, options.surface ? &temperatures : nullptr)) {
        if (options.surface && !filler.runsSurfaceRules()) {
            throw std::runtime_error("the overworld's surface rules do not run in this build");
        }
    }

    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&&) = delete;
    World& operator=(World&&) = delete;
    ~World() = default;

    [[nodiscard]] static data::ResourceLocation overworld() {
        return data::ResourceLocation::parse("minecraft:overworld");
    }

    [[nodiscard]] static settings::NoiseSettings adjusted(settings::NoiseSettings base,
                                                          const Options& options) {
        if (!base.aquifersEnabled || !base.oreVeinsEnabled) {
            throw std::runtime_error(options.preset.toString() +
                                     " does not enable both aquifers and ore veins; this bench "
                                     "measures a shipped preset that does");
        }
        base.aquifersEnabled = options.aquifers;
        base.oreVeinsEnabled = options.veins;
        return base;
    }

    [[nodiscard]] static biome::ParameterList readParameters(const std::filesystem::path& dir) {
        std::ifstream file(dir / "biome_parameters" / "minecraft" / "overworld.json");
        if (!file) {
            throw std::runtime_error("no biome_parameters/minecraft/overworld.json under " +
                                     dir.string() + "; run tools/fetch-vanilla");
        }
        return biome::ParameterList::fromJson(nlohmann::json::parse(file), overworld());
    }

    [[nodiscard]] static std::vector<data::ResourceLocation>
    wantedNoises(const settings::LoadedSettings& loaded, const surface::RuleGraph& rules) {
        std::vector<data::ResourceLocation> wanted = loaded.graph.referencedNoises();
        for (const data::ResourceLocation& id : rules.referencedNoises()) {
            wanted.push_back(id);
        }
        for (const char* id :
             {"minecraft:surface", "minecraft:surface_secondary", "minecraft:clay_bands_offset"}) {
            wanted.push_back(data::ResourceLocation::parse(id));
        }
        return wanted;
    }
};

/// A block state's full text, properties in their (sorted) map order.
[[nodiscard]] std::string stateText(const settings::BlockState& block) {
    std::string text = block.name.toString();
    if (!block.properties.empty()) {
        text += '[';
        bool first = true;
        for (const auto& [key, value] : block.properties) {
            if (!first) {
                text += ',';
            }
            text += key;
            text += '=';
            text += value;
            first = false;
        }
        text += ']';
    }
    return text;
}

/// The dump, and the FNV-1a of every byte written to it.
class Dump {
public:
    explicit Dump(const std::filesystem::path& path) {
        if (!path.empty()) {
            out_ = std::make_unique<std::ofstream>(path, std::ios::binary | std::ios::trunc);
            if (!*out_) {
                throw std::runtime_error("cannot write " + path.string());
            }
        }
    }

    void bytes(const void* data, const std::size_t size) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            hash_ = (hash_ ^ p[i]) * kFnvPrime;
        }
        if (out_) {
            out_->write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        }
    }

    template<typename T>
    void value(const T v) {
        bytes(&v, sizeof(v));
    }

    [[nodiscard]] std::uint16_t idOf(const settings::BlockState& block) {
        const std::string text = stateText(block);
        const auto found = ids_.find(text);
        if (found != ids_.end()) {
            return found->second;
        }
        const auto id = static_cast<std::uint16_t>(names_.size());
        ids_.emplace(text, id);
        names_.push_back(text);
        return id;
    }

    void finish() {
        value(static_cast<std::uint32_t>(names_.size()));
        for (const std::string& name : names_) {
            value(static_cast<std::uint32_t>(name.size()));
            bytes(name.data(), name.size());
        }
        if (out_) {
            out_->flush();
            if (!*out_) {
                throw std::runtime_error("writing the dump failed");
            }
        }
    }

    [[nodiscard]] std::uint64_t hash() const noexcept { return hash_; }

private:
    std::unique_ptr<std::ofstream> out_;
    std::uint64_t hash_ = kFnvOffset;
    std::map<std::string, std::uint16_t> ids_;
    std::vector<std::string> names_;
};

struct Tally {
    std::uint64_t air = 0;
    std::uint64_t water = 0;
    std::uint64_t lava = 0;
    std::uint64_t other = 0;
    std::uint64_t marks = 0;
};

void record(Dump& dump, Tally& tally, const terrain::ChunkBuffer& buffer, const std::int64_t seed,
            const std::int32_t chunkX, const std::int32_t chunkZ) {
    dump.value(seed);
    dump.value(chunkX);
    dump.value(chunkZ);
    std::vector<std::uint16_t> paletteIds;
    paletteIds.reserve(buffer.palette().size());
    for (const settings::BlockState& block : buffer.palette()) {
        paletteIds.push_back(dump.idOf(block));
    }
    for (std::int32_t y = buffer.minY(); y < buffer.minY() + buffer.height(); ++y) {
        for (int z = 0; z < kChunkWidth; ++z) {
            for (int x = 0; x < kChunkWidth; ++x) {
                const std::uint16_t index = buffer.paletteIndexAt(x, y, z);
                dump.value(paletteIds.at(index));
                const std::string name = buffer.palette().at(index).name.toString();
                if (name == "minecraft:air") {
                    ++tally.air;
                } else if (name == "minecraft:water") {
                    ++tally.water;
                } else if (name == "minecraft:lava") {
                    ++tally.lava;
                } else {
                    ++tally.other;
                }
            }
        }
    }
    dump.value(static_cast<std::uint32_t>(buffer.fluidUpdates().size()));
    for (const terrain::ChunkBuffer::FluidUpdate& mark : buffer.fluidUpdates()) {
        dump.value(mark.localX);
        dump.value(mark.y);
        dump.value(mark.localZ);
    }
    tally.marks += buffer.fluidUpdates().size();
}

int run(const Options& options) {
    Dump dump(options.dump);
    Tally tally;
    double totalCpu = 0.0;
    double totalWall = 0.0;
    std::size_t totalChunks = 0;
    const auto chunkCount = static_cast<std::size_t>(options.chunkCountX) *
                            static_cast<std::size_t>(options.chunkCountZ);

    std::printf("%s: aquifers %s, veins %s, surface %s, chunks [%d, %d] + %dx%d, repeat %d\n",
                options.preset.toString().c_str(), options.aquifers ? "on" : "off",
                options.veins ? "on" : "off", options.surface ? "on" : "off", options.chunkMinX,
                options.chunkMinZ, options.chunkCountX, options.chunkCountZ, options.repeat);
    for (const std::int64_t seed : options.seeds) {
        const double compileStart = cpuSeconds();
        const World world(options, seed);
        const double compileCpu = cpuSeconds() - compileStart;

        // Each chunk's fastest fill over the passes, summed: a load spike on
        // one chunk of one pass then costs that chunk alone.
        std::vector<double> chunkCpu(chunkCount, std::numeric_limits<double>::infinity());
        std::vector<double> chunkWall(chunkCount, std::numeric_limits<double>::infinity());
        for (int pass = 0; pass < options.repeat; ++pass) {
            std::size_t index = 0;
            for (std::int32_t dz = 0; dz < options.chunkCountZ; ++dz) {
                for (std::int32_t dx = 0; dx < options.chunkCountX; ++dx) {
                    const std::int32_t chunkX = options.chunkMinX + dx;
                    const std::int32_t chunkZ = options.chunkMinZ + dz;
                    terrain::ChunkBuffer buffer(world.settings.geometry);
                    const auto wallStart = std::chrono::steady_clock::now();
                    const double cpuStart = cpuSeconds();
                    world.filler.fill(chunkX, chunkZ, buffer);
                    chunkCpu[index] = std::min(chunkCpu[index], cpuSeconds() - cpuStart);
                    chunkWall[index] = std::min(
                        chunkWall[index],
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart)
                            .count());
                    if (pass == 0) {
                        record(dump, tally, buffer, seed, chunkX, chunkZ);
                    }
                    ++index;
                }
            }
        }
        double bestCpu = 0.0;
        double bestWall = 0.0;
        for (std::size_t i = 0; i < chunkCount; ++i) {
            bestCpu += chunkCpu[i];
            bestWall += chunkWall[i];
        }
        totalCpu += bestCpu;
        totalWall += bestWall;
        totalChunks += chunkCount;
        std::printf("seed %lld: compile %.3f s cpu; %.2f ms/chunk cpu, %.2f ms/chunk wall over %zu "
                    "chunks\n",
                    static_cast<long long>(seed), compileCpu,
                    1000.0 * bestCpu / static_cast<double>(chunkCount),
                    1000.0 * bestWall / static_cast<double>(chunkCount), chunkCount);
        std::fflush(stdout);
    }
    dump.finish();
    std::printf("all: %.2f ms/chunk cpu, %.2f ms/chunk wall over %zu chunks\n",
                1000.0 * totalCpu / static_cast<double>(totalChunks),
                1000.0 * totalWall / static_cast<double>(totalChunks), totalChunks);
    std::printf(
        "blocks: air %llu, water %llu, lava %llu, other %llu; fluid-update marks %llu\n",
        static_cast<unsigned long long>(tally.air), static_cast<unsigned long long>(tally.water),
        static_cast<unsigned long long>(tally.lava), static_cast<unsigned long long>(tally.other),
        static_cast<unsigned long long>(tally.marks));
    std::printf("dump fnv1a %016llx\n", static_cast<unsigned long long>(dump.hash()));
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::vector<std::string_view> args(argv, argv + static_cast<std::size_t>(argc));
        return run(parse(args));
    } catch (const std::exception& error) {
        std::fprintf(stderr, "stratum_aquifer_cost_bench: %s\n", error.what());
        return 1;
    }
}
