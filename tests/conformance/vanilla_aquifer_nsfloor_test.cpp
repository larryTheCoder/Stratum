// Stratum — the aborting near-surface floor, through the barrier.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// `cellFluidLevel` floors an aborting near-surface cell at `lambda`, where the
// two other dry outcomes return the sentinel `kNeverLevel`. No block readout
// can separate the two: such a cell is dry at every y >= lambda either way,
// and below lambda the global lava sea takes the block first. They part only
// in the barrier's pressure term, which weighs both sources' levels whether or
// not either is readable — and every world that reached this branch before
// held `barrier` at -2.0, where no barrier forms.
//
// `tools/analysis/aquifer-nsfloor-probe.sh` is the near-surface probe's field
// with vanilla's barrier noise switched on: a three-valued surface that sends
// cells into the aborting floor beside cells that flood to the sea, at a
// floodedness past both of the level rule's gates. Each scale ships a readout
// dimension, so the surface is read out of the world rather than rebuilt.
//
// The case scores the barrier on every block above lambda, twice: with the
// floor as built, and with that one branch returning the sentinel instead.
// Where the two verdicts differ, the server's block says which floor it used.
//
// ONE SEED PER PROBE DIRECTORY, read from its manifest; every `nsfloor_s*`
// directory present is scored, and the case SKIPs when there is none. The
// fixtures are Mojang-derived and never committed (SPEC §12).
#include "support/probe_corpus.hpp"

#include <stratum/aquifer/barrier.hpp>
#include <stratum/aquifer/fluid_type.hpp>
#include <stratum/aquifer/lattice.hpp>
#include <stratum/aquifer/sampling.hpp>
#include <stratum/aquifer/selection.hpp>
#include <stratum/aquifer/substance.hpp>
#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/graph.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/javamath.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace stratum;

constexpr std::int32_t kSeaLevel = 63;
constexpr std::int32_t kChunks = 8;
constexpr std::int32_t kSpan = kChunks * 16;
constexpr double kDensity = -1.0;    ///< the probe's constant `raw_final_density`
constexpr double kFloodedness = 0.9; ///< past both of the level rule's gates
/// The highest row scored. The probe's surface tops out at 96, and a sea
/// cell's level is `sea_level`, so nothing above this can hold a barrier.
constexpr std::int32_t kTopY = 100;

/// The three arms, as the probe writes them.
constexpr double kLow = -70.0;
constexpr double kMid = -20.0;
constexpr double kHigh = 96.0;

[[nodiscard]] std::filesystem::path fixtures() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11";
}

/// The surface field, one value per column, from a readout dimension — the
/// same harness and the same height thresholds as
/// vanilla_aquifer_varying_surface_test.cpp's.
class Field {
public:
    explicit Field(const std::filesystem::path& readout) : values_(kSpan * kSpan, 0.0) {
        const auto file = region::RegionFile::open(readout);
        for (std::int32_t cz = 0; cz < kChunks; ++cz) {
            for (std::int32_t cx = 0; cx < kChunks; ++cx) {
                REQUIRE(file.hasChunk(cx, cz));
                const auto chunk = chunk::Chunk::decode(nbt::read(file.readChunk(cx, cz)).root);
                for (std::int32_t lz = 0; lz < 16; ++lz) {
                    for (std::int32_t lx = 0; lx < 16; ++lx) {
                        std::int32_t top = -65;
                        for (std::int32_t y = 250; y >= -64; --y) {
                            const auto* block = chunk.blockAt(lx, y, lz);
                            if (block != nullptr && block->name != "minecraft:air") {
                                top = y;
                                break;
                            }
                        }
                        const double arm = top < 100 ? kLow : (top < 160 ? kMid : kHigh);
                        values_[index((cx * 16) + lx, (cz * 16) + lz)] = arm;
                    }
                }
            }
        }
    }

    [[nodiscard]] static bool inside(const std::int32_t x, const std::int32_t z) noexcept {
        return x >= 0 && x < kSpan && z >= 0 && z < kSpan;
    }

    [[nodiscard]] double at(const std::int32_t x, const std::int32_t z) const {
        return values_[index(x, z)];
    }

private:
    [[nodiscard]] static std::size_t index(const std::int32_t x, const std::int32_t z) noexcept {
        return static_cast<std::size_t>((z * kSpan) + x);
    }

    std::vector<double> values_;
};

