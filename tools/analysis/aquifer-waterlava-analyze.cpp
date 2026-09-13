// Stratum — reads back tools/analysis/aquifer-waterlava-probe.sh's worlds.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Walks the rows at and around the global lava sea's top —
// `lambda = min(-54, sea_level)` — and, for every block on them, computes
// three answers from the SAME inputs: this build's own `computeSubstance`
// (substance.hpp, end to end, the function the filler calls); the BARE
// fall-through the substance decision used to be — `placesBarrier`, then
// the nearest source's own reading — with no Q6.3 and no Q2.4 in front of
// it; and the bare fall-through with every source typed WATER, which is
// the predicate as it was before Q6.4's mixed-type branch landed. Where
// the first two differ is exactly the set of blocks Q6.3 and Q2.4 change;
// where the last two differ is exactly the set the mixed-type Π changes;
// and the server's own block says which one is right in each case.
//
// THE MIXED-TYPE BRANCH is scored on the rows just above the sea, the one
// place in this world where lava-typed sources (centred below `lambda`)
// compete with water-typed ones: per row, the server's real barriers the
// old same-type Π misses against the ones the committed Π misses, and the
// stone each writes that the server does not. "One reads lava and the
// other water" was ambiguous between comparing the two statuses' TYPE
// fields and comparing what each READS at `y`; the committed predicate
// takes the second — a pair that BOTH read fluid, of different fluids,
// takes the constant, and a pair that disagrees at `y` takes the level
// formula whatever its types. The two refuted readings are still counted
// so the refutation stays reproducible: where the nearest pair is mixed
// and disagrees at `y`, a 2x2 of "the constant would fire" against "the
// formula fires" with the server's stone in each cell (the type-field
// reading), and where a mixed pair both read AIR with `D + w * 2 > 0`,
// the server's stone (the types-regardless reading).
//
// THE RESIDUAL — real barriers no type reading touches — is tested here
// against the LEVEL representation rather than left as noise. This build
// reports a dry source as `level = lambda` (the spec's `never` is -32512)
// and clamps a ladder that falls below `lambda` up to it; on rows 0-3
// above `lambda` that puts the lower plane right under the block, which
// is exactly where Π's `h <= 0` branch parts from its `h > 0` one. A
// second `BarrierAt` per block carries the spec's own levels for the
// same sources (this world's constant psl makes them a closed form:
// floodedness past 0.8 is the sea — or -54 below lambda — past 0.4 the
// unclamped ladder, else `never`) and is scored the same way.
//
// Real `barrier`/`fluid_level_floodedness`/`fluid_level_spread` noise, read
// through this build's own density::Interpreter as the barrier analyzer
// does — never a hand-rolled replica. `preliminary_surface_level` is the
// probe's constant 96 and `lava` its constant 0.0; both, with each
// dimension's density, sea level and floor, are read off the probe's own
// spec.json rather than assumed. (With `lava` a constant 0.0 the only
// lava-typed sources are those centred below `lambda` — which is exactly
// what puts them on the rows just above the sea, and nowhere else.)
//
//   g++ -std=c++20 -O2 -I lib/include -I build/dev/lib/generated \
//       -I build/dev/_deps/nlohmann_json-src/single_include \
//       tools/analysis/aquifer-waterlava-analyze.cpp -L build/dev/lib -lstratum_core -lz \
//       -o build/aquifer-waterlava-analyze
//   build/aquifer-waterlava-analyze .fixtures/1.21.11/probes/waterlava_s42
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

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace stratum;

