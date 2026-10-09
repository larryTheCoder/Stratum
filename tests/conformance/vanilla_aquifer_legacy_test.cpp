// Stratum — the aquifer under `legacy_random_source`, against the server.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// No vanilla dimension combines the legacy source with aquifers, so nothing
// on disk said where the lattice's cells sit under it, and this build refused
// the pair by name. The server generates the pair when a datapack asks, and
// tools/analysis/legacy-aquifer-probe.sh asks: one world per seed, four
// dimensions in it —
//
//   lj   open void, every aquifer input a constant, legacy_random_source TRUE
//   mj   the same dimension with the flag false: the control
//   lc   a named noise read with the flag on, mc with it off: the POSITIVE
//        control, which says the flag reached this world at all
//
// With every input a constant a block is the lattice's decision alone, so a
// whole arm can be replayed through `aquifer::computeSubstance` — the call
// the filler makes — and scored block for block, fluid the server's ticks
// moved after generation set apart by support/fluid_flow.hpp's shapes and
// nothing else. No server output is ever compared with other server output
// here: each arm is scored against the forward model, with one centre source
// or the other.
//
// What the worlds say (SPEC §11, "The aquifer under the legacy source"): the
// flag reaches the lattice, and the legacy centres are java.util.Random's —
// two forks around String.hashCode("minecraft:aquifer"), the position mix
// XORed into the stream seed, then nextInt(10), (9), (10). That rule was the
// one survivor of 7200 candidates on seed 42 alone
// (tools/analysis/legacy-aquifer-analyze.cpp), frozen, and then confirmed on
// 31337 and on 42's 48-bit twin, which were generated afterwards.
//
// The fixtures are Mojang-derived and never committed (SPEC §12).
#include "support/fluid_flow.hpp"
#include "support/probe_corpus.hpp"
#include "support/probe_settings.hpp"
#include "support/temp_path.hpp"

#include <stratum/aquifer/lattice.hpp>
#include <stratum/aquifer/substance.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/density/random_source.hpp>
#include <stratum/javamath.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/rule_graph.hpp>
#include <stratum/terrain/filler.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <system_error>