/// Whether a cell takes the aborting near-surface floor: the early return of
/// `cellFluidLevel` whose value is `lambda`. Spelled out here, from the
/// branch's own documented condition, so the rival can replace that one
/// value and nothing else; the case asserts the shipped function returns
/// `lambda` on every cell this picks.
[[nodiscard]] bool takesFloor(const aquifer::CellFluid& cell) {
    const std::int32_t lambda = aquifer::lambdaLevel(cell.seaLevel);
    const std::int32_t oceanGate = javamath::wrappingSub(cell.seaLevel, aquifer::kOceanGateOffset);
    const bool nearSurface =
        cell.surface.gate < oceanGate &&
        javamath::wrappingSub(cell.surface.gate, cell.centreY) < aquifer::kNearSurfaceDepth;
    if (!nearSurface || !cell.surface.aborted) {
        return false;
    }
    return !(cell.centreY >= lambda &&
             cell.centreY >
                 javamath::wrappingAdd(cell.surface.cap, aquifer::kNearSurfaceFloorOffset));
}

/// Q2.3/Q2.5's per-chunk cutoff, as the filler computes it: above it the
/// global picker decides and no barrier forms. Empty when the chunk's
/// lattice rectangle leaves the readout footprint, where it is unknown.
template<typename SurfaceAt>
[[nodiscard]] std::optional<std::int32_t> ySkipLevelOf(std::int32_t chunkX, std::int32_t chunkZ,
                                                       SurfaceAt&& surfaceAt) {
    const aquifer::YSkipRectangle rect = aquifer::ySkipRectangle(chunkX * 16, chunkZ * 16);
    std::int32_t maxSurface = std::numeric_limits<std::int32_t>::min();
    for (std::int32_t z = rect.minZ; z <= rect.maxZ; z += aquifer::kYSkipSampleStride) {
        for (std::int32_t x = rect.minX; x <= rect.maxX; x += aquifer::kYSkipSampleStride) {
            const std::optional<double> psl = surfaceAt(x, z);
            if (!psl.has_value()) {
                return std::nullopt;
            }
            maxSurface = std::max(maxSurface, javamath::floorToInt(*psl));
        }
    }
    return aquifer::ySkip(maxSurface);
}

struct Score {
    long long blocks = 0;
    long long serverStone = 0;
    long long misses = 0;     ///< server stone the built floor does not write
    long long falseStone = 0; ///< stone the built floor writes and the server does not
    long long floorCells = 0; ///< distinct sources that take the floor
    long long floorNotLambda = 0;
    /// Blocks where the two floors give different verdicts, and which one
    /// the server's block agrees with there.
    long long contested = 0;
    long long lambdaRight = 0;
    long long sentinelRight = 0;
    /// The other unmeasured choice, on the floodedness-0 arms: an aborted
    /// cell nothing floods stays dry (as built) or reads lambda (the rival).
    long long dryContested = 0;
    long long dryStaysDryRight = 0;
    long long dryLambdaRight = 0;
};

/// One aquifer arm of the probe, and the readout that names its field.
struct NsArm {
    const char* world;
    const char* readout;
    double floodedness;
};

