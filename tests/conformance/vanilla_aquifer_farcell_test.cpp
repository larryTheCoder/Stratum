// Stratum — the aquifer's own position mix, where its x product overflows.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The aquifer draws each cell's centre from vanilla's position mix at the
// CELL index (SPEC §11): `(int)(x * 3129871) ^ (z * 116129781L) ^ y`,
// squared and shifted. The x term is a 32-bit product, which parts from a
// 64-bit one only past |x| 686 — and every corpus the other aquifer cases
// read sits well inside that (the comb and the legacy pair at cells 0..7,
// the golden regions below cell 160). So for the aquifer, that part of the
// mix rested on the surface depth's jitter
// (vanilla_above_preliminary_surface_test.cpp), the same function at block
// coordinates: inferred, not seen.
//
// tools/analysis/aquifer-farcell-probe.sh puts the legacy aquifer probe's two
// arms — mj on Xoroshiro128++, lj on java.util.Random, every router input a
// constant — at cells 704..711 and at -712..-705 on both axes. Three readings:
//
//   * the comb's one-bit readout of each cell's vertical draw
//     (vanilla_aquifer_jitter_test.cpp) under the shipped mix and three
//     rivals, each rival with its own centres throughout, so no rival is
//     scored on territories the shipped mix drew;
//   * the shipped lattice replayed block for block through computeSubstance,
//     with the other source's lattice as the control that the replay sees a
//     wrong one;
//   * the shipped filler end to end on chunks of each world, which adds the
//     filler's own block-to-cell arithmetic at negative coordinates.
//
// The fixtures are Mojang-derived and never committed (SPEC §12).
#include "support/fluid_flow.hpp"
#include "support/probe_corpus.hpp"
#include "support/probe_settings.hpp"
#include "support/temp_path.hpp"

#include <stratum/aquifer/lattice.hpp>
#include <stratum/aquifer/substance.hpp>
#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/density/random_source.hpp>
#include <stratum/javamath.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/rng/java_random.hpp>
#include <stratum/rng/xoroshiro128.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/rule_graph.hpp>
#include <stratum/terrain/filler.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

using stratum::aquifer::CentreSource;
using stratum::aquifer::Jitter;
using stratum::density::RandomSource;
using stratum::test::Category;

constexpr const char* kScript = "tools/analysis/aquifer-farcell-probe.sh";
constexpr std::int64_t kSeed = 42;
constexpr std::int32_t kChunks = 8;
constexpr std::int32_t kWindow = kChunks * 16;
/// The last cell index at which `(int)(x * 3129871)` and `x * 3129871L` are
/// the same number: 2^31 / 3129871 is 686.1.
constexpr std::int32_t kLastSharedX = 686;
/// The same for z's factor, 116129781: 2^31 / 116129781 is 18.5.
constexpr std::int32_t kLastSharedZ = 18;

/// The comb's readout: a cell of layer -4 holds fluid at y -42 exactly when
/// its vertical draw is the top one (vanilla_aquifer_jitter_test.cpp).
constexpr std::int32_t kProbeY = -42;
constexpr std::int32_t kThresholdLayer = -4;
constexpr int kMinColumns = 120;

struct Placement {
    const char* corpus;
    std::int32_t originChunkX;
    std::int32_t originChunkZ;
    std::int32_t regionX;
    std::int32_t regionZ;
};

/// Cells are 16 blocks and start on a chunk boundary, so a chunk's index is
/// its cell's.
constexpr std::array<Placement, 2> kPlacements{{
    {"farcell_pos_s42", 704, 704, 22, 22},
    {"farcell_neg_s42", -712, -712, -23, -23},
}};

struct ArmOf {
    const char* name;
    RandomSource source;
};

constexpr std::array<ArmOf, 2> kArms{
    {{"mj", RandomSource::Xoroshiro}, {"lj", RandomSource::Legacy}}};

[[nodiscard]] std::filesystem::path probeDir(const Placement& placement) {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11" / "probes" / placement.corpus;
}

[[nodiscard]] std::filesystem::path regionPath(const Placement& placement, const char* arm) {
    return probeDir(placement) / arm /
           ("r." + std::to_string(placement.regionX) + "." + std::to_string(placement.regionZ) +
            ".mca");
}

