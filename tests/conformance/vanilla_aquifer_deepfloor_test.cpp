// Stratum — Q6.4's `/10` arm, the barrier's FLOOR, against real server blocks.
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
//     assertion the agree-guard used to fail, by 480 354 blocks of the
//     server's 4 982 316 (barrier.hpp's header has the full ablation).
//
//   * The `/10` arm actually DECIDES blocks, rather than merely being
//     entered. Without this the arm could silently go dead again — which is
//     exactly how it stayed unmeasured — and every other assertion here
//     would still pass.
//
//   * Divisor 3 in the `/10` slot is strictly WORSE, on blocks where the two
//     disagree and the server has already answered. This is the ablation
//     spec/aquifer-spec.md's Q6.4 asks for ("ablation on each of the four
//     divisors independently"), run against the server rather than against
//     another model.
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

#include <nlohmann/json.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
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

/// `aquifer::placesBarrier` with ONE divisor opened up, so Q6.4's fourth arm
/// can be ablated against the server. At `tenDivisor = 10` this must be the
/// committed function exactly, and the test asserts that on every block
/// before it trusts any comparison.
struct Ablation {
    double tenDivisor = 10.0;
    /// Set when the block's verdict was decided by the `3 + t <= 0` arm.
    bool decidedByTen = false;

    [[nodiscard]] double pressure(const std::int32_t levelA, const std::int32_t levelB,
                                  const std::int32_t y, const double barrierNoise, bool& fromTen) {
        const std::int32_t deltaInt = levelA < levelB ? levelB - levelA : levelA - levelB;
        fromTen = false;
        if (deltaInt == 0) {
            return 0.0;
        }
        const auto delta = static_cast<double>(deltaInt);
        const double m = (static_cast<double>(levelA) + static_cast<double>(levelB)) / 2.0;
        const double h = static_cast<double>(y) + 0.5 - m;
        const double t = (delta / 2.0) - std::abs(h);
        double u = std::numeric_limits<double>::quiet_NaN();
        if (h > 0) {
            u = (t > 0) ? (t / 1.5) : (t / 2.5);
        } else {
            const double threeT = 3.0 + t;
            if (threeT > 0) {
                u = threeT / 3.0;
            } else {
                u = threeT / tenDivisor;
                fromTen = true;
            }
        }
        const double noiseTerm = (std::abs(u) <= 2.0) ? barrierNoise : 0.0;
        return 2.0 * (noiseTerm + u);
    }

    [[nodiscard]] bool term(const double density, const double weight,
                            const aquifer::BarrierSource& a, const aquifer::BarrierSource& b,
                            const std::int32_t y, const double barrierNoise, bool& fromTen) {
        fromTen = false;
        const bool aFluid = y < a.level;
        const bool bFluid = y < b.level;
        if (aFluid && bFluid && a.type != b.type) {
            return (density + (weight * aquifer::kMixedTypePressure)) > 0.0;
        }
        return (density + (weight * pressure(a.level, b.level, y, barrierNoise, fromTen))) > 0.0;
    }

    [[nodiscard]] bool places(const aquifer::BarrierAt& at) {
        decidedByTen = false;
        const auto sim = [](const std::int64_t di, const std::int64_t dj) {
            return 1.0 -
                   static_cast<double>(dj - di) / static_cast<double>(aquifer::kSimilarityRange);
        };
        const double s12 = sim(at.nearest.distanceSq, at.second.distanceSq);
        if (s12 <= 0.0) {
            return false;
        }
        bool fromTen = false;
        if (term(at.density, s12, at.nearest, at.second, at.y, at.barrier, fromTen)) {
            decidedByTen = fromTen;
            return true;
        }
        const double s13 = sim(at.nearest.distanceSq, at.third.distanceSq);
        if (s13 > 0.0 &&
            term(at.density, s12 * s13, at.nearest, at.third, at.y, at.barrier, fromTen)) {
            decidedByTen = fromTen;
            return true;
        }
        const double s23 = sim(at.second.distanceSq, at.third.distanceSq);
        if (s23 > 0.0 &&
            term(at.density, s12 * s23, at.second, at.third, at.y, at.barrier, fromTen)) {
            decidedByTen = fromTen;
            return true;
        }
        return false;
    }
};

struct Score {
    long long blocks = 0;
    long long serverStone = 0;
    long long misses = 0;
    long long falseStone = 0;
    long long decidedByTen = 0;
    /// Blocks where divisor 10 and divisor 3 give different verdicts.
    long long contested = 0;
    /// Of those, the ones each divisor gets RIGHT.
    long long tenCorrect = 0;
    long long threeCorrect = 0;
    long long committedMismatches = 0;
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
    std::size_t dimensionsScored = 0;

