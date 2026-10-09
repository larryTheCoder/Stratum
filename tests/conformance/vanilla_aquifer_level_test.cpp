// Stratum — the level rule on a constant surface, block for block (MA).
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// SPEC §11 used to rest the level rule on four per-seed, per-cell scores,
// 99.9806-99.9902%, taken on about 1370 constant-surface probe dimensions
// whose specs, readout and scored model are all gone. A remainder of one or
// two cells in ten thousand could never be attributed, because nothing in
// the tree could reproduce it. `tools/analysis/aquifer-level-probe.sh`
// rebuilds that regime on the same axes — preliminary surface -54 to 141,
// sea level 32 to 200, the spreads, barriers and floodednesses the old
// campaign varied — frozen, with packed ice as the default fluid so that
// nothing the aquifer places can move. Every router entry is a constant, so
// each world is the aquifer's decision and nothing else, and this case
// replays the filler's own path — the global picker above the chunk's
// y_skip, `aquifer::computeSubstance` at and below it — against the block
// the server wrote. A per-cell readout is a function of the blocks, so a
// world that matches block for block matches under any readout.
//
// Five cases:
//   * every block of three seeds' ice worlds from y -51 up, exactly: no flow
//     allowance, because packed ice cannot flow and no lava reaches -51;
//   * Q5.3(a) off the ocean branch: a source centred more than twenty above
//     a land surface takes the sea. The first 75 dimensions a seed missed
//     only that, on 16 blocks of one of them; the land arms make it
//     measurable, and the rival readings in aquifer-level-rivals.hpp — no
//     clause, a margin of 19 or 21, a threshold on the sea — are each
//     refuted where they part from the build;
//   * the bonus spelling at the two exact crossings, where a pre-divided
//     11/640 floods cells the product over the divisor leaves dry;
//   * a fractional surface is floored, where truncation and rounding part;
//   * the water twin: packed ice stands where water stands, so the ice
//     worlds measure what a water world would.
//
// Rows -54..-52 are tallied and not asserted in ice: at spread 0.9 a ladder
// in the band under -40 reaches -51, and a source centred below the lava
// sea is lava, so lava stands beside packed ice there. The server's barrier
// between that pair is not the one it builds between lava and water (the ice
// worlds part from the build on about 0.7% of those rows, stone in both
// directions), and ChunkFiller refuses a non-water default fluid by name, so
// the tally is reported rather than asserted. The water twin scores those
// rows.
//
// The fixtures are Mojang-derived and never committed (SPEC §12).
#include "aquifer-level-rivals.hpp"
#include "support/fluid_flow.hpp"
#include "support/probe_corpus.hpp"

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
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace aquifer = stratum::aquifer;
namespace javamath = stratum::javamath;
namespace level = stratum::analysis::level;
using stratum::test::Category;

constexpr std::int32_t kChunks = 8;
constexpr std::int32_t kWindow = kChunks * 16;
/// Two blocks in from the force-loaded window on every side, so that a
/// block's horizontal neighbours — which the flow classifier reads — are in
/// what the server generated.
constexpr std::int32_t kEdgeMargin = 2;
constexpr std::int32_t kTopRow = 319;
/// The lava sea's top: every sea level in this corpus is above -54.
constexpr std::int32_t kLambda = -54;
/// lambda + 3, the first row no lava can reach in these worlds: a lava
/// source above the sea is one centred below it, whose ladder is at most
/// -60 + 9.
constexpr std::int32_t kFirstAssertedRow = -51;
/// How far above max(sea level, surface) a row is still read. The barrier's
/// lid sits a few blocks above the higher of two levels; past this every
/// block is air under every reading.
constexpr std::int32_t kRowsAboveTop = 16;
constexpr std::array<std::int64_t, 3> kIceSeeds{42, 31337, 8675309};
constexpr std::int64_t kWaterSeed = 42;
constexpr std::int32_t kGridSurfaces = 66; // -54, -51, ..., 141
/// Columns of the scored window at stride 4: 31 a side.
constexpr long long kStride4Columns = 31LL * 31LL;
constexpr const char* kScript = "tools/analysis/aquifer-level-probe.sh";
constexpr const char* kIce = "minecraft:packed_ice";
constexpr const char* kWater = "minecraft:water";

