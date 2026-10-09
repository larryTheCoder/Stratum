// Stratum — the lava override's level ceiling, replayed against the server.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// `fluidTypeOf` turns a source to lava when `|lava| > 0.3` and its level is
// at most -10, inclusively, and -10 is an absolute constant: not relative to
// `sea_level`, not to lambda. The 0.3 strictness and the -10 side are
// replayed on the fluid-type probe (vanilla_aquifer_fluid_boundaries_test.cpp);
// a level of exactly -9 is not, because no ladder level there lands on it,
// and one sea level cannot tell -10 from `sea_level - 73` at the shipped sea.
// Until this case only aquifer-fluidtype-analyze had read the worlds that
// decide both.
//
// tools/analysis/aquifer-fluidceiling-probe.sh builds them: three arms, one
// corpus each, every router entry a constant and `lava` 0.5, so the top of
// every column's fluid body is one level and one type a dimension:
//
//   fluidceiling_s42  arm Q   the sea branch: the level IS `sea_level`,
//                             -12 .. -7;
//   fluidceilingp_s42 arm P   the psl cap at sea -16: the level is
//                             floor(psl), -12 .. -7;
//   fluidceilingl_s42 arm P'  arm P at sea -70 (lambda -70): -12, -10, -9, -8.
//
// First, block for block, every fourth column over the rows the aquifer
// decides here: the filler's own path (the global picker above y_skip,
// `aquifer::computeSubstance` at and below it) against the server's block,
// with fluid that moved allowed through support/fluid_flow.hpp's shapes and
// nothing else. Then each dimension is read off the server alone, on every
// column of the window: the level its fluid body tops out at, and the fluid
// there. Those readings decide the rule — -10 lava and -9 water on all three
// arms — and refute its four rivals by name: a ceiling of -9, a strict one
// (-11), one relative to the sea (`L <= sea_level - 73`) and one relative to
// lambda (`L <= lambda + 44`), each identical to -10 at the shipped sea.
//
// Below the readout, the other fluid can appear: a source centred under the
// lava sea is lava whatever its level, and on arms P and P' such sources own
// the first rows above lambda in the water dimensions. The case holds it to
// a band there, in the model and on the server, rather than counting it.
//
// Counts the model alone decides are pinned exactly; counts the server's
// frozen remnant of flow could move are bounded. One seed: the level and the
// type at the top of every column are the arm's by construction, so a second
// seed moves only the jitter, and with it how many blocks of that band are
// lava. About 30 s in a Debug build.
//
// The fixtures are Mojang-derived and never committed (SPEC §12).
#include "support/fluid_flow.hpp"
#include "support/probe_corpus.hpp"

#include <stratum/aquifer/fluid_type.hpp>
#include <stratum/aquifer/lattice.hpp>
#include <stratum/aquifer/sampling.hpp>
#include <stratum/aquifer/substance.hpp>
#include <stratum/chunk/chunk.hpp>
#include <stratum/javamath.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace aquifer = stratum::aquifer;
namespace javamath = stratum::javamath;
using stratum::test::Category;

constexpr std::int64_t kSeed = 42;
constexpr const char* kScript = "tools/analysis/aquifer-fluidceiling-probe.sh";
constexpr std::int32_t kChunks = 8;
constexpr std::int32_t kWindow = kChunks * 16;
/// Every column of the force-loaded window: the readout's population.
constexpr long long kWindowColumns = static_cast<long long>(kWindow) * kWindow;
/// Two blocks in from the window's edge on every side, so that a block's
/// horizontal neighbours — which the flow classifier reads — are in what
/// the server generated.
constexpr std::int32_t kEdgeMargin = 2;
/// The replay's column stride on both axes.
constexpr std::int32_t kStride = 4;
/// Columns of the replay: 31 a side.
constexpr long long kReplayColumns = 31LL * 31LL;
/// Rows replayed below lambda: the lava sea's top, where water meets it.
constexpr std::int32_t kRowsBelowLambda = 4;
/// Rows replayed above the higher of the level and the sea: the barrier's
/// lid sits a few blocks over a level, and past this every reading is air.
constexpr std::int32_t kRowsAboveTop = 16;
/// How far above lambda the two fluids can meet. A source centred below the
/// lava sea is lava whatever its level, and owns blocks a cell or so above
/// lambda (to lambda + 9 on arm P and lambda + 10 on arm P' at seed 42; none
/// on arm Q); water meets the sea's own top at lambda - 1. Two cells: above
/// this every fluid block is the dimension's one fluid, and nothing flows.
constexpr std::int32_t kBandAboveLambda = 2 * aquifer::kCellPitchY;
/// Flow allowed per replayed column, on average: the frozen remnant leaves
/// about one block a column on a water arm, most of it obsidian where water
/// sits on the lava sea at lambda - 1.
constexpr long long kFlowPerColumn = 3;