void scoreProbe(const std::filesystem::path& probeDir, Score& total) {
    stratum::test::requireFrozen(probeDir, "tools/analysis/aquifer-nsfloor-probe.sh");
    std::ifstream manifestFile(probeDir / "manifest.json");
    const auto seed = nlohmann::json::parse(manifestFile).at("seed").get<std::int64_t>();

    const auto pack = data::Pack::open(fixtures() / "worldgen");
    density::Graph::Builder builder(pack);
    const density::NodeIndex barrierNode = builder.add({{"type", "minecraft:noise"},
                                                        {"noise", "minecraft:aquifer_barrier"},
                                                        {"xz_scale", 1.0},
                                                        {"y_scale", 0.5}});
    const density::Graph graph = builder.release();
    const std::vector<data::ResourceLocation> wanted{
        data::ResourceLocation::parse("minecraft:aquifer_barrier")};
    const auto noises =
        density::NoiseRegistry::create(pack, wanted, seed, density::RandomSource::Xoroshiro);
    const density::Interpreter interp(graph, noises);
    density::Interpreter::CornerCache cache(interp.cacheSize());
    const aquifer::CentreSource centres(seed);
    const std::int32_t lambda = aquifer::lambdaLevel(kSeaLevel);

    for (const NsArm& arm :
         {NsArm{"nsb_8", "nsr_8", kFloodedness}, NsArm{"nsb_16", "nsr_16", kFloodedness},
          NsArm{"nsd_8", "nsr_8", 0.0}, NsArm{"nsd_16", "nsr_16", 0.0}}) {
        INFO("probe " << probeDir.filename().string() << ", arm " << arm.world);
        const std::filesystem::path world = probeDir / arm.world / "r.0.0.mca";
        const std::filesystem::path readout = probeDir / arm.readout / "r.0.0.mca";
        REQUIRE(std::filesystem::is_regular_file(world));
        REQUIRE(std::filesystem::is_regular_file(readout));
        const Field field{readout};
        const auto file = region::RegionFile::open(world);
        std::vector<aquifer::CellIndex> floorSeen;

        for (std::int32_t cz = 0; cz < kChunks; ++cz) {
            for (std::int32_t cx = 0; cx < kChunks; ++cx) {
                REQUIRE(file.hasChunk(cx, cz));
                const std::optional<std::int32_t> ySkipLevel =
                    ySkipLevelOf(cx, cz, [&](std::int32_t sx, std::int32_t sz) {
                        return Field::inside(sx, sz) ? std::optional<double>(field.at(sx, sz))
                                                     : std::nullopt;
                    });
                if (!ySkipLevel.has_value()) {
                    continue; // the cutoff reads past the readout: not measurable here
                }
                const auto ch = chunk::Chunk::decode(nbt::read(file.readChunk(cx, cz)).root);
                for (std::int32_t lz = 0; lz < 16; ++lz) {
                    for (std::int32_t lx = 0; lx < 16; ++lx) {
                        const std::int32_t x = (cx * 16) + lx;
                        const std::int32_t z = (cz * 16) + lz;
                        // From lambda + 1: on row lambda itself Q6.3 puts
                        // water over the lava sea whatever the barrier says.
                        // Up to y_skip: above it the global picker decides.
                        for (std::int32_t y = lambda + 1; y <= std::min(kTopY, *ySkipLevel); ++y) {
                            const auto* block = ch.blockAt(lx, y, lz);
                            if (block == nullptr) {
                                continue;
                            }
                            const bool serverStone = block->name == "minecraft:stone";
                            if (!serverStone && block->name != "minecraft:water" &&
                                block->name != "minecraft:air") {
                                continue; // what fluid left behind: neither reading's to score
                            }

                            const aquifer::Selection sel = aquifer::selectSources(centres, x, y, z);
                            bool reachable = true;
                            std::array<aquifer::BarrierSource, 3> built{};
                            std::array<aquifer::BarrierSource, 3> rival{};
                            std::array<aquifer::BarrierSource, 3> dryRival{};
                            bool anyAbortedDry = false;
                            bool anyFloor = false;
                            for (std::size_t r = 0; r < 3 && reachable; ++r) {
                                const auto& src = sel.ranked[r];
                                const auto sampler = [&](const std::int32_t sx, std::int32_t,
                                                         const std::int32_t sz) {
                                    if (!Field::inside(sx, sz)) {
                                        reachable = false;
                                        return 0.0;
                                    }
                                    return field.at(sx, sz);
                                };
                                const aquifer::PslRead read =
                                    aquifer::readPreliminarySurface(sampler, src.centre, kSeaLevel);
                                const aquifer::CellFluid cell{.centreY = src.centre.y,
                                                              .surface = read,
                                                              .seaLevel = kSeaLevel,
                                                              .floodedness = arm.floodedness,
                                                              .spread = 0.0};
                                const aquifer::SourceStatus status =
                                    aquifer::sourceStatus(cell, 0.0);
                                const std::int32_t level = status.level;
                                const bool floor = takesFloor(cell);
                                if (floor) {
                                    anyFloor = true;
                                    total.floorNotLambda += static_cast<long long>(level != lambda);
                                    if (std::find(floorSeen.begin(), floorSeen.end(), src.cell) ==
                                        floorSeen.end()) {
                                        floorSeen.push_back(src.cell);
                                    }
                                }
                                const aquifer::FluidType type = status.type;
                                built[r] = aquifer::BarrierSource{
                                    .level = level, .distanceSq = src.distanceSq, .type = type};
                                rival[r] = built[r];
                                if (floor) {
                                    rival[r].level = aquifer::kNeverLevel;
                                }
                                dryRival[r] = built[r];
                                if (read.aborted && level == aquifer::kNeverLevel) {
                                    dryRival[r].level = lambda;
                                    anyAbortedDry = true;
                                }
                            }
                            if (!reachable) {
                                continue;
                            }

                            const double barrierNoise = interp.evaluate(
                                barrierNode, density::Point{.x = x, .y = y, .z = z}, cache);
                            const auto verdict =
                                [&](const std::array<aquifer::BarrierSource, 3>& s) {
                                    aquifer::BarrierAt at;
                                    at.y = y;
                                    at.density = kDensity;
                                    at.nearest = s[0];
                                    at.second = s[1];
                                    at.third = s[2];
                                    at.barrier = barrierNoise;
                                    return aquifer::placesBarrier(at);
                                };
                            const bool builtStone = verdict(built);

                            ++total.blocks;
                            total.serverStone += static_cast<long long>(serverStone);
                            total.misses += static_cast<long long>(serverStone && !builtStone);
                            total.falseStone += static_cast<long long>(!serverStone && builtStone);
                            if (anyAbortedDry) {
                                const bool rivalStone = verdict(dryRival);
                                if (rivalStone != builtStone) {
                                    ++total.dryContested;
                                    total.dryStaysDryRight +=
                                        static_cast<long long>(builtStone == serverStone);
                                    total.dryLambdaRight +=
                                        static_cast<long long>(rivalStone == serverStone);
                                }
                            }
                            if (anyFloor) {
                                const bool rivalStone = verdict(rival);
                                if (rivalStone != builtStone) {
                                    ++total.contested;
                                    total.lambdaRight +=
                                        static_cast<long long>(builtStone == serverStone);
                                    total.sentinelRight +=
                                        static_cast<long long>(rivalStone == serverStone);
                                }
                            }
                        }
                    }
                }
            }
        }
        total.floorCells += static_cast<long long>(floorSeen.size());
    }
}

} // namespace