namespace {

using stratum::density::RandomSource;
using stratum::test::Category;

constexpr const char* kScript = "tools/analysis/legacy-aquifer-probe.sh";
constexpr std::int64_t kTwin = 42 ^ std::numeric_limits<std::int64_t>::min();
constexpr std::array<std::int64_t, 3> kSeeds{42, 31337, kTwin};
constexpr std::int32_t kChunks = 8;
constexpr std::int32_t kWindow = kChunks * 16;
/// Every fourth column, two blocks in from the force-loaded window's edge so
/// that the flow classifier's horizontal neighbours are inside it.
constexpr std::int32_t kColumnStride = 4;
constexpr std::int32_t kEdgeMargin = 2;
/// The lava band: under the probe's constants (`lava` -1, so a level of -10
/// or below is lava) the cells of layers -5 and -4 hold lava or nothing down
/// to the global lava sea's top, and a golden lava SOURCE is never one of
/// fluid_flow.hpp's shapes. So a wrong lattice's disagreements here cannot be
/// forgiven as flow at all.
constexpr std::int32_t kLavaBandFrom = -54;
constexpr std::int32_t kLavaBandTo = -37;

[[nodiscard]] std::filesystem::path probeDir(const std::int64_t seed) {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11" / "probes" /
           ("aqlegacy_s" + std::to_string(seed));
}

[[nodiscard]] std::optional<nlohmann::json> loadSpec(const std::int64_t seed) {
    std::ifstream in(probeDir(seed) / "spec.json");
    if (!in) {
        return std::nullopt;
    }
    return nlohmann::json::parse(in);
}

[[nodiscard]] const nlohmann::json& entryNamed(const nlohmann::json& spec, const char* name) {
    for (const auto& entry : spec) {
        if (entry.at("name") == name) {
            return entry;
        }
    }
    FAIL("the probe's spec has no arm " << name);
    return spec; // unreachable: FAIL throws
}

/// The arms' declared constants, which this case requires to be constants:
/// anything else is a world it cannot replay.
struct Arm {
    double density = 0.0;
    std::int32_t seaLevel = 0;
    std::int32_t minY = 0;
    std::int32_t height = 0;
    double barrier = 0.0;
    double floodedness = 0.0;
    double spread = 0.0;
    double lava = 0.0;
    double psl = 0.0;
};

[[nodiscard]] double constantEntry(const nlohmann::json& router, const char* name) {
    INFO("router entry " << name);
    REQUIRE(router.at(name).is_number());
    return router.at(name).get<double>();
}

[[nodiscard]] Arm armOf(const nlohmann::json& entry) {
    const nlohmann::json& router = entry.at("router");
    REQUIRE(entry.at("raw_final_density").at("type") == "minecraft:constant");
    return Arm{.density = entry.at("raw_final_density").at("argument").get<double>(),
               .seaLevel = entry.at("sea_level").get<std::int32_t>(),
               .minY = entry.at("min_y").get<std::int32_t>(),
               .height = entry.at("height").get<std::int32_t>(),
               .barrier = constantEntry(router, "barrier"),
               .floodedness = constantEntry(router, "fluid_level_floodedness"),
               .spread = constantEntry(router, "fluid_level_spread"),
               .lava = constantEntry(router, "lava"),
               .psl = constantEntry(router, "preliminary_surface_level")};
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
    /// Unexplained, in the lava band.
    long long unexplainedLava = 0;
};

/// One arm of one world, replayed with the centres @p centres.
[[nodiscard]] Tally replay(const std::filesystem::path& region, const Arm& arm,
                           const stratum::aquifer::CentreSource& centres) {
    stratum::test::GoldenRegion golden(region);
    stratum::aquifer::StatusCache statuses; // every input constant: one per world
    const auto constant = [](double value) {
        return [value](std::int32_t, std::int32_t, std::int32_t) { return value; };
    };
    Tally tally;
    for (std::int32_t z = kEdgeMargin; z < kWindow - kEdgeMargin; z += kColumnStride) {
        for (std::int32_t x = kEdgeMargin; x < kWindow - kEdgeMargin; x += kColumnStride) {
            REQUIRE(golden.hasChunk(x / 16, z / 16));
            for (std::int32_t y = arm.minY; y < arm.minY + arm.height; ++y) {
                const auto* theirs = golden.blockAt(x, y, z);
                const Category g = stratum::test::categoryOf(
                    theirs != nullptr ? theirs->name : std::string("minecraft:air"));
                const Category r = expectedCategory(stratum::aquifer::computeSubstance(
                    centres,
                    stratum::aquifer::AquiferQuery{
                        .x = x, .y = y, .z = z, .density = arm.density, .seaLevel = arm.seaLevel},
                    statuses, constant(arm.barrier), constant(arm.floodedness),
                    constant(arm.spread), constant(arm.lava), constant(arm.psl),
                    // Erosion and depth are 0 in the probe: Q5.9 cannot fire.
                    stratum::aquifer::NoDeepDark{}));
                ++tally.blocks;
                if (g == r) {
                    ++tally.agree;
                } else if (stratum::test::explainedByFlow(golden, x, y, z, g, r)) {
                    ++tally.flow;
                } else {
                    ++tally.unexplained;
                    tally.unexplainedLava +=
                        static_cast<long long>(y >= kLavaBandFrom && y <= kLavaBandTo);
                }
            }
        }
    }
    return tally;
}

/// The highest block that is not air in a column of a readout dimension.
[[nodiscard]] std::int32_t surfaceOf(stratum::test::GoldenRegion& region, std::int32_t x,
                                     std::int32_t z) {
    for (std::int32_t y = 319; y >= -64; --y) {
        const auto* block = region.blockAt(x, y, z);
        if (block != nullptr && stratum::test::categoryOf(block->name) != Category::Air) {
            return y;
        }
    }
    return -65;
}

/// The corpus for @p seed, checked; nullopt when it was never generated.
[[nodiscard]] std::optional<nlohmann::json> corpus(const std::int64_t seed) {
    std::optional<nlohmann::json> spec = loadSpec(seed);
    if (!spec) {
        return std::nullopt;
    }
    stratum::test::requireFrozen(probeDir(seed), kScript);
    stratum::test::requireSeed(probeDir(seed), seed);

    // The two aquifer arms are one dimension but for the flag.
    const nlohmann::json& lj = entryNamed(*spec, "lj");
    const nlohmann::json& mj = entryNamed(*spec, "mj");
    REQUIRE(lj.at("legacy_random_source") == true);
    REQUIRE(mj.at("legacy_random_source") == false);
    REQUIRE(lj.at("aquifers_enabled") == true);
    REQUIRE(lj.at("ore_veins_enabled") == false);
    nlohmann::json a = lj;
    nlohmann::json b = mj;
    for (nlohmann::json* arm : {&a, &b}) {
        arm->erase("name");
        arm->erase("legacy_random_source");
    }
    REQUIRE(a == b);
    REQUIRE(entryNamed(*spec, "lc").at("legacy_random_source") == true);
    REQUIRE(entryNamed(*spec, "mc").at("legacy_random_source") == false);
    return spec;
}

/// A worldgen tree of the probe's own noise settings, removed on exit.
class ScratchTree {
public:
    ScratchTree() : path_(stratum::test::tempPath("stratum-legacy-aquifer")) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_ / "density_function");
        std::filesystem::create_directories(path_ / "noise_settings");
    }

    ScratchTree(const ScratchTree&) = delete;
    ScratchTree& operator=(const ScratchTree&) = delete;
    ScratchTree(ScratchTree&&) = delete;
    ScratchTree& operator=(ScratchTree&&) = delete;

    ~ScratchTree() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace

