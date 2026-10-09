// Stratum — the fluid type's two boundaries, replayed against the server.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// `fluidTypeOf` turns a source to lava when `|lava| > 0.3` — strictly — and
// its level is at most -10 — inclusively. Both were settled on the server by
// `tools/analysis/aquifer-fluidtype-probe.sh` (SPEC §11), and until this case
// only the analyzer had ever read that world back: the one conformance case
// on fluid type (`vanilla_aquifer_fluid_type_test.cpp`) reads real noise,
// which never lands on exactly 0.3 or on a level of exactly -10, and so is
// blind to both.
//
// Every router entry in the probe's dimensions is a constant, so each world
// is the aquifer's decision and nothing else: this replays
// `aquifer::computeSubstance` — the call the filler makes — at every block of
// every fourth column, against the block the server wrote. The probe
// force-loads its chunks without freezing ticks, so water the server's fluid
// ticks moved is allowed through `support/fluid_flow.hpp`'s shapes and
// nothing else — the 42 blocks of stone on row lambda it once set apart as a
// barrier residual included: lava fell onto water there. Then the two boundaries are pinned by
// name: `lava` exactly 0.3 makes no lava and the next double up does, and a level of exactly -10 is
// lava while -9 is not.
//
// The fixture is Mojang-derived and never committed (SPEC §12).
#include "support/fluid_flow.hpp"
#include "support/probe_corpus.hpp"

#include <stratum/aquifer/lattice.hpp>
#include <stratum/aquifer/substance.hpp>
#include <stratum/chunk/chunk.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

namespace {

using stratum::test::Category;

constexpr std::int32_t kChunks = 8;
constexpr std::int32_t kColumnStride = 4;
/// Two blocks in from the force-loaded window's edge on every side, so that
/// a block's horizontal neighbours — which the flow classifier reads — are
/// inside the window the server generated.
constexpr std::int32_t kEdgeMargin = 2;
constexpr std::int64_t kSeed = 42;

[[nodiscard]] std::filesystem::path fixtures() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11";
}

/// A router entry the probe declared, which this case requires to be a
/// constant: anything else is a world it cannot replay.
[[nodiscard]] double constantEntry(const nlohmann::json& router, const char* name) {
    INFO("router entry " << name);
    REQUIRE(router.at(name).is_number());
    return router.at(name).get<double>();
}

[[nodiscard]] Category expectedCategory(const stratum::aquifer::SubstanceAt& substance) {
    switch (substance.substance) {
        case stratum::aquifer::Substance::Air:
            return Category::Air;
        case stratum::aquifer::Substance::Solid:
            return Category::Solid;
        case stratum::aquifer::Substance::Fluid:
            return substance.fluidType == stratum::aquifer::FluidType::Lava ? Category::Lava
                                                                            : Category::Water;
    }
    return Category::Solid;
}

struct Tally {
    long long blocks = 0;
    long long agree = 0;
    long long flow = 0;
    long long unexplained = 0;
    /// Lava SOURCES the aquifer placed — above the global lava sea, which
    /// every dimension has below min(-54, sea_level) whatever its sources
    /// hold.
    long long serverLava = 0;
    long long oursLava = 0;
    /// Our lava source where the server has obsidian: water flowed onto it.
    long long oursLavaQuenched = 0;
};

} // namespace