TEST_CASE("the aborting near-surface floor as the barrier weighs it", "[conformance][aquifer]") {
    std::vector<std::filesystem::path> probes;
    const std::filesystem::path root = fixtures() / "probes";
    if (std::filesystem::is_directory(root)) {
        for (const auto& entry : std::filesystem::directory_iterator(root)) {
            if (entry.is_directory() &&
                entry.path().filename().string().rfind("nsfloor_s", 0) == 0) {
                probes.push_back(entry.path());
            }
        }
    }
    if (probes.empty() || !std::filesystem::is_directory(fixtures() / "worldgen")) {
        SKIP("no nsfloor_s* aquifer probe under "
             << root << "; generate one with tools/analysis/aquifer-nsfloor-probe.sh");
    }
    std::ranges::sort(probes);

    Score total;
    for (const auto& probe : probes) {
        scoreProbe(probe, total);
    }
    INFO("probes " << probes.size() << ": blocks " << total.blocks << ", server stone "
                   << total.serverStone << ", misses " << total.misses << ", false stone "
                   << total.falseStone << "; floor cells " << total.floorCells << " (not at lambda "
                   << total.floorNotLambda << "); contested " << total.contested
                   << ", lambda right " << total.lambdaRight << ", sentinel right "
                   << total.sentinelRight << "; aborted-dry contested " << total.dryContested
                   << ", stays dry right " << total.dryStaysDryRight << ", lambda right "
                   << total.dryLambdaRight);

    // The corpus has to reach the branch, or nothing below means anything.
    REQUIRE(total.floorCells > 0);
    // The branch picked out here is the shipped one: it returns lambda.
    REQUIRE(total.floorNotLambda == 0);
    REQUIRE(total.serverStone > 0);

    // The floor is lambda, not the sentinel. Wherever the two give the
    // barrier different verdicts the server sides with lambda: 1666 of 1666
    // over three seeds when first run, 1830 of 1830 once the scoring applied
    // y_skip and the abort floor.
    REQUIRE(total.contested >= 1000);
    CHECK(total.lambdaRight == total.contested);
    CHECK(total.sentinelRight == 0);

    // And the barrier is exact here. It was not before pipeline engine v4:
    // it wrote 7 690 blocks of stone the server does not, every one at a pair
    // of sources one of which read a level below lambda after an aborted
    // scan — which now reads lambda (SPEC §11, "Sub-lambda levels and the
    // near-surface sea").
    CHECK(total.misses == 0);
    CHECK(total.falseStone == 0);
    // The floodedness-0 arms: an aborted cell nothing floods stays dry (as
    // built, unmeasured before these arms) — exactness above covers it; the
    // contested count in the message says how much the arms could see.
}

