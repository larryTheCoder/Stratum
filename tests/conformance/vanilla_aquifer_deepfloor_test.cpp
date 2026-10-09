// Stratum — Q6.4's `/10` and `/2.5` arms, the barrier's FLOOR and LID,
// against real server blocks.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// `vanilla_aquifer_barrier3source_test.cpp` scores the barrier on worlds
// where every flooded cell takes `sea_level`. That makes two neighbouring
// sources hold the SAME level, `Δ = 0`, and Q6.4's Π is zero before any of
// its four arms is chosen — so those worlds cannot see the `/10` arm at all,
// and for four campaigns it was recorded as "0 uses" and carried on the
// clean-room spec's word.
//
// `tools/analysis/aquifer-deepfloor-probe.sh` is the world that can see it:
// `fluid_level_floodedness` pinned to a constant 0.6, strictly between
// lattice.hpp's two measured gates, so every cell takes the LADDER and
// neighbouring cells hold DIFFERENT levels. What this test pins:
//
//   * The predicate is EXACT there — no server barrier unwritten, and no
//     block of stone written that the server does not have. That is the
//     assertion the agree-guard used to fail (barrier.hpp's header has the
//     full ablation), and the guard is scored here too: reinstated on
//     both-air pairs, on both-fluid pairs, or on both, it only ever leaves
//     server barriers unwritten — most of them on the both-fluid side.
//
//   * The `/10` arm actually DECIDES blocks, rather than merely being
//     entered, and so does the `/2.5` arm. Without this either arm could
//     silently go dead again — which is exactly how both stayed unmeasured
//     — and every other assertion here would still pass.
//
//   * Each of the two divisors is BRACKETED by the server from both sides:
//     9.9 and 2.4 leave server barriers unwritten and write no stone the
//     server lacks, 10.1 and 2.6 the reverse, on every seed. Divisor 3 in
//     the `/10` slot is strictly worse on the blocks where it and 10
//     disagree, and in the `/2.5` slot (the `/3` arm's own constant, which
//     the unit case used to accept on its boundary) it writes false stone.
//     This is the ablation spec/aquifer-spec.md's Q6.4 asks for ("ablation
//     on each of the four divisors independently"), run against the server
//     rather than against another model.
//
//   * Q4.1's twelve-cell window beats the symmetric 27-cell set: different
//     levels on neighbouring sources are what let the rival's rank 3 change a
//     verdict at all, so this corpus is the first that can tell them apart.
//
//   * Q4.4's later-wins tie-break, at ranks 2-3 and 3-4: swapping a tied
//     pair changes the verdict on 27 and 1131 blocks, and the server writes
//     the later-wins verdict on all of them. The 27 are what the clean-room
//     spec's open question 9 never saw — a rank 2-3 tie deciding a block.
//
// The second case checks the recipe itself: the probe's control dimension,
// real floodedness and spread with nothing rescaled, IS `barrier3way`'s
// `d_neg0_3` block for block, and the predicate is exact on it on all three
// seeds — so what the deep dimensions measure is not an artefact of the
// constant floodedness that creates them.
//
// Nothing this reads is committed: the worlds are Mojang-derived (SPEC §12).
#include "support/probe_corpus.hpp"
#include "support/probe_spec.hpp"

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

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace stratum;

constexpr std::int32_t kSeaLevel = 63;
/// The probe's constant floodedness: strictly between lattice.hpp's
/// `kFloodedLocalThreshold` and `kFloodedSeaThreshold`, so every cell takes
/// the ladder rather than the sea. This is the whole reason the worlds hold
/// neighbouring sources at DIFFERENT levels.
constexpr double kLadderFloodedness = 0.6;
/// A corner of the forceloaded window. The arm this test exists for decides
/// thousands of blocks per dimension here, which is ample, and the full
/// 8x8 sweep is the analyzer's job rather than the suite's.
constexpr std::int32_t kChunks = 4;

/// The dimensions of `aquifer-deepfloor-probe.sh` that create the deep
/// both-fluid pairs, with their own `raw_final_density` constants.
struct Dimension {
    const char* name;
    double density;
    std::int32_t psl;
};

constexpr std::array<Dimension, 2> kDimensions{
    {{"deep_d005", -0.05, 200}, {"deep_d03", -0.3, 200}}};

/// Every seed the probe script has been run for. Each is scored on its own —
/// one seed agreeing with an RNG-driven model is not evidence.
constexpr std::array<const char*, 3> kProbeDirs{{"aqdeep", "aqdeep2", "aqdeep3"}};

[[nodiscard]] std::filesystem::path fixtures() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11";
}

/// Which of Q6.4's four arms a term took. `None` is everything else: an
/// equal-level pair (`Δ = 0`), the mixed-type constant, or a guarded pair.
enum class Arm : std::uint8_t { None, OnePointFive, TwoPointFive, Three, Ten };

/// `aquifer::placesBarrier` with the two divisors that were dead under the
/// agree-guard opened up, and that guard reconstructable, so each can be
/// ablated against the server. At the defaults this must be the committed
/// function exactly, and both cases assert that on every block before they
/// trust any comparison.
struct Ablation {
    /// The FLOOR's divisor: `h <= 0` with `3 + t <= 0`.
    double tenDivisor = 10.0;
    /// The LID's divisor: `h > 0` with `t <= 0`.
    double twoFiveDivisor = 2.5;
    /// The refuted agree-guard (`aFluid == bFluid -> no term`), reinstated
    /// on pairs that both read air, both read fluid, or — both set — as it
    /// once stood.
    bool guardBothAir = false;
    bool guardBothFluid = false;
    /// The arm of the term that fired, or `None` when none did.
    Arm decidedBy = Arm::None;