/// The probe's three arms: which corpus, and where the level comes from.
struct Arm {
    const char* corpus;
    const char* label;
    /// Arm Q: floodedness past the sea gate, so the level IS `sea_level`.
    /// Otherwise the psl cap binds and the level is floor(psl).
    bool levelIsSea;
    /// Every dimension the arm declares, in the script's order.
    std::array<const char*, 6> dims;
    std::size_t dimCount;
};

constexpr std::array<Arm, 3> kArms{{
    {"fluidceiling",
     "Q (the sea branch)",
     true,
     {"q_sea_m12", "q_sea_m11", "q_sea_m10", "q_sea_m9", "q_sea_m8", "q_sea_m7"},
     6},
    {"fluidceilingp",
     "P (the psl cap, sea -16)",
     false,
     {"p_psl_m12", "p_psl_m11", "p_psl_m10", "p_psl_m9", "p_psl_m8", "p_psl_m7"},
     6},
    {"fluidceilingl",
     "P' (the psl cap, sea -70)",
     false,
     {"p70_psl_m12", "p70_psl_m10", "p70_psl_m9", "p70_psl_m8", "", ""},
     4},
}};

[[nodiscard]] std::filesystem::path probes() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11" / "probes";
}

[[nodiscard]] std::filesystem::path corpusDir(const Arm& arm) {
    return probes() / (std::string(arm.corpus) + "_s" + std::to_string(kSeed));
}

