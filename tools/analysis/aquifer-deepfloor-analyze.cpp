// Stratum — reads back tools/analysis/aquifer-deepfloor-probe.sh's worlds.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Scores two questions at once on every stone/water/air block, against the
// server's own answer:
//
//   1. THE AGREE-GUARD. `termFires` used to refuse any pair of sources that
//      read the same thing at the block. Four models are scored — the guard
//      as it stood, lifted for both-air pairs only, lifted for both-fluid
//      pairs only, and gone (Q6.4 as written) — so a win for the barrier's
//      LID cannot silently license its FLOOR or the other way round.
//
//   2. THE FOUR DIVISORS. Each of `/2.5` and `/10` is swept independently
//      over a ladder that brackets it from BOTH sides, which is what turns
//      "the spec's value fits" into "no other value does". A divisor too
//      small leaves server barriers unwritten (misses); one too large
//      writes stone the server does not have (false stone); only the right
//      one does neither. The `/10`-vs-`/3` head-to-head count is printed
//      separately, because a world where that count is zero cannot measure
//      the divisor at all however many blocks it holds — `barrier3way` is
//      exactly such a world, and this analyzer says so rather than leaving
//      it to be argued afterwards.
//
// Everything is read through this build's own `data::Pack`,
// `density::Interpreter`, `aquifer::selectSources`, `aquifer::cellFluidLevel`
// and `aquifer::fluidTypeOf` — not a parallel reimplementation — and the
// parameterised predicate below is asserted equal to the committed
// `aquifer::placesBarrier` on every block it scores (the mismatch counter
// must print 0).
//
//   g++ -std=c++20 -O2 -I lib/include -I build/dev/lib/generated \
//       -I build/dev/_deps/nlohmann_json-src/single_include \
//       tools/analysis/aquifer-deepfloor-analyze.cpp -L build/dev/lib \
//       -lstratum_core -lz -o build/aquifer-deepfloor-analyze
//   build/aquifer-deepfloor-analyze .fixtures/1.21.11/probes/aqdeep
//
// Nothing this reads is committed: the worlds are Mojang-derived (SPEC §12).
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
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using namespace stratum;