    [[nodiscard]] double pressure(const std::int32_t levelA, const std::int32_t levelB,
                                  const std::int32_t y, const double barrierNoise, Arm& arm) const {
        const std::int32_t deltaInt = levelA < levelB ? levelB - levelA : levelA - levelB;
        arm = Arm::None;
        if (deltaInt == 0) {
            return 0.0;
        }
        const auto delta = static_cast<double>(deltaInt);
        const double m = (static_cast<double>(levelA) + static_cast<double>(levelB)) / 2.0;
        const double h = static_cast<double>(y) + 0.5 - m;
        const double t = (delta / 2.0) - std::abs(h);
        double u = std::numeric_limits<double>::quiet_NaN();
        if (h > 0) {
            if (t > 0) {
                u = t / 1.5;
                arm = Arm::OnePointFive;
            } else {
                u = t / twoFiveDivisor;
                arm = Arm::TwoPointFive;
            }
        } else {
            const double threeT = 3.0 + t;
            if (threeT > 0) {
                u = threeT / 3.0;
                arm = Arm::Three;
            } else {
                u = threeT / tenDivisor;
                arm = Arm::Ten;
            }
        }
        const double noiseTerm = (std::abs(u) <= 2.0) ? barrierNoise : 0.0;
        return 2.0 * (noiseTerm + u);
    }

    [[nodiscard]] bool term(const double density, const double weight,
                            const aquifer::BarrierSource& a, const aquifer::BarrierSource& b,
                            const std::int32_t y, const double barrierNoise, Arm& arm) const {
        arm = Arm::None;
        const bool aFluid = y < a.level;
        const bool bFluid = y < b.level;
        if (aFluid && bFluid && a.type != b.type) {
            return (density + (weight * aquifer::kMixedTypePressure)) > 0.0;
        }
        if (aFluid == bFluid && (aFluid ? guardBothFluid : guardBothAir)) {
            return false;
        }
        return (density + (weight * pressure(a.level, b.level, y, barrierNoise, arm))) > 0.0;
    }

    [[nodiscard]] bool places(const aquifer::BarrierAt& at) {
        decidedBy = Arm::None;
        const auto sim = [](const std::int64_t di, const std::int64_t dj) {
            return 1.0 -
                   static_cast<double>(dj - di) / static_cast<double>(aquifer::kSimilarityRange);
        };
        const double s12 = sim(at.nearest.distanceSq, at.second.distanceSq);
        if (s12 <= 0.0) {
            return false;
        }
        Arm arm = Arm::None;
        if (term(at.density, s12, at.nearest, at.second, at.y, at.barrier, arm)) {
            decidedBy = arm;
            return true;
        }
        const double s13 = sim(at.nearest.distanceSq, at.third.distanceSq);
        if (s13 > 0.0 && term(at.density, s12 * s13, at.nearest, at.third, at.y, at.barrier, arm)) {
            decidedBy = arm;
            return true;
        }
        const double s23 = sim(at.second.distanceSq, at.third.distanceSq);
        if (s23 > 0.0 && term(at.density, s12 * s23, at.second, at.third, at.y, at.barrier, arm)) {
            decidedBy = arm;
            return true;
        }
        return false;
    }
};

/// One model's disagreements with the server, on stone/water/air blocks.
struct Tally {
    /// Server stone the model leaves open.
    long long misses = 0;
    /// Stone the model writes where the server has water or air.
    long long falseStone = 0;

    void add(const bool observedStone, const bool modelStone) {
        misses += static_cast<long long>(observedStone && !modelStone);
        falseStone += static_cast<long long>(!observedStone && modelStone);
    }
};

/// Each once-dead divisor moved to either side of its measured value. The
/// predicate is monotone in each (a larger divisor raises `u` toward 0, and
/// a term only fires with `|u| <= 2`), so given an exact shipped value the
/// low side can only miss and the high side can only write false stone.
struct Brackets {
    Tally tenLow;  // 9.9 in the FLOOR's slot
    Tally tenHigh; // 10.1
    Tally lidLow;  // 2.4 in the LID's slot
    Tally lidHigh; // 2.6
};

struct Score {
    long long blocks = 0;
    long long serverStone = 0;
    long long misses = 0;
    long long falseStone = 0;
    long long decidedByTen = 0;
    long long decidedByTwoFive = 0;
    /// Blocks where divisor 10 and divisor 3 give different verdicts.
    long long contested = 0;
    /// Of those, the ones each divisor gets RIGHT.
    long long tenCorrect = 0;
    long long threeCorrect = 0;
    long long committedMismatches = 0;
    Brackets brackets;
    /// 3.0 in the LID's slot: the `/3` arm's constant.
    Tally lidAdjacent;
    /// The agree-guard, reinstated on both-air pairs, both-fluid pairs, both.
    Tally guardAir;
    Tally guardFluid;
    Tally guardBoth;
    /// Q4.1's rival window, the symmetric 27 cells around the home cell:
    /// blocks where its first three ranks differ from the shipped window's,
    /// the ones where that changes the verdict, and which window the server
    /// sides with there.
    long long windowDiffers = 0;
    long long windowDecides = 0;
    long long twelveCorrect = 0;
    long long rivalCorrect = 0;
    /// Spec Q4.4 by rank pair (1-2, 2-3, 3-4): blocks whose two ranks tie in
    /// squared distance, the ones where swapping the tied pair changes the
    /// verdict, and which order the server sides with there. Stone is
    /// stone: no fluid that moved can make it or take it away.
    std::array<long long, 3> ties{};
    std::array<long long, 3> tieDecides{};
    std::array<long long, 3> laterCorrect{};
    std::array<long long, 3> earlierCorrect{};
};

} // namespace