[[nodiscard]] const nlohmann::json& entryNamed(const nlohmann::json& spec, const char* name) {
    // REQUIRE rather than FAIL-then-return: MSVC sees the return after an
    // unconditional FAIL as unreachable (C4702), and warnings are errors.
    const nlohmann::json* named = nullptr;
    for (const auto& entry : spec) {
        if (entry.at("name") == name) {
            named = &entry;
            break;
        }
    }
    INFO("the probe's spec has no arm " << name);
    REQUIRE(named != nullptr);
    return *named;
}

/// The corpus for @p placement, checked; nullopt when it was never generated.
[[nodiscard]] std::optional<nlohmann::json> corpus(const Placement& placement) {
    std::ifstream in(probeDir(placement) / "spec.json");
    if (!in) {
        return std::nullopt;
    }
    nlohmann::json spec = nlohmann::json::parse(in);
    stratum::test::requireFrozen(probeDir(placement), kScript);
    stratum::test::requireSeed(probeDir(placement), kSeed);

    // Where the server forceloaded is what every reading below assumes.
    std::ifstream manifestIn(probeDir(placement) / "manifest.json");
    const nlohmann::json manifest = nlohmann::json::parse(manifestIn);
    INFO(placement.corpus << "'s manifest: " << manifest.dump());
    REQUIRE(manifest.at("origin_chunk").at(0).get<std::int32_t>() == placement.originChunkX);
    REQUIRE(manifest.at("origin_chunk").at(1).get<std::int32_t>() == placement.originChunkZ);
    REQUIRE(manifest.at("chunks").get<std::int32_t>() == kChunks);
    REQUIRE(regionPath(placement, "mj").filename() == manifest.at("region").get<std::string>());

    // The two arms are one dimension but for the flag.
    const nlohmann::json& mj = entryNamed(spec, "mj");
    const nlohmann::json& lj = entryNamed(spec, "lj");
    REQUIRE(mj.at("legacy_random_source") == false);
    REQUIRE(lj.at("legacy_random_source") == true);
    REQUIRE(mj.at("aquifers_enabled") == true);
    REQUIRE(mj.at("ore_veins_enabled") == false);
    nlohmann::json a = mj;
    nlohmann::json b = lj;
    for (nlohmann::json* arm : {&a, &b}) {
        arm->erase("name");
        arm->erase("legacy_random_source");
    }
    REQUIRE(a == b);
    return spec;
}

// --- the mix and its rivals -------------------------------------------------

/// The position mix with one choice in it changed: the three the surface
/// depth's case scores (vanilla_above_preliminary_surface_test.cpp).
/// `Shipped` is `rng::positionSeed`, spelled here and checked against the
/// library's own centres before anything is read into the others.
enum class Mix : std::uint8_t {
    Shipped,
    WideX,       ///< the x term multiplied in 64 bits, not 32
    NarrowZ,     ///< the z term multiplied in 32 bits, not 64
    LogicalShift ///< `>>>` 16 instead of `>>` 16
};

constexpr std::array<Mix, 4> kMixes{Mix::Shipped, Mix::WideX, Mix::NarrowZ, Mix::LogicalShift};

[[nodiscard]] const char* nameOf(const Mix mix) {
    switch (mix) {
        case Mix::Shipped:
            return "shipped";
        case Mix::WideX:
            return "x in 64 bits";
        case Mix::NarrowZ:
            return "z in 32 bits";
        case Mix::LogicalShift:
            return "logical shift";
    }
    return "?";
}

[[nodiscard]] std::uint64_t mixUnder(const Mix mix, const std::int32_t x, const std::int32_t y,
                                     const std::int32_t z) noexcept {
    const auto wide = [](const std::int32_t value, const std::uint64_t factor) {
        return static_cast<std::uint64_t>(static_cast<std::int64_t>(value)) * factor;
    };
    const auto narrow = [](const std::int32_t value, const std::uint32_t factor) {
        return static_cast<std::uint64_t>(static_cast<std::int64_t>(
            static_cast<std::int32_t>(static_cast<std::uint32_t>(value) * factor)));
    };
    const std::uint64_t xTerm =
        mix == Mix::WideX ? wide(x, UINT64_C(3129871)) : narrow(x, UINT32_C(3129871));
    const std::uint64_t zTerm =
        mix == Mix::NarrowZ ? narrow(z, UINT32_C(116129781)) : wide(z, UINT64_C(116129781));
    std::uint64_t value = xTerm ^ zTerm ^ static_cast<std::uint64_t>(static_cast<std::int64_t>(y));
    value = (value * value * UINT64_C(42317861)) + (value * UINT64_C(11));
    const auto mixed = static_cast<std::int64_t>(value);
    return static_cast<std::uint64_t>(mix == Mix::LogicalShift ? stratum::javamath::ushr(mixed, 16)
                                                               : stratum::javamath::shr(mixed, 16));
}

