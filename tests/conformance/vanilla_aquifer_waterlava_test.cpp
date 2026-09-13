// Stratum — the aquifer at the global lava sea's top, scored on the server.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Two clauses of the substance decision live only at the lava sea, and
// neither had ever been reached by a probe: Q2.4 (below the sea's top the
// sea wins, whatever any source says) and Q6.3 (ON the top row a nearest
// source reading water is water outright, with no barrier). Both reduce to
// Q1.2's global picker, a function of `y` alone, which is why the exception
// owns exactly one row — `y == lambda = min(-54, sea_level)` — and why no
// earlier barrier probe saw it: `aquifer-barrier-probe.sh` keeps the sea out
// of its world on purpose.
//
// `tools/analysis/aquifer-waterlava-probe.sh` builds the worlds: the
// barrier probe's own configuration (real barrier/floodedness/spread noise,
// constant psl 96, constant lava 0.0, three densities) at `min_y` -64 so
// the sea is real, plus one arm with `sea_level` -70 so the row has to MOVE
// with lambda. This reads them the way `aquifer-waterlava-analyze.cpp` does:
// the SAME committed `computeSubstance` the filler calls, end to end, against
// the BARE fall-through it used to be (`placesBarrier`, then the nearest
// source's own reading) on the same inputs — so what is scored is exactly
// "where the old logic writes stone and the server does not".
//
// WHAT WAS MEASURED (three seeds, 42 / 31337 / 8675309):
//
//   * On the shipped sea (63) the row is EMPTY. Across 3 seeds x 3
//     densities x 16384 columns, no source reads water at y = -54 at all —
//     a source's ladder there sits at -60 plus a multiple of 3, so water on
//     the row needs a spread of 0.9 or a sea-gated source centred within
//     five blocks of the sea, and neither occurred once. Both the server
//     and this build write 0 stone on the row. The exception is real but,
//     in vanilla's own overworld configuration, close to unreachable.
//   * At sea_level -70 the ladder hands the row water sources, and the
//     exception fires on 278 / 1102 / 1940 blocks. The bare logic writes
//     stone on 14 / 50 / 70 of them (19 / 176 / 178 once Q6.4's constant
//     is in the predicate: a lava body against the row's water); the
//     server writes stone on 0 of 3320 either way, so the exception's
//     precedence over Q6.6 holds against the new term too. One row up it
//     is inert: the two decisions agree on every block.
//   * The ASYMMETRY holds: where the nearest source reads AIR on the row,
//     the server does write barriers (93 / 239 / 244 of them), exactly as
//     on the rows above.
//   * Q2.4: below the sea the bare fall-through had the TYPE wrong on 7682 /
//     5330 / 5736 of 16384 blocks per density at sea 63 (water where the
//     server has lava); with the sea handled first it is 16384 / 16384. The
//     residual at sea -70 (16 / 166 / 41 blocks) is falling water directly
//     under water with obsidian beneath — water that reached the row after
//     generation (211 of the 223 under an aquifer-placed source, 12 under a
//     flow front) and dropped into the lava under it before that lava could
//     turn to obsidian — post-generation mechanics, not the aquifer. Not
//     one of the 223 is anything else.
//
// THE SAME WORLDS ALSO SCORE Q6.4'S MIXED-TYPE BRANCH — the second test
// case here. The `sea_level` -70 arm is the one world this project has
// where lava-typed sources (centred below lambda, with `lava` a constant
// 0.0) compete with water-typed ones on rows the lattice owns. Per block,
// the committed `placesBarrier` is called twice on the same three ranked
// sources: once as typed, once with every source retyped water — the
// predicate exactly as it was before the branch landed, where no agreeing
// pair ever fires. The server's stone says which is right, and the two
// readings of "one reads lava and the other water" the branch does NOT
// take are counted alongside so their refutation stays pinned:
//
//   * Comparing what each source READS at y (the committed reading): a
//     lava body meeting a water body — both fluid — takes the constant, a
//     pair that disagrees at y takes the level formula whatever its types.
//     On the rows above the sea, pooled over three seeds, the retyped
//     predicate misses 1240 of the server's real barriers in mixed
//     junctions and the committed one 330; neither writes a single block
//     of stone the server does not (0 / 0). Every block the constant adds
//     is a real barrier.
//   * Comparing the two TYPE FIELDS behind the disagree guard — so that a
//     lava-typed source reading AIR against a water-typed one reading
//     fluid would take the constant: on the nearest pair, where the
//     constant alone would fire and the formula does not, the server has
//     stone on 0 of 33 blocks; where the formula fires and the constant
//     would not, on every one of 252. Refuted.
//   * The types differing REGARDLESS of readings — stone between two
//     drained cells of different type: the server has stone on 0.0-0.5%
//     of the blocks the constant would fill (0-4 of 424-1145 per row above
//     the sea). Refuted.
//
// What the branch leaves — 330 of 1240 — is not a type question: this
// build reports a dry source as `level = lambda` (the spec's `never` is
// -32512) and clamps a ladder below lambda up to it, which on the rows 0-3
// above the sea puts a plane right under the block that the spec does not
// have. `aquifer-waterlava-analyze.cpp` re-scores the same blocks at the
// spec's own levels: 0 misses and 0 false stone on rows lambda+1..+3 on
// all three seeds. That is the next slice, named in PROGRESS.md; it is a
// contract change to `cellFluidLevel`, not a barrier change.
//
// ONE SEED PER PROBE DIRECTORY, read from its manifest; every
// `waterlava_s*` directory present is scored, and the case SKIPs when there
// is none. The fixtures are Mojang-derived and never committed (SPEC §12).
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
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace stratum;