    for (const char* probeName : kProbeDirs) {
        const std::filesystem::path probeDir = fixtures() / "probes" / probeName;
        const std::filesystem::path manifestPath = probeDir / "manifest.json";
        const std::filesystem::path specPath = probeDir / "spec.json";
        if (!std::filesystem::is_regular_file(manifestPath) ||
            !std::filesystem::is_regular_file(specPath)) {
            continue;
        }
        std::ifstream manifestFile(manifestPath);
        const nlohmann::json manifest = nlohmann::json::parse(manifestFile);
        const auto seed = manifest.at("seed").get<std::int64_t>();
        std::ifstream specFile(specPath);
        const nlohmann::json spec = nlohmann::json::parse(specFile);

        for (const Dimension& dim : kDimensions) {
            const std::filesystem::path regionPath = probeDir / dim.name / "r.0.0.mca";
            if (!std::filesystem::is_regular_file(regionPath)) {
                continue;
            }
            // The world on disk must be the world this test believes it is.
            // A spec that has drifted from `kDimensions` would score real
            // blocks against the wrong density and quietly pass or fail for
            // the wrong reason (SPEC §8: no silent best-effort).
            bool found = false;
            for (const auto& entry : spec) {
                if (entry.at("name").get<std::string>() != dim.name) {
                    continue;
                }
                found = true;
                CHECK(entry.at("raw_final_density").at("argument").get<double>() ==
                      Catch::Approx(dim.density));
                CHECK(entry.at("router").at("preliminary_surface_level").get<double>() ==
                      Catch::Approx(static_cast<double>(dim.psl)));
                CHECK(entry.at("router").at("fluid_level_floodedness").get<double>() ==
                      Catch::Approx(kLadderFloodedness));
                CHECK(entry.at("sea_level").get<std::int32_t>() == kSeaLevel);
            }
            REQUIRE(found);

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
            const aquifer::CentreSource centres(seed);
            const aquifer::PslRead surface = aquifer::constantSurface(dim.psl);

            const auto file = region::RegionFile::open(regionPath);
            Ablation asTen{.tenDivisor = 10.0};
            Ablation asThree{.tenDivisor = 3.0};

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

                                const aquifer::Selection sel =
                                    aquifer::selectSources(centres, x, y, z);
                                std::array<aquifer::BarrierSource, 3> src{};
                                for (std::size_t r = 0; r < 3; ++r) {
                                    const auto& s = sel.ranked[r];
                                    const aquifer::SamplePos sp =
                                        aquifer::spreadSample(s.cell, s.centre);
                                    const double spread = interp.evaluate(
                                        spreadNode, density::Point{.x = sp.x, .y = sp.y, .z = sp.z},
                                        cache);
                                    const aquifer::CellFluid cell{.centreY = s.centre.y,
                                                                  .surface = surface,
                                                                  .seaLevel = kSeaLevel,
                                                                  .floodedness = kLadderFloodedness,
                                                                  .spread = spread};
                                    const std::int32_t level = aquifer::cellFluidLevel(cell);
                                    src[r] = aquifer::BarrierSource{
                                        .level = level,
                                        .distanceSq = s.distanceSq,
                                        .type = aquifer::fluidTypeOf(
                                            aquifer::FluidTypeAt{.centreY = s.centre.y,
                                                                 .level = level,
                                                                 .seaLevel = kSeaLevel,
                                                                 .lava = 0.0})};
                                }

                                aquifer::BarrierAt at;
                                at.y = y;
                                at.density = dim.density;
                                at.nearest = src[0];
                                at.second = src[1];
                                at.third = src[2];
                                at.barrier = interp.evaluate(
                                    barrierNode, density::Point{.x = x, .y = y, .z = z}, cache);

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
                                if (asTen.decidedByTen) {
                                    ++pooled.decidedByTen;
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
                            }
                        }
                    }
                }
            }
            ++dimensionsScored;
        }
    }

    // A probe directory that exists but holds none of the dimensions this
    // case reads is a broken corpus, not an absent one — fail loudly rather
    // than passing on an empty tally (SPEC §8).
    REQUIRE(dimensionsScored > 0);

    INFO("blocks " << pooled.blocks << ", server stone " << pooled.serverStone << ", /10-decided "
                   << pooled.decidedByTen << ", contested " << pooled.contested);

    // The ablation at divisor 10 IS the committed predicate. Nothing below
    // means anything if this fails.
    REQUIRE(pooled.committedMismatches == 0);

    // The arm must actually be doing work — this is the assertion that goes
    // red if it ever falls back out of reach, which is how it stayed
    // unmeasured for four campaigns.
    CHECK(pooled.decidedByTen > 1000);
    CHECK(pooled.contested > 100);

    // Exact against the server: nothing unwritten, nothing invented.
    CHECK(pooled.misses == 0);
    CHECK(pooled.falseStone == 0);

    // And the divisor itself, by ablation: on every block where 10 and 3
    // disagree, the server sides with 10.
    CHECK(pooled.tenCorrect == pooled.contested);
    CHECK(pooled.threeCorrect == 0);
}