/// A cell's three draws with the mix @p mix, from @p centres' own generator
/// and base: the shipped `CentreSource::jitterOf` with the mix swapped.
[[nodiscard]] Jitter jitterUnder(const CentreSource& centres, const Mix mix, const std::int32_t cx,
                                 const std::int32_t cy, const std::int32_t cz) {
    const std::uint64_t mixed = mixUnder(mix, cx, cy, cz);
    if (centres.source() == RandomSource::Legacy) {
        stratum::rng::JavaRandom random{centres.legacySeed() ^ static_cast<std::int64_t>(mixed)};
        const std::int32_t jx = random.nextInt(stratum::aquifer::kJitterBoundX);
        const std::int32_t jy = random.nextInt(stratum::aquifer::kJitterBoundY);
        const std::int32_t jz = random.nextInt(stratum::aquifer::kJitterBoundZ);
        return Jitter{.x = jx, .y = jy, .z = jz};
    }
    const stratum::rng::Seed128 base = centres.base();
    stratum::rng::Xoroshiro128PlusPlus random{
        stratum::rng::Seed128{.lo = base.lo ^ mixed, .hi = base.hi}};
    const std::int32_t jx = random.nextInt(stratum::aquifer::kJitterBoundX);
    const std::int32_t jy = random.nextInt(stratum::aquifer::kJitterBoundY);
    const std::int32_t jz = random.nextInt(stratum::aquifer::kJitterBoundZ);
    return Jitter{.x = jx, .y = jy, .z = jz};
}

/// How many of the 8x8 layer -4 cells from (@p cellX, @p cellZ) a rival
/// draws differently from the shipped mix. Model only: no corpus.
[[nodiscard]] int cellsParted(const CentreSource& centres, const Mix mix, const std::int32_t cellX,
                              const std::int32_t cellZ) {
    int parted = 0;
    for (std::int32_t dz = 0; dz < kChunks; ++dz) {
        for (std::int32_t dx = 0; dx < kChunks; ++dx) {
            parted += static_cast<int>(
                jitterUnder(centres, mix, cellX + dx, kThresholdLayer, cellZ + dz) !=
                jitterUnder(centres, Mix::Shipped, cellX + dx, kThresholdLayer, cellZ + dz));
        }
    }
    return parted;
}

// --- the one-bit readout ------------------------------------------------------

/// Whether each column of the window holds fluid at y -42, row-major in z
/// from the window's low corner. Every chunk is required: a missing one is a
/// broken corpus, not a smaller sample.
[[nodiscard]] std::vector<std::uint8_t> wetAtProbe(const Placement& placement, const char* arm) {
    std::vector<std::uint8_t> wet(static_cast<std::size_t>(kWindow) * kWindow, 0);
    const auto file = stratum::region::RegionFile::open(regionPath(placement, arm));
    for (std::int32_t dz = 0; dz < kChunks; ++dz) {
        for (std::int32_t dx = 0; dx < kChunks; ++dx) {
            const std::int32_t chunkX = placement.originChunkX + dx;
            const std::int32_t chunkZ = placement.originChunkZ + dz;
            REQUIRE(file.hasChunk(chunkX, chunkZ));
            const auto chunk = stratum::chunk::Chunk::decode(
                stratum::nbt::read(file.readChunk(chunkX, chunkZ)).root);
            REQUIRE(chunk.x() == chunkX);
            REQUIRE(chunk.z() == chunkZ);
            for (int lz = 0; lz < 16; ++lz) {
                for (int lx = 0; lx < 16; ++lx) {
                    const auto* block = chunk.blockAt(lx, kProbeY, lz);
                    REQUIRE(block != nullptr);
                    const auto at =
                        static_cast<std::size_t>((((dz * 16) + lz) * kWindow) + (dx * 16) + lx);
                    wet[at] = static_cast<std::uint8_t>(block->name == "minecraft:water" ||
                                                        block->name == "minecraft:lava");
                }
            }
        }
    }
    return wet;
}