namespace {

/// Which pairs are allowed to reach `levelPressure` at all. `Ungated` is the
/// shipped reading and the spec's; the other three reconstruct the guard
/// this probe refuted, so the ablation stays runnable after the fact.
enum class Guard : std::uint8_t { Shipped, LiftAir, LiftFluid, Ungated };

/// Which of Q6.4's four arms a pair took.
enum class Arm : std::uint8_t { None, D15, D25, D3, D10 };

struct Params {
    Guard guard = Guard::Ungated;
    double d15 = 1.5;
    double d25 = 2.5;
    double d3 = 3.0;
    double d10 = 10.0;
};

double similarity(const std::int64_t di, const std::int64_t dj) {
    return 1.0 - static_cast<double>(dj - di) / static_cast<double>(aquifer::kSimilarityRange);
}

double levelPressure(const std::int32_t la, const std::int32_t lb, const std::int32_t y,
                     const double barrierNoise, const Params& p, Arm& arm) {
    const std::int32_t deltaInt = la < lb ? lb - la : la - lb;
    if (deltaInt == 0) {
        arm = Arm::None;
        return 0.0;
    }
    const auto delta = static_cast<double>(deltaInt);
    const double m = (static_cast<double>(la) + static_cast<double>(lb)) / 2.0;
    const double h = static_cast<double>(y) + 0.5 - m;
    const double r = delta / 2.0;
    const double t = r - std::abs(h);
    double u = std::numeric_limits<double>::quiet_NaN();
    if (h > 0) {
        if (t > 0) {
            u = t / p.d15;
            arm = Arm::D15;
        } else {
            u = t / p.d25;
            arm = Arm::D25;
        }
    } else {
        const double threeT = 3.0 + t;
        if (threeT > 0) {
            u = threeT / p.d3;
            arm = Arm::D3;
        } else {
            u = threeT / p.d10;
            arm = Arm::D10;
        }
    }
    const double noiseTerm = (std::abs(u) <= 2.0) ? barrierNoise : 0.0;
    return 2.0 * (noiseTerm + u);
}

bool termFires(const double density, const double weight, const aquifer::BarrierSource& a,
               const aquifer::BarrierSource& b, const std::int32_t y, const double barrierNoise,
               const Params& p, Arm& arm) {
    arm = Arm::None;
    const bool aFluid = y < a.level;
    const bool bFluid = y < b.level;
    if (aFluid && bFluid && a.type != b.type) {
        return (density + (weight * aquifer::kMixedTypePressure)) > 0.0;
    }
    if (aFluid == bFluid) {
        const bool bothAir = !aFluid;
        const bool allowed = (p.guard == Guard::Ungated) ||
                             (p.guard == Guard::LiftAir && bothAir) ||
                             (p.guard == Guard::LiftFluid && !bothAir);
        if (!allowed) {
            return false;
        }
    }
    return (density + (weight * levelPressure(a.level, b.level, y, barrierNoise, p, arm))) > 0.0;
}

/// How often each arm was ENTERED, which is not the same as how often it
/// decided anything — an arm can be entered by a pair whose term then fails.
struct ArmCensus {
    std::array<long long, 5> entry{};
};

bool placesBarrier(const aquifer::BarrierAt& at, const Params& p, ArmCensus* census,
                   Arm* decidingArm) {
    if (decidingArm != nullptr) {
        *decidingArm = Arm::None;
    }
    const auto note = [&](const Arm arm) {
        if (census != nullptr) {
            ++census->entry[static_cast<std::size_t>(arm)];
        }
    };
    const auto win = [&](const Arm arm) {
        note(arm);
        if (decidingArm != nullptr) {
            *decidingArm = arm;
        }
    };
    const double s12 = similarity(at.nearest.distanceSq, at.second.distanceSq);
    if (s12 <= 0.0) {
        return false;
    }
    Arm arm = Arm::None;
    if (termFires(at.density, s12, at.nearest, at.second, at.y, at.barrier, p, arm)) {
        win(arm);
        return true;
    }
    note(arm);
    const double s13 = similarity(at.nearest.distanceSq, at.third.distanceSq);
    if (s13 > 0.0) {
        if (termFires(at.density, s12 * s13, at.nearest, at.third, at.y, at.barrier, p, arm)) {
            win(arm);
            return true;
        }
        note(arm);
    }
    const double s23 = similarity(at.second.distanceSq, at.third.distanceSq);
    if (s23 > 0.0) {
        if (termFires(at.density, s12 * s23, at.second, at.third, at.y, at.barrier, p, arm)) {
            win(arm);
            return true;
        }
        note(arm);
    }
    return false;
}

struct Score {
    long long total = 0;
    long long serverStone = 0;
    /// Server stone this model leaves unwritten.
    long long miss = 0;
    /// Stone this model writes where the server has none.
    long long falseStone = 0;
};

void add(Score& s, const bool observedStone, const bool modelSolid) {
    ++s.total;
    if (observedStone) {
        ++s.serverStone;
        if (!modelSolid) {
            ++s.miss;
        }
    } else if (modelSolid) {
        ++s.falseStone;
    }
}

void accumulate(Score& into, const Score& from) {
    into.total += from.total;
    into.serverStone += from.serverStone;
    into.miss += from.miss;
    into.falseStone += from.falseStone;
}

struct Dim {
    std::string name;
    double density = 0.0;
    std::int32_t seaLevel = 63;
    std::int32_t minY = -48;
    std::int32_t height = 320;
    double psl = 96.0;
    double lava = 0.0;
    nlohmann::json barrier;
    nlohmann::json flood;
    nlohmann::json spread;
};

std::vector<Dim> readSpec(const std::filesystem::path& path) {
    std::ifstream in(path);
    const nlohmann::json spec = nlohmann::json::parse(in);
    std::vector<Dim> dims;
    for (const auto& entry : spec) {
        Dim d;
        d.name = entry.at("name").get<std::string>();
        d.density = entry.at("raw_final_density").at("argument").get<double>();
        d.seaLevel = entry.at("sea_level").get<std::int32_t>();
        d.minY = entry.at("min_y").get<std::int32_t>();
        d.height = entry.at("height").get<std::int32_t>();
        const auto& router = entry.at("router");
        d.psl = router.at("preliminary_surface_level").get<double>();
        // Every dimension this probe emits holds `lava` at a constant, on
        // purpose (the probe's own header says why). A noise here would
        // need the contracted `lavaSample` indices, so refuse rather than
        // quietly score the wrong thing — SPEC §8.
        if (!router.at("lava").is_number()) {
            std::fprintf(stderr, "dimension %s drives `lava` through a node; this analyzer "
                                 "reads it as a constant only\n",
                         d.name.c_str());
            std::exit(2);
        }
        d.lava = router.at("lava").get<double>();
        d.barrier = router.at("barrier");
        d.flood = router.at("fluid_level_floodedness");
        d.spread = router.at("fluid_level_spread");
        dims.push_back(d);
    }
    return dims;
}

/// The divisor ladders. Each brackets its own arm from both sides: the
/// entries below the measured value must miss, those above must write false
/// stone, and only the measured one may do neither.
const std::vector<double>& d10Ladder() {
    static const std::vector<double> ladder{1.5, 2.5, 3.0,  5.0,  8.0,  9.0,  9.5,
                                            9.9, 10.0, 10.1, 10.5, 11.0, 12.0, 20.0};
    return ladder;
}

const std::vector<double>& d25Ladder() {
    static const std::vector<double> ladder{1.5, 2.0, 2.4, 2.5, 2.6, 3.0, 10.0};
    return ladder;
}

struct Totals {
    Score shipped;
    Score liftAir;
    Score liftFluid;
    Score ungated;
    std::vector<Score> d10{d10Ladder().size()};
    std::vector<Score> d25{d25Ladder().size()};
    ArmCensus census;
    long long decidedByTen = 0;
    long long tenVersusThreeFlips = 0;
    long long tenVersusThreeServerStone = 0;
    long long committedMismatches = 0;
};

void report(const char* label, const Totals& t) {
    std::printf("%s blocks=%lld serverStone=%lld\n", label, t.shipped.total,
                t.shipped.serverStone);
    std::printf("    guarded (as it was)   miss=%-8lld falseStone=%-8lld\n", t.shipped.miss,
                t.shipped.falseStone);
    std::printf("    lift both-air only    miss=%-8lld falseStone=%-8lld\n", t.liftAir.miss,
                t.liftAir.falseStone);
    std::printf("    lift both-fluid only  miss=%-8lld falseStone=%-8lld\n", t.liftFluid.miss,
                t.liftFluid.falseStone);
    std::printf("    un-gated (shipped)    miss=%-8lld falseStone=%-8lld\n", t.ungated.miss,
                t.ungated.falseStone);
    std::printf("    arm entries: /1.5=%lld /2.5=%lld /3=%lld /10=%lld   decided by /10=%lld\n",
                t.census.entry[static_cast<std::size_t>(Arm::D15)],
                t.census.entry[static_cast<std::size_t>(Arm::D25)],
                t.census.entry[static_cast<std::size_t>(Arm::D3)],
                t.census.entry[static_cast<std::size_t>(Arm::D10)], t.decidedByTen);
    std::printf("    /10 vs /3: %lld blocks decided differently, %lld of them server stone%s\n",
                t.tenVersusThreeFlips, t.tenVersusThreeServerStone,
                t.tenVersusThreeFlips == 0 ? "   <- THIS WORLD CANNOT MEASURE THE DIVISOR" : "");
    std::printf("    /10 ladder (miss/falseStone):");
    for (std::size_t i = 0; i < d10Ladder().size(); ++i) {
        std::printf(" %g:%lld/%lld", d10Ladder()[i], t.d10[i].miss, t.d10[i].falseStone);
    }
    std::printf("\n    /2.5 ladder (miss/falseStone):");
    for (std::size_t i = 0; i < d25Ladder().size(); ++i) {
        std::printf(" %g:%lld/%lld", d25Ladder()[i], t.d25[i].miss, t.d25[i].falseStone);
    }
    std::printf("\n    disagreements with the committed placesBarrier: %lld\n",
                t.committedMismatches);
}

void merge(Totals& into, const Totals& from) {
    accumulate(into.shipped, from.shipped);
    accumulate(into.liftAir, from.liftAir);
    accumulate(into.liftFluid, from.liftFluid);
    accumulate(into.ungated, from.ungated);
    for (std::size_t i = 0; i < into.d10.size(); ++i) {
        accumulate(into.d10[i], from.d10[i]);
    }
    for (std::size_t i = 0; i < into.d25.size(); ++i) {
        accumulate(into.d25[i], from.d25[i]);
    }
    for (std::size_t i = 0; i < into.census.entry.size(); ++i) {
        into.census.entry[i] += from.census.entry[i];
    }
    into.decidedByTen += from.decidedByTen;
    into.tenVersusThreeFlips += from.tenVersusThreeFlips;
    into.tenVersusThreeServerStone += from.tenVersusThreeServerStone;
    into.committedMismatches += from.committedMismatches;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: aquifer-deepfloor-analyze <probe-dir>\n");
        return 2;
    }
    const std::filesystem::path root = argv[1];
    std::ifstream manifestFile(root / "manifest.json");
    const nlohmann::json manifest = nlohmann::json::parse(manifestFile);
    const auto seed = manifest.at("seed").get<std::int64_t>();
    const std::vector<Dim> dims = readSpec(root / "spec.json");