TEST_CASE("the fluid type's two boundaries, block for block against the server",
          "[conformance][aquifer]") {
    const std::filesystem::path probe = fixtures() / "probes" / "fluidtype";
    if (!std::filesystem::is_regular_file(probe / "spec.json")) {
        SKIP("no fluid-type probe at "
             << probe
             << "; generate it with "
                "tools/analysis/aquifer-fluidtype-probe.sh --accept-eula");
    }
    stratum::test::requireFrozen(probe, "tools/analysis/aquifer-fluidtype-probe.sh");
    std::ifstream manifestFile(probe / "manifest.json");
    REQUIRE(nlohmann::json::parse(manifestFile).at("seed").get<std::int64_t>() == kSeed);
    std::ifstream specFile(probe / "spec.json");
    const nlohmann::json spec = nlohmann::json::parse(specFile);
    const stratum::aquifer::CentreSource centres{kSeed, stratum::density::RandomSource::Xoroshiro};

    std::map<std::string, Tally> byDimension;
    for (const auto& entry : spec) {
        const std::string name = entry.at("name").get<std::string>();
        INFO("dimension " << name);
        const nlohmann::json& router = entry.at("router");
        const double density = entry.at("raw_final_density").at("argument").get<double>();
        const auto seaLevel = entry.at("sea_level").get<std::int32_t>();
        const auto minY = entry.at("min_y").get<std::int32_t>();
        const auto height = entry.at("height").get<std::int32_t>();
        const double barrier = constantEntry(router, "barrier");
        const double floodedness = constantEntry(router, "fluid_level_floodedness");
        const double spread = constantEntry(router, "fluid_level_spread");
        const double lava = constantEntry(router, "lava");
        const double psl = constantEntry(router, "preliminary_surface_level");
        const auto constant = [](double value) {
            return [value](std::int32_t, std::int32_t, std::int32_t) { return value; };
        };
        const std::int32_t lambda = stratum::aquifer::lambdaLevel(seaLevel);

        const std::filesystem::path region = probe / name / "r.0.0.mca";
        REQUIRE(std::filesystem::is_regular_file(region));
        stratum::test::GoldenRegion golden(region);
        stratum::aquifer::StatusCache statuses; // every input constant: one per world
        Tally& tally = byDimension[name];
        std::string samples; // the first few unexplained blocks, for the failure
        for (std::int32_t z = kEdgeMargin; z < (kChunks * 16) - kEdgeMargin; z += kColumnStride) {
            for (std::int32_t x = kEdgeMargin; x < (kChunks * 16) - kEdgeMargin;
                 x += kColumnStride) {
                REQUIRE(golden.hasChunk(x / 16, z / 16));
                for (std::int32_t y = minY; y < minY + height; ++y) {
                    const auto* theirs = golden.blockAt(x, y, z);
                    const Category g = stratum::test::categoryOf(
                        theirs != nullptr ? theirs->name : std::string("minecraft:air"));
                    const stratum::aquifer::SubstanceAt ours = stratum::aquifer::computeSubstance(
                        centres,
                        stratum::aquifer::AquiferQuery{
                            .x = x, .y = y, .z = z, .density = density, .seaLevel = seaLevel},
                        statuses, constant(barrier), constant(floodedness), constant(spread),
                        constant(lava), constant(psl),
                        // Erosion and depth are 0 in a probe: Q5.9 cannot fire.
                        stratum::aquifer::NoDeepDark{});
                    const Category r = expectedCategory(ours);
                    ++tally.blocks;
                    if (y >= lambda) {
                        // SOURCES only: flowing lava is the server's ticks,
                        // which the first pass never places.
                        tally.serverLava +=
                            (g == Category::Lava && stratum::test::fluidLevel(theirs) == 0) ? 1 : 0;
                        tally.oursLava += r == Category::Lava ? 1 : 0;
                        tally.oursLavaQuenched +=
                            (r == Category::Lava && stratum::test::fluidContactBlock(theirs)) ? 1
                                                                                              : 0;
                    }
                    if (g == r) {
                        ++tally.agree;
                    } else if (stratum::test::explainedByFlow(golden, x, y, z, g, r)) {
                        ++tally.flow;
                    } else {
                        ++tally.unexplained;
                        if (tally.unexplained <= 8) {
                            samples +=
                                " [server " +
                                (theirs != nullptr ? theirs->toString() : std::string("<none>")) +
                                ", ours " + std::to_string(static_cast<int>(r)) + " at " +
                                std::to_string(x) + " " + std::to_string(y) + " " +
                                std::to_string(z) + "]";
                        }
                    }
                }
            }
        }
        INFO("first unexplained:" << samples);
        CHECK(tally.unexplained == 0);
        CHECK(tally.agree > tally.blocks * 9 / 10);
    }

    // Every arm the probe declares, read: a missing one failed above.
    REQUIRE(byDimension.size() == spec.size());

    // Strict at 0.3: exactly 0.3 makes no lava, the next double up makes it.
    REQUIRE(byDimension.contains("a_exact"));
    REQUIRE(byDimension.contains("a_above"));
    CHECK(byDimension.at("a_exact").serverLava == 0);
    CHECK(byDimension.at("a_exact").oursLava == 0);
    CHECK(byDimension.at("a_above").serverLava > 0);
    // Every lava source this build places is one the server has, or one its
    // water quenched to obsidian afterwards.
    CHECK(byDimension.at("a_above").oursLava ==
          byDimension.at("a_above").serverLava + byDimension.at("a_above").oursLavaQuenched);

    // Inclusive at -10: a ladder level of exactly -10 is lava, -9 is not.
    REQUIRE(byDimension.contains("c_neg10"));
    REQUIRE(byDimension.contains("c_neg9"));
    CHECK(byDimension.at("c_neg10").serverLava > 0);
    CHECK(byDimension.at("c_neg10").oursLava ==
          byDimension.at("c_neg10").serverLava + byDimension.at("c_neg10").oursLavaQuenched);
    CHECK(byDimension.at("c_neg9").oursLava ==
          byDimension.at("c_neg9").serverLava + byDimension.at("c_neg9").oursLavaQuenched);
}