/// Exact comparison by bits: the project keeps -Wfloat-equal on, and every
/// value compared here is a constant a spec wrote, meant exactly.
[[nodiscard]] bool same(const double a, const double b) {
    return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

[[nodiscard]] bool whole(const double value) {
    return same(std::floor(value), value);
}

[[nodiscard]] std::filesystem::path probes() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11" / "probes";
}

[[nodiscard]] std::string corpusName(const char* family, const std::int64_t seed) {
    return std::string(family) + "_s" + std::to_string(seed);
}

/// One dimension of the probe: every input a constant.
struct Dim {
    std::string name;
    double psl = 0.0;
    std::int32_t sea = 0;
    double floodedness = 0.0;
    double spread = 0.0;
    double barrier = 0.0;
    std::string fluid;
};

[[nodiscard]] double constantEntry(const nlohmann::json& router, const char* name) {
    INFO("router entry " << name);
    REQUIRE(router.at(name).is_number());
    return router.at(name).get<double>();
}

/// The dimension as declared, refusing anything this case cannot replay:
/// terrain, a varying input, a lava override, another world height.
[[nodiscard]] Dim parseDim(const nlohmann::json& entry) {
    Dim dim;
    dim.name = entry.at("name").get<std::string>();
    INFO("dimension " << dim.name);
    REQUIRE(entry.at("raw_final_density").at("type").get<std::string>() == "minecraft:constant");
    REQUIRE(same(entry.at("raw_final_density").at("argument").get<double>(), -1.0));
    REQUIRE(entry.at("min_y").get<std::int32_t>() == -64);
    REQUIRE(entry.at("height").get<std::int32_t>() == 384);
    REQUIRE(entry.at("aquifers_enabled").get<bool>());
    const nlohmann::json& router = entry.at("router");
    REQUIRE(same(constantEntry(router, "lava"), 0.0));
    dim.psl = constantEntry(router, "preliminary_surface_level");
    dim.floodedness = constantEntry(router, "fluid_level_floodedness");
    dim.spread = constantEntry(router, "fluid_level_spread");
    dim.barrier = constantEntry(router, "barrier");
    dim.sea = entry.at("sea_level").get<std::int32_t>();
    dim.fluid = entry.at("default_fluid").at("Name").get<std::string>();
    // Every row this case reads, and lambda -54 in all of them.
    REQUIRE(aquifer::lambdaLevel(dim.sea) == kLambda);
    return dim;
}

struct Corpus {
    std::filesystem::path dir;
    std::int64_t seed = 0;
    std::vector<Dim> dims;
};

/// A frozen corpus from @p seed, every entry of its spec with a region.
[[nodiscard]] Corpus openCorpus(const std::string& name, const std::int64_t seed) {
    Corpus corpus{.dir = probes() / name, .seed = seed, .dims = {}};
    INFO("probe corpus " << corpus.dir);
    REQUIRE(std::filesystem::is_regular_file(corpus.dir / "spec.json"));
    stratum::test::requireFrozen(corpus.dir, kScript);
    stratum::test::requireSeed(corpus.dir, seed);
    std::ifstream specFile(corpus.dir / "spec.json");
    const nlohmann::json spec = nlohmann::json::parse(specFile);
    REQUIRE(spec.is_array());
    for (const auto& entry : spec) {
        corpus.dims.push_back(parseDim(entry));
        REQUIRE(
            std::filesystem::is_regular_file(corpus.dir / corpus.dims.back().name / "r.0.0.mca"));
    }
    return corpus;
}

[[nodiscard]] std::filesystem::path regionOf(const Corpus& corpus, const Dim& dim) {
    return corpus.dir / dim.name / "r.0.0.mca";
}

/// The highest row read: past it every reading gives air.
[[nodiscard]] std::int32_t topRow(const Dim& dim) {
    return std::min(kTopRow, std::max(dim.sea, javamath::floorToInt(dim.psl)) + kRowsAboveTop);
}

/// Q2.5's cutoff for a constant surface: the same in every chunk.
[[nodiscard]] std::int32_t ySkipOf(const double psl) {
    return aquifer::chunkYSkip(level::constant(psl), 0, 0);
}

/// The probe's inputs to the shared decision (aquifer-level-rivals.hpp),
/// with @p psl in place of the dimension's own so that a case can replay a
/// rival reading of the surface.
[[nodiscard]] level::Dim paramsOf(const Dim& dim, const double psl) {
    return level::Dim{.psl = psl,
                      .sea = dim.sea,
                      .floodedness = dim.floodedness,
                      .spread = dim.spread,
                      .barrier = dim.barrier};
}