namespace {

struct Dimension {
    std::string name;
    double density = 0.0;
    std::int32_t seaLevel = 0;
    std::int32_t minY = 0;
    std::int32_t height = 0;
    double psl = 0.0;
    double lava = 0.0;
};

/// What the server left at a block, reduced to what the aquifer decided.
enum class Observed { Stone, Water, Lava, Air, Other };

Observed classify(const std::string& name) {
    if (name == "minecraft:stone") {
        return Observed::Stone;
    }
    if (name == "minecraft:water") {
        return Observed::Water;
    }
    // Obsidian is lava that touched water after generation (the block BELOW
    // a water block resting on the sea); it was lava when the aquifer placed
    // it.
    if (name == "minecraft:lava" || name == "minecraft:obsidian") {
        return Observed::Lava;
    }
    if (name == "minecraft:air") {
        return Observed::Air;
    }
    return Observed::Other;
}

bool isStone(aquifer::SubstanceAt s) {
    return s.substance == aquifer::Substance::Solid;
}

bool categoryMatches(Observed o, aquifer::SubstanceAt s) {
    switch (s.substance) {
        case aquifer::Substance::Solid:
            return o == Observed::Stone;
        case aquifer::Substance::Fluid:
            return o == Observed::Water || o == Observed::Lava;
        case aquifer::Substance::Air:
            return o == Observed::Air;
    }
    return false;
}

bool typeMatches(Observed o, aquifer::SubstanceAt s) {
    return (o == Observed::Water && s.fluidType == aquifer::FluidType::Default) ||
           (o == Observed::Lava && s.fluidType == aquifer::FluidType::Lava);
}

/// Q6.1's similarity, on squared distances — the same one-liner
/// `placesBarrier` uses, needed here only to weigh the ALTERNATIVE
/// mixed-type readings the committed predicate does not implement.
double similarity(const std::int64_t di, const std::int64_t dj) {
    return 1.0 -
           static_cast<double>(dj - di) / static_cast<double>(aquifer::kSimilarityRange);
}

/// The types-regardless reading of Q6.4's first clause: would a mixed-type
/// pair that BOTH read air at `y` carry the constant past `D` on its
/// weight alone? (The committed predicate never consults Π for such a
/// pair.)
bool bothAirMixedWouldFire(const aquifer::BarrierAt& at) {
    const double s12 = similarity(at.nearest.distanceSq, at.second.distanceSq);
    if (s12 <= 0.0) {
        return false; // Q6.2: no barrier evaluation at all.
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

/// The type-field reading of Q6.4's first clause, on the single-weighted
/// nearest pair: mixed-type, disagreeing at `y`, competing (`s12 > 0`) —
/// and, for such a pair, whether the constant alone would carry it.
struct NearestPairMixedDisagreeing {
    bool applies = false;
    bool constantFires = false;
};

NearestPairMixedDisagreeing nearestPairTypeField(const aquifer::BarrierAt& at) {
    const double s12 = similarity(at.nearest.distanceSq, at.second.distanceSq);
    const bool aFluid = at.y < at.nearest.level;
    const bool bFluid = at.y < at.second.level;
    if (s12 <= 0.0 || at.nearest.type == at.second.type || aFluid == bFluid) {
        return {};
    }
    return {.applies = true,
            .constantFires = at.density + (s12 * aquifer::kMixedTypePressure) > 0.0};
}

/// The clean-room spec's own level for one source in THIS world (constant
/// psl, so Q5.3's short-circuits never fire and `S_min` is the constant):
/// past the sea gate `Global(Q).level` — -54 below lambda, the sea at or
/// above it — past the local gate the ladder with no clamp at lambda, and
/// otherwise the sentinel `never`.
constexpr std::int32_t kNever = -32512;

std::int32_t specLevelOf(const aquifer::CellIndex& centre, const double floodedness,
                         const double spread, const std::int32_t seaLevel,
                         const std::int32_t psl) {
    const std::int32_t lambda = aquifer::lambdaLevel(seaLevel);
    if (floodedness > aquifer::kFloodedSeaThreshold) {
        return centre.y < lambda ? aquifer::kLavaLevel : seaLevel;
    }
    if (floodedness > aquifer::kFloodedLocalThreshold) {
        const std::int32_t onLattice =
            (aquifer::kBasePitch * aquifer::levelBand(centre.y)) + aquifer::kBasePhase;
        return std::min(psl, onLattice + aquifer::spreadOffset(spread));
    }
    return kNever;
}

/// Whether any competing pair (weight > 0) on this block is mixed-type —
/// the junctions the branch can touch at all.
bool hasMixedPair(const aquifer::BarrierAt& at) {
    const double s12 = similarity(at.nearest.distanceSq, at.second.distanceSq);
    if (s12 <= 0.0) {
        return false;
    }
    const double s13 = similarity(at.nearest.distanceSq, at.third.distanceSq);
    const double s23 = similarity(at.second.distanceSq, at.third.distanceSq);
    return at.nearest.type != at.second.type ||
           (s13 > 0.0 && at.nearest.type != at.third.type) ||
           (s23 > 0.0 && at.second.type != at.third.type);
}

struct RowTally {
    long long total = 0;
    long long other = 0;
    long long observedStone = 0;
    long long bareStone = 0;
    long long nowStone = 0;
    long long bareCategoryMatch = 0;
    long long nowCategoryMatch = 0;
    // Q6.4's mixed-type branch: the server's real barriers against the
    // predicate with every source typed water (OLD, the same-type formula
    // alone) and against the committed typed predicate (NEW), on blocks
    // where at least one competing pair is mixed-type and on the rest;
    // then both again with the spec's own LEVELS (SPEC-LEVEL) in place of
    // this build's.
    long long mixedBlocks = 0;      // some competing pair is mixed-type
    long long mixedServerStone = 0; // ... and the server wrote stone
    long long mixedOldMiss = 0;     // server stone, old says no
    long long mixedNewMiss = 0;     // server stone, new says no
    long long mixedOldFalse = 0;    // old says stone, server no
    long long mixedNewFalse = 0;    // new says stone, server no
    long long mixedSpecOldMiss = 0; // the same four, at the spec's levels
    long long mixedSpecNewMiss = 0;
    long long mixedSpecOldFalse = 0;
    long long mixedSpecNewFalse = 0;
    long long pureServerStone = 0; // no mixed pair: server stone ...
    long long pureNewMiss = 0;     // ... the predicate misses (old == new here)
    long long pureNewFalse = 0;
    long long pureSpecNewMiss = 0;
    long long pureSpecNewFalse = 0;
    // The refuted type-field reading, as a 2x2 on the nearest pair where
    // it is mixed and disagrees at y: the constant's verdict against the
    // formula's, with the server's stone in each cell.
    long long tfBlocks = 0;
    long long tfConstantOnly = 0; // the constant would fire, the formula does not
    long long tfConstantOnlyServerStone = 0;
    long long tfFormulaOnly = 0; // the formula fires, the constant would not
    long long tfFormulaOnlyServerStone = 0;
    // The refuted types-regardless reading: a mixed pair both reading AIR
    // that the constant alone would carry, where the committed predicate
    // says no stone.
    long long altBothAir = 0;
    long long altBothAirServerStone = 0;
    // Fluid TYPE, where the server says fluid and the build agrees.
    long long bareBothFluid = 0;
    long long bareTypeMatch = 0;
    long long nowBothFluid = 0;
    long long nowTypeMatch = 0;
    // Q6.3's own population: the nearest source reads WATER here and the
    // global picker reads lava one block down.
    long long fires = 0;
    long long firesBareStone = 0;     // the bare logic would write stone
    long long firesObservedStone = 0; // the server actually did
    long long firesRescued = 0;       // bare said stone, server did not
    long long firesOther = 0;         // a block that is neither stone nor a fluid nor air
    // The asymmetric control: the nearest source reads AIR on the row. Q6.3
    // says nothing here, so the barrier should fire exactly as it does on
    // any other row.
    long long nearestAir = 0;
    long long nearestAirBareStone = 0;
    long long nearestAirObservedStone = 0;
    long long nearestAirCategoryMatch = 0;
    // The nearest source is LAVA-typed and reads fluid on the row: the
    // mixed-type Pi branch, deliberately out of scope, counted so that it is
    // visibly separate from the water case.
    long long nearestLava = 0;
    long long nearestLavaObservedStone = 0;
    // The server's water on the row, by its `level` property: 0 is a source
    // block the generator placed, anything else is flow that happened
    // AFTER generation (falling from a body above, spreading over the sea)
    // and says nothing about the aquifer's own decision.
    std::map<std::string, long long> waterLevels;
    // Water BELOW the sea's top, where Q2.4 says lava. Measured: every one
    // is falling water (`level` 8) sitting directly under water — an
    // aquifer-placed source on 211 of 223, a flow front on the other 12 —
    // with obsidian beneath: water that dropped into the lava under it
    // before that lava could react and turn to obsidian. Post-generation
    // mechanics, not the aquifer: `belowSeaWaterUnexplained` counts the
    // ones that pattern does NOT explain, and it reads zero.
    long long belowSeaWater = 0;
    long long belowSeaWaterUnexplained = 0;
};

void report(const std::string& dim, std::int32_t y, std::int32_t lambda, const RowTally& t) {
    std::string levels;
    for (const auto& [lvl, n] : t.waterLevels) {
        levels += " level=" + lvl + ":" + std::to_string(n);
    }
    const auto pct = [](long long a, long long b) {
        return b ? 100.0 * static_cast<double>(a) / static_cast<double>(b) : 0.0;
    };
    std::printf("  %s y=%d (lambda%+d): n=%lld other=%lld  stone: server=%lld bare=%lld now=%lld"
                "  category: bare=%.4f%% now=%.4f%%  type: bare=%lld/%lld now=%lld/%lld\n",
                dim.c_str(), y, y - lambda, t.total, t.other, t.observedStone, t.bareStone,
                t.nowStone, pct(t.bareCategoryMatch, t.total), pct(t.nowCategoryMatch, t.total),
                t.bareTypeMatch, t.bareBothFluid, t.nowTypeMatch, t.nowBothFluid);
    std::printf(
        "      Q6.3 fires=%lld  bare-stone=%lld  server-stone=%lld  rescued=%lld  other=%lld"
        "  | nearest-air=%lld bare-stone=%lld server-stone=%lld match=%lld"
        "  | nearest-lava=%lld server-stone=%lld\n",
        t.fires, t.firesBareStone, t.firesObservedStone, t.firesRescued, t.firesOther, t.nearestAir,
        t.nearestAirBareStone, t.nearestAirObservedStone, t.nearestAirCategoryMatch, t.nearestLava,
        t.nearestLavaObservedStone);
    std::printf("      server water by level:%s", levels.empty() ? " (none)" : levels.c_str());
    if (t.belowSeaWater > 0) {
        std::printf("  | water below the sea: %lld, unexplained: %lld", t.belowSeaWater,
                    t.belowSeaWaterUnexplained);
    }
    std::printf("\n");
    if (t.mixedBlocks > 0 || t.observedStone > 0) {
        std::printf("      Q6.4 mixed-pair blocks=%lld server-stone=%lld  real-barrier misses: "
                    "old=%lld (%.1f%%) new=%lld (%.1f%%)  false stone: old=%lld new=%lld"
                    "  | pure: server-stone=%lld misses=%lld (%.1f%%) false=%lld\n",
                    t.mixedBlocks, t.mixedServerStone, t.mixedOldMiss,
                    pct(t.mixedOldMiss, t.mixedServerStone), t.mixedNewMiss,
                    pct(t.mixedNewMiss, t.mixedServerStone), t.mixedOldFalse, t.mixedNewFalse,
                    t.pureServerStone, t.pureNewMiss, pct(t.pureNewMiss, t.pureServerStone),
                    t.pureNewFalse);
        std::printf("      Q6.4 at the SPEC's levels (never for dry, ladder unclamped): mixed "
                    "misses old=%lld new=%lld (%.1f%%) false old=%lld new=%lld  | pure "
                    "misses=%lld (%.1f%%) false=%lld\n",
                    t.mixedSpecOldMiss, t.mixedSpecNewMiss,
                    pct(t.mixedSpecNewMiss, t.mixedServerStone), t.mixedSpecOldFalse,
                    t.mixedSpecNewFalse, t.pureSpecNewMiss,
                    pct(t.pureSpecNewMiss, t.pureServerStone), t.pureSpecNewFalse);
        std::printf("      Q6.4 refuted readings: type-field (nearest pair mixed, disagreeing) "
                    "n=%lld constant-only=%lld server-stone=%lld formula-only=%lld "
                    "server-stone=%lld  | types-regardless (both air, D + w*2 > 0) n=%lld "
                    "server-stone=%lld\n",
                    t.tfBlocks, t.tfConstantOnly, t.tfConstantOnlyServerStone, t.tfFormulaOnly,
                    t.tfFormulaOnlyServerStone, t.altBothAir, t.altBothAirServerStone);
    }
}

std::vector<Dimension> readSpec(const std::filesystem::path& specPath) {
    std::ifstream in(specPath);
    const nlohmann::json spec = nlohmann::json::parse(in);
    std::vector<Dimension> dims;
    for (const auto& entry : spec) {
        Dimension d;
        d.name = entry.at("name").get<std::string>();
        d.density = entry.at("raw_final_density").at("argument").get<double>();
        d.seaLevel = entry.at("sea_level").get<std::int32_t>();
        d.minY = entry.at("min_y").get<std::int32_t>();
        d.height = entry.at("height").get<std::int32_t>();
        d.psl = entry.at("router").at("preliminary_surface_level").get<double>();
        d.lava = entry.at("router").at("lava").get<double>();
        dims.push_back(d);
    }
    return dims;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: aquifer-waterlava-analyze <probe-dir>\n");
        return 2;
    }
    const std::filesystem::path root = argv[1];
    std::ifstream manifestFile(root / "manifest.json");
    const nlohmann::json manifest = nlohmann::json::parse(manifestFile);
    const std::int64_t seed = manifest.at("seed").get<std::int64_t>();
    const std::vector<Dimension> dims = readSpec(root / "spec.json");

    const char* fixturesEnv = std::getenv("STRATUM_FIXTURES_DIR");
    const std::filesystem::path fixturesDir =
        (fixturesEnv ? std::filesystem::path(fixturesEnv) : std::filesystem::path(".fixtures")) /
        "1.21.11";
    const auto pack = data::Pack::open(fixturesDir / "worldgen");

    density::Graph::Builder builder(pack);
    const nlohmann::json barrierJson = {{"type", "minecraft:noise"},
                                        {"noise", "minecraft:aquifer_barrier"},
                                        {"xz_scale", 1.0},
                                        {"y_scale", 0.5}};
    const nlohmann::json floodJson = {{"type", "minecraft:noise"},
                                      {"noise", "minecraft:aquifer_fluid_level_floodedness"},
                                      {"xz_scale", 1.0},
                                      {"y_scale", 0.67}};
    const nlohmann::json spreadJson = {{"type", "minecraft:noise"},
                                       {"noise", "minecraft:aquifer_fluid_level_spread"},
                                       {"xz_scale", 1.0},
                                       {"y_scale", 0.7142857142857143}};
    const density::NodeIndex barrierNode = builder.add(barrierJson);
    const density::NodeIndex floodNode = builder.add(floodJson);
    const density::NodeIndex spreadNode = builder.add(spreadJson);
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

    std::printf("seed %lld\n", static_cast<long long>(seed));
    std::map<std::string, long long> otherNames;

    for (const Dimension& dim : dims) {
        const std::filesystem::path regionPath = root / dim.name / "r.0.0.mca";
        if (!std::filesystem::is_regular_file(regionPath)) {
            std::printf("=== %s: no region, skipped\n", dim.name.c_str());
            continue;
        }
        const std::int32_t lambda = aquifer::lambdaLevel(dim.seaLevel);
        const auto pslAt = [&](std::int32_t, std::int32_t, std::int32_t) { return dim.psl; };
        const auto lavaAt = [&](std::int32_t, std::int32_t, std::int32_t) { return dim.lava; };
        const aquifer::PslRead surface =
            aquifer::constantSurface(static_cast<std::int32_t>(std::floor(dim.psl)));

        // The rows: one below the sea's top (Q2.4's territory), the top
        // itself (Q6.3's), three above (a control where the two clauses must
        // be inert) — and -55..-53 on every arm, so the low-sea arm shows
        // -54 as an ordinary row.
        std::set<std::int32_t> rows{lambda - 1, lambda, lambda + 1, lambda + 2,
                                    lambda + 3, -55,    -54,        -53};
        std::map<std::int32_t, RowTally> tallies;

        std::printf("=== %s (D=%.2f, sea_level=%d, lambda=%d, min_y=%d) ===\n", dim.name.c_str(),
                    dim.density, dim.seaLevel, lambda, dim.minY);
        aquifer::StatusCache statusCache;
        int dumped = 0;
        const auto file = region::RegionFile::open(regionPath);
        for (std::int32_t cz = 0; cz < 8; ++cz) {
            for (std::int32_t cx = 0; cx < 8; ++cx) {
                if (!file.hasChunk(cx, cz)) {
                    continue;
                }
                const auto ch = chunk::Chunk::decode(nbt::read(file.readChunk(cx, cz)).root);
                for (int lz = 0; lz < 16; ++lz) {
                    for (int lx = 0; lx < 16; ++lx) {
                        const std::int32_t x = (cx * 16) + lx;
                        const std::int32_t z = (cz * 16) + lz;
                        for (const std::int32_t y : rows) {
                            if (y < dim.minY || y >= dim.minY + dim.height) {
                                continue;
                            }
                            const auto* b = ch.blockAt(lx, y, lz);
                            const Observed observed = b ? classify(b->name) : Observed::Other;
                            RowTally& t = tallies[y];
                            ++t.total;
                            if (observed == Observed::Other) {
                                ++t.other;
                                ++otherNames[b ? b->name : "<missing>"];
                            }

                            // THIS BUILD, end to end.
                            const aquifer::AquiferQuery query{.x = x,
                                                              .y = y,
                                                              .z = z,
                                                              .density = dim.density,
                                                              .seaLevel = dim.seaLevel};
                            const aquifer::SubstanceAt now =
                                aquifer::computeSubstance(centres, query, statusCache, barrierAt,
                                                          floodednessAt, spreadAt, lavaAt, pslAt);

                            // THE BARE FALL-THROUGH: rank, status, barrier,
                            // nearest reading — the same pieces, no Q6.3 and
                            // no Q2.4 in front of them.
                            const aquifer::Selection selection =
                                aquifer::selectSources(centres, x, y, z);
                            std::array<std::int32_t, 3> level{};
                            std::array<std::int32_t, 3> specLevel{};
                            std::array<aquifer::FluidType, 3> type{};
                            for (std::size_t r = 0; r < 3; ++r) {
                                const auto& src = selection.ranked[r];
                                const aquifer::SamplePos fp =
                                    aquifer::floodednessSample(src.centre);
                                const aquifer::SamplePos sp =
                                    aquifer::spreadSample(src.cell, src.centre);
                                const double f = floodednessAt(fp.x, fp.y, fp.z);
                                const double s = spreadAt(sp.x, sp.y, sp.z);
                                const aquifer::CellFluid cell{.centreY = src.centre.y,
                                                              .surface = surface,
                                                              .seaLevel = dim.seaLevel,
                                                              .floodedness = f,
                                                              .spread = s};
                                level[r] = aquifer::cellFluidLevel(cell);
                                specLevel[r] =
                                    specLevelOf(src.centre, f, s, dim.seaLevel,
                                                static_cast<std::int32_t>(std::floor(dim.psl)));
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
                            // The predicate as it was before Q6.4's first
                            // clause: every source water-typed, so Π is the
                            // level formula for every pair and no agreeing
                            // pair ever fires.
                            const auto allWater = [](aquifer::BarrierAt copy) {
                                copy.nearest.type = aquifer::FluidType::Default;
                                copy.second.type = aquifer::FluidType::Default;
                                copy.third.type = aquifer::FluidType::Default;
                                return copy;
                            };
                            const bool oldStone = aquifer::placesBarrier(allWater(at));
                            const bool typedStone = aquifer::placesBarrier(at);
                            // The same two, at the spec's own levels.
                            aquifer::BarrierAt specAt = at;
                            specAt.nearest.level = specLevel[0];
                            specAt.second.level = specLevel[1];
                            specAt.third.level = specLevel[2];
                            const bool specOldStone = aquifer::placesBarrier(allWater(specAt));
                            const bool specNewStone = aquifer::placesBarrier(specAt);
                            const bool nearestFluid = y < level[0];
                            const aquifer::FluidType nearestType = type[0];
                            aquifer::SubstanceAt bare;
                            if (typedStone) {
                                bare = aquifer::SubstanceAt{.substance = aquifer::Substance::Solid};
                            } else if (nearestFluid) {
                                bare = aquifer::SubstanceAt{.substance = aquifer::Substance::Fluid,
                                                            .fluidType = nearestType};
                            } else {
                                bare = aquifer::SubstanceAt{.substance = aquifer::Substance::Air};
                            }

                            if (observed == Observed::Water) {
                                std::string lvl = "?";
                                for (const auto& [k, v] : b->properties) {
                                    if (k == "level") {
                                        lvl = v;
                                    }
                                }
                                ++t.waterLevels[lvl];
                            }
                            // Water strictly below the sea's top is against
                            // Q2.4 as written: dump the first few, with the
                            // column around them, rather than only count.
                            bool dumpThis = false;
                            if (observed == Observed::Water && y < lambda) {
                                ++t.belowSeaWater;
                                const auto* above = ch.blockAt(lx, y + 1, lz);
                                const auto* below = ch.blockAt(lx, y - 1, lz);
                                bool falling = false;
                                for (const auto& [k, v] : b->properties) {
                                    falling = falling || (k == "level" && v == "8");
                                }
                                const bool fellIn = falling && above != nullptr &&
                                                    above->name == "minecraft:water" &&
                                                    below != nullptr &&
                                                    below->name == "minecraft:obsidian";
                                t.belowSeaWaterUnexplained += !fellIn;
                                dumpThis = !fellIn;
                            }
                            if (dumpThis && dumped < 16) {
                                ++dumped;
                                std::string column;
                                for (std::int32_t yy = y - 2; yy <= y + 4; ++yy) {
                                    const auto* bb = ch.blockAt(lx, yy, lz);
                                    std::string nm = bb ? bb->name : "<none>";
                                    if (bb) {
                                        for (const auto& [k, v] : bb->properties) {
                                            nm += "[" + k + "=" + v + "]";
                                        }
                                    }
                                    column += " " + std::to_string(yy) + ":" + nm;
                                }
                                std::string model;
                                for (std::int32_t yy = y; yy <= y + 1; ++yy) {
                                    const aquifer::Selection sel =
                                        aquifer::selectSources(centres, x, yy, z);
                                    for (std::size_t r = 0; r < 2; ++r) {
                                        const auto& src = sel.ranked[r];
                                        const aquifer::SamplePos fp =
                                            aquifer::floodednessSample(src.centre);
                                        const aquifer::SamplePos sp =
                                            aquifer::spreadSample(src.cell, src.centre);
                                        const aquifer::CellFluid cell{
                                            .centreY = src.centre.y,
                                            .surface = surface,
                                            .seaLevel = dim.seaLevel,
                                            .floodedness = floodednessAt(fp.x, fp.y, fp.z),
                                            .spread = spreadAt(sp.x, sp.y, sp.z)};
                                        model += " y" + std::to_string(yy) + "r" +
                                                 std::to_string(r) + "=c(" +
                                                 std::to_string(src.centre.x) + "," +
                                                 std::to_string(src.centre.y) + "," +
                                                 std::to_string(src.centre.z) + ")L" +
                                                 std::to_string(aquifer::cellFluidLevel(cell)) +
                                                 "d" + std::to_string(src.distanceSq) + "f" +
                                                 std::to_string(cell.floodedness).substr(0, 5);
                                    }
                                }
                                std::printf("      ANOMALY water below lambda at (%d,%d,%d):%s\n"
                                            "        model:%s\n",
                                            x, y, z, column.c_str(), model.c_str());
                            }
                            const bool obsStone = observed == Observed::Stone;
                            t.observedStone += obsStone;
                            t.bareStone += isStone(bare);
                            t.nowStone += isStone(now);
                            t.bareCategoryMatch += categoryMatches(observed, bare);
                            t.nowCategoryMatch += categoryMatches(observed, now);
                            const bool obsFluid =
                                observed == Observed::Water || observed == Observed::Lava;
                            if (obsFluid && bare.substance == aquifer::Substance::Fluid) {
                                ++t.bareBothFluid;
                                t.bareTypeMatch += typeMatches(observed, bare);
                            }
                            if (obsFluid && now.substance == aquifer::Substance::Fluid) {
                                ++t.nowBothFluid;
                                t.nowTypeMatch += typeMatches(observed, now);
                            }

                            const bool nearestWater =
                                nearestFluid && nearestType == aquifer::FluidType::Default;
                            const bool globalLavaBelow = (y - 1) < lambda;
                            if (nearestWater && globalLavaBelow) {
                                ++t.fires;
                                t.firesBareStone += isStone(bare);
                                t.firesObservedStone += obsStone;
                                t.firesRescued += (isStone(bare) && !obsStone);
                                t.firesOther += (observed == Observed::Other);
                            }
                            if (!nearestFluid) {
                                ++t.nearestAir;
                                t.nearestAirBareStone += isStone(bare);
                                t.nearestAirObservedStone += obsStone;
                                t.nearestAirCategoryMatch += categoryMatches(observed, bare);
                            }
                            if (nearestFluid && nearestType == aquifer::FluidType::Lava) {
                                ++t.nearestLava;
                                t.nearestLavaObservedStone += obsStone;
                            }

                            // Q6.4: scored on the rows the lattice owns
                            // (Q2.4 answers everything below lambda without
                            // a barrier), and off the one row Q6.3 pre-empts
                            // the predicate on — where the predicate's own
                            // answer is not what the server was asked.
                            const bool q63Row =
                                y == lambda && nearestWater; // the exception fires
                            if (y >= lambda && !q63Row && observed != Observed::Other) {
                                if (hasMixedPair(at)) {
                                    ++t.mixedBlocks;
                                    t.mixedServerStone += obsStone;
                                    t.mixedOldMiss += (obsStone && !oldStone);
                                    t.mixedNewMiss += (obsStone && !typedStone);
                                    t.mixedOldFalse += (!obsStone && oldStone);
                                    t.mixedNewFalse += (!obsStone && typedStone);
                                    t.mixedSpecOldMiss += (obsStone && !specOldStone);
                                    t.mixedSpecNewMiss += (obsStone && !specNewStone);
                                    t.mixedSpecOldFalse += (!obsStone && specOldStone);
                                    t.mixedSpecNewFalse += (!obsStone && specNewStone);
                                } else {
                                    t.pureServerStone += obsStone;
                                    t.pureNewMiss += (obsStone && !typedStone);
                                    t.pureNewFalse += (!obsStone && typedStone);
                                    t.pureSpecNewMiss += (obsStone && !specNewStone);
                                    t.pureSpecNewFalse += (!obsStone && specNewStone);
                                }
                                const NearestPairMixedDisagreeing tf = nearestPairTypeField(at);
                                if (tf.applies) {
                                    ++t.tfBlocks;
                                    if (tf.constantFires && !oldStone) {
                                        ++t.tfConstantOnly;
                                        t.tfConstantOnlyServerStone += obsStone;
                                    }
                                    if (!tf.constantFires && oldStone) {
                                        ++t.tfFormulaOnly;
                                        t.tfFormulaOnlyServerStone += obsStone;
                                    }
                                }
                                if (!typedStone && bothAirMixedWouldFire(at)) {
                                    ++t.altBothAir;
                                    t.altBothAirServerStone += obsStone;
                                }
                            }
                        }
                    }
                }
            }
        }
        for (const auto& [y, t] : tallies) {
            report(dim.name, y, lambda, t);
        }
    }
    if (!otherNames.empty()) {
        std::printf("\nblocks classified as 'other':\n");
        for (const auto& [name, n] : otherNames) {
            std::printf("  %-32s %lld\n", name.c_str(), n);
        }
    }
    return 0;
}