constexpr std::int32_t kChunks = 8;

[[nodiscard]] std::filesystem::path fixtures() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11";
}

struct Dimension {
    std::string name;
    double density = 0.0;
    std::int32_t seaLevel = 0;
    std::int32_t minY = 0;
    std::int32_t height = 0;
    double psl = 0.0;
    double lava = 0.0;
};

[[nodiscard]] std::vector<Dimension> readSpec(const std::filesystem::path& specPath) {
    std::ifstream in(specPath);
    const nlohmann::json spec = nlohmann::json::parse(in);
    std::vector<Dimension> dims;
    for (const auto& entry : spec) {
        dims.push_back(Dimension{
            .name = entry.at("name").get<std::string>(),
            .density = entry.at("raw_final_density").at("argument").get<double>(),
            .seaLevel = entry.at("sea_level").get<std::int32_t>(),
            .minY = entry.at("min_y").get<std::int32_t>(),
            .height = entry.at("height").get<std::int32_t>(),
            .psl = entry.at("router").at("preliminary_surface_level").get<double>(),
            .lava = entry.at("router").at("lava").get<double>(),
        });
    }
    return dims;
}

[[nodiscard]] bool isWater(const chunk::BlockState* b) {
    return b != nullptr && b->name == "minecraft:water";
}

/// Falling water: `level` 8. What a water block becomes when it drops into
/// the block below it, which — measured — is the only way water ever ends
/// up below the sea's top.
[[nodiscard]] bool isFallingWater(const chunk::BlockState* b) {
    return isWater(b) && std::ranges::any_of(b->properties, [](const auto& kv) {
               return kv.first == "level" && kv.second == "8";
           });
}

/// Q6.1's similarity, on squared distances — needed here only to weigh the
/// two readings of Q6.4 the committed predicate does not take.
[[nodiscard]] double similarity(const std::int64_t di, const std::int64_t dj) {
    return 1.0 - static_cast<double>(dj - di) / static_cast<double>(aquifer::kSimilarityRange);
}

/// Whether any competing pair on this block is of mixed type.
[[nodiscard]] bool hasMixedPair(const aquifer::BarrierAt& at) {
    const double s12 = similarity(at.nearest.distanceSq, at.second.distanceSq);
    if (s12 <= 0.0) {
        return false;
    }
    const double s13 = similarity(at.nearest.distanceSq, at.third.distanceSq);
    const double s23 = similarity(at.second.distanceSq, at.third.distanceSq);
    return at.nearest.type != at.second.type || (s13 > 0.0 && at.nearest.type != at.third.type) ||
           (s23 > 0.0 && at.second.type != at.third.type);
}

/// The type-field reading, on the nearest pair: mixed, competing, and
/// disagreeing at y — and whether the constant alone would carry it.
struct TypeFieldReading {
    bool applies = false;
    bool constantFires = false;
};