[[nodiscard]] Category categoryOf(const level::Verdict verdict) {
    switch (verdict) {
        case level::Verdict::Air:
            return Category::Air;
        case level::Verdict::Fluid:
            return Category::Water;
        case level::Verdict::Lava:
            return Category::Lava;
        case level::Verdict::Solid:
            return Category::Solid;
    }
    return Category::Solid;
}

/// What this build places at (x, y, z) — ChunkFiller's own path with the
/// dimension's constants: the global picker above the chunk's y_skip,
/// `aquifer::computeSubstance` at and below it. `Water` stands for the
/// default fluid.
[[nodiscard]] Category built(const aquifer::CentreSource& centres, const Dim& dim, const double psl,
                             const std::int32_t ySkipLevel, aquifer::StatusCache& cache,
                             const std::int32_t x, const std::int32_t y, const std::int32_t z) {
    return categoryOf(level::library(centres, paramsOf(dim, psl), ySkipLevel, cache, x, y, z));
}

/// A server block in an ICE world, by exact name: packed ice is the default
/// fluid. Anything else is a block this world cannot hold, and is returned
/// empty so the caller fails with its name.
[[nodiscard]] std::optional<Category> iceCategory(const stratum::chunk::BlockState* block) {
    if (block == nullptr) {
        return std::nullopt;
    }
    if (block->name == kIce) {
        return Category::Water;
    }
    const Category category = stratum::test::categoryOf(block->name);
    if (category == Category::Solid && block->name != "minecraft:stone") {
        return std::nullopt;
    }
    if (category == Category::Water) {
        return std::nullopt; // water has no source in an ice world
    }
    return category;
}

[[nodiscard]] const char* nameOf(const Category category) {
    switch (category) {
        case Category::Air:
            return "air";
        case Category::Water:
            return "default fluid";
        case Category::Lava:
            return "lava";
        case Category::Solid:
            return "stone";
    }
    return "?";
}

/// Each scored column of the window at @p stride, x fastest.
template<typename Visit>
void forEachColumn(const std::int32_t stride, Visit&& visit) {
    for (std::int32_t z = kEdgeMargin; z < kWindow - kEdgeMargin; z += stride) {
        for (std::int32_t x = kEdgeMargin; x < kWindow - kEdgeMargin; x += stride) {
            visit(x, z);
        }
    }
}

/// Which outcome of the level rule a source took — read off `cellLevel`'s
/// own answer, not recomputed: the global picker's sea that Q5.3 short-
/// circuits to (the near-surface return, or a source more than twenty above
/// a land surface) by its origin, the dry outcome by the sentinel, the gated
/// sea by its level, the ladder otherwise.
enum class Outcome : std::uint8_t { ShortCircuit, Sea, Ladder, Dry };
constexpr std::size_t kOutcomes = 4;

[[nodiscard]] Outcome outcomeOf(const aquifer::CellLevel& level, const std::int32_t sea) {
    if (level.origin == aquifer::LevelOrigin::NearSurfaceSea) {
        return Outcome::ShortCircuit;
    }
    if (level.level == aquifer::kNeverLevel) {
        return Outcome::Dry;
    }
    return level.level == sea ? Outcome::Sea : Outcome::Ladder;
}

/// The sources centred inside the scored window and rows of @p dim, by
/// outcome — the liveness of the rule's branches in this corpus. Model-only.
[[nodiscard]] std::array<long long, kOutcomes> outcomesIn(const aquifer::CentreSource& centres,
                                                          const Dim& dim) {
    std::array<long long, kOutcomes> counts{};
    const std::int32_t top = topRow(dim);
    for (std::int32_t cy = javamath::floorDiv(kFirstAssertedRow, aquifer::kCellPitchY) - 1;
         cy <= javamath::floorDiv(top, aquifer::kCellPitchY); ++cy) {
        for (std::int32_t cz = -1; cz <= kChunks; ++cz) {
            for (std::int32_t cx = -1; cx <= kChunks; ++cx) {
                const aquifer::CellIndex centre = centres.centreOf(cx, cy, cz);
                if (centre.x < kEdgeMargin || centre.x >= kWindow - kEdgeMargin ||
                    centre.z < kEdgeMargin || centre.z >= kWindow - kEdgeMargin ||
                    centre.y < kFirstAssertedRow || centre.y > top) {
                    continue;
                }
                const aquifer::CellFluid cell{.centreY = centre.y,
                                              .surface = aquifer::readPreliminarySurface(
                                                  level::constant(dim.psl), centre, dim.sea),
                                              .seaLevel = dim.sea,
                                              .floodedness = dim.floodedness,
                                              .spread = dim.spread,
                                              .deepDark = false};
                ++counts[static_cast<std::size_t>(outcomeOf(aquifer::cellLevel(cell), dim.sea))];
            }
        }
    }
    return counts;
}