TEST_CASE("the aquifer's centres under the legacy source are the ones the server drew",
          "[conformance][aquifer][legacy]") {
    int worlds = 0;
    for (const std::int64_t seed : kSeeds) {
        INFO("seed " << seed);
        const std::optional<nlohmann::json> spec = corpus(seed);
        if (!spec) {
            SKIP("no legacy aquifer probe at " << probeDir(seed) << "; generate it with " << kScript
                                               << " --accept-eula " << seed);
        }
        ++worlds;

        // THE POSITIVE CONTROL. A named noise is known to move under the flag,
        // so lc against mc says the flag reached this world; without it, lj
        // matching mj could mean the flag was dropped. Terrain only, no fluid:
        // nothing here can flow. Measured 16272-16304 of 16384.
        {
            stratum::test::GoldenRegion lc(probeDir(seed) / "lc" / "r.0.0.mca");
            stratum::test::GoldenRegion mc(probeDir(seed) / "mc" / "r.0.0.mca");
            int differ = 0;
            for (std::int32_t z = 0; z < kWindow; ++z) {
                for (std::int32_t x = 0; x < kWindow; ++x) {
                    differ += static_cast<int>(surfaceOf(lc, x, z) != surfaceOf(mc, x, z));
                }
            }
            INFO("lc against mc: " << differ << " of " << kWindow * kWindow << " columns");
            CHECK(differ > (kWindow * kWindow * 9) / 10);
        }

        const Arm lj = armOf(entryNamed(*spec, "lj"));
        const Arm mj = armOf(entryNamed(*spec, "mj"));
        const std::filesystem::path ljRegion = probeDir(seed) / "lj" / "r.0.0.mca";
        const std::filesystem::path mjRegion = probeDir(seed) / "mj" / "r.0.0.mca";
        const stratum::aquifer::CentreSource legacy{seed, RandomSource::Legacy};
        const stratum::aquifer::CentreSource modern{seed, RandomSource::Xoroshiro};

        // Each arm, exact under its own source. Flow is bounded, never pinned:
        // the frozen worlds keep a run-dependent remnant (SPEC §7), measured
        // 4-9 blocks of 5.9 million per arm.
        const Tally ljLegacy = replay(ljRegion, lj, legacy);
        const Tally mjModern = replay(mjRegion, mj, modern);
        for (const Tally* tally : {&ljLegacy, &mjModern}) {
            INFO("blocks " << tally->blocks << ", agree " << tally->agree << ", flow "
                           << tally->flow << ", unexplained " << tally->unexplained);
            CHECK(tally->unexplained == 0);
            CHECK(tally->agree > tally->blocks * 9 / 10);
            CHECK(tally->flow * 1000 <= tally->blocks);
        }

        // And each arm is NOT the other source's: this is what makes the two
        // exact replays above a measurement of the flag rather than of a
        // world the jitter cannot be seen in. A wrong lattice leaves about
        // 20000 of these 369024 blocks unexplained (the analyzer's `null`, and
        // measured 19864-21626 here), a quarter of which is the bound; the
        // lava band's share cannot be flow at all (measured 4684-4811).
        const Tally ljModern = replay(ljRegion, lj, modern);
        const Tally mjLegacy = replay(mjRegion, mj, legacy);
        INFO("lj with Xoroshiro centres: unexplained "
             << ljModern.unexplained << " (" << ljModern.unexplainedLava << " in the lava band)");
        INFO("mj with legacy centres: unexplained "
             << mjLegacy.unexplained << " (" << mjLegacy.unexplainedLava << " in the lava band)");
        CHECK(ljModern.unexplained >= 5000);
        CHECK(ljModern.unexplainedLava >= 1000);
        CHECK(mjLegacy.unexplained >= 5000);
        CHECK(mjLegacy.unexplainedLava >= 1000);

        if (seed == kTwin) {
            // 42 with the sign bit set: the same world to java.util.Random,
            // which keeps 48 bits, and a different one to Xoroshiro128++. The
            // server's legacy arm is seed 42's legacy lattice exactly, and its
            // modern arm is not seed 42's modern lattice.
            const Tally asFortyTwo =
                replay(ljRegion, lj, stratum::aquifer::CentreSource{42, RandomSource::Legacy});
            CHECK(asFortyTwo.unexplained == 0);
            const Tally modernAsFortyTwo =
                replay(mjRegion, mj, stratum::aquifer::CentreSource{42, RandomSource::Xoroshiro});
            CHECK(modernAsFortyTwo.unexplained >= 5000);
        }
    }
    CHECK(worlds == static_cast<int>(kSeeds.size()));
}