struct Score {
    int checked = 0;
    int agree = 0;
    int observedPositive = 0;
    int predictedPositive = 0;
};

/// The comb's readout of one arm under one mix: every column goes to the
/// nearest of the window's layer -4 centres as @p mix draws them, and a cell
/// with enough columns is wet when most of them are; it is predicted wet
/// when @p mix draws its vertical jitter at the top of the range.
[[nodiscard]] Score score(const std::vector<std::uint8_t>& wet, const Placement& placement,
                          const CentreSource& centres, const Mix mix) {
    std::array<std::array<Jitter, kChunks>, kChunks> drawn{};
    for (std::int32_t dz = 0; dz < kChunks; ++dz) {
        for (std::int32_t dx = 0; dx < kChunks; ++dx) {
            drawn.at(static_cast<std::size_t>(dz)).at(static_cast<std::size_t>(dx)) =
                jitterUnder(centres, mix, placement.originChunkX + dx, kThresholdLayer,
                            placement.originChunkZ + dz);
        }
    }

    struct Territory {
        int wet = 0;
        int total = 0;
    };

    std::array<std::array<Territory, kChunks>, kChunks> territories{};
    for (std::int32_t z = 0; z < kWindow; ++z) {
        for (std::int32_t x = 0; x < kWindow; ++x) {
            // Window-relative, so the nine candidates are the cells around
            // the column's own chunk that the window holds.
            int ownerX = -1;
            int ownerZ = -1;
            std::int64_t best = std::int64_t{1} << 60;
            for (int ox = -1; ox <= 1; ++ox) {
                for (int oz = -1; oz <= 1; ++oz) {
                    const int qx = (x / 16) + ox;
                    const int qz = (z / 16) + oz;
                    if (qx < 0 || qx >= kChunks || qz < 0 || qz >= kChunks) {
                        continue;
                    }
                    const Jitter& jitter =
                        drawn.at(static_cast<std::size_t>(qz)).at(static_cast<std::size_t>(qx));
                    const std::int64_t ddx = x - ((qx * 16) + jitter.x);
                    const std::int64_t ddz = z - ((qz * 16) + jitter.z);
                    const std::int64_t distance = (ddx * ddx) + (ddz * ddz);
                    if (distance < best) {
                        best = distance;
                        ownerX = qx;
                        ownerZ = qz;
                    }
                }
            }
            Territory& territory = territories.at(static_cast<std::size_t>(ownerZ))
                                       .at(static_cast<std::size_t>(ownerX));
            ++territory.total;
            territory.wet +=
                static_cast<int>(wet.at(static_cast<std::size_t>((z * kWindow) + x)) != 0);
        }
    }
    Score result;
    for (std::size_t dz = 0; dz < kChunks; ++dz) {
        for (std::size_t dx = 0; dx < kChunks; ++dx) {
            const Territory& territory = territories.at(dz).at(dx);
            if (territory.total < kMinColumns) {
                continue;
            }
            const bool observed = territory.wet * 2 > territory.total;
            const bool predicted = drawn.at(dz).at(dx).y >= stratum::aquifer::kJitterBoundY - 1;
            ++result.checked;
            result.observedPositive += static_cast<int>(observed);
            result.predictedPositive += static_cast<int>(predicted);
            result.agree += static_cast<int>(observed == predicted);
        }
    }
    return result;
}

// --- the block-for-block replay -------------------------------------------------

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

/// Every eighth column, two blocks in from the forceloaded window's edge so
/// that the flow classifier's horizontal neighbours are inside it.
constexpr std::int32_t kColumnStride = 8;
constexpr std::int32_t kEdgeMargin = 2;
/// The lava band (vanilla_aquifer_legacy_test.cpp): under the probe's
/// constants the cells of layers -5 and -4 hold lava or nothing, and a lava
/// SOURCE is never one of fluid_flow.hpp's shapes, so a wrong lattice's
/// disagreements here cannot be forgiven as flow at all.
constexpr std::int32_t kLavaBandFrom = -54;
constexpr std::int32_t kLavaBandTo = -37;

struct Tally {
    long long blocks = 0;
    long long agree = 0;
    long long flow = 0;
    long long unexplained = 0;
    /// Unexplained, in the lava band.
    long long unexplainedLava = 0;
};