[[nodiscard]] bool onOceanBranch(const Dim& dim) {
    return javamath::floorToInt(dim.psl) < dim.sea - aquifer::kOceanGateOffset;
}

[[nodiscard]] const Dim& dimNamed(const Corpus& corpus, const std::string_view name) {
    const auto found = std::find_if(corpus.dims.begin(), corpus.dims.end(),
                                    [&](const Dim& dim) { return dim.name == name; });
    INFO(corpus.dir << " has no dimension " << name);
    REQUIRE(found != corpus.dims.end());
    return *found;
}

/// Present ice corpora: none (the case skips), or all three (a partial set
/// fails rather than scoring whatever it finds).
[[nodiscard]] bool iceCorpusPresent() {
    std::size_t present = 0;
    for (const std::int64_t seed : kIceSeeds) {
        if (std::filesystem::is_regular_file(probes() / corpusName("levelice", seed) /
                                             "spec.json")) {
            ++present;
        }
    }
    INFO(present << " of " << kIceSeeds.size()
                 << " levelice corpora present; regenerate all with tools/probe-worlds generate "
                    "--only aquifer-level-probe.sh --accept-eula");
    REQUIRE((present == 0 || present == kIceSeeds.size()));
    return present == kIceSeeds.size();
}

} // namespace

TEST_CASE("the level rule on a constant surface, every block of three seeds in packed ice",
          "[conformance][aquifer]") {
    if (!iceCorpusPresent()) {
        SKIP("no levelice probes under " << probes() << "; generate them with " << kScript
                                         << " --accept-eula <seed> for seeds 42, 31337, 8675309");
    }
    long long blocks = 0;
    long long expectedBlocks = 0;
    long long lowRows = 0;
    long long lowRowsAgree = 0;
    for (const std::int64_t seed : kIceSeeds) {
        const Corpus corpus = openCorpus(corpusName("levelice", seed), seed);
        const aquifer::CentreSource centres{seed, stratum::density::RandomSource::Xoroshiro};
        INFO("seed " << seed);

        // The grid is whole: every surface -54, -51, ..., 141 once.
        std::set<std::int32_t> grid;
        for (const Dim& dim : corpus.dims) {
            REQUIRE(dim.fluid == kIce);
            if (dim.name.starts_with("g")) {
                REQUIRE(whole(dim.psl));
                grid.insert(javamath::floorToInt(dim.psl));
            }
        }
        REQUIRE(grid.size() == static_cast<std::size_t>(kGridSurfaces));
        REQUIRE(*grid.begin() == -54);
        REQUIRE(*grid.rbegin() == 141);

        // Liveness: every outcome of the rule owns sources on both branches.
        std::array<long long, kOutcomes> ocean{};
        std::array<long long, kOutcomes> off{};
        for (const Dim& dim : corpus.dims) {
            const auto counts = outcomesIn(centres, dim);
            auto& into = onOceanBranch(dim) ? ocean : off;
            for (std::size_t o = 0; o < kOutcomes; ++o) {
                into[o] += counts[o];
            }
        }
        INFO("ocean branch sources: near-surface " << ocean[0] << ", sea " << ocean[1]
                                                   << ", ladder " << ocean[2] << ", dry "
                                                   << ocean[3]);
        INFO("off-branch sources: more than twenty above the surface "
             << off[0] << ", sea " << off[1] << ", ladder " << off[2] << ", dry " << off[3]);
        for (const long long count : ocean) {
            CHECK(count >= 200);
        }
        for (const long long count : off) {
            CHECK(count >= 50);
        }

        for (const Dim& dim : corpus.dims) {
            INFO("dimension " << dim.name << " (psl " << dim.psl << ", sea " << dim.sea
                              << ", floodedness " << dim.floodedness << ", spread " << dim.spread
                              << ", barrier " << dim.barrier << ")");
            stratum::test::GoldenRegion golden(regionOf(corpus, dim));
            aquifer::StatusCache cache; // every input constant: one per world
            const std::int32_t ySkipLevel = ySkipOf(dim.psl);
            const std::int32_t top = topRow(dim);
            long long dimBlocks = 0;
            long long disagree = 0;
            std::set<std::string> foreign;
            std::string samples;
            forEachColumn(4, [&](const std::int32_t x, const std::int32_t z) {
                REQUIRE(golden.hasChunk(x / 16, z / 16));
                for (std::int32_t y = kLambda; y <= top; ++y) {
                    const auto* block = golden.blockAt(x, y, z);
                    const Category ours = built(centres, dim, dim.psl, ySkipLevel, cache, x, y, z);
                    const std::optional<Category> theirs = iceCategory(block);
                    if (y < kFirstAssertedRow) {
                        ++lowRows;
                        lowRowsAgree += (theirs.has_value() && *theirs == ours) ? 1 : 0;
                        continue;
                    }
                    if (!theirs.has_value()) {
                        foreign.insert(block != nullptr ? block->name : std::string("<none>"));
                        continue;
                    }
                    ++dimBlocks;
                    if (*theirs != ours) {
                        ++disagree;
                        if (disagree <= 6) {
                            samples += " [" + std::to_string(x) + " " + std::to_string(y) + " " +
                                       std::to_string(z) + ": server " + nameOf(*theirs) +
                                       ", built " + nameOf(ours) + "]";
                        }
                    }
                }
            });
            std::string foreignNames;
            for (const std::string& name : foreign) {
                foreignNames += " " + name;
            }
            INFO("blocks an ice world cannot hold:" << foreignNames);
            CHECK(foreign.empty());
            INFO("first disagreements:" << samples);
            CHECK(disagree == 0);
            blocks += dimBlocks;
            expectedBlocks += kStride4Columns * (top - kFirstAssertedRow + 1);
        }
    }
    // Every column at stride 4 from -51 to 16 above the higher of the sea
    // and the surface was read: about 3.5e7 blocks over the 255 dimensions.
    CHECK(blocks == expectedBlocks);
    CHECK(blocks > 30'000'000);
    // Rows -54..-52, where lava can stand beside packed ice: reported only.
    INFO("rows -54..-52: " << lowRowsAgree << " of " << lowRows << " blocks agree");
    CHECK(lowRows > 0);
}