namespace {

/// One constant-surface arm of `aquifer-capfloor-probe.sh`.
struct CapArm {
    const char* name;
    double psl;
    double density;
};

struct CapScore {
    long long blocks = 0;
    long long serverStone = 0;
    long long misses = 0;
    long long falseStone = 0;
    long long subLambdaSources = 0; ///< source readings below lambda, other than the sentinel
    /// The rival floor: a cap below lambda floors a wet level even when the
    /// scan did not abort. Blocks where it changes the verdict, and which of
    /// the two the server sides with.
    long long capContested = 0;
    long long abortRight = 0;
    long long capRight = 0;
};

void scoreCapProbe(const std::filesystem::path& probeDir, const CapArm& arm, CapScore& score) {
    stratum::test::requireFrozen(probeDir, "tools/analysis/aquifer-capfloor-probe.sh");
    std::ifstream manifestFile(probeDir / "manifest.json");
    const auto seed = nlohmann::json::parse(manifestFile).at("seed").get<std::int64_t>();

    const auto pack = data::Pack::open(fixtures() / "worldgen");
    density::Graph::Builder builder(pack);
    const density::NodeIndex barrierNode = builder.add({{"type", "minecraft:noise"},
                                                        {"noise", "minecraft:aquifer_barrier"},
                                                        {"xz_scale", 1.0},
                                                        {"y_scale", 0.5}});
    // The probe's own fast-sampled spread, as the deep-floor probe's.
    const density::NodeIndex spreadNode =
        builder.add({{"type", "minecraft:mul"},
                     {"argument1", 4.0},
                     {"argument2",
                      {{"type", "minecraft:noise"},
                       {"noise", "minecraft:aquifer_fluid_level_spread"},
                       {"xz_scale", 4.0},
                       {"y_scale", 0.7142857142857143 * 4.0}}}});
    const density::Graph graph = builder.release();
    const std::vector<data::ResourceLocation> wanted{
        data::ResourceLocation::parse("minecraft:aquifer_barrier"),
        data::ResourceLocation::parse("minecraft:aquifer_fluid_level_spread")};
    const auto noises =
        density::NoiseRegistry::create(pack, wanted, seed, density::RandomSource::Xoroshiro);
    const density::Interpreter interp(graph, noises);
    density::Interpreter::CornerCache cache(interp.cacheSize());
    const aquifer::CentreSource centres(seed);
    const std::int32_t lambda = aquifer::lambdaLevel(kSeaLevel);
    // The scan itself, on a constant field: unlike `constantSurface` it
    // aborts when the constant is below the threshold, which is the point.
    const aquifer::PslRead surface = aquifer::readPreliminarySurface(
        [&](std::int32_t, std::int32_t, std::int32_t) { return arm.psl; }, aquifer::CellIndex{},
        kSeaLevel);

    const std::filesystem::path world = probeDir / arm.name / "r.0.0.mca";
    REQUIRE(std::filesystem::is_regular_file(world));
    const auto file = region::RegionFile::open(world);
    const std::int32_t ySkipLevel = aquifer::ySkip(javamath::floorToInt(arm.psl));
    for (std::int32_t cz = 0; cz < kChunks; ++cz) {
        for (std::int32_t cx = 0; cx < kChunks; ++cx) {
            REQUIRE(file.hasChunk(cx, cz));
            const auto ch = chunk::Chunk::decode(nbt::read(file.readChunk(cx, cz)).root);
            for (std::int32_t lz = 0; lz < 16; ++lz) {
                for (std::int32_t lx = 0; lx < 16; ++lx) {
                    const std::int32_t x = (cx * 16) + lx;
                    const std::int32_t z = (cz * 16) + lz;
                    for (std::int32_t y = lambda + 1; y <= std::min(kTopY, ySkipLevel); ++y) {
                        const auto* block = ch.blockAt(lx, y, lz);
                        if (block == nullptr) {
                            continue;
                        }
                        const bool serverStone = block->name == "minecraft:stone";
                        if (!serverStone && block->name != "minecraft:water" &&
                            block->name != "minecraft:air") {
                            continue;
                        }
                        const aquifer::Selection sel = aquifer::selectSources(centres, x, y, z);
                        std::array<aquifer::BarrierSource, 3> src{};
                        std::array<aquifer::BarrierSource, 3> capRival{};
                        bool anyCapFloor = false;
                        for (std::size_t r = 0; r < 3; ++r) {
                            const auto& s = sel.ranked[r];
                            const aquifer::SamplePos sp = aquifer::spreadSample(s.cell, s.centre);
                            const double spread = interp.evaluate(
                                spreadNode, density::Point{.x = sp.x, .y = sp.y, .z = sp.z}, cache);
                            const aquifer::SourceStatus status =
                                aquifer::sourceStatus(aquifer::CellFluid{.centreY = s.centre.y,
                                                                         .surface = surface,
                                                                         .seaLevel = kSeaLevel,
                                                                         .floodedness = 0.6,
                                                                         .spread = spread},
                                                      0.0);
                            const std::int32_t level = status.level;
                            score.subLambdaSources += static_cast<long long>(
                                level < lambda && level != aquifer::kNeverLevel);
                            src[r] = aquifer::BarrierSource{
                                .level = level, .distanceSq = s.distanceSq, .type = status.type};
                            capRival[r] = src[r];
                            if (!surface.aborted && surface.cap < lambda && level < lambda &&
                                level != aquifer::kNeverLevel) {
                                capRival[r].level = lambda;
                                anyCapFloor = true;
                            }
                        }
                        aquifer::BarrierAt at;
                        at.y = y;
                        at.density = arm.density;
                        at.nearest = src[0];
                        at.second = src[1];
                        at.third = src[2];
                        at.barrier = interp.evaluate(barrierNode,
                                                     density::Point{.x = x, .y = y, .z = z}, cache);
                        const bool ours = aquifer::placesBarrier(at);
                        if (anyCapFloor) {
                            aquifer::BarrierAt rivalAt = at;
                            rivalAt.nearest = capRival[0];
                            rivalAt.second = capRival[1];
                            rivalAt.third = capRival[2];
                            const bool rival = aquifer::placesBarrier(rivalAt);
                            if (rival != ours) {
                                ++score.capContested;
                                score.abortRight += static_cast<long long>(ours == serverStone);
                                score.capRight += static_cast<long long>(rival == serverStone);
                            }
                        }
                        ++score.blocks;
                        score.serverStone += static_cast<long long>(serverStone);
                        score.misses += static_cast<long long>(serverStone && !ours);
                        score.falseStone += static_cast<long long>(!serverStone && ours);
                    }
                }
            }
        }
    }
}

} // namespace

