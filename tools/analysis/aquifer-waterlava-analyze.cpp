// Stratum — reads back tools/analysis/aquifer-waterlava-probe.sh's worlds.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Walks the rows at and around the global lava sea's top —
// `lambda = min(-54, sea_level)` — and, for every block on them, computes
// two answers from the SAME inputs: this build's own `computeSubstance`
// (substance.hpp, end to end, the function the filler calls), and the BARE
// fall-through the substance decision used to be — `placesBarrier`, then the
// nearest source's own reading — with no Q6.3 and no Q2.4 in front of it.
// Where the two differ is exactly the set of blocks the two spec clauses
// change, and the server's own block says which one is right.
//
// Real `barrier`/`fluid_level_floodedness`/`fluid_level_spread` noise, read
// through this build's own density::Interpreter as the barrier analyzer
// does — never a hand-rolled replica. `preliminary_surface_level` is the
// probe's constant 96 and `lava` its constant 0.0; both, with each
// dimension's density, sea level and floor, are read off the probe's own
// spec.json rather than assumed.
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

struct RowTally {
    long long total = 0;
    long long other = 0;
    long long observedStone = 0;
    long long bareStone = 0;
    long long nowStone = 0;
    long long bareCategoryMatch = 0;
    long long nowCategoryMatch = 0;
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
        aquifer::LevelCache levelCache;
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
                                aquifer::computeSubstance(centres, query, levelCache, barrierAt,
                                                          floodednessAt, spreadAt, lavaAt, pslAt);

                            // THE BARE FALL-THROUGH: rank, level, barrier,
                            // nearest reading — the same pieces, no Q6.3 and
                            // no Q2.4 in front of them.
                            const aquifer::Selection selection =
                                aquifer::selectSources(centres, x, y, z);
                            std::array<std::int32_t, 3> level{};
                            for (std::size_t r = 0; r < 3; ++r) {
                                const auto& src = selection.ranked[r];
                                const aquifer::SamplePos fp =
                                    aquifer::floodednessSample(src.centre);
                                const aquifer::SamplePos sp =
                                    aquifer::spreadSample(src.cell, src.centre);
                                const aquifer::CellFluid cell{.centreY = src.centre.y,
                                                              .surface = surface,
                                                              .seaLevel = dim.seaLevel,
                                                              .floodedness =
                                                                  floodednessAt(fp.x, fp.y, fp.z),
                                                              .spread = spreadAt(sp.x, sp.y, sp.z)};
                                level[r] = aquifer::cellFluidLevel(cell);
                            }
                            aquifer::BarrierAt at;
                            at.y = y;
                            at.density = dim.density;
                            at.nearest = aquifer::BarrierSource{
                                .level = level[0], .distanceSq = selection.ranked[0].distanceSq};
                            at.second = aquifer::BarrierSource{
                                .level = level[1], .distanceSq = selection.ranked[1].distanceSq};
                            at.third = aquifer::BarrierSource{
                                .level = level[2], .distanceSq = selection.ranked[2].distanceSq};
                            at.barrier = barrierAt(x, y, z);
                            const bool nearestFluid = y < level[0];
                            const aquifer::CellIndex& nc = selection.ranked[0].centre;
                            const aquifer::SamplePos lp = aquifer::lavaSample(nc);
                            const aquifer::FluidType nearestType = aquifer::fluidTypeOf(
                                aquifer::FluidTypeAt{.centreY = nc.y,
                                                     .level = level[0],
                                                     .seaLevel = dim.seaLevel,
                                                     .lava = lavaAt(lp.x, lp.y, lp.z)});
                            aquifer::SubstanceAt bare;
                            if (aquifer::placesBarrier(at)) {
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