TEST_CASE("a source more than twenty above a land surface takes the sea, Q5.3(a)",
          "[conformance][aquifer]") {
    if (!iceCorpusPresent()) {
        SKIP("no levelice probes under " << probes());
    }
    // The l<psl> arms sit off the ocean branch at its edge (sea psl + 8),
    // barrier +2, and the m<psl> arms with the sea 30 lower. Each rival in
    // aquifer-level-rivals.hpp reads Q5.3(a) another way for a source off the
    // ocean branch — not at all (the build before this probe), at a margin of
    // 19 or 21, or keyed on the sea — and every block where one parts from
    // the build, the server's block is the build's. Every column, over the
    // rows around the sea's level where the clause can show at all (a source
    // it moves is centred more than twelve above the sea, and walls itself
    // off just above that level).
    std::array<long long, level::kRivalCount> parted{};
    std::array<long long, level::kRivalCount> serverBuilt{};
    std::array<long long, level::kRivalCount> serverRival{};
    long long landArms = 0;
    for (const std::int64_t seed : kIceSeeds) {
        const Corpus corpus = openCorpus(corpusName("levelice", seed), seed);
        const aquifer::CentreSource centres{seed, stratum::density::RandomSource::Xoroshiro};
        for (const Dim& dim : corpus.dims) {
            if (!dim.name.starts_with("l") && !dim.name.starts_with("m")) {
                continue;
            }
            INFO("seed " << seed << ", " << dim.name);
            REQUIRE(!onOceanBranch(dim));
            ++landArms;
            const level::Dim params = paramsOf(dim, dim.psl);
            stratum::test::GoldenRegion world(regionOf(corpus, dim));
            aquifer::StatusCache cache;
            level::Statuses shipped(params, nullptr);
            std::vector<level::Statuses> rivals;
            rivals.reserve(level::kRivals.size());
            for (const level::Rival& rival : level::kRivals) {
                rivals.emplace_back(params, &rival);
            }
            const std::int32_t ySkipLevel = ySkipOf(dim.psl);
            // Every row read is the lattice's: under y_skip, over lambda.
            REQUIRE(level::latticeDecides(params, ySkipLevel, dim.sea - 4));
            REQUIRE(level::latticeDecides(params, ySkipLevel, dim.sea + 8));
            long long copyDrift = 0;
            forEachColumn(1, [&](const std::int32_t x, const std::int32_t z) {
                for (std::int32_t y = dim.sea - 4; y <= dim.sea + 8; ++y) {
                    const level::Verdict ours =
                        level::library(centres, params, ySkipLevel, cache, x, y, z);
                    const aquifer::Selection selection = aquifer::selectSources(centres, x, y, z);
                    copyDrift += level::decideRanked(selection, params, shipped, y) != ours ? 1 : 0;
                    const std::optional<Category> server = iceCategory(world.blockAt(x, y, z));
                    for (std::size_t r = 0; r < rivals.size(); ++r) {
                        const level::Verdict other =
                            level::decideRanked(selection, params, rivals.at(r), y);
                        if (other == ours) {
                            continue;
                        }
                        ++parted.at(r);
                        serverBuilt.at(r) += server == categoryOf(ours) ? 1 : 0;
                        serverRival.at(r) += server == categoryOf(other) ? 1 : 0;
                    }
                }
            });
            // The rivals' shared decision is the library's, block for block.
            CHECK(copyDrift == 0);
        }
    }
    REQUIRE(landArms == 30);
    for (std::size_t r = 0; r < level::kRivals.size(); ++r) {
        const level::Rival& rival = level::kRivals.at(r);
        INFO(rival.name << " parts from the build on " << parted.at(r) << " blocks; the server "
                        << "holds the build's on " << serverBuilt.at(r) << ", the rival's on "
                        << serverRival.at(r));
        // Each rival is refuted where it parts, and parts often enough to be.
        CHECK(parted.at(r) >= 300);
        CHECK(serverBuilt.at(r) == parted.at(r));
        CHECK(serverRival.at(r) == 0);
    }
}