/// One arm of one placement, replayed with the centres @p centres.
[[nodiscard]] Tally replay(const Placement& placement, const char* armName, const Arm& arm,
                           const CentreSource& centres) {
    stratum::test::GoldenRegion golden(regionPath(placement, armName), placement.regionX,
                                       placement.regionZ);
    stratum::aquifer::StatusCache statuses; // every input constant: one per world
    const auto constant = [](double value) {
        return [value](std::int32_t, std::int32_t, std::int32_t) { return value; };
    };
    const std::int32_t fromX = placement.originChunkX * 16;
    const std::int32_t fromZ = placement.originChunkZ * 16;
    Tally tally;
    for (std::int32_t dz = kEdgeMargin; dz < kWindow - kEdgeMargin; dz += kColumnStride) {
        for (std::int32_t dx = kEdgeMargin; dx < kWindow - kEdgeMargin; dx += kColumnStride) {
            const std::int32_t x = fromX + dx;
            const std::int32_t z = fromZ + dz;
            REQUIRE(golden.hasChunk(stratum::javamath::floorDiv(x, 16),
                                    stratum::javamath::floorDiv(z, 16)));
            for (std::int32_t y = arm.minY; y < arm.minY + arm.height; ++y) {
                const auto* theirs = golden.blockAt(x, y, z);
                REQUIRE(theirs != nullptr);
                const Category g = stratum::test::categoryOf(theirs->name);
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

/// A worldgen tree of the probe's own noise settings, removed on exit.
class ScratchTree {
public:
    ScratchTree() : path_(stratum::test::tempPath("stratum-farcell-aquifer")) {
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

TEST_CASE("the aquifer's own position mix multiplies x in 32 bits where only far cells tell",
          "[conformance][aquifer][rng]") {
    // WHERE ONLY FAR CELLS TELL, model only. At the comb's cells 0..7 the
    // rivals that change the x or z product draw exactly the shipped
    // centres, so no aquifer corpus at the origin could ever refute them;
    // only the logical shift parts there (it moves every negative mix), and
    // under the legacy source not even that (below).
    for (const auto& [name, source] : kArms) {
        INFO("arm " << name);
        const CentreSource centres{kSeed, source};
        CHECK(cellsParted(centres, Mix::WideX, 0, 0) == 0);
        CHECK(cellsParted(centres, Mix::NarrowZ, 0, 0) == 0);
        CHECK((cellsParted(centres, Mix::LogicalShift, 0, 0) == 0) ==
              (source == RandomSource::Legacy));
    }
    for (const Placement& placement : kPlacements) {
        for (const std::int32_t cell :
             {placement.originChunkX, placement.originChunkZ, placement.originChunkX + kChunks - 1,
              placement.originChunkZ + kChunks - 1}) {
            REQUIRE((cell > kLastSharedX || cell < -kLastSharedX));
            REQUIRE((cell > kLastSharedZ || cell < -kLastSharedZ));
        }
    }

    // Model only: of each window's 64 cells, how many a rival draws
    // differently, per placement, arm (mj, lj) and rival (x in 64 bits,
    // z in 32 bits, logical shift). The two products part every cell but
    // two: lj's (706, 705) and (711, 710) under x in 64 bits, the 1-in-900
    // chance of three equal draws from different stream seeds. The logical
    // shift parts the cells whose mix is negative under Xoroshiro, and NONE
    // under the legacy source: java.util.Random keeps 48 bits of its seed,
    // and `>>` and `>>>` differ in the top 16 alone.
    static constexpr std::array<std::array<std::array<int, 3>, kArms.size()>, kPlacements.size()>
        kParted{{{{{64, 64, 31}, {62, 64, 0}}}, {{{64, 64, 24}, {64, 64, 0}}}}};

    std::array<std::array<Score, kMixes.size()>, kArms.size()> totals{};
    std::array<std::array<int, kMixes.size()>, kArms.size()> partedTotals{};
    std::ostringstream record;
    int placements = 0;
    for (std::size_t p = 0; p < kPlacements.size(); ++p) {
        const Placement& placement = kPlacements.at(p);
        INFO(placement.corpus);
        const std::optional<nlohmann::json> spec = corpus(placement);
        if (!spec) {
            SKIP("no far-cell aquifer probe at " << probeDir(placement) << "; generate it with "
                                                 << kScript << " --accept-eula " << kSeed);
        }
        ++placements;
        for (std::size_t a = 0; a < kArms.size(); ++a) {
            const ArmOf& arm = kArms.at(a);
            INFO("arm " << arm.name);
            const CentreSource centres{kSeed, arm.source};
            // The mix spelled above is the library's to the bit, or the
            // rivals perturb a lookalike.
            for (std::int32_t dz = -1; dz <= kChunks; ++dz) {
                for (std::int32_t dx = -1; dx <= kChunks; ++dx) {
                    const std::int32_t cx = placement.originChunkX + dx;
                    const std::int32_t cz = placement.originChunkZ + dz;
                    REQUIRE(jitterUnder(centres, Mix::Shipped, cx, kThresholdLayer, cz) ==
                            centres.jitterOf(cx, kThresholdLayer, cz));
                }
            }
            const std::vector<std::uint8_t> wet = wetAtProbe(placement, arm.name);
            for (std::size_t m = 0; m < kMixes.size(); ++m) {
                const Score s = score(wet, placement, centres, kMixes.at(m));
                const int parted = cellsParted(centres, kMixes.at(m), placement.originChunkX,
                                               placement.originChunkZ);
                record << placement.corpus << " " << arm.name << " " << nameOf(kMixes.at(m)) << ": "
                       << s.agree << " of " << s.checked << " cells, observed positive "
                       << s.observedPositive << ", predicted positive " << s.predictedPositive
                       << "; draws parted from the shipped mix's on " << parted << " of 64\n";
                if (m > 0) {
                    CHECK(parted == kParted.at(p).at(a).at(m - 1));
                }
                partedTotals.at(a).at(m) += parted;
                Score& total = totals.at(a).at(m);
                total.checked += s.checked;
                total.agree += s.agree;
                total.observedPositive += s.observedPositive;
                total.predictedPositive += s.predictedPositive;
            }
        }
    }
    REQUIRE(placements == static_cast<int>(kPlacements.size()));
    INFO(record.str());

    for (std::size_t a = 0; a < kArms.size(); ++a) {
        INFO("arm " << kArms.at(a).name);
        const Score& shipped = totals.at(a).at(0);
        // The measurement: the library's mix on every cell the readout
        // scores, on both signs (64 + 64 for mj; 63 + 64 for lj, whose
        // positive window leaves one cell short of the column floor).
        CHECK(shipped.checked >= 120);
        CHECK(shipped.agree == shipped.checked);
        CHECK(shipped.predictedPositive == shipped.observedPositive);
        CHECK(shipped.observedPositive > shipped.checked / 20);
        // And each rival, scored with its own centres throughout, wrong on
        // a share of cells no single misread could explain: a fresh draw
        // misplaces about one cell in five, a logical shift half that.
        // Bounds, not counts: a rival's territories are not the server's,
        // so its majority votes can sit near half. A rival that draws every
        // cell as the shipped mix does scores what the shipped mix scores.
        for (std::size_t m = 1; m < kMixes.size(); ++m) {
            INFO(nameOf(kMixes.at(m)));
            const Score& rival = totals.at(a).at(m);
            if (partedTotals.at(a).at(m) == 0) {
                CHECK(rival.agree == shipped.agree);
                CHECK(rival.checked == shipped.checked);
            } else {
                CHECK(rival.checked - rival.agree > rival.checked / 20);
            }
        }
    }
}

TEST_CASE("the shipped aquifer lattice replays the far cells block for block",
          "[conformance][aquifer]") {
    // Each arm against the forward model with its own source's centres,
    // every eighth column of each window, the whole column; fluid the
    // frozen world's remnant moved is set apart by fluid_flow.hpp's shapes
    // and nothing else. And each arm against the OTHER source's centres,
    // which is what says the replay sees a wrong lattice at these cells.
    int placements = 0;
    for (const Placement& placement : kPlacements) {
        INFO(placement.corpus);
        const std::optional<nlohmann::json> spec = corpus(placement);
        if (!spec) {
            SKIP("no far-cell aquifer probe at " << probeDir(placement) << "; generate it with "
                                                 << kScript << " --accept-eula " << kSeed);
        }
        ++placements;
        for (const ArmOf& arm : kArms) {
            INFO("arm " << arm.name);
            const Arm constants = armOf(entryNamed(*spec, arm.name));
            const CentreSource own{kSeed, arm.source};
            const CentreSource other{kSeed, arm.source == RandomSource::Legacy
                                                ? RandomSource::Xoroshiro
                                                : RandomSource::Legacy};
            const Tally exact = replay(placement, arm.name, constants, own);
            const Tally wrong = replay(placement, arm.name, constants, other);
            INFO("own centres: blocks " << exact.blocks << ", agree " << exact.agree << ", flow "
                                        << exact.flow << ", unexplained " << exact.unexplained);
            INFO("the other source's: unexplained "
                 << wrong.unexplained << " (" << wrong.unexplainedLava << " in the lava band)");
            CHECK(exact.unexplained == 0);
            CHECK(exact.agree > exact.blocks * 9 / 10);
            // Flow is bounded, never pinned: the frozen worlds keep a
            // run-dependent remnant (SPEC §7).
            CHECK(exact.flow * 1000 <= exact.blocks);
            // A wrong lattice leaves 5428-5652 of these 98304 blocks
            // unexplained, a quarter of which is the bound; the lava band's
            // share (1329-1355) cannot be flow at all.
            CHECK(wrong.unexplained >= 1300);
            CHECK(wrong.unexplainedLava >= 300);
        }
    }
    CHECK(placements == static_cast<int>(kPlacements.size()));
}

TEST_CASE("the shipped filler generates the far aquifer cells block for block",
          "[conformance][aquifer]") {
    // The wiring end to end on both signs: the probe's own settings rebuilt
    // from its spec.json, through the registry each arm's flag selects and
    // ChunkFiller::compile, the call world::CompiledDimension makes, so the
    // filler's own block-to-cell arithmetic at chunk -709 is in the loop,
    // against the server, name for name.
    int placements = 0;
    for (const Placement& placement : kPlacements) {
        INFO(placement.corpus);
        const std::optional<nlohmann::json> spec = corpus(placement);
        if (!spec) {
            SKIP("no far-cell aquifer probe at " << probeDir(placement) << "; generate it with "
                                                 << kScript << " --accept-eula " << kSeed);
        }
        ++placements;

        const ScratchTree scratch;
        for (const ArmOf& arm : kArms) {
            std::ofstream out(scratch.path() / "noise_settings" /
                              (std::string(arm.name) + ".json"));
            out << stratum::test::probeNoiseSettings(entryNamed(*spec, arm.name)).dump();
        }
        const stratum::data::Pack pack = stratum::data::Pack::open(scratch.path());
        const stratum::settings::LoadedSettings loaded = stratum::settings::loadAll(pack);

        for (const ArmOf& arm : kArms) {
            INFO("arm " << arm.name);
            const stratum::data::ResourceLocation id{"minecraft", arm.name};
            const stratum::settings::NoiseSettings& dimension = loaded.settings.at(id);
            // The source the dimension declares, exactly as
            // world::CompiledDimension chooses it.
            const RandomSource source =
                dimension.legacyRandomSource ? RandomSource::Legacy : RandomSource::Xoroshiro;
            REQUIRE(source == arm.source);
            const auto noises = stratum::density::NoiseRegistry::create(
                pack, loaded.graph.referencedNoises(), kSeed, source);
            const stratum::surface::RuleGraph rules =
                stratum::surface::RuleGraph::resolve(dimension.surfaceRule, id);
            const stratum::terrain::ChunkFiller filler =
                stratum::terrain::ChunkFiller::compile(loaded.graph, noises, dimension, &rules);
            REQUIRE(filler.runsSurfaceRules());

            stratum::test::GoldenRegion region(regionPath(placement, arm.name), placement.regionX,
                                               placement.regionZ);
            stratum::terrain::ChunkBuffer buffer(dimension.geometry);
            long long blocks = 0;
            long long exact = 0;
            long long flow = 0;
            long long wrong = 0;
            // A 2x2 block of chunks inside the window: the whole window is
            // the replay case's job, and a debug-build fill is the slow part.
            for (std::int32_t dz = 3; dz < 5; ++dz) {
                for (std::int32_t dx = 3; dx < 5; ++dx) {
                    const std::int32_t chunkX = placement.originChunkX + dx;
                    const std::int32_t chunkZ = placement.originChunkZ + dz;
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
                                REQUIRE(theirs != nullptr);
                                const std::string& served = theirs->name;
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
                                    UNSCOPED_INFO(arm.name << " (" << x << ", " << y << ", " << z
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
    CHECK(placements == static_cast<int>(kPlacements.size()));
}