/// Exact comparison by bits: the project keeps -Wfloat-equal on, and every
/// value compared here is a constant a spec wrote, meant exactly.
[[nodiscard]] bool same(const double a, const double b) {
    return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

[[nodiscard]] double constantEntry(const nlohmann::json& router, const char* name) {
    INFO("router entry " << name);
    REQUIRE(router.at(name).is_number());
    return router.at(name).get<double>();
}

[[nodiscard]] auto constant(const double value) {
    return [value](std::int32_t, std::int32_t, std::int32_t) { return value; };
}

/// One dimension as declared, refusing anything this case cannot replay.
struct Dim {
    std::string name;
    std::int32_t sea = 0;
    std::int32_t minY = 0;
    std::int32_t height = 0;
    double psl = 0.0;
    double floodedness = 0.0;
    double spread = 0.0;
    double barrier = 0.0;
    double lava = 0.0;
};

[[nodiscard]] Dim parseDim(const nlohmann::json& entry) {
    Dim dim;
    dim.name = entry.at("name").get<std::string>();
    INFO("dimension " << dim.name);
    REQUIRE(entry.at("raw_final_density").at("type").get<std::string>() == "minecraft:constant");
    REQUIRE(same(entry.at("raw_final_density").at("argument").get<double>(), -1.0));
    REQUIRE(entry.at("aquifers_enabled").get<bool>());
    REQUIRE(entry.at("default_fluid").at("Name").get<std::string>() == "minecraft:water");
    dim.sea = entry.at("sea_level").get<std::int32_t>();
    dim.minY = entry.at("min_y").get<std::int32_t>();
    dim.height = entry.at("height").get<std::int32_t>();
    const nlohmann::json& router = entry.at("router");
    dim.psl = constantEntry(router, "preliminary_surface_level");
    dim.floodedness = constantEntry(router, "fluid_level_floodedness");
    dim.spread = constantEntry(router, "fluid_level_spread");
    dim.barrier = constantEntry(router, "barrier");
    dim.lava = constantEntry(router, "lava");
    // Past the threshold under either strictness: only the level decides.
    REQUIRE((dim.lava < 0.0 ? -dim.lava : dim.lava) > aquifer::kLavaThreshold);
    return dim;
}

/// The arm's own level for @p dim: what the probe is built to put on the
/// sources that hold the top of every column, and what the readouts below
/// are checked against.
[[nodiscard]] std::int32_t intendedLevel(const Arm& arm, const Dim& dim) {
    return arm.levelIsSea ? dim.sea : javamath::floorToInt(dim.psl);
}

[[nodiscard]] Category categoryOf(const aquifer::SubstanceAt& substance) {
    switch (substance.substance) {
        case aquifer::Substance::Air:
            return Category::Air;
        case aquifer::Substance::Solid:
            return Category::Solid;
        case aquifer::Substance::Fluid:
            return substance.fluidType == aquifer::FluidType::Lava ? Category::Lava
                                                                   : Category::Water;
    }
    return Category::Solid;
}

/// What this build places at (x, y, z): ChunkFiller's own path with the
/// dimension's constants — the global picker above the chunk's y_skip
/// (Q2.3), `aquifer::computeSubstance` at and below it.
[[nodiscard]] Category built(const aquifer::CentreSource& centres, const Dim& dim,
                             const std::int32_t ySkipLevel, aquifer::StatusCache& statuses,
                             const std::int32_t x, const std::int32_t y, const std::int32_t z) {
    if (!aquifer::consultsLattice(y, ySkipLevel)) {
        if (y < aquifer::lambdaLevel(dim.sea)) {
            return Category::Lava;
        }
        return y < dim.sea ? Category::Water : Category::Air;
    }
    return categoryOf(aquifer::computeSubstance(
        centres,
        aquifer::AquiferQuery{.x = x, .y = y, .z = z, .density = -1.0, .seaLevel = dim.sea},
        statuses, constant(dim.barrier), constant(dim.floodedness), constant(dim.spread),
        constant(dim.lava), constant(dim.psl),
        // Erosion and depth are 0 in a probe: Q5.9 cannot fire.
        aquifer::NoDeepDark{}));
}

/// One dimension, read off the server alone and replayed.
struct Tally {
    // The replay, every kStride-th column, rows lambda - 4 .. top.
    long long blocks = 0;
    long long agree = 0;
    long long flow = 0;
    long long unexplained = 0;
    /// Flow above the band where the two fluids meet: none moves there.
    long long flowAboveBand = 0;
    std::string samples; ///< the first few unexplained blocks, for the failure
    /// Model only: replayed columns whose fluid tops out at the intended
    /// level in the predicate's fluid; replayed blocks at or above lambda
    /// holding the other fluid, and of those, the ones above the band.
    long long oursColumnsAtLevel = 0;
    long long oursOther = 0;
    long long oursOtherAboveBand = 0;

    // The server's readout.
    /// Every column of the window, by the level its topmost fluid SOURCE at
    /// or above lambda reads (its y + 1), and that source's fluid: [0]
    /// water, [1] lava. Columns with none are not counted.
    std::map<std::int32_t, std::array<long long, 2>> serverColumnsByLevel;
    /// In the replayed columns: blocks of the other fluid, any `level`
    /// property, from lambda up, and of those, the ones above the band; and
    /// fluid of either kind above the top row.
    long long serverOther = 0;
    long long serverOtherAboveBand = 0;
    long long serverFluidAboveTop = 0;
};

[[nodiscard]] std::string describe(const Tally& t) {
    std::string out =
        "replay: blocks " + std::to_string(t.blocks) + ", agree " + std::to_string(t.agree) +
        ", flow " + std::to_string(t.flow) + " (above the band " + std::to_string(t.flowAboveBand) +
        "), unexplained " + std::to_string(t.unexplained) + "; ours: at the level " +
        std::to_string(t.oursColumnsAtLevel) + " of " + std::to_string(kReplayColumns) +
        " columns, other fluid " + std::to_string(t.oursOther) + " (above the band " +
        std::to_string(t.oursOtherAboveBand) + "); server: other fluid " +
        std::to_string(t.serverOther) + " (above the band " +
        std::to_string(t.serverOtherAboveBand) + "), fluid above the top row " +
        std::to_string(t.serverFluidAboveTop) + ", columns by level (water/lava):";
    for (const auto& [level, columns] : t.serverColumnsByLevel) {
        out += " " + std::to_string(level) + "=" + std::to_string(columns[0]) + "/" +
               std::to_string(columns[1]);
    }
    return out;
}

[[nodiscard]] Category blockCategory(const stratum::chunk::BlockState* block) {
    return stratum::test::categoryOf(block != nullptr ? block->name : std::string("minecraft:air"));
}

[[nodiscard]] Tally scoreDimension(const std::filesystem::path& dir, const Arm& arm, const Dim& dim,
                                   const aquifer::CentreSource& centres) {
    const std::filesystem::path region = dir / dim.name / "r.0.0.mca";
    REQUIRE(std::filesystem::is_regular_file(region));
    stratum::test::GoldenRegion golden(region);
    aquifer::StatusCache statuses; // every input constant: one per world

    const std::int32_t lambda = aquifer::lambdaLevel(dim.sea);
    const std::int32_t level = intendedLevel(arm, dim);
    // The type the build gives a source of this level and `lava`, centred at
    // lambda — at or above the lava sea, so its own test decides.
    const bool lava =
        aquifer::fluidTypeOf(aquifer::FluidTypeAt{.centreY = lambda,
                                                  .level = level,
                                                  .seaLevel = dim.sea,
                                                  .lava = dim.lava,
                                                  .origin = aquifer::LevelOrigin::Cell}) ==
        aquifer::FluidType::Lava;
    const Category fluid = lava ? Category::Lava : Category::Water;
    const Category other = lava ? Category::Water : Category::Lava;
    // y_skip on a constant surface is the same in every chunk.
    const std::int32_t ySkipLevel = aquifer::chunkYSkip(constant(dim.psl), 0, 0);
    const std::int32_t worldTop = dim.minY + dim.height - 1;
    const std::int32_t top = std::min(worldTop, std::max(dim.sea, level) + kRowsAboveTop);
    const std::int32_t bottom = std::max(dim.minY, lambda - kRowsBelowLambda);
    const std::int32_t bandTop = lambda + kBandAboveLambda;

    // Every chunk of the window, or the readout below is of a partial world.
    for (std::int32_t chunkZ = 0; chunkZ < kChunks; ++chunkZ) {
        for (std::int32_t chunkX = 0; chunkX < kChunks; ++chunkX) {
            INFO("chunk " << chunkX << ", " << chunkZ);
            REQUIRE(golden.hasChunk(chunkX, chunkZ));
        }
    }

    Tally tally;
    for (std::int32_t z = 0; z < kWindow; ++z) {
        for (std::int32_t x = 0; x < kWindow; ++x) {
            // The server's readout, every column: its topmost source.
            std::int32_t topSource = lambda - 1;
            Category topFluid = Category::Air;
            for (std::int32_t y = lambda; y <= top; ++y) {
                const stratum::chunk::BlockState* theirs = golden.blockAt(x, y, z);
                const Category g = blockCategory(theirs);
                if (stratum::test::isFluid(g) && stratum::test::fluidLevel(theirs) == 0) {
                    topSource = y;
                    topFluid = g;
                }
            }
            if (topSource >= lambda) {
                ++tally.serverColumnsByLevel[topSource + 1][topFluid == Category::Lava ? 1 : 0];
            }

            // The replay, every kStride-th column inside the margin.
            if (x < kEdgeMargin || z < kEdgeMargin || x >= kWindow - kEdgeMargin ||
                z >= kWindow - kEdgeMargin || (x - kEdgeMargin) % kStride != 0 ||
                (z - kEdgeMargin) % kStride != 0) {
                continue;
            }
            for (std::int32_t y = top + 1; y <= worldTop; ++y) {
                tally.serverFluidAboveTop += static_cast<long long>(
                    stratum::test::isFluid(blockCategory(golden.blockAt(x, y, z))));
            }
            std::int32_t oursTop = lambda - 1;
            bool oursTopIsFluid = true;
            for (std::int32_t y = bottom; y <= top; ++y) {
                const stratum::chunk::BlockState* theirs = golden.blockAt(x, y, z);
                const Category g = blockCategory(theirs);
                const Category r = built(centres, dim, ySkipLevel, statuses, x, y, z);
                if (y >= lambda) {
                    if (stratum::test::isFluid(r)) {
                        oursTop = y;
                        oursTopIsFluid = r == fluid;
                    }
                    tally.oursOther += static_cast<long long>(r == other);
                    tally.oursOtherAboveBand += static_cast<long long>(r == other && y > bandTop);
                    tally.serverOther += static_cast<long long>(g == other);
                    tally.serverOtherAboveBand += static_cast<long long>(g == other && y > bandTop);
                }
                ++tally.blocks;
                if (g == r) {
                    ++tally.agree;
                } else if (stratum::test::explainedByFlow(golden, x, y, z, g, r)) {
                    ++tally.flow;
                    tally.flowAboveBand += static_cast<long long>(y > bandTop);
                } else {
                    ++tally.unexplained;
                    if (tally.unexplained <= 8) {
                        tally.samples +=
                            " [server " +
                            (theirs != nullptr ? theirs->toString() : std::string("<none>")) +
                            ", ours " + std::to_string(static_cast<int>(r)) + " at " +
                            std::to_string(x) + " " + std::to_string(y) + " " + std::to_string(z) +
                            "]";
                    }
                }
            }
            tally.oursColumnsAtLevel +=
                static_cast<long long>(oursTop + 1 == level && oursTopIsFluid);
        }
    }
    return tally;
}

/// One dimension's reading, from the server's blocks alone.
struct Reading {
    std::string name;
    std::int32_t sea = 0;
    std::int32_t lambda = 0;
    std::int32_t level = 0;
    bool lava = false;
};

/// The ceiling as built, and four rivals, each identical to it at the
/// shipped sea (63, lambda -54) — which is why one sea level cannot decide.
enum Rule : std::uint8_t { Built, Minus9, Minus11, SeaRelative, LambdaRelative, kRules };

constexpr std::array<std::string_view, kRules> kRuleNames{
    "the build (L <= -10)", "L <= -9", "L <= -11 (strict at -10)", "L <= sea_level - 73",
    "L <= lambda + 44"};

[[nodiscard]] bool rulePredictsLava(const Rule rule, const Reading& reading, const double lava) {
    switch (rule) {
        case Built:
            return aquifer::fluidTypeOf(aquifer::FluidTypeAt{
                       .centreY = reading.lambda,
                       .level = reading.level,
                       .seaLevel = reading.sea,
                       .lava = lava,
                       .origin = aquifer::LevelOrigin::Cell}) == aquifer::FluidType::Lava;
        case Minus9:
            return reading.level <= -9;
        case Minus11:
            return reading.level <= -11;
        case SeaRelative:
            return reading.level <= reading.sea - 73;
        case LambdaRelative:
            return reading.level <= reading.lambda + 44;
        case kRules:
            break;
    }
    return false;
}

} // namespace