TEST_CASE("which source levels the barrier weighs at lambda", "[conformance][aquifer]") {
    std::vector<std::filesystem::path> probes;
    const std::filesystem::path root = fixtures() / "probes";
    if (std::filesystem::is_directory(root)) {
        for (const auto& entry : std::filesystem::directory_iterator(root)) {
            if (entry.is_directory() &&
                entry.path().filename().string().rfind("capfloor_s", 0) == 0) {
                probes.push_back(entry.path());
            }
        }
    }
    if (probes.empty() || !std::filesystem::is_directory(fixtures() / "worldgen")) {
        SKIP("no capfloor_s* aquifer probe under "
             << root << "; generate one with tools/analysis/aquifer-capfloor-probe.sh");
    }
    std::ranges::sort(probes);
    // Each arm is exact: the barrier writes every block of stone the server
    // does and no other. cf58 is the near-surface type's arm (20 462 blocks of
    // false barrier before that fix), cf66 the aborted floor's control and
    // cf200 the unclamped ladder's (a blanket floor misses 1 017 there).
    // cf58d is the arm that would expose the floor following a cap below
    // lambda rather than the abort, which is the spec's reading and the one
    // built: its contested count says how much it could see.
    for (const CapArm& arm : {CapArm{"cf58", -58.0, -1.0}, CapArm{"cf66", -66.0, -1.0},
                              CapArm{"cf200", 200.0, -1.0}, CapArm{"cf58d", -58.0, -0.05}}) {
        CapScore score;
        for (const auto& probe : probes) {
            scoreCapProbe(probe, arm, score);
        }
        INFO("arm " << arm.name << ": blocks " << score.blocks << ", server stone "
                    << score.serverStone << ", misses " << score.misses << ", false stone "
                    << score.falseStone << ", sub-lambda source readings " << score.subLambdaSources
                    << "; cap-floor contested " << score.capContested << ", abort right "
                    << score.abortRight << ", cap right " << score.capRight);
        REQUIRE(score.blocks > 100000);
        REQUIRE(score.serverStone > 0);
        CHECK(score.misses == 0);
        CHECK(score.falseStone == 0);
        CHECK(score.abortRight == score.capContested);
    }
}