[[nodiscard]] TypeFieldReading typeFieldReading(const aquifer::BarrierAt& at) {
    const double s12 = similarity(at.nearest.distanceSq, at.second.distanceSq);
    const bool aFluid = at.y < at.nearest.level;
    const bool bFluid = at.y < at.second.level;
    if (s12 <= 0.0 || at.nearest.type == at.second.type || aFluid == bFluid) {
        return {};
    }
    return {.applies = true,
            .constantFires = at.density + (s12 * aquifer::kMixedTypePressure) > 0.0};
}

/// The types-regardless reading: would a mixed pair that BOTH read air
/// carry the constant past D on its weight alone?
[[nodiscard]] bool bothAirMixedWouldFire(const aquifer::BarrierAt& at) {
    const double s12 = similarity(at.nearest.distanceSq, at.second.distanceSq);
    if (s12 <= 0.0) {
        return false;
    }
    const double s13 = similarity(at.nearest.distanceSq, at.third.distanceSq);
    const double s23 = similarity(at.second.distanceSq, at.third.distanceSq);
    const auto fires = [&](const aquifer::BarrierSource& a, const aquifer::BarrierSource& b,
                           const double weight) {
        const bool bothAir = !(at.y < a.level) && !(at.y < b.level);
        return a.type != b.type && weight > 0.0 && bothAir &&
               at.density + (weight * aquifer::kMixedTypePressure) > 0.0;
    };
    return fires(at.nearest, at.second, s12) || fires(at.nearest, at.third, s12 * s13) ||
           fires(at.second, at.third, s12 * s23);
}

struct Score {
    // Q6.4, on every row the lattice owns (y >= lambda) except the blocks
    // Q6.3 pre-empts the predicate on. "Old" is the predicate with every
    // source retyped water; "new" is the committed one.
    long long mixedServerStone = 0; // some competing pair is mixed-type, server stone
    long long mixedOldMiss = 0;
    long long mixedNewMiss = 0;
    long long mixedOldFalse = 0;
    long long mixedNewFalse = 0;
    long long pureServerStone = 0; // no mixed pair
    long long pureOldMiss = 0;
    long long pureNewMiss = 0;
    long long pureFalse = 0;               // old == new here by construction; either
    long long constantAdds = 0;            // new fires, old does not ...
    long long constantAddsServerStone = 0; // ... and the server has stone
    // The refuted readings, on the rows ABOVE lambda.
    long long tfFormulaOnly = 0;
    long long tfFormulaOnlyServerStone = 0;
    long long tfConstantOnly = 0;
    long long tfConstantOnlyServerStone = 0;
    long long bothAir = 0;
    long long bothAirServerStone = 0;

    // The row the exception owns.
    long long fires = 0;                 // nearest reads water, global lava below
    long long firesBareStone = 0;        // the bare fall-through writes stone
    long long firesServerStone = 0;      // the server did
    long long firesNowFluid = 0;         // computeSubstance says fluid
    long long nearestAir = 0;            // the asymmetric control ...
    long long nearestAirServerStone = 0; // ... where the server still writes barriers
    // The rows above it, where the exception must be inert.
    long long aboveTotal = 0;
    long long aboveDisagree = 0; // computeSubstance != bare
    // The row below it, Q2.4's.
    long long belowTotal = 0;
    long long belowNowLava = 0;
    long long belowServerLava = 0;
    long long belowServerWater = 0;
    long long belowServerWaterUnexplained = 0;
    long long belowBareWrongType = 0;
};