TEST_CASE("the bonus spelling at both exact crossings, against the world it would build",
          "[conformance][aquifer]") {
    if (!iceCorpusPresent()) {
        SKIP("no levelice probes under " << probes());
    }
    // x11 and x6 sit exactly on the sea gate's crossing at reach 45 (depth
    // 11) and reach 50 (depth 6), where a pre-divided fl(11/640) fires and
    // (reach * 11) / 640 does not. Their controls sit a ten-millionth
    // higher, where both spellings fire: so each control IS the world the
    // pre-divided spelling predicts for its twin (it moves no other gate:
    // the local gate's crossing is depth 36 and 31, nowhere near either
    // value). Every block where the two built worlds part is a block that
    // tells the spellings apart, and the server's own two worlds are read
    // there, every column, over the rows a cell centred at 30 can own.
    constexpr std::int32_t kFromRow = 10;
    constexpr std::int32_t kToRow = 50;
    for (const std::int64_t seed : kIceSeeds) {
        const Corpus corpus = openCorpus(corpusName("levelice", seed), seed);
        const aquifer::CentreSource centres{seed, stratum::density::RandomSource::Xoroshiro};
        for (const auto& [exact, above] : {std::pair<const char*, const char*>{"x11", "x11c"},
                                           std::pair<const char*, const char*>{"x6", "x6c"}}) {
            INFO("seed " << seed << ", " << exact << " against " << above);
            const Dim& atCrossing = dimNamed(corpus, exact);
            const Dim& control = dimNamed(corpus, above);
            REQUIRE(same(atCrossing.psl, control.psl));
            REQUIRE(atCrossing.floodedness < control.floodedness);
            stratum::test::GoldenRegion crossingWorld(regionOf(corpus, atCrossing));
            stratum::test::GoldenRegion controlWorld(regionOf(corpus, control));
            aquifer::StatusCache crossingCache;
            aquifer::StatusCache controlCache;
            const std::int32_t ySkipLevel = ySkipOf(atCrossing.psl);
            long long parted = 0;
            long long crossingAsBuilt = 0;
            long long crossingAsPreDivided = 0;
            long long controlAsBuilt = 0;
            forEachColumn(1, [&](const std::int32_t x, const std::int32_t z) {
                for (std::int32_t y = kFromRow; y <= kToRow; ++y) {
                    const Category shipped = built(centres, atCrossing, atCrossing.psl, ySkipLevel,
                                                   crossingCache, x, y, z);
                    const Category preDivided =
                        built(centres, control, control.psl, ySkipLevel, controlCache, x, y, z);
                    if (shipped == preDivided) {
                        continue;
                    }
                    ++parted;
                    const std::optional<Category> server =
                        iceCategory(crossingWorld.blockAt(x, y, z));
                    const std::optional<Category> serverControl =
                        iceCategory(controlWorld.blockAt(x, y, z));
                    crossingAsBuilt += (server == shipped) ? 1 : 0;
                    crossingAsPreDivided += (server == preDivided) ? 1 : 0;
                    controlAsBuilt += (serverControl == preDivided) ? 1 : 0;
                }
            });
            INFO(parted << " blocks part the spellings");
            // Enough cells at the crossing depth in every seed's window to
            // decide it (each owns a few thousand blocks).
            REQUIRE(parted >= 1000);
            CHECK(crossingAsBuilt == parted);
            CHECK(crossingAsPreDivided == 0);
            CHECK(controlAsBuilt == parted);
        }
    }
}