TEST_CASE("Q6.4's fourth divisor is 10, on the server's own deep barriers",
          "[conformance][aquifer]") {
    // Skip before touching the pack: on a machine with no fixtures at all
    // this must skip, not throw.
    bool anyProbe = false;
    for (const char* probeName : kProbeDirs) {
        anyProbe = anyProbe || std::filesystem::is_regular_file(fixtures() / "probes" / probeName /
                                                                "manifest.json");
    }
    if (!std::filesystem::is_directory(fixtures() / "worldgen") || !anyProbe) {
        SKIP("no aquifer-deepfloor probe under "
             << (fixtures() / "probes")
             << "; generate it with tools/analysis/aquifer-deepfloor-probe.sh --accept-eula "
                "42 (and 31337, 8675309)");
    }

    const auto pack = data::Pack::open(fixtures() / "worldgen");
    const std::vector<data::ResourceLocation> wanted{
        data::ResourceLocation::parse("minecraft:aquifer_barrier"),
        data::ResourceLocation::parse("minecraft:aquifer_fluid_level_floodedness"),
        data::ResourceLocation::parse("minecraft:aquifer_fluid_level_spread")};

    Score pooled;
    // The brackets once more per seed: one seed agreeing with an RNG-driven
    // model is not evidence, and neither is a pooled count one seed carries.
    std::array<Brackets, kProbeDirs.size()> perSeed{};
    // Rank 2-3 ties that decide a verdict, per seed and dimension.
    std::array<std::array<long long, kDimensions.size()>, kProbeDirs.size()> rank23Decides{};
    std::size_t dimensionsScored = 0;

    for (std::size_t seedIndex = 0; seedIndex < kProbeDirs.size(); ++seedIndex) {
        const char* probeName = kProbeDirs[seedIndex];
        const std::filesystem::path probeDir = fixtures() / "probes" / probeName;
        const std::filesystem::path manifestPath = probeDir / "manifest.json";
        // Any probe present means all three must be: one seed agreeing with
        // an RNG-driven model is not evidence, so a partial corpus fails here
        // rather than passing on whichever seed happens to exist.
        INFO("probe " << probeName << " — generate it with "
                      << "tools/analysis/aquifer-deepfloor-probe.sh --accept-eula <seed>");
        REQUIRE(std::filesystem::is_regular_file(manifestPath));
        stratum::test::requireFrozen(probeDir, "tools/analysis/aquifer-deepfloor-probe.sh");
        std::ifstream manifestFile(manifestPath);
        const nlohmann::json manifest = nlohmann::json::parse(manifestFile);
        const auto seed = manifest.at("seed").get<std::int64_t>();
        const nlohmann::json spec = stratum::test::readSpec(probeDir);

        for (std::size_t dimIndex = 0; dimIndex < kDimensions.size(); ++dimIndex) {
            const Dimension& dim = kDimensions.at(dimIndex);
            const std::filesystem::path regionPath = probeDir / dim.name / "r.0.0.mca";
            INFO("dimension " << dim.name);
            REQUIRE(std::filesystem::is_regular_file(regionPath));
            // The world on disk must be the world this test believes it is.
            // A spec that has drifted from `kDimensions` would score real
            // blocks against the wrong density and quietly pass or fail for
            // the wrong reason (SPEC §8: no silent best-effort).
            const nlohmann::json& entry = stratum::test::specEntry(spec, probeDir, dim.name);
            CHECK(entry.at("raw_final_density").at("argument").get<double>() ==
                  Catch::Approx(dim.density));
            CHECK(entry.at("router").at("preliminary_surface_level").get<double>() ==
                  Catch::Approx(static_cast<double>(dim.psl)));
            CHECK(entry.at("router").at("fluid_level_floodedness").get<double>() ==
                  Catch::Approx(kLadderFloodedness));
            CHECK(entry.at("sea_level").get<std::int32_t>() == kSeaLevel);

            const nlohmann::json barrierJson = {{"type", "minecraft:noise"},
                                                {"noise", "minecraft:aquifer_barrier"},
                                                {"xz_scale", 1.0},
                                                {"y_scale", 0.5}};
            // The probe's own fast-sampled spread. Constant floodedness is
            // read straight off the spec rather than through the graph.
            const nlohmann::json spreadJson = {{"type", "minecraft:mul"},
                                               {"argument1", 4.0},
                                               {"argument2",
                                                {{"type", "minecraft:noise"},
                                                 {"noise", "minecraft:aquifer_fluid_level_spread"},
                                                 {"xz_scale", 4.0},
                                                 {"y_scale", 0.7142857142857143 * 4.0}}}};
            density::Graph::Builder builder(pack);
            const density::NodeIndex barrierNode = builder.add(barrierJson);
            const density::NodeIndex spreadNode = builder.add(spreadJson);
            const density::Graph graph = builder.release();
            const auto noises = density::NoiseRegistry::create(pack, wanted, seed,
                                                               density::RandomSource::Xoroshiro);
            const density::Interpreter interp(graph, noises);
            density::Interpreter::CornerCache cache(interp.cacheSize());
            const aquifer::CentreSource centres(seed, stratum::density::RandomSource::Xoroshiro);
            const aquifer::PslRead surface = aquifer::constantSurface(dim.psl);

            const auto file = region::RegionFile::open(regionPath);
            aquifer::CellIndex rivalHome{
                .x = std::numeric_limits<std::int32_t>::min(), .y = 0, .z = 0};
            std::array<aquifer::Candidate, 27> rivalWindow{};
            Ablation asTen{.tenDivisor = 10.0};
            Ablation asThree{.tenDivisor = 3.0};
            Ablation tenLow{.tenDivisor = 9.9};
            Ablation tenHigh{.tenDivisor = 10.1};
            Ablation lidLow{.twoFiveDivisor = 2.4};
            Ablation lidHigh{.twoFiveDivisor = 2.6};
            Ablation lidAdjacent{.twoFiveDivisor = 3.0};
            Ablation guardAir{.guardBothAir = true};
            Ablation guardFluid{.guardBothFluid = true};
            Ablation guardBoth{.guardBothAir = true, .guardBothFluid = true};

            for (std::int32_t cz = 0; cz < kChunks; ++cz) {
                for (std::int32_t cx = 0; cx < kChunks; ++cx) {
                    REQUIRE(file.hasChunk(cx, cz));
                    const auto ch = chunk::Chunk::decode(nbt::read(file.readChunk(cx, cz)).root);
                    for (int lz = 0; lz < 16; ++lz) {
                        for (int lx = 0; lx < 16; ++lx) {
                            const std::int32_t x = (cx * 16) + lx;
                            const std::int32_t z = (cz * 16) + lz;
                            for (std::int32_t y = -48; y <= 271; ++y) {
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

                                const double barrierNoise = interp.evaluate(
                                    barrierNode, density::Point{.x = x, .y = y, .z = z}, cache);
                                const auto barrierFor = [&](const aquifer::Selection& sel) {
                                    std::array<aquifer::BarrierSource, 3> src{};
                                    for (std::size_t r = 0; r < 3; ++r) {
                                        const auto& s = sel.ranked[r];
                                        const aquifer::SamplePos sp =
                                            aquifer::spreadSample(s.cell, s.centre);
                                        const double spread = interp.evaluate(
                                            spreadNode,
                                            density::Point{.x = sp.x, .y = sp.y, .z = sp.z}, cache);
                                        const aquifer::CellFluid cell{.centreY = s.centre.y,
                                                                      .surface = surface,
                                                                      .seaLevel = kSeaLevel,
                                                                      .floodedness =
                                                                          kLadderFloodedness,
                                                                      .spread = spread};
                                        const aquifer::SourceStatus status =
                                            aquifer::sourceStatus(cell, 0.0);
                                        src[r] = aquifer::BarrierSource{.level = status.level,
                                                                        .distanceSq = s.distanceSq,
                                                                        .type = status.type};
                                    }
                                    aquifer::BarrierAt at;
                                    at.y = y;
                                    at.density = dim.density;
                                    at.nearest = src[0];
                                    at.second = src[1];
                                    at.third = src[2];
                                    at.barrier = barrierNoise;
                                    return at;
                                };
                                const aquifer::Selection sel =
                                    aquifer::selectSources(centres, x, y, z);
                                const aquifer::BarrierAt at = barrierFor(sel);

                                const bool shipped = aquifer::placesBarrier(at);
                                const bool ten = asTen.places(at);
                                const bool three = asThree.places(at);

                                ++pooled.blocks;
                                if (ten != shipped) {
                                    ++pooled.committedMismatches;
                                }
                                if (observedStone) {
                                    ++pooled.serverStone;
                                    if (!shipped) {
                                        ++pooled.misses;
                                    }
                                } else if (shipped) {
                                    ++pooled.falseStone;
                                }
                                if (asTen.decidedBy == Arm::Ten) {
                                    ++pooled.decidedByTen;
                                }
                                if (asTen.decidedBy == Arm::TwoPointFive) {
                                    ++pooled.decidedByTwoFive;
                                }
                                if (ten != three) {
                                    ++pooled.contested;
                                    if (ten == observedStone) {
                                        ++pooled.tenCorrect;
                                    }
                                    if (three == observedStone) {
                                        ++pooled.threeCorrect;
                                    }
                                }

                                // Both brackets, pooled and for this seed.
                                const bool tenLowStone = tenLow.places(at);
                                const bool tenHighStone = tenHigh.places(at);
                                const bool lidLowStone = lidLow.places(at);
                                const bool lidHighStone = lidHigh.places(at);
                                for (Brackets* into : {&pooled.brackets, &perSeed[seedIndex]}) {
                                    into->tenLow.add(observedStone, tenLowStone);
                                    into->tenHigh.add(observedStone, tenHighStone);
                                    into->lidLow.add(observedStone, lidLowStone);
                                    into->lidHigh.add(observedStone, lidHighStone);
                                }
                                pooled.lidAdjacent.add(observedStone, lidAdjacent.places(at));
                                pooled.guardAir.add(observedStone, guardAir.places(at));
                                pooled.guardFluid.add(observedStone, guardFluid.places(at));
                                pooled.guardBoth.add(observedStone, guardBoth.places(at));

                                // Q4.1: the same block through the symmetric
                                // 27-cell window, in x, y, z order.
                                const aquifer::CellIndex home = aquifer::cellOf(x, y, z);
                                if (!(home == rivalHome)) {
                                    rivalHome = home;
                                    std::size_t i = 0;
                                    for (std::int32_t dx = -1; dx <= 1; ++dx) {
                                        for (std::int32_t dy = -1; dy <= 1; ++dy) {
                                            for (std::int32_t dz = -1; dz <= 1; ++dz) {
                                                const aquifer::CellIndex cell{.x = home.x + dx,
                                                                              .y = home.y + dy,
                                                                              .z = home.z + dz};
                                                rivalWindow[i++] =
                                                    aquifer::Candidate{.cell = cell,
                                                                       .centre = centres.centreOf(
                                                                           cell.x, cell.y, cell.z)};
                                            }
                                        }
                                    }
                                }
                                // Q4.4: each tied pair swapped on its own.
                                for (std::size_t p = 0; p < 3; ++p) {
                                    if (sel.ranked[p].distanceSq != sel.ranked[p + 1].distanceSq) {
                                        continue;
                                    }
                                    ++pooled.ties.at(p);
                                    aquifer::Selection swapped = sel;
                                    std::swap(swapped.ranked[p], swapped.ranked[p + 1]);
                                    const bool earlierStone =
                                        aquifer::placesBarrier(barrierFor(swapped));
                                    if (earlierStone != shipped) {
                                        ++pooled.tieDecides.at(p);
                                        if (p == 1) {
                                            ++rank23Decides.at(seedIndex).at(dimIndex);
                                        }
                                        pooled.laterCorrect.at(p) +=
                                            static_cast<long long>(shipped == observedStone);
                                        pooled.earlierCorrect.at(p) +=
                                            static_cast<long long>(earlierStone == observedStone);
                                    }
                                }

                                const aquifer::Selection rival =
                                    aquifer::rankCandidates(x, y, z, rivalWindow);
                                bool sameRanks = true;
                                for (std::size_t r = 0; r < 3; ++r) {
                                    sameRanks =
                                        sameRanks && rival.ranked[r].cell == sel.ranked[r].cell &&
                                        rival.ranked[r].distanceSq == sel.ranked[r].distanceSq;
                                }
                                if (!sameRanks) {
                                    ++pooled.windowDiffers;
                                    const bool rivalStone =
                                        aquifer::placesBarrier(barrierFor(rival));
                                    if (rivalStone != shipped) {
                                        ++pooled.windowDecides;
                                        pooled.twelveCorrect +=
                                            static_cast<long long>(shipped == observedStone);
                                        pooled.rivalCorrect +=
                                            static_cast<long long>(rivalStone == observedStone);
                                    }
                                }
                            }
                        }
                    }
                }
            }
            ++dimensionsScored;
        }
    }

    // Every seed, every dimension: a partial corpus failed above already,
    // and this is the count that says so in one place (SPEC §8).
    REQUIRE(dimensionsScored == kProbeDirs.size() * kDimensions.size());

    const Brackets& b = pooled.brackets;
    INFO("blocks " << pooled.blocks << ", server stone " << pooled.serverStone << ", /10-decided "
                   << pooled.decidedByTen << ", /2.5-decided " << pooled.decidedByTwoFive
                   << ", contested " << pooled.contested
                   << "; the 27-cell window differs in its first three ranks on "
                   << pooled.windowDiffers << ", changes the verdict on " << pooled.windowDecides
                   << " (12-cell right " << pooled.twelveCorrect << ", 27-cell right "
                   << pooled.rivalCorrect << ")");
    INFO("misses/false stone: /10 slot 9.9 "
         << b.tenLow.misses << "/" << b.tenLow.falseStone << ", 10.1 " << b.tenHigh.misses << "/"
         << b.tenHigh.falseStone << "; /2.5 slot 2.4 " << b.lidLow.misses << "/"
         << b.lidLow.falseStone << ", 2.6 " << b.lidHigh.misses << "/" << b.lidHigh.falseStone
         << ", 3.0 " << pooled.lidAdjacent.misses << "/" << pooled.lidAdjacent.falseStone);
    INFO("agree-guard on both-air "
         << pooled.guardAir.misses << "/" << pooled.guardAir.falseStone << ", on both-fluid "
         << pooled.guardFluid.misses << "/" << pooled.guardFluid.falseStone << ", on both "
         << pooled.guardBoth.misses << "/" << pooled.guardBoth.falseStone);

    // The ablation at the default divisors IS the committed predicate.
    // Nothing below means anything if this fails.
    REQUIRE(pooled.committedMismatches == 0);

    // Both arms must actually be doing work — these are the assertions that
    // go red if either ever falls back out of reach, which is how both
    // stayed unmeasured for four campaigns. Measured 16263 and 4032.
    CHECK(pooled.decidedByTen > 1000);
    CHECK(pooled.decidedByTwoFive > 1300);
    CHECK(pooled.contested > 100);

    // Exact against the server: nothing unwritten, nothing invented.
    CHECK(pooled.misses == 0);
    CHECK(pooled.falseStone == 0);

    // And the divisor itself, by ablation: on every block where 10 and 3
    // disagree, the server sides with 10.
    CHECK(pooled.tenCorrect == pooled.contested);
    CHECK(pooled.threeCorrect == 0);

    // Both divisors, bracketed from both sides by the server. Monotonicity
    // (see `Brackets`) makes each side one-directional, so the `== 0` half
    // is what an exact shipped value implies and the floor is what the
    // server shows: a divisor 1% (FLOOR) or 4% (LID) off is visible.
    // Measured 160, 159, 194 and 193 (SPEC §11); the floors are about a
    // third of that, never the counts themselves.
    CHECK(b.tenLow.misses >= 50);
    CHECK(b.tenLow.falseStone == 0);
    CHECK(b.tenHigh.falseStone >= 50);
    CHECK(b.tenHigh.misses == 0);
    CHECK(b.lidLow.misses >= 60);
    CHECK(b.lidLow.falseStone == 0);
    CHECK(b.lidHigh.falseStone >= 60);
    CHECK(b.lidHigh.misses == 0);
    // The `/3` arm's constant in the LID's slot: what the old unit bracket
    // accepted on its boundary, and what the server refuses. Measured 931.
    CHECK(pooled.lidAdjacent.falseStone >= 300);
    CHECK(pooled.lidAdjacent.misses == 0);
    for (std::size_t s = 0; s < kProbeDirs.size(); ++s) {
        INFO("seed corpus " << kProbeDirs[s] << ": 9.9 " << perSeed[s].tenLow.misses << "/"
                            << perSeed[s].tenLow.falseStone << ", 10.1 "
                            << perSeed[s].tenHigh.misses << "/" << perSeed[s].tenHigh.falseStone
                            << ", 2.4 " << perSeed[s].lidLow.misses << "/"
                            << perSeed[s].lidLow.falseStone << ", 2.6 " << perSeed[s].lidHigh.misses
                            << "/" << perSeed[s].lidHigh.falseStone);
        CHECK(perSeed[s].tenLow.misses > 0);
        CHECK(perSeed[s].tenHigh.falseStone > 0);
        CHECK(perSeed[s].lidLow.misses > 0);
        CHECK(perSeed[s].lidHigh.falseStone > 0);
    }

    // The agree-guard, reinstated three ways: each only ever leaves server
    // barriers unwritten, and the both-fluid half — where the FLOOR the
    // guard hid lives — is by far the larger. Measured 3824 (both-air), 69811 (both-fluid)
    // and 73635 (both).
    CHECK(pooled.guardAir.falseStone == 0);
    CHECK(pooled.guardFluid.falseStone == 0);
    CHECK(pooled.guardBoth.falseStone == 0);
    CHECK(pooled.guardAir.misses >= 1200);
    CHECK(pooled.guardFluid.misses > pooled.guardAir.misses);
    CHECK(pooled.guardBoth.misses >= pooled.guardFluid.misses);

    // Q4.1's window, against the symmetric 27-cell rival. Here, and not on
    // the two-source corpora, the rival changes verdicts: neighbouring cells
    // hold different levels, so the three-source barrier reaches rank 3,
    // where the two windows part. Measured 118980 blocks with different first
    // three ranks and 39 different verdicts, every one of them the server's
    // way for the spec's window. The floor keeps the population from passing
    // empty.
    REQUIRE(pooled.windowDecides >= 30);
    CHECK(pooled.twelveCorrect == pooled.windowDecides);
    CHECK(pooled.rivalCorrect == 0);

    // Q4.4 on the server's own stone, rank pair by rank pair — and the
    // answer to the clean-room spec's open question 9, which measured rank
    // 2-3 ties changing 0 blocks of 792 338 on real-noise generation and
    // found no mechanism for the zero. Here, where floodedness 0.6 gives
    // neighbouring cells three DIFFERENT ladder levels and the density sits
    // at -0.3 or -0.05, rank 2-3 ties decide blocks on every seed, and the
    // server writes the later-wins order's verdict on every one of them
    // (golden_aquifer_tiebreak_test.cpp has why real generation almost never
    // gets here). A rank 1-2 swap never moves the barrier at all (Q6.6 is
    // symmetric in the nearest pair when they tie). The decided counts are
    // the model's alone over the blocks scored — stone, water or air, which
    // flow cannot change between — so they are pinned exactly; measured on
    // 4x4 chunks:
    // ties 86 800 / 117 266 / 139 678, decided 0 / 27 / 1131, the 27 being
    // 13 / 4 / 10 per seed, 25 of them at -0.3.
    INFO("ties " << pooled.ties[0] << " / " << pooled.ties[1] << " / " << pooled.ties[2]
                 << "; decided " << pooled.tieDecides[0] << " / " << pooled.tieDecides[1] << " / "
                 << pooled.tieDecides[2] << "; later right " << pooled.laterCorrect[0] << " / "
                 << pooled.laterCorrect[1] << " / " << pooled.laterCorrect[2] << "; earlier right "
                 << pooled.earlierCorrect[0] << " / " << pooled.earlierCorrect[1] << " / "
                 << pooled.earlierCorrect[2]);
    CHECK(pooled.tieDecides[0] == 0);
    CHECK(pooled.tieDecides[1] == 27);
    CHECK(pooled.tieDecides[2] == 1131);
    for (std::size_t p = 0; p < 3; ++p) {
        CHECK(pooled.laterCorrect.at(p) == pooled.tieDecides.at(p));
        CHECK(pooled.earlierCorrect.at(p) == 0);
    }
    for (std::size_t si = 0; si < kProbeDirs.size(); ++si) {
        INFO("seed corpus " << kProbeDirs.at(si) << ": rank 2-3 decides "
                            << rank23Decides.at(si).at(0) << " at " << kDimensions.at(0).density
                            << ", " << rank23Decides.at(si).at(1) << " at "
                            << kDimensions.at(1).density);
        CHECK(rank23Decides.at(si).at(0) + rank23Decides.at(si).at(1) > 0);
    }
}