void scoreProbe(const std::filesystem::path& probeDir, Score& total) {
    std::ifstream manifestFile(probeDir / "manifest.json");
    const nlohmann::json manifest = nlohmann::json::parse(manifestFile);
    const std::int64_t seed = manifest.at("seed").get<std::int64_t>();
    const std::vector<Dimension> dims = readSpec(probeDir / "spec.json");

    const auto pack = data::Pack::open(fixtures() / "worldgen");
    density::Graph::Builder builder(pack);
    const density::NodeIndex barrierNode = builder.add({{"type", "minecraft:noise"},
                                                        {"noise", "minecraft:aquifer_barrier"},
                                                        {"xz_scale", 1.0},
                                                        {"y_scale", 0.5}});
    const density::NodeIndex floodNode =
        builder.add({{"type", "minecraft:noise"},
                     {"noise", "minecraft:aquifer_fluid_level_floodedness"},
                     {"xz_scale", 1.0},
                     {"y_scale", 0.67}});
    const density::NodeIndex spreadNode =
        builder.add({{"type", "minecraft:noise"},
                     {"noise", "minecraft:aquifer_fluid_level_spread"},
                     {"xz_scale", 1.0},
                     {"y_scale", 0.7142857142857143}});
    const density::Graph graph = builder.release();
    const std::vector<data::ResourceLocation> wanted{
        data::ResourceLocation::parse("minecraft:aquifer_barrier"),
        data::ResourceLocation::parse("minecraft:aquifer_fluid_level_floodedness"),
        data::ResourceLocation::parse("minecraft:aquifer_fluid_level_spread")};
    const auto noises =
        density::NoiseRegistry::create(pack, wanted, seed, density::RandomSource::Xoroshiro);
    const density::Interpreter interp(graph, noises);
    density::Interpreter::CornerCache cache(interp.cacheSize());
    const aquifer::CentreSource centres(seed);

    const auto barrierAt = [&](std::int32_t x, std::int32_t y, std::int32_t z) {
        return interp.evaluate(barrierNode, density::Point{.x = x, .y = y, .z = z}, cache);
    };
    const auto floodednessAt = [&](std::int32_t x, std::int32_t y, std::int32_t z) {
        return interp.evaluate(floodNode, density::Point{.x = x, .y = y, .z = z}, cache);
    };
    const auto spreadAt = [&](std::int32_t x, std::int32_t y, std::int32_t z) {
        return interp.evaluate(spreadNode, density::Point{.x = x, .y = y, .z = z}, cache);
    };

    for (const Dimension& dim : dims) {
        const std::filesystem::path regionPath = probeDir / dim.name / "r.0.0.mca";
        if (!std::filesystem::is_regular_file(regionPath)) {
            continue;
        }
        const std::int32_t lambda = aquifer::lambdaLevel(dim.seaLevel);
        const auto pslAt = [&](std::int32_t, std::int32_t, std::int32_t) { return dim.psl; };
        const auto lavaAt = [&](std::int32_t, std::int32_t, std::int32_t) { return dim.lava; };
        const aquifer::PslRead surface =
            aquifer::constantSurface(static_cast<std::int32_t>(std::floor(dim.psl)));
        aquifer::StatusCache statusCache;

        const auto file = region::RegionFile::open(regionPath);
        for (std::int32_t cz = 0; cz < kChunks; ++cz) {
            for (std::int32_t cx = 0; cx < kChunks; ++cx) {
                if (!file.hasChunk(cx, cz)) {
                    continue;
                }
                const auto ch = chunk::Chunk::decode(nbt::read(file.readChunk(cx, cz)).root);
                for (std::int32_t lz = 0; lz < 16; ++lz) {
                    for (std::int32_t lx = 0; lx < 16; ++lx) {
                        const std::int32_t x = (cx * 16) + lx;
                        const std::int32_t z = (cz * 16) + lz;
                        for (std::int32_t y = lambda - 1; y <= lambda + 3; ++y) {
                            if (y < dim.minY || y >= dim.minY + dim.height) {
                                continue;
                            }
                            const auto* b =
                                ch.blockAt(static_cast<int>(lx), y, static_cast<int>(lz));
                            REQUIRE(b != nullptr);
                            const bool serverStone = b->name == "minecraft:stone";
                            const bool serverLava =
                                b->name == "minecraft:lava" || b->name == "minecraft:obsidian";
                            const bool serverWater = b->name == "minecraft:water";

                            const aquifer::AquiferQuery query{.x = x,
                                                              .y = y,
                                                              .z = z,
                                                              .density = dim.density,
                                                              .seaLevel = dim.seaLevel};
                            const aquifer::SubstanceAt now =
                                aquifer::computeSubstance(centres, query, statusCache, barrierAt,
                                                          floodednessAt, spreadAt, lavaAt, pslAt);

                            // The bare fall-through, from the same pieces.
                            const aquifer::Selection selection =
                                aquifer::selectSources(centres, x, y, z);
                            std::array<std::int32_t, 3> level{};
                            std::array<aquifer::FluidType, 3> type{};
                            for (std::size_t r = 0; r < 3; ++r) {
                                const auto& src = selection.ranked[r];
                                const aquifer::SamplePos fp =
                                    aquifer::floodednessSample(src.centre);
                                const aquifer::SamplePos sp =
                                    aquifer::spreadSample(src.cell, src.centre);
                                level[r] = aquifer::cellFluidLevel(aquifer::CellFluid{
                                    .centreY = src.centre.y,
                                    .surface = surface,
                                    .seaLevel = dim.seaLevel,
                                    .floodedness = floodednessAt(fp.x, fp.y, fp.z),
                                    .spread = spreadAt(sp.x, sp.y, sp.z)});
                                const aquifer::SamplePos lp = aquifer::lavaSample(src.centre);
                                type[r] = aquifer::fluidTypeOf(
                                    aquifer::FluidTypeAt{.centreY = src.centre.y,
                                                         .level = level[r],
                                                         .seaLevel = dim.seaLevel,
                                                         .lava = lavaAt(lp.x, lp.y, lp.z)});
                            }
                            aquifer::BarrierAt at;
                            at.y = y;
                            at.density = dim.density;
                            at.nearest =
                                aquifer::BarrierSource{.level = level[0],
                                                       .distanceSq = selection.ranked[0].distanceSq,
                                                       .type = type[0]};
                            at.second =
                                aquifer::BarrierSource{.level = level[1],
                                                       .distanceSq = selection.ranked[1].distanceSq,
                                                       .type = type[1]};
                            at.third =
                                aquifer::BarrierSource{.level = level[2],
                                                       .distanceSq = selection.ranked[2].distanceSq,
                                                       .type = type[2]};
                            at.barrier = barrierAt(x, y, z);
                            // The predicate before Q6.4's first clause:
                            // every source water-typed.
                            aquifer::BarrierAt retyped = at;
                            retyped.nearest.type = aquifer::FluidType::Default;
                            retyped.second.type = aquifer::FluidType::Default;
                            retyped.third.type = aquifer::FluidType::Default;
                            const bool oldStone = aquifer::placesBarrier(retyped);
                            const bool newStone = aquifer::placesBarrier(at);
                            const bool nearestFluid = y < level[0];
                            const aquifer::FluidType nearestType = type[0];
                            aquifer::SubstanceAt bare;
                            if (newStone) {
                                bare = aquifer::SubstanceAt{.substance = aquifer::Substance::Solid};
                            } else if (nearestFluid) {
                                bare = aquifer::SubstanceAt{.substance = aquifer::Substance::Fluid,
                                                            .fluidType = nearestType};
                            } else {
                                bare = aquifer::SubstanceAt{.substance = aquifer::Substance::Air};
                            }
                            const bool bareStone = bare.substance == aquifer::Substance::Solid;

                            // Q6.4, wherever the predicate's own answer is
                            // what the server was asked: not below lambda
                            // (Q2.4), not where Q6.3 pre-empts it, and not
                            // on a block that is neither stone, fluid nor
                            // air.
                            const bool nearestWaterHere =
                                nearestFluid && nearestType == aquifer::FluidType::Default;
                            const bool plainBlock = serverStone || serverLava || serverWater ||
                                                    b->name == "minecraft:air";
                            if (y >= lambda && !(y == lambda && nearestWaterHere) && plainBlock) {
                                if (hasMixedPair(at)) {
                                    total.mixedServerStone += serverStone;
                                    total.mixedOldMiss += (serverStone && !oldStone);
                                    total.mixedNewMiss += (serverStone && !newStone);
                                    total.mixedOldFalse += (!serverStone && oldStone);
                                    total.mixedNewFalse += (!serverStone && newStone);
                                } else {
                                    total.pureServerStone += serverStone;
                                    total.pureOldMiss += (serverStone && !oldStone);
                                    total.pureNewMiss += (serverStone && !newStone);
                                    total.pureFalse += (!serverStone && (oldStone || newStone));
                                }
                                if (newStone && !oldStone) {
                                    ++total.constantAdds;
                                    total.constantAddsServerStone += serverStone;
                                }
                                if (y > lambda) {
                                    const TypeFieldReading tf = typeFieldReading(at);
                                    if (tf.applies && tf.constantFires && !oldStone) {
                                        ++total.tfConstantOnly;
                                        total.tfConstantOnlyServerStone += serverStone;
                                    }
                                    if (tf.applies && !tf.constantFires && oldStone) {
                                        ++total.tfFormulaOnly;
                                        total.tfFormulaOnlyServerStone += serverStone;
                                    }
                                    if (!newStone && bothAirMixedWouldFire(at)) {
                                        ++total.bothAir;
                                        total.bothAirServerStone += serverStone;
                                    }
                                }
                            }

                            if (y < lambda) {
                                ++total.belowTotal;
                                total.belowNowLava += (now.substance == aquifer::Substance::Fluid &&
                                                       now.fluidType == aquifer::FluidType::Lava);
                                total.belowServerLava += serverLava;
                                if (serverWater) {
                                    ++total.belowServerWater;
                                    const auto* above = ch.blockAt(static_cast<int>(lx), y + 1,
                                                                   static_cast<int>(lz));
                                    const auto* below = ch.blockAt(static_cast<int>(lx), y - 1,
                                                                   static_cast<int>(lz));
                                    const bool fellIn = isFallingWater(b) && isWater(above) &&
                                                        below != nullptr &&
                                                        below->name == "minecraft:obsidian";
                                    total.belowServerWaterUnexplained += !fellIn;
                                }
                                total.belowBareWrongType +=
                                    (bare.substance != aquifer::Substance::Fluid ||
                                     bare.fluidType != aquifer::FluidType::Lava);
                            } else if (y == lambda) {
                                const bool nearestWater =
                                    nearestFluid && nearestType == aquifer::FluidType::Default;
                                if (nearestWater) {
                                    ++total.fires;
                                    total.firesBareStone += bareStone;
                                    total.firesServerStone += serverStone;
                                    total.firesNowFluid +=
                                        (now.substance == aquifer::Substance::Fluid);
                                } else if (!nearestFluid) {
                                    ++total.nearestAir;
                                    total.nearestAirServerStone += serverStone;
                                }
                            } else {
                                ++total.aboveTotal;
                                total.aboveDisagree += (now.substance != bare.substance ||
                                                        now.fluidType != bare.fluidType);
                            }
                        }
                    }
                }
            }
        }
    }
}