    const char* fixturesEnv = std::getenv("STRATUM_FIXTURES_DIR");
    const std::filesystem::path fixturesDir =
        (fixturesEnv ? std::filesystem::path(fixturesEnv) : std::filesystem::path(".fixtures")) /
        "1.21.11";
    const auto pack = data::Pack::open(fixturesDir / "worldgen");

    const std::vector<data::ResourceLocation> wanted{
        data::ResourceLocation::parse("minecraft:aquifer_barrier"),
        data::ResourceLocation::parse("minecraft:aquifer_fluid_level_floodedness"),
        data::ResourceLocation::parse("minecraft:aquifer_fluid_level_spread")};

    std::printf("### %s (seed %lld)\n", root.string().c_str(), static_cast<long long>(seed));
    Totals pooled;

    for (const Dim& dim : dims) {
        const std::filesystem::path regionPath = root / dim.name / "r.0.0.mca";
        if (!std::filesystem::is_regular_file(regionPath)) {
            std::printf("=== %s: no region, skipped\n", dim.name.c_str());
            continue;
        }
        // Each dimension drives its own floodedness and spread, so the graph
        // is rebuilt per dimension rather than shared.
        density::Graph::Builder builder(pack);
        const density::NodeIndex barrierNode = builder.add(dim.barrier);
        const density::NodeIndex floodNode = builder.add(dim.flood);
        const density::NodeIndex spreadNode = builder.add(dim.spread);
        const density::Graph graph = builder.release();
        const auto noises =
            density::NoiseRegistry::create(pack, wanted, seed, density::RandomSource::Xoroshiro);
        const density::Interpreter interp(graph, noises);
        density::Interpreter::CornerCache cache(interp.cacheSize());
        const aquifer::CentreSource centres(seed);
        const aquifer::PslRead surface =
            aquifer::constantSurface(static_cast<std::int32_t>(std::floor(dim.psl)));

        Totals here;
        const std::int32_t maxY = dim.minY + dim.height - 1;
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
                        for (std::int32_t y = dim.minY; y <= maxY; ++y) {
                            const auto* block = ch.blockAt(lx, y, lz);
                            if (block == nullptr) {
                                continue;
                            }
                            bool observedStone = false;
                            if (block->name == "minecraft:stone") {
                                observedStone = true;
                            } else if (block->name != "minecraft:water" &&
                                       block->name != "minecraft:air") {
                                continue;
                            }

                            const aquifer::Selection sel = aquifer::selectSources(centres, x, y, z);
                            std::array<aquifer::BarrierSource, 3> src{};
                            for (std::size_t r = 0; r < 3; ++r) {
                                const auto& s = sel.ranked[r];
                                const aquifer::SamplePos fp = aquifer::floodednessSample(s.centre);
                                const aquifer::SamplePos sp =
                                    aquifer::spreadSample(s.cell, s.centre);
                                const double flood = interp.evaluate(
                                    floodNode, density::Point{.x = fp.x, .y = fp.y, .z = fp.z},
                                    cache);
                                const double spread = interp.evaluate(
                                    spreadNode, density::Point{.x = sp.x, .y = sp.y, .z = sp.z},
                                    cache);
                                const aquifer::CellFluid cell{.centreY = s.centre.y,
                                                              .surface = surface,
                                                              .seaLevel = dim.seaLevel,
                                                              .floodedness = flood,
                                                              .spread = spread};
                                const std::int32_t level = aquifer::cellFluidLevel(cell);
                                src[r] = aquifer::BarrierSource{
                                    .level = level,
                                    .distanceSq = s.distanceSq,
                                    .type = aquifer::fluidTypeOf(
                                        aquifer::FluidTypeAt{.centreY = s.centre.y,
                                                             .level = level,
                                                             .seaLevel = dim.seaLevel,
                                                             .lava = dim.lava})};
                            }

                            aquifer::BarrierAt at;
                            at.y = y;
                            at.density = dim.density;
                            at.nearest = src[0];
                            at.second = src[1];
                            at.third = src[2];
                            at.barrier = interp.evaluate(
                                barrierNode, density::Point{.x = x, .y = y, .z = z}, cache);

                            Params p;
                            p.guard = Guard::Shipped;
                            add(here.shipped, observedStone, placesBarrier(at, p, nullptr, nullptr));
                            p.guard = Guard::LiftAir;
                            add(here.liftAir, observedStone, placesBarrier(at, p, nullptr, nullptr));
                            p.guard = Guard::LiftFluid;
                            add(here.liftFluid, observedStone,
                                placesBarrier(at, p, nullptr, nullptr));

                            p.guard = Guard::Ungated;
                            Arm deciding = Arm::None;
                            const bool ungated = placesBarrier(at, p, &here.census, &deciding);
                            add(here.ungated, observedStone, ungated);
                            if (deciding == Arm::D10) {
                                ++here.decidedByTen;
                            }
                            // The un-gated model IS the committed predicate;
                            // anything else here is a harness bug.
                            if (ungated != aquifer::placesBarrier(at)) {
                                ++here.committedMismatches;
                            }

                            for (std::size_t i = 0; i < d10Ladder().size(); ++i) {
                                Params q;
                                q.d10 = d10Ladder()[i];
                                add(here.d10[i], observedStone,
                                    placesBarrier(at, q, nullptr, nullptr));
                            }
                            for (std::size_t i = 0; i < d25Ladder().size(); ++i) {
                                Params q;
                                q.d25 = d25Ladder()[i];
                                add(here.d25[i], observedStone,
                                    placesBarrier(at, q, nullptr, nullptr));
                            }
                            Params asThree;
                            asThree.d10 = 3.0;
                            if (placesBarrier(at, asThree, nullptr, nullptr) != ungated) {
                                ++here.tenVersusThreeFlips;
                                if (observedStone) {
                                    ++here.tenVersusThreeServerStone;
                                }
                            }
                        }
                    }
                }
            }
        }

        char label[256];
        std::snprintf(label, sizeof(label), "=== %s (D=%g, psl=%g)", dim.name.c_str(), dim.density,
                      dim.psl);
        report(label, here);
        merge(pooled, here);
    }

    std::printf("\n");
    report("=== POOLED", pooled);
    return 0;
}