TEST_CASE("the lava override's level ceiling is -10 and absolute on three arms of the server",
          "[conformance][aquifer]") {
    std::size_t present = 0;
    for (const Arm& arm : kArms) {
        present += static_cast<std::size_t>(
            std::filesystem::is_regular_file(corpusDir(arm) / "spec.json"));
    }
    if (present == 0) {
        SKIP("no fluid-ceiling probes under " << probes() << "; generate them with " << kScript
                                              << " --accept-eula 42");
    }
    INFO(present << " of " << kArms.size()
                 << " fluid-ceiling corpora present; regenerate all three with " << kScript
                 << " --accept-eula 42");
    REQUIRE(present == kArms.size());

    const aquifer::CentreSource centres{kSeed, stratum::density::RandomSource::Xoroshiro};
    std::vector<Reading> readings;
    std::map<std::string, double> lavaOf;
    for (const Arm& arm : kArms) {
        const std::filesystem::path dir = corpusDir(arm);
        INFO("arm " << arm.label << ", corpus " << dir);
        stratum::test::requireFrozen(dir, kScript);
        stratum::test::requireSeed(dir, kSeed);
        std::ifstream specFile(dir / "spec.json");
        const nlohmann::json spec = nlohmann::json::parse(specFile);
        REQUIRE(spec.is_array());
        // Every dimension the arm is built from, and nothing else.
        REQUIRE(spec.size() == arm.dimCount);
        for (std::size_t i = 0; i < arm.dimCount; ++i) {
            REQUIRE(spec.at(i).at("name").get<std::string>() == arm.dims.at(i));
        }

        for (const auto& entry : spec) {
            const Dim dim = parseDim(entry);
            const std::int32_t level = intendedLevel(arm, dim);
            const std::int32_t lambda = aquifer::lambdaLevel(dim.sea);
            INFO("dimension " << dim.name << " (sea " << dim.sea << ", psl " << dim.psl
                              << ", floodedness " << dim.floodedness << ", spread " << dim.spread
                              << ", lava " << dim.lava << "; level " << level << ", lambda "
                              << lambda << ")");
            const Tally tally = scoreDimension(dir, arm, dim, centres);
            WARN(dim.name << ": " << describe(tally));
            INFO("first unexplained:" << tally.samples);

            // Block for block: whatever disagrees is fluid that moved, and
            // only in the band where the two fluids meet.
            CHECK(tally.unexplained == 0);
            CHECK(tally.flowAboveBand == 0);
            // Bounded, not pinned: the frozen remnant moves it run to run.
            CHECK(tally.flow <= kFlowPerColumn * kReplayColumns);
            // Model only, so exact: every replayed column tops out at the
            // arm's level in the predicate's fluid, and the other fluid (a
            // source centred below lambda is lava) stays in the band.
            CHECK(tally.oursColumnsAtLevel == kReplayColumns);
            CHECK(tally.oursOtherAboveBand == 0);

            // The server's reading, from its blocks alone. Nothing carries a
            // fluid upward, so these zeros hold whatever the remnant did.
            CHECK(tally.serverFluidAboveTop == 0);
            CHECK(tally.serverOtherAboveBand == 0);
            // Its fluid tops out at the arm's level in one fluid: in nearly
            // every column of the window (bounded), and in none with the
            // other fluid, which never reaches that high (exact).
            const auto atLevel = tally.serverColumnsByLevel.find(level);
            REQUIRE(atLevel != tally.serverColumnsByLevel.end());
            const long long waterColumns = atLevel->second[0];
            const long long lavaColumns = atLevel->second[1];
            const bool serverLava = lavaColumns > waterColumns;
            CHECK(std::min(waterColumns, lavaColumns) == 0);
            CHECK(std::max(waterColumns, lavaColumns) * 100 >= kWindowColumns * 99);
            readings.push_back(Reading{.name = dim.name,
                                       .sea = dim.sea,
                                       .lambda = lambda,
                                       .level = level,
                                       .lava = serverLava});
            lavaOf[dim.name] = dim.lava;
        }
    }

    // The readings decide the rule. Every one the build's predicate gets
    // right; every rival gets at least one wrong.
    REQUIRE(readings.size() == 16U);
    std::array<std::vector<std::string>, kRules> wrong;
    for (const Reading& reading : readings) {
        for (std::size_t r = 0; r < kRules; ++r) {
            if (rulePredictsLava(static_cast<Rule>(r), reading, lavaOf.at(reading.name)) !=
                reading.lava) {
                wrong.at(r).push_back(reading.name);
            }
        }
    }
    for (std::size_t r = 0; r < kRules; ++r) {
        std::string names;
        for (const std::string& name : wrong.at(r)) {
            names += " " + name;
        }
        WARN(kRuleNames.at(r) << " is wrong on " << wrong.at(r).size() << " of " << readings.size()
                              << " readings:" << names);
        if (r == Built) {
            CHECK(wrong.at(r).empty());
        } else {
            CHECK(!wrong.at(r).empty());
        }
    }

    // By name: -10 is lava and -9 water on every arm — the sea branch, and
    // the psl cap at sea -16 and at sea -70 (lambda -70).
    const auto readingOf = [&](const std::string_view name) {
        const auto found = std::ranges::find_if(
            readings, [&](const Reading& reading) { return reading.name == name; });
        INFO("no reading for " << name);
        REQUIRE(found != readings.end());
        return *found;
    };
    for (const std::string_view name : {"q_sea_m12", "q_sea_m11", "q_sea_m10", "p_psl_m12",
                                        "p_psl_m11", "p_psl_m10", "p70_psl_m12", "p70_psl_m10"}) {
        INFO(name);
        CHECK(readingOf(name).lava);
    }
    for (const std::string_view name : {"q_sea_m9", "q_sea_m8", "q_sea_m7", "p_psl_m9", "p_psl_m8",
                                        "p_psl_m7", "p70_psl_m9", "p70_psl_m8"}) {
        INFO(name);
        CHECK(!readingOf(name).lava);
    }
}