/// Every `waterlava_s*` probe present, scored ONCE for both test cases —
/// the walk reads four region files per seed and calls the whole substance
/// decision on every block of eight rows, so it is not repeated per case.
struct ScoredProbes {
    std::vector<std::filesystem::path> probes;
    Score total;
};

[[nodiscard]] const ScoredProbes& scoredProbes() {
    static const ScoredProbes scored = [] {
        ScoredProbes out;
        const std::filesystem::path probesRoot = fixtures() / "probes";
        if (std::filesystem::is_directory(probesRoot)) {
            for (const auto& entry : std::filesystem::directory_iterator(probesRoot)) {
                if (entry.is_directory() &&
                    entry.path().filename().string().rfind("waterlava_s", 0) == 0 &&
                    std::filesystem::is_regular_file(entry.path() / "manifest.json")) {
                    out.probes.push_back(entry.path());
                }
            }
        }
        std::ranges::sort(out.probes);
        for (const auto& probe : out.probes) {
            scoreProbe(probe, out.total);
        }
        return out;
    }();
    return scored;
}

} // namespace

TEST_CASE("water resting on the global lava sea is water, not a barrier",
          "[conformance][aquifer]") {
    const ScoredProbes& scored = scoredProbes();
    const std::vector<std::filesystem::path>& probes = scored.probes;
    if (probes.empty()) {
        SKIP("no waterlava_s* aquifer probe under " << fixtures() / "probes"
                                                    << "; generate one with "
                                                       "tools/analysis/aquifer-waterlava-probe.sh");
    }
    const Score& total = scored.total;

    INFO("probes " << probes.size() << ": fires " << total.fires << ", bare stone "
                   << total.firesBareStone << ", server stone " << total.firesServerStone
                   << "; nearest-air " << total.nearestAir << " with server stone "
                   << total.nearestAirServerStone << "; above " << total.aboveTotal << " disagree "
                   << total.aboveDisagree << "; below " << total.belowTotal << " now-lava "
                   << total.belowNowLava << " server-lava " << total.belowServerLava
                   << " server-water " << total.belowServerWater << " (unexplained "
                   << total.belowServerWaterUnexplained << "), bare wrong type "
                   << total.belowBareWrongType);

    // The exception's population, and the control that the case can tell
    // the two decisions apart at all: the bare fall-through must be writing
    // stone on some of it (measured 14 / 50 / 70 per seed at sea -70 with
    // the level formula alone; 19 / 176 / 178 now that a lava body against
    // the row's water takes Q6.4's constant — and the server still 0). At
    // the shipped sea the population is empty (measured 0 on three seeds),
    // so a single low-sea arm is what makes this non-vacuous.
    REQUIRE(total.fires >= 200);
    REQUIRE(total.firesBareStone >= 10);

    // Q6.3: the server never writes stone there, and this build no longer
    // does either.
    CHECK(total.firesServerStone == 0);
    CHECK(total.firesNowFluid == total.fires);

    // The asymmetry: a nearest source reading AIR on the same row still
    // gets barriers from the server (measured 93 / 239 / 244).
    CHECK(total.nearestAirServerStone > 0);

    // One row up and beyond, the exception is inert: the two decisions are
    // the same function.
    CHECK(total.aboveDisagree == 0);

    // Q2.4: below the sea this build is lava on every block; the server is
    // lava (or the obsidian it became) on all but the water that fell into
    // it after generation — every such block is FALLING water, directly
    // under water, with the obsidian that lava became beneath it.
    CHECK(total.belowNowLava == total.belowTotal);
    CHECK(total.belowServerLava + total.belowServerWater == total.belowTotal);
    CHECK(total.belowServerWaterUnexplained == 0);
    CHECK(total.belowServerWater * 100 < total.belowTotal);
    // The control: the bare fall-through was wrong about the type on a large
    // share of those blocks (measured 33-67% at sea 63).
    CHECK(total.belowBareWrongType * 10 > total.belowTotal);
}