TEST_CASE("a fractional preliminary surface is floored, not truncated or rounded",
          "[conformance][aquifer]") {
    if (!iceCorpusPresent()) {
        SKIP("no levelice probes under " << probes());
    }
    // Flooring, truncation toward zero and Java's Math.round give different
    // integers at -10.4 (-11 / -10 / -10), -10.6 (-11 / -10 / -11), -20.5
    // (-21 / -20 / -20), -20.25 (-21 / -20 / -20) and 40.5 (40 / 40 / 41).
    // Each rival is replayed as the build would run with that integer as its
    // surface, which moves the near-surface edge, both gates' depths, the
    // ladder's cap and y_skip alike.
    long long truncParted = 0;
    long long roundParted = 0;
    long long serverFloored = 0;
    long long serverTruncated = 0;
    long long serverRounded = 0;
    for (const std::int64_t seed : kIceSeeds) {
        const Corpus corpus = openCorpus(corpusName("levelice", seed), seed);
        const aquifer::CentreSource centres{seed, stratum::density::RandomSource::Xoroshiro};
        for (const char* name : {"fr104", "fr106", "fr205", "fr2025", "fr405"}) {
            INFO("seed " << seed << ", " << name);
            const Dim& dim = dimNamed(corpus, name);
            REQUIRE(!whole(dim.psl));
            const double truncated = std::trunc(dim.psl);
            const double rounded = std::floor(dim.psl + 0.5); // Java's Math.round
            stratum::test::GoldenRegion world(regionOf(corpus, dim));
            aquifer::StatusCache floorCache;
            aquifer::StatusCache truncCache;
            aquifer::StatusCache roundCache;
            const std::int32_t top = topRow(dim);
            forEachColumn(4, [&](const std::int32_t x, const std::int32_t z) {
                for (std::int32_t y = kFirstAssertedRow; y <= top; ++y) {
                    const Category floored =
                        built(centres, dim, dim.psl, ySkipOf(dim.psl), floorCache, x, y, z);
                    const Category viaTrunc =
                        built(centres, dim, truncated, ySkipOf(truncated), truncCache, x, y, z);
                    const Category viaRound =
                        built(centres, dim, rounded, ySkipOf(rounded), roundCache, x, y, z);
                    if (floored == viaTrunc && floored == viaRound) {
                        continue;
                    }
                    const std::optional<Category> server = iceCategory(world.blockAt(x, y, z));
                    if (floored != viaTrunc) {
                        ++truncParted;
                        serverTruncated += (server == viaTrunc) ? 1 : 0;
                    }
                    if (floored != viaRound) {
                        ++roundParted;
                        serverRounded += (server == viaRound) ? 1 : 0;
                    }
                    serverFloored += (server == floored) ? 1 : 0;
                }
            });
        }
    }
    INFO("truncation parts on " << truncParted << " blocks, rounding on " << roundParted);
    REQUIRE(truncParted >= 1000);
    REQUIRE(roundParted >= 1000);
    CHECK(serverTruncated == 0);
    CHECK(serverRounded == 0);
    // Every block where either rival parts from flooring is the floor's.
    CHECK(serverFloored >= std::max(truncParted, roundParted));
}

