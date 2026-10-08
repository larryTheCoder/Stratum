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

    for (const char* tag : {"8", "16"}) {
        INFO("probe " << probeDir.filename().string() << ", scale " << tag);
        const std::filesystem::path world = probeDir / (std::string("nsb_") + tag) / "r.0.0.mca";
        const std::filesystem::path readout = probeDir / (std::string("nsr_") + tag) / "r.0.0.mca";
        REQUIRE(std::filesystem::is_regular_file(world));
        REQUIRE(std::filesystem::is_regular_file(readout));
        const Field field{readout};
        const auto file = region::RegionFile::open(world);
        std::vector<aquifer::CellIndex> floorSeen;

        for (std::int32_t cz = 0; cz < kChunks; ++cz) {
            for (std::int32_t cx = 0; cx < kChunks; ++cx) {
                REQUIRE(file.hasChunk(cx, cz));
                const auto ch = chunk::Chunk::decode(nbt::read(file.readChunk(cx, cz)).root);
                for (std::int32_t lz = 0; lz < 16; ++lz) {
                    for (std::int32_t lx = 0; lx < 16; ++lx) {
                        const std::int32_t x = (cx * 16) + lx;
                        const std::int32_t z = (cz * 16) + lz;
                        // From lambda + 1: on row lambda itself Q6.3 puts
                        // water over the lava sea whatever the barrier says.
                        for (std::int32_t y = lambda + 1; y <= kTopY; ++y) {
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
                                                              .floodedness = kFloodedness,
                                                              .spread = 0.0};
                                const std::int32_t level = aquifer::cellFluidLevel(cell);
                                const bool floor = takesFloor(cell);
                                if (floor) {
                                    anyFloor = true;
                                    total.floorNotLambda += static_cast<long long>(level != lambda);
                                    if (std::find(floorSeen.begin(), floorSeen.end(), src.cell) ==
                                        floorSeen.end()) {
                                        floorSeen.push_back(src.cell);
                                    }
                                }
                                const aquifer::FluidType type = aquifer::fluidTypeOf(
                                    aquifer::FluidTypeAt{.centreY = src.centre.y,
                                                         .level = level,
                                                         .seaLevel = kSeaLevel,
                                                         .lava = 0.0});
                                built[r] = aquifer::BarrierSource{
                                    .level = level, .distanceSq = src.distanceSq, .type = type};
                                rival[r] = built[r];
                                if (floor) {
                                    rival[r].level = aquifer::kNeverLevel;
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
                   << total.sentinelRight);

    // The corpus has to reach the branch, or nothing below means anything.
    REQUIRE(total.floorCells > 0);
    // The branch picked out here is the shipped one: it returns lambda.
    REQUIRE(total.floorNotLambda == 0);
    REQUIRE(total.serverStone > 0);

    // The floor is lambda, not the sentinel. Wherever the two give the
    // barrier different verdicts the server sides with lambda: measured 1666
    // of 1666 over three seeds, 102 floor cells.
    REQUIRE(total.contested >= 1000);
    CHECK(total.lambdaRight == total.contested);
    CHECK(total.sentinelRight == 0);

    // And the barrier misses nothing the server built here. What it is NOT
    // yet is exact: it writes stone the server does not, 7718 blocks over the
    // same three seeds, every one at a pair of sources one of which reads a
    // level below lambda (a capped ladder, or a deep one) — a residual this
    // corpus found and nothing earlier could, being the first with the real
    // barrier noise over an aborting surface. It is SPEC §11's open item,
    // pinned here so that it cannot grow unnoticed; the fix that closes it
    // turns this into an equality.
    CHECK(total.misses == 0);
    CHECK(total.falseStone <= 7718);
}