TEST_CASE("the deepfloor control arm is barrier3way's world, and Q6.4 is exact on its real noise",
          "[conformance][aquifer]") {
    // `ctlreal_d03` restores vanilla's own floodedness and spread and moves
    // nothing else, so the deep dimensions' exactness carries over to real
    // noise only if that one change is the whole of what the recipe does.
    // Two checks: the server's control world IS `barrier3way`'s `d_neg0_3`,
    // block for block; and the predicate is exact on the control on every
    // seed the probe was run for.
    const std::filesystem::path barrier3way = fixtures() / "probes" / "barrier3way";
    bool anyProbe = std::filesystem::is_regular_file(barrier3way / "manifest.json");
    for (const char* probeName : kProbeDirs) {
        anyProbe = anyProbe || std::filesystem::is_regular_file(fixtures() / "probes" / probeName /
                                                                "manifest.json");
    }
    if (!std::filesystem::is_directory(fixtures() / "worldgen") || !anyProbe) {
        SKIP("no aquifer-deepfloor or barrier3way probe under "
             << (fixtures() / "probes")
             << "; generate them with tools/analysis/aquifer-probes.sh --accept-eula");
    }

    constexpr const char* kControl = "ctlreal_d03";
    constexpr const char* kTwin = "d_neg0_3";
    constexpr double kControlDensity = -0.3;
    constexpr std::int32_t kControlPsl = 96;
    constexpr std::int32_t kWindowChunks = 8;
    constexpr std::int32_t kMinY = -48;
    constexpr std::int32_t kMaxY = 271;

    // The named entry of a corpus's spec.json, with its name removed so two
    // corpora's entries compare on content alone.
    const auto unnamedEntry = [](const std::filesystem::path& probeDir, const std::string& name) {
        nlohmann::json unnamed = stratum::test::specEntry(probeDir, name);
        unnamed.erase("name");
        return unnamed;
    };

    // (A) The server against the server. Both corpora are seed 42 and
    // frozen, and their two dimensions are one JSON object but for the
    // name, so if the dimension's name does not enter the server's seeding
    // they are one world.
    const std::filesystem::path aqdeep = fixtures() / "probes" / kProbeDirs[0];
    for (const std::filesystem::path& dir : {aqdeep, barrier3way}) {
        INFO("probe " << dir << " — generate it with tools/analysis/aquifer-probes.sh "
                      << "--accept-eula");
        REQUIRE(std::filesystem::is_regular_file(dir / "manifest.json"));
        stratum::test::requireFrozen(dir, "tools/analysis/aquifer-probes.sh");
        stratum::test::requireSeed(dir, 42);
    }
    const nlohmann::json control = unnamedEntry(aqdeep, kControl);
    const nlohmann::json twin = unnamedEntry(barrier3way, kTwin);
    CHECK(control.at("raw_final_density").at("argument").get<double>() ==
          Catch::Approx(kControlDensity));
    CHECK(control.at("router").at("preliminary_surface_level").get<double>() ==
          Catch::Approx(static_cast<double>(kControlPsl)));
    CHECK(control.at("router").at("lava").get<double>() == Catch::Approx(0.0));
    CHECK(control.at("sea_level").get<std::int32_t>() == kSeaLevel);
    CHECK(control.at("min_y").get<std::int32_t>() == kMinY);
    CHECK(control.at("min_y").get<std::int32_t>() + control.at("height").get<std::int32_t>() - 1 ==
          kMaxY);
    REQUIRE(control == twin);

    long long compared = 0;
    long long controlStone = 0;
    long long stoneDiffs = 0;
    long long otherDiffs = 0;
    {
        const auto controlFile = region::RegionFile::open(aqdeep / kControl / "r.0.0.mca");
        const auto twinFile = region::RegionFile::open(barrier3way / kTwin / "r.0.0.mca");
        for (std::int32_t cz = 0; cz < kWindowChunks; ++cz) {
            for (std::int32_t cx = 0; cx < kWindowChunks; ++cx) {
                REQUIRE(controlFile.hasChunk(cx, cz));
                REQUIRE(twinFile.hasChunk(cx, cz));
                const auto a = chunk::Chunk::decode(nbt::read(controlFile.readChunk(cx, cz)).root);
                const auto b = chunk::Chunk::decode(nbt::read(twinFile.readChunk(cx, cz)).root);
                for (int lz = 0; lz < 16; ++lz) {
                    for (int lx = 0; lx < 16; ++lx) {
                        for (std::int32_t y = kMinY; y <= kMaxY; ++y) {
                            const auto* pa = a.blockAt(lx, y, lz);
                            const auto* pb = b.blockAt(lx, y, lz);
                            const std::string_view na =
                                pa == nullptr ? std::string_view{} : std::string_view{pa->name};
                            const std::string_view nb =
                                pb == nullptr ? std::string_view{} : std::string_view{pb->name};
                            ++compared;
                            const bool aStone = na == "minecraft:stone";
                            const bool bStone = nb == "minecraft:stone";
                            controlStone += static_cast<long long>(aStone);
                            if (aStone != bStone) {
                                ++stoneDiffs;
                            } else if (na != nb) {
                                ++otherDiffs;
                            }
                        }
                    }
                }
            }
        }
    }
    // Any other difference would be fluid that moved before the freeze
    // (SPEC §7) — water and air, run-dependent, so reported and never
    // pinned (measured 0). Neither world holds lava, so flow writes no
    // stone in either.
    INFO("compared " << compared << " blocks: control stone " << controlStone
                     << ", stone differing from " << kTwin << " " << stoneDiffs
                     << ", other differences (flow) " << otherDiffs);
    REQUIRE(compared ==
            static_cast<long long>(kWindowChunks) * kWindowChunks * 256 * (kMaxY - kMinY + 1));
    REQUIRE(controlStone > 1000);
    CHECK(stoneDiffs == 0);

    // (B) The predicate on the control, every seed, real noise throughout:
    // the graph is the corpus's own router JSON, as the analyzer builds it.
    const auto pack = data::Pack::open(fixtures() / "worldgen");
    const std::vector<data::ResourceLocation> wanted{
        data::ResourceLocation::parse("minecraft:aquifer_barrier"),
        data::ResourceLocation::parse("minecraft:aquifer_fluid_level_floodedness"),
        data::ResourceLocation::parse("minecraft:aquifer_fluid_level_spread")};

    struct ControlScore {
        long long blocks = 0;
        long long serverStone = 0;
        long long committedMismatches = 0;
        Tally shipped;
        Tally guardAir;
        /// Each rung of `kLidLadder` in the LID's slot.
        std::array<Tally, 6> lid{};
        /// Blocks where 3 in the FLOOR's slot gives a different verdict.
        long long tenVersusThree = 0;
    };

    constexpr std::array<double, 6> kLidLadder{{1.5, 2.0, 2.4, 2.6, 3.0, 10.0}};
    // The rungs the assertions read, by name.
    constexpr std::size_t kLid20 = 1;
    constexpr std::size_t kLid24 = 2;
    constexpr std::size_t kLid26 = 3;
    constexpr std::size_t kLid30 = 4;
    constexpr std::size_t kLid100 = 5;
    ControlScore pooled;

    for (const char* probeName : kProbeDirs) {
        const std::filesystem::path probeDir = fixtures() / "probes" / probeName;
        INFO("probe " << probeName << " — generate it with "
                      << "tools/analysis/aquifer-deepfloor-probe.sh --accept-eula <seed>");
        REQUIRE(std::filesystem::is_regular_file(probeDir / "manifest.json"));
        stratum::test::requireFrozen(probeDir, "tools/analysis/aquifer-deepfloor-probe.sh");
        std::ifstream manifestFile(probeDir / "manifest.json");
        const auto seed = nlohmann::json::parse(manifestFile).at("seed").get<std::int64_t>();
        const nlohmann::json entry = unnamedEntry(probeDir, kControl);
        // Every seed's control must be the same recipe as seed 42's.
        REQUIRE(entry == control);
        const nlohmann::json& router = entry.at("router");
        // A `lava` node would need the contracted lava sample positions;
        // this reads it as a constant only, and refuses anything else rather
        // than scoring the wrong thing (SPEC §8).
        {
            INFO("dimension " << kControl << " drives `lava` through a node");
            REQUIRE(router.at("lava").is_number());
        }
        const double lava = router.at("lava").get<double>();

        density::Graph::Builder builder(pack);
        const density::NodeIndex barrierNode = builder.add(router.at("barrier"));
        const density::NodeIndex floodNode = builder.add(router.at("fluid_level_floodedness"));
        const density::NodeIndex spreadNode = builder.add(router.at("fluid_level_spread"));
        const density::Graph graph = builder.release();
        const auto noises =
            density::NoiseRegistry::create(pack, wanted, seed, density::RandomSource::Xoroshiro);
        const density::Interpreter interp(graph, noises);
        density::Interpreter::CornerCache cache(interp.cacheSize());
        const aquifer::CentreSource centres(seed, stratum::density::RandomSource::Xoroshiro);
        const aquifer::PslRead surface = aquifer::constantSurface(kControlPsl);

        // A source's status is a function of its cell alone — floodedness at
        // the cell's centre, spread at its lattice indices, a constant
        // surface and lava — so it is computed once per cell. The three
        // sources of a block are still ranked per block.
        std::map<std::array<std::int32_t, 3>, aquifer::SourceStatus> statusOf;
        const auto status = [&](const aquifer::Source& s) {
            const std::array<std::int32_t, 3> key{s.cell.x, s.cell.y, s.cell.z};
            if (const auto it = statusOf.find(key); it != statusOf.end()) {
                return it->second;
            }
            const aquifer::SamplePos fp = aquifer::floodednessSample(s.centre);
            const aquifer::SamplePos sp = aquifer::spreadSample(s.cell, s.centre);
            const aquifer::CellFluid cell{
                .centreY = s.centre.y,
                .surface = surface,
                .seaLevel = kSeaLevel,
                .floodedness = interp.evaluate(
                    floodNode, density::Point{.x = fp.x, .y = fp.y, .z = fp.z}, cache),
                .spread = interp.evaluate(spreadNode,
                                          density::Point{.x = sp.x, .y = sp.y, .z = sp.z}, cache)};
            const aquifer::SourceStatus computed = aquifer::sourceStatus(cell, lava);
            statusOf.emplace(key, computed);
            return computed;
        };

        Ablation asShipped;
        Ablation asThree{.tenDivisor = 3.0};
        Ablation guardAir{.guardBothAir = true};
        std::array<Ablation, kLidLadder.size()> lid{};
        for (std::size_t i = 0; i < kLidLadder.size(); ++i) {
            lid[i].twoFiveDivisor = kLidLadder[i];
        }

        // `selectSources` is `rankCandidates` over `candidatesFor` the home
        // cell; walking a column, the window is rebuilt only when the home
        // cell changes.
        aquifer::CellIndex home{.x = std::numeric_limits<std::int32_t>::min(), .y = 0, .z = 0};
        std::array<aquifer::Candidate, aquifer::kCandidateCount> window{};
        const auto file = region::RegionFile::open(probeDir / kControl / "r.0.0.mca");
        for (std::int32_t cz = 0; cz < kWindowChunks; ++cz) {
            for (std::int32_t cx = 0; cx < kWindowChunks; ++cx) {
                REQUIRE(file.hasChunk(cx, cz));
                const auto ch = chunk::Chunk::decode(nbt::read(file.readChunk(cx, cz)).root);
                for (int lz = 0; lz < 16; ++lz) {
                    for (int lx = 0; lx < 16; ++lx) {
                        const std::int32_t x = (cx * 16) + lx;
                        const std::int32_t z = (cz * 16) + lz;
                        for (std::int32_t y = kMinY; y <= kMaxY; ++y) {
                            const auto* block = ch.blockAt(lx, y, lz);
                            if (block == nullptr) {
                                continue;
                            }
                            const bool observedStone = block->name == "minecraft:stone";
                            if (!observedStone && block->name != "minecraft:water" &&
                                block->name != "minecraft:air") {
                                continue;
                            }
                            if (const aquifer::CellIndex cell = aquifer::cellOf(x, y, z);
                                !(cell == home)) {
                                home = cell;
                                window = aquifer::candidatesFor(centres, home);
                            }
                            const aquifer::Selection sel = aquifer::rankCandidates(x, y, z, window);
                            std::array<aquifer::BarrierSource, 3> src{};
                            for (std::size_t r = 0; r < 3; ++r) {
                                const aquifer::SourceStatus st = status(sel.ranked[r]);
                                src[r] =
                                    aquifer::BarrierSource{.level = st.level,
                                                           .distanceSq = sel.ranked[r].distanceSq,
                                                           .type = st.type};
                            }
                            aquifer::BarrierAt at;
                            at.y = y;
                            at.density = kControlDensity;
                            at.nearest = src[0];
                            at.second = src[1];
                            at.third = src[2];
                            // Every model here, the committed one included,
                            // returns false outright at `s12 <= 0` (Q6.2)
                            // whatever `barrier` reads, so the noise is read
                            // only where it can matter.
                            if (aquifer::similarity(at.nearest.distanceSq, at.second.distanceSq) >
                                0.0) {
                                at.barrier = interp.evaluate(
                                    barrierNode, density::Point{.x = x, .y = y, .z = z}, cache);
                            }

                            const bool shipped = aquifer::placesBarrier(at);
                            ++pooled.blocks;
                            pooled.serverStone += static_cast<long long>(observedStone);
                            pooled.committedMismatches +=
                                static_cast<long long>(asShipped.places(at) != shipped);
                            pooled.shipped.add(observedStone, shipped);
                            pooled.guardAir.add(observedStone, guardAir.places(at));
                            for (std::size_t i = 0; i < lid.size(); ++i) {
                                pooled.lid[i].add(observedStone, lid[i].places(at));
                            }
                            pooled.tenVersusThree +=
                                static_cast<long long>(asThree.places(at) != shipped);
                        }
                    }
                }
            }
        }
    }

    INFO("control, three seeds: blocks "
         << pooled.blocks << ", server stone " << pooled.serverStone << ", shipped "
         << pooled.shipped.misses << "/" << pooled.shipped.falseStone << ", agree-guard on "
         << "both-air " << pooled.guardAir.misses << "/" << pooled.guardAir.falseStone);
    std::ostringstream ladder;
    for (std::size_t i = 0; i < kLidLadder.size(); ++i) {
        ladder << " " << kLidLadder[i] << " " << pooled.lid[i].misses << "/"
               << pooled.lid[i].falseStone;
    }
    INFO("/2.5 slot (misses/false stone):"
         << ladder.str() << "; /3 in the /10 slot changes " << pooled.tenVersusThree
         << " verdicts (where that is 0, this world cannot weigh /10 at all)");

    REQUIRE(pooled.committedMismatches == 0);
    REQUIRE(pooled.serverStone > 1000);
    // Exact on real noise, on every seed.
    CHECK(pooled.shipped.misses == 0);
    CHECK(pooled.shipped.falseStone == 0);
    // The agree-guard is visible on real noise too: it is what `barrier3way`'s
    // last six misses were. Measured 104 over the three seeds.
    CHECK(pooled.guardAir.misses >= 30);
    CHECK(pooled.guardAir.falseStone == 0);
    // Corroboration of the LID's divisor on a world not built for it. Real
    // noise reaches the arm far more rarely than the ladder does — 2.4 and
    // 2.6 move 1 and 5 blocks here, too few to pin, so only the deep
    // dimensions above bracket 2.5 tightly — but the wider rungs point the
    // same way: 2.0 leaves server barriers unwritten (33), 3.0 and 10 write
    // stone the server does not (28, 386). Floors are a third of those.
    CHECK(pooled.lid[kLid20].misses >= 10);
    CHECK(pooled.lid[kLid20].falseStone == 0);
    CHECK(pooled.lid[kLid24].falseStone == 0);
    CHECK(pooled.lid[kLid26].misses == 0);
    CHECK(pooled.lid[kLid30].falseStone >= 9);
    CHECK(pooled.lid[kLid30].misses == 0);
    CHECK(pooled.lid[kLid100].falseStone >= 120);
    CHECK(pooled.lid[kLid100].misses == 0);
}