TEST_CASE("packed ice stands where water stands: the level probe's water twin",
          "[conformance][aquifer]") {
    const std::string waterName = corpusName("levelwater", kWaterSeed);
    if (!std::filesystem::is_regular_file(probes() / waterName / "spec.json")) {
        SKIP("no " << waterName << " probe under " << probes() << "; generate it with " << kScript
                   << " --accept-eula --water " << kWaterSeed);
    }
    const Corpus water = openCorpus(waterName, kWaterSeed);
    const Corpus ice = openCorpus(corpusName("levelice", kWaterSeed), kWaterSeed);
    const aquifer::CentreSource centres{kWaterSeed, stratum::density::RandomSource::Xoroshiro};
    REQUIRE(water.dims.size() == 4);

    long long blocks = 0;
    long long flow = 0;
    long long twinBlocks = 0;
    long long twinFlow = 0;
    long long lowTwin = 0;
    long long lowTwinAgree = 0;
    for (const Dim& wet : water.dims) {
        INFO("dimension " << wet.name);
        REQUIRE(wet.fluid == kWater);
        const Dim& frozen = dimNamed(ice, wet.name);
        // The same dimension in every input but the fluid.
        REQUIRE(same(frozen.psl, wet.psl));
        REQUIRE(frozen.sea == wet.sea);
        REQUIRE(same(frozen.floodedness, wet.floodedness));
        REQUIRE(same(frozen.spread, wet.spread));
        REQUIRE(same(frozen.barrier, wet.barrier));

        stratum::test::GoldenRegion waterWorld(regionOf(water, wet));
        stratum::test::GoldenRegion iceWorld(regionOf(ice, frozen));
        aquifer::StatusCache cache;
        const std::int32_t ySkipLevel = ySkipOf(wet.psl);
        const std::int32_t top = topRow(wet);
        long long unexplained = 0;
        long long twinUnexplained = 0;
        std::string samples;
        forEachColumn(2, [&](const std::int32_t x, const std::int32_t z) {
            REQUIRE(waterWorld.hasChunk(x / 16, z / 16));
            for (std::int32_t y = kLambda; y <= top; ++y) {
                const auto* block = waterWorld.blockAt(x, y, z);
                const Category server = stratum::test::categoryOf(
                    block != nullptr ? block->name : std::string("minecraft:air"));
                // The water world against this build, from lambda: Q6.3 and
                // the mixed-type pressure are measured with water.
                const Category ours = built(centres, wet, wet.psl, ySkipLevel, cache, x, y, z);
                ++blocks;
                if (server != ours) {
                    if (stratum::test::explainedByFlow(waterWorld, x, y, z, server, ours)) {
                        ++flow;
                    } else {
                        ++unexplained;
                        if (unexplained <= 6) {
                            samples += " [" + std::to_string(x) + " " + std::to_string(y) + " " +
                                       std::to_string(z) + ": server " +
                                       (block != nullptr ? block->toString() : "<none>") +
                                       ", built " + nameOf(ours) + "]";
                        }
                    }
                }
                // The twin: the ice world's block where the water world's is.
                const std::optional<Category> frozenBlock = iceCategory(iceWorld.blockAt(x, y, z));
                if (y < kFirstAssertedRow) {
                    ++lowTwin;
                    lowTwinAgree += (frozenBlock == server) ? 1 : 0;
                    continue;
                }
                ++twinBlocks;
                if (frozenBlock == server) {
                    continue;
                }
                if (frozenBlock.has_value() &&
                    stratum::test::explainedByFlow(waterWorld, x, y, z, server, *frozenBlock)) {
                    ++twinFlow;
                } else {
                    ++twinUnexplained;
                }
            }
        });
        INFO("first unexplained:" << samples);
        CHECK(unexplained == 0);
        CHECK(twinUnexplained == 0);
    }
    INFO("water against the build: "
         << flow << " of " << blocks << " blocks moved; twin: " << twinFlow << " of " << twinBlocks
         << "; rows -54..-52: " << lowTwinAgree << " of " << lowTwin << " agree");
    // A frozen world's remnant is run-dependent (SPEC §7): bounded, never
    // pinned. Each of these is a small fraction of what it scores.
    CHECK(flow * 1000 < blocks);
    CHECK(twinFlow * 1000 < twinBlocks);
    CHECK(blocks > 1'000'000);
}