TEST_CASE("the shipped filler generates the legacy aquifer probe block for block",
          "[conformance][aquifer][legacy]") {
    // The wiring, end to end: the probe's own settings, rebuilt from its
    // spec.json the way density-probe.sh wrote them, through the registry
    // a dimension's flag selects and ChunkFiller::compile — the call
    // world::CompiledDimension makes — against the server, name for name.
    // Before this change ChunkFiller::compile refused lj by name.
    int worlds = 0;
    for (const std::int64_t seed : kSeeds) {
        INFO("seed " << seed);
        const std::optional<nlohmann::json> spec = corpus(seed);
        if (!spec) {
            SKIP("no legacy aquifer probe at " << probeDir(seed) << "; generate it with " << kScript
                                               << " --accept-eula " << seed);
        }
        ++worlds;

        const ScratchTree scratch;
        for (const char* name : {"lj", "mj"}) {
            std::ofstream out(scratch.path() / "noise_settings" / (std::string(name) + ".json"));
            out << stratum::test::probeNoiseSettings(entryNamed(*spec, name)).dump();
        }
        const stratum::data::Pack pack = stratum::data::Pack::open(scratch.path());
        const stratum::settings::LoadedSettings loaded = stratum::settings::loadAll(pack);

        for (const char* name : {"lj", "mj"}) {
            INFO("dimension " << name);
            const stratum::data::ResourceLocation id{"minecraft", name};
            const stratum::settings::NoiseSettings& dimension = loaded.settings.at(id);
            // The source the dimension declares, exactly as
            // world::CompiledDimension chooses it.
            const RandomSource source =
                dimension.legacyRandomSource ? RandomSource::Legacy : RandomSource::Xoroshiro;
            const auto noises = stratum::density::NoiseRegistry::create(
                pack, loaded.graph.referencedNoises(), seed, source);
            const stratum::surface::RuleGraph rules =
                stratum::surface::RuleGraph::resolve(dimension.surfaceRule, id);
            const stratum::terrain::ChunkFiller filler =
                stratum::terrain::ChunkFiller::compile(loaded.graph, noises, dimension, &rules);
            REQUIRE(filler.runsSurfaceRules());

            stratum::test::GoldenRegion region(probeDir(seed) / name / "r.0.0.mca");
            stratum::terrain::ChunkBuffer buffer(dimension.geometry);
            long long blocks = 0;
            long long exact = 0;
            long long flow = 0;
            long long wrong = 0;
            // A 3x3 block of chunks inside the window: the whole window is the
            // replay case's job, and a debug-build fill is the slow part.
            for (std::int32_t chunkZ = 2; chunkZ < 5; ++chunkZ) {
                for (std::int32_t chunkX = 2; chunkX < 5; ++chunkX) {
                    REQUIRE(region.hasChunk(chunkX, chunkZ));
                    filler.fill(chunkX, chunkZ, buffer);
                    for (int localZ = 0; localZ < 16; ++localZ) {
                        for (int localX = 0; localX < 16; ++localX) {
                            const std::int32_t x = (chunkX * 16) + localX;
                            const std::int32_t z = (chunkZ * 16) + localZ;
                            for (std::int32_t y = dimension.geometry.minY;
                                 y < dimension.geometry.minY + dimension.geometry.height; ++y) {
                                ++blocks;
                                const auto* theirs = region.blockAt(x, y, z);
                                const std::string served =
                                    theirs != nullptr ? theirs->name : "minecraft:air";
                                const std::string ours =
                                    buffer.at(localX, y, localZ).name.toString();
                                if (served == ours) {
                                    ++exact;
                                    continue;
                                }
                                const Category g = stratum::test::categoryOf(served);
                                const Category r = stratum::test::categoryOf(ours);
                                if (g != r &&
                                    stratum::test::explainedByFlow(region, x, y, z, g, r)) {
                                    ++flow;
                                    continue;
                                }
                                ++wrong;
                                if (wrong <= 8) {
                                    UNSCOPED_INFO(name << " (" << x << ", " << y << ", " << z
                                                       << "): server " << served << ", ours "
                                                       << ours);
                                }
                            }
                        }
                    }
                }
            }
            INFO("exact " << exact << ", flow " << flow << ", wrong " << wrong << " of " << blocks);
            CHECK(wrong == 0);
            CHECK(exact + flow == blocks);
            CHECK(flow * 1000 <= blocks);
        }
    }
    CHECK(worlds == static_cast<int>(kSeeds.size()));
}