TEST_CASE("a lava body meeting a water body is walled off, and nothing else changes",
          "[conformance][aquifer]") {
    const ScoredProbes& scored = scoredProbes();
    if (scored.probes.empty()) {
        SKIP("no waterlava_s* aquifer probe under " << fixtures() / "probes"
                                                    << "; generate one with "
                                                       "tools/analysis/aquifer-waterlava-probe.sh");
    }
    const Score& total = scored.total;

    INFO("probes " << scored.probes.size() << ": mixed-junction server stone "
                   << total.mixedServerStone << ", misses old " << total.mixedOldMiss << " new "
                   << total.mixedNewMiss << ", false old " << total.mixedOldFalse << " new "
                   << total.mixedNewFalse << "; pure server stone " << total.pureServerStone
                   << " misses old " << total.pureOldMiss << " new " << total.pureNewMiss
                   << " false " << total.pureFalse << "; the constant adds " << total.constantAdds
                   << " (server stone " << total.constantAddsServerStone
                   << "); type-field formula-only " << total.tfFormulaOnly << " (stone "
                   << total.tfFormulaOnlyServerStone << ") constant-only " << total.tfConstantOnly
                   << " (stone " << total.tfConstantOnlyServerStone << "); both-air "
                   << total.bothAir << " (stone " << total.bothAirServerStone << ")");

    // The control: the corpus has to hold lava bodies meeting water bodies
    // at a separation the constant carries, or nothing below can tell the
    // two predicates apart. Measured 18 / 430 / 665 per seed at sea -70 —
    // one seed alone (42) is too few; this REQUIREs the pooled set.
    REQUIRE(total.constantAdds >= 100);
    REQUIRE(total.mixedOldMiss >= 500);

    // Every block the constant adds is a real barrier: 100% server stone.
    CHECK(total.constantAddsServerStone == total.constantAdds);
    // Neither predicate writes stone the server does not, anywhere on the
    // rows the lattice owns (measured 0 / 0 on three seeds).
    CHECK(total.mixedOldFalse == 0);
    CHECK(total.mixedNewFalse == 0);
    CHECK(total.pureFalse == 0);
    // The branch cuts the real barriers missed in mixed junctions by more
    // than half (measured 1698 -> 590 pooled over all rows and seeds; 1240
    // -> 330 on the rows above the sea alone) ...
    CHECK(total.mixedNewMiss * 2 < total.mixedOldMiss);
    // ... and touches nothing where no pair is mixed.
    CHECK(total.pureNewMiss == total.pureOldMiss);

    // The type-field reading, refuted: where the formula fires and the
    // constant would not, every block is a real barrier (252 of 252); where
    // the constant alone would fire, none is (0 of 33).
    REQUIRE(total.tfFormulaOnly >= 50);
    CHECK(total.tfFormulaOnlyServerStone == total.tfFormulaOnly);
    CHECK(total.tfConstantOnlyServerStone * 10 <= total.tfConstantOnly);

    // The types-regardless reading, refuted: stone between two drained
    // cells of different type on 0.0-0.5% of the blocks it would fill
    // (0-4 of 424-1145 per row above the sea).
    REQUIRE(total.bothAir >= 1000);
    CHECK(total.bothAirServerStone * 50 < total.bothAir);
}
