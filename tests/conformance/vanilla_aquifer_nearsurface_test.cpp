// Stratum — the near-surface probe: the floor's comparand and the abort's sea.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// SPEC §10's MA blocker 2 closed on two findings that lived only in an
// analyzer (tools/analysis/aquifer-nearsurface-analyze.cpp), on a corpus
// nothing regenerated: the aborting near-surface floor compares the centre
// with the scan's `cap`, not its `gate`; and an aborted scan off the
// near-surface path is refused the sea that floodedness would grant it.
// `tools/analysis/aquifer-nearsurface-probe.sh` is the world that separates
// both: a three-valued surface (96 / -20 / -70: above `sea_level - 8`, below
// it, below the scan's abort), floodedness 0.9 past both of the level rule's
// gates, the barrier noise held at -2.0, and a readout dimension per scale.
// tools/probe-worlds generates it for seeds 42 and 31337.
//
// Three subsets of sources, each against one rival reading of its own
// branch, and nothing else changed:
//
//   floor   on the near-surface path, aborted, `gate != cap`: the build
//           takes the sea when the centre is more than twenty above `cap`;
//           the rival reads `gate` there.
//   above   off the near-surface path, aborted, centred at or above lambda,
//           anchor at or above `sea_level - 8`: the build takes A_lava (-54,
//           lava) before the level rule; the rival ignores the abort, which
//           at floodedness 0.9 is the sea.
//   below   the same with the anchor below `sea_level - 8` (the depth path).
//
// Each is scored two ways. The ANALYZER'S READOUT, reproduced here block for
// block: every block of the footprint whose nearest source is in the subset,
// water or lava counted wet and air dry (anything else skipped), against a
// bare `y < level` of that nearest source — the figures MA blocker 2 quoted.
// And the DECISION: the filler's own path (the global picker above the
// chunk's y_skip, `aquifer::computeSubstance` at and below it), with the
// rival's statuses through `computeSubstanceWith` wherever a ranked source
// of the subset reads differently under it, on rows lambda to 70 of every
// second column on each axis two in from the footprint's edge, and on every
// block the readout misses. Every block the build gets wrong is put to
// support/fluid_flow.hpp's `explainedByFlow`.
//
// WHAT THE READOUT'S RESIDUAL WAS. MA blocker 2 quoted 0.9911-0.9941 and
// 0.9812-0.9829 for refusing the sea, a 0.6-1.9% shortfall nothing
// explained, off a corpus generated before probe worlds were frozen. On
// these frozen corpora the same readout scores 0.9979-0.9996, and every
// block it misses inside the margin is fluid that moved: water flowing, or
// made a source again, on the rows where a sea source's water meets an
// A_lava source's air with no wall between them. None of it is Q5.3(a) off
// the ocean branch (pipeline engine v12), which reaches no source here.
//
// FLOW. The worlds are frozen but keep a run-dependent remnant (SPEC §7), so
// every count that a server block decides is bounded, never pinned; only
// what the model alone decides (which sources take which branch, and where
// the readings part) is exact.
//
// About a minute in a Debug build on a loaded four-core machine, most of it
// the readout's every-block pass.
//
// The fixtures are Mojang-derived and never committed (SPEC §12).
#include "support/fluid_flow.hpp"
#include "support/probe_corpus.hpp"

#include <stratum/aquifer/fluid_type.hpp>
#include <stratum/aquifer/lattice.hpp>
#include <stratum/aquifer/sampling.hpp>
#include <stratum/aquifer/selection.hpp>
#include <stratum/aquifer/substance.hpp>
#include <stratum/chunk/chunk.hpp>
#include <stratum/density/random_source.hpp>
#include <stratum/javamath.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/noise/perlin.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/rng/xoroshiro128.hpp>

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
#include <tuple>
#include <vector>

namespace {

namespace aquifer = stratum::aquifer;
namespace javamath = stratum::javamath;
using stratum::test::Category;

constexpr std::array<std::int64_t, 2> kSeeds{42, 31337};
constexpr const char* kScript = "tools/analysis/aquifer-nearsurface-probe.sh";

constexpr std::int32_t kSeaLevel = 63;
constexpr std::int32_t kChunks = 8;
constexpr std::int32_t kSpan = kChunks * 16;
/// Two blocks in from the forceloaded footprint on every side, where the
/// decision is scored: the flow classifier reads a block's neighbours, and
/// the region holds nothing west or north of the footprint.
constexpr std::int32_t kEdgeMargin = 2;
/// The decision is scored on every second column on each axis (a quarter of
/// the footprint), and on every block the analyzer's readout misses.
constexpr std::int32_t kColumnStride = 2;
constexpr std::int32_t kMinY = -64;
constexpr std::int32_t kMaxY = 319;
/// The highest row the decision is scored on. Every level here is the sea
/// (63) or A_lava (-54), and at barrier -2.0 a wall stands only where a
/// pair's `u` passes 2 — strictly between the two levels — so above the sea
/// every reading is air (the case checks the server agrees).
constexpr std::int32_t kTopY = 70;
/// The rows inside the sea's top and inside lambda where a sea source and an
/// A_lava one meet with no wall between them, or the thinnest. The pair's
/// levels are 117 apart, and its pressure weighs the barrier noise wherever
/// its `u` is within 2 — the three rows under 63 and the three from lambda
/// up — where -2.0 keeps any wall from forming. On the fourth (59 and -51)
/// `u` is 2.33 and 2.17, the noise is not weighed, and the term fires only
/// where the pair's similarity passes about 0.21 and 0.23; further in, the
/// wall thickens.
constexpr std::int32_t kContactRows = 4;

/// The probe's constants, checked against its spec.json rather than trusted.
constexpr double kFloodedness = 0.9;
constexpr double kBarrier = -2.0;
constexpr double kLow = -70.0;
constexpr double kMid = -20.0;
constexpr double kHigh = 96.0;
constexpr double kThresholdLow = -0.25;
constexpr double kThresholdHigh = 0.25;

/// Exact comparison by bits: every value compared is a constant a spec wrote.
[[nodiscard]] bool same(const double a, const double b) {
    return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

[[nodiscard]] std::filesystem::path probes() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11" / "probes";
}

[[nodiscard]] std::filesystem::path corpusDir(const std::int64_t seed) {
    return probes() / ("nearsurface_s" + std::to_string(seed));
}

/// One aquifer dimension of the probe and the readout that names its field.
struct Arm {
    const char* world;
    const char* readout;
};

constexpr std::array<Arm, 2> kArms{{{"nsf_8", "nsr_8"}, {"nsf_16", "nsr_16"}}};

// --- the spec --------------------------------------------------------------

/// A constant density function: a bare number, or `minecraft:constant`.
[[nodiscard]] double constantOf(const nlohmann::json& node) {
    if (node.is_number()) {
        return node.get<double>();
    }
    REQUIRE(node.at("type").get<std::string>() == "minecraft:constant");
    return node.at("argument").get<double>();
}

/// The probe's three-armed field: `range_choice` over one noise, nested.
struct FieldSpec {
    double xzScale = 0.0;
    double thresholdLow = 0.0;
    double thresholdHigh = 0.0;
    std::array<double, 3> arms{};
};

[[nodiscard]] FieldSpec parseField(const nlohmann::json& node) {
    FieldSpec field;
    REQUIRE(node.at("type").get<std::string>() == "minecraft:range_choice");
    const nlohmann::json& input = node.at("input");
    REQUIRE(input.at("type").get<std::string>() == "minecraft:noise");
    REQUIRE(input.at("noise").get<std::string>() == "stratum:probe_noise");
    REQUIRE(same(input.at("y_scale").get<double>(), 0.0));
    field.xzScale = input.at("xz_scale").get<double>();
    REQUIRE(same(node.at("min_inclusive").get<double>(), -1000.0));
    field.thresholdLow = node.at("max_exclusive").get<double>();
    const nlohmann::json& upper = node.at("when_out_of_range");
    REQUIRE(upper.at("type").get<std::string>() == "minecraft:range_choice");
    REQUIRE(upper.at("input") == input);
    REQUIRE(same(upper.at("min_inclusive").get<double>(), -1000.0));
    field.thresholdHigh = upper.at("max_exclusive").get<double>();
    field.arms = {constantOf(node.at("when_in_range")), constantOf(upper.at("when_in_range")),
                  constantOf(upper.at("when_out_of_range"))};
    REQUIRE(same(field.thresholdLow, kThresholdLow));
    REQUIRE(same(field.thresholdHigh, kThresholdHigh));
    return field;
}

[[nodiscard]] const nlohmann::json& entryNamed(const nlohmann::json& spec, const char* name) {
    const nlohmann::json* found = nullptr;
    for (const auto& entry : spec) {
        if (entry.at("name").get<std::string>() == name) {
            found = &entry;
        }
    }
    INFO("spec.json has no entry " << name);
    REQUIRE(found != nullptr);
    return *found;
}

/// One arm's field, refusing anything this case does not replay: every
/// router input it scores against is the constant written here, and the
/// readout reads the same noise at the same scale and thresholds.
[[nodiscard]] FieldSpec parseArm(const nlohmann::json& spec, const Arm& arm) {
    const nlohmann::json& world = entryNamed(spec, arm.world);
    INFO("spec entry " << arm.world);
    REQUIRE(world.at("min_y").get<std::int32_t>() == kMinY);
    REQUIRE(world.at("height").get<std::int32_t>() == kMaxY - kMinY + 1);
    REQUIRE(world.at("aquifers_enabled").get<bool>());
    REQUIRE(world.at("sea_level").get<std::int32_t>() == kSeaLevel);
    REQUIRE(same(constantOf(world.at("raw_final_density")), -1.0));
    REQUIRE(world.at("default_fluid").at("Name").get<std::string>() == "minecraft:water");
    const nlohmann::json& router = world.at("router");
    REQUIRE(same(constantOf(router.at("barrier")), kBarrier));
    REQUIRE(same(constantOf(router.at("lava")), 0.0));
    REQUIRE(same(constantOf(router.at("fluid_level_floodedness")), kFloodedness));
    REQUIRE(same(constantOf(router.at("fluid_level_spread")), 0.0));
    const FieldSpec field = parseField(router.at("preliminary_surface_level"));
    REQUIRE(same(field.arms[0], kLow));
    REQUIRE(same(field.arms[1], kMid));
    REQUIRE(same(field.arms[2], kHigh));

    const nlohmann::json& readout = entryNamed(spec, arm.readout);
    INFO("spec entry " << arm.readout);
    const FieldSpec indicator = parseField(readout.at("function"));
    REQUIRE(same(indicator.xzScale, field.xzScale));
    REQUIRE(same(indicator.arms[0], -1.0));
    REQUIRE(same(indicator.arms[1], 0.0));
    REQUIRE(same(indicator.arms[2], 1.0));
    return field;
}

// --- the field ---------------------------------------------------------------

/// The surface field, rebuilt from the noise the manifest declares — so it
/// is defined wherever a scan reads, past the footprint too — and checked
/// against the readout dimension wherever the aquifer can read it.
class Field {
public:
    Field(const stratum::noise::NormalNoise& noise, const FieldSpec& spec)
        : noise_(&noise), spec_(spec) {}

    [[nodiscard]] double at(const std::int32_t x, const std::int32_t z) const {
        const double n = noise_->sample(static_cast<double>(x) * spec_.xzScale, 0.0,
                                        static_cast<double>(z) * spec_.xzScale);
        if (n < spec_.thresholdLow) {
            return spec_.arms[0];
        }
        return n < spec_.thresholdHigh ? spec_.arms[1] : spec_.arms[2];
    }

    /// `preliminary_surface_level` as a router read.
    [[nodiscard]] double operator()(const std::int32_t x, std::int32_t /*y*/,
                                    const std::int32_t z) const {
        return at(x, z);
    }

private:
    const stratum::noise::NormalNoise* noise_;
    FieldSpec spec_;
};

/// The rebuilt field against the readout at every quart corner of the
/// footprint — the only columns the aquifer reads (its anchors are
/// multiples of four, its offsets of sixteen, y_skip's samples of four), and
/// the ones `flat_cache` pins the readout to. Returns the corners that
/// disagree; @p corners counts those read.
[[nodiscard]] long long readoutMismatches(const std::filesystem::path& readout, const Field& field,
                                          long long& corners) {
    const auto file = stratum::region::RegionFile::open(readout);
    long long mismatches = 0;
    for (std::int32_t cz = 0; cz < kChunks; ++cz) {
        for (std::int32_t cx = 0; cx < kChunks; ++cx) {
            REQUIRE(file.hasChunk(cx, cz));
            const auto chunk =
                stratum::chunk::Chunk::decode(stratum::nbt::read(file.readChunk(cx, cz)).root);
            for (std::int32_t lz = 0; lz < 16; lz += 4) {
                for (std::int32_t lx = 0; lx < 16; lx += 4) {
                    std::int32_t top = kMinY - 1;
                    for (std::int32_t y = 250; y >= kMinY; --y) {
                        const auto* block = chunk.blockAt(lx, y, lz);
                        if (block != nullptr && block->name != "minecraft:air") {
                            top = y;
                            break;
                        }
                    }
                    // K * F + gradient = 0 puts the -1 / 0 / +1 arms near
                    // y 61, 128 and 195.
                    const double arm = top < 100 ? kLow : (top < 160 ? kMid : kHigh);
                    ++corners;
                    mismatches += static_cast<long long>(
                        !same(arm, field.at((cx * 16) + lx, (cz * 16) + lz)));
                }
            }
        }
    }
    return mismatches;
}

// --- the readings ------------------------------------------------------------

enum class Subset : std::uint8_t { None, Floor, Above, Below };
constexpr std::array<Subset, 3> kSubsets{Subset::Floor, Subset::Above, Subset::Below};

[[nodiscard]] std::size_t indexOf(const Subset subset) {
    return static_cast<std::size_t>(subset) - 1U;
}

[[nodiscard]] const char* nameOf(const Subset subset) {
    switch (subset) {
        case Subset::Floor:
            return "near-surface floor, cap against gate";
        case Subset::Above:
            return "aborted, anchor at or above sea-8, A_lava against the sea";
        case Subset::Below:
            return "aborted, anchor below sea-8, A_lava against the sea";
        case Subset::None:
            break;
    }
    return "none";
}

[[nodiscard]] bool onNearSurfacePath(const aquifer::CellFluid& cell) {
    const std::int32_t oceanGate = javamath::wrappingSub(cell.seaLevel, aquifer::kOceanGateOffset);
    return cell.surface.gate < oceanGate &&
           javamath::wrappingSub(cell.surface.gate, cell.centreY) < aquifer::kNearSurfaceDepth;
}

/// Which rival reading a source's own branch is open to, from `cellLevel`'s
/// documented conditions.
[[nodiscard]] Subset subsetOf(const aquifer::CellFluid& cell) {
    if (!cell.surface.aborted) {
        return Subset::None;
    }
    if (onNearSurfacePath(cell)) {
        return cell.surface.gate != cell.surface.cap ? Subset::Floor : Subset::None;
    }
    if (cell.centreY < aquifer::lambdaLevel(cell.seaLevel)) {
        return Subset::None; // the trailing guard's A_lava under either reading
    }
    const std::int32_t oceanGate = javamath::wrappingSub(cell.seaLevel, aquifer::kOceanGateOffset);
    return cell.surface.anchor >= oceanGate ? Subset::Above : Subset::Below;
}

/// The aborting near-surface floor, spelled with @p comparand where the build
/// reads `cap`: the sea for a centre at or above lambda more than twenty
/// above it, A_lava otherwise; typed as the library types those origins.
[[nodiscard]] aquifer::SourceStatus floorStatus(const aquifer::CellFluid& cell,
                                                const std::int32_t comparand, const double lava) {
    const bool sea =
        cell.centreY >= aquifer::lambdaLevel(cell.seaLevel) &&
        cell.centreY > javamath::wrappingAdd(comparand, aquifer::kNearSurfaceFloorOffset);
    const aquifer::CellLevel level =
        sea ? aquifer::CellLevel{.level = cell.seaLevel,
                                 .origin = aquifer::LevelOrigin::NearSurfaceSea}
            : aquifer::CellLevel{.level = aquifer::kLavaLevel,
                                 .origin = aquifer::LevelOrigin::GlobalLava};
    return aquifer::SourceStatus{
        .level = level.level,
        .type = aquifer::fluidTypeOf(aquifer::FluidTypeAt{.centreY = cell.centreY,
                                                          .level = level.level,
                                                          .seaLevel = cell.seaLevel,
                                                          .lava = lava,
                                                          .origin = level.origin})};
}

/// One source: its inputs, the build's status, which subset its branch is
/// in, and the rival reading's status (the build's when it is in none).
struct SourceInfo {
    aquifer::SourceStatus built{};
    aquifer::SourceStatus rival{};
    Subset subset = Subset::None;
    /// The build's status is not what the branch's documented reading gives
    /// (for the abort's two subsets: A_lava, with the sea as the rival).
    /// Either way the case would have picked out something other than the
    /// branch it names.
    bool inconsistent = false;
};

/// A router entry the probe holds constant.
[[nodiscard]] auto constant(const double value) {
    return [value](std::int32_t, std::int32_t, std::int32_t) { return value; };
}

/// A source's inputs to its status, read where the library reads them.
[[nodiscard]] aquifer::RankedCell inputsOf(const aquifer::Source& source, const Field& field) {
    return aquifer::rankedCellOf(source, kSeaLevel, field, constant(kFloodedness), constant(0.0),
                                 constant(0.0), aquifer::NoDeepDark{});
}

[[nodiscard]] SourceInfo sourceInfoOf(const aquifer::Source& source, const Field& field) {
    const aquifer::RankedCell inputs = inputsOf(source, field);
    SourceInfo info;
    info.built = aquifer::sourceStatus(inputs.cell, inputs.lava);
    info.rival = info.built;
    info.subset = subsetOf(inputs.cell);
    switch (info.subset) {
        case Subset::Floor: {
            const aquifer::SourceStatus asBuilt =
                floorStatus(inputs.cell, inputs.cell.surface.cap, inputs.lava);
            info.rival = floorStatus(inputs.cell, inputs.cell.surface.gate, inputs.lava);
            info.inconsistent =
                asBuilt.level != info.built.level || asBuilt.type != info.built.type;
            break;
        }
        case Subset::Above:
        case Subset::Below: {
            aquifer::CellFluid ignored = inputs.cell;
            ignored.surface.aborted = false;
            info.rival = aquifer::sourceStatus(ignored, inputs.lava);
            info.inconsistent = info.built.level != aquifer::kLavaLevel ||
                                info.built.type != aquifer::FluidType::Lava ||
                                info.rival.level != kSeaLevel ||
                                info.rival.type != aquifer::FluidType::Default;
            break;
        }
        case Subset::None:
            break;
    }
    return info;
}

using Key = std::tuple<std::int32_t, std::int32_t, std::int32_t>;

[[nodiscard]] Key keyOf(const aquifer::CellIndex& at) {
    return Key{at.x, at.y, at.z};
}

/// Every source an arm's blocks rank, memoized by centre. The last one asked
/// for is held apart: along a column the nearest source changes once in
/// about twelve rows.
class Sources {
public:
    explicit Sources(const Field& field) : field_(&field) {}

    [[nodiscard]] const SourceInfo& of(const aquifer::Source& source) {
        const Key key = keyOf(source.centre);
        if (last_ != nullptr && key == lastKey_) {
            return *last_;
        }
        auto found = memo_.find(key);
        if (found == memo_.end()) {
            found = memo_.emplace(key, sourceInfoOf(source, *field_)).first;
            inconsistent_ += static_cast<long long>(found->second.inconsistent);
        }
        lastKey_ = key;
        last_ = &found->second;
        return found->second;
    }

    /// Sources ranked so far whose status is not their branch's reading.
    [[nodiscard]] long long inconsistent() const { return inconsistent_; }

private:
    const Field* field_;
    std::map<Key, SourceInfo> memo_;
    long long inconsistent_ = 0;
    Key lastKey_{};
    const SourceInfo* last_ = nullptr;
};

/// `selectSources`, split at its two public halves so that the twelve
/// candidates of a home cell are drawn once rather than per block.
class Ranker {
public:
    explicit Ranker(const aquifer::CentreSource& centres) : centres_(&centres) {}

    [[nodiscard]] aquifer::Selection rank(const std::int32_t x, const std::int32_t y,
                                          const std::int32_t z) {
        const Key key = keyOf(aquifer::cellOf(x, y, z));
        if (last_ == nullptr || key != lastKey_) {
            auto found = candidates_.find(key);
            if (found == candidates_.end()) {
                const auto [cx, cy, cz] = key;
                found =
                    candidates_
                        .emplace(key, aquifer::candidatesFor(
                                          *centres_, aquifer::CellIndex{.x = cx, .y = cy, .z = cz}))
                        .first;
            }
            lastKey_ = key;
            last_ = &found->second;
        }
        return aquifer::rankCandidates(x, y, z, *last_);
    }

private:
    const aquifer::CentreSource* centres_;
    std::map<Key, std::array<aquifer::Candidate, aquifer::kCandidateCount>> candidates_;
    Key lastKey_{};
    const std::array<aquifer::Candidate, aquifer::kCandidateCount>* last_ = nullptr;
};

/// The sources centred in the footprint, from the model alone: how many
/// each subset holds, and how many Q5.3(a) off the ocean branch (pipeline
/// engine v12) reaches — a scan that did not abort, off the near-surface
/// path, centred more than twenty above `cap`.
struct Census {
    std::array<long long, 3> subsets{};
    long long clause = 0;
};

[[nodiscard]] Census censusOf(const aquifer::CentreSource& centres, const Field& field) {
    Census census;
    for (std::int32_t cy = javamath::floorDiv(kMinY, aquifer::kCellPitchY) - 1;
         cy <= javamath::floorDiv(kMaxY, aquifer::kCellPitchY) + 1; ++cy) {
        for (std::int32_t cz = -1; cz <= kChunks; ++cz) {
            for (std::int32_t cx = -1; cx <= kChunks; ++cx) {
                const aquifer::CellIndex centre = centres.centreOf(cx, cy, cz);
                if (centre.x < 0 || centre.x >= kSpan || centre.z < 0 || centre.z >= kSpan ||
                    centre.y < kMinY || centre.y > kMaxY) {
                    continue;
                }
                const aquifer::Source source{.cell = aquifer::CellIndex{.x = cx, .y = cy, .z = cz},
                                             .centre = centre};
                const Subset subset = sourceInfoOf(source, field).subset;
                if (subset != Subset::None) {
                    ++census.subsets.at(indexOf(subset));
                }
                const aquifer::CellFluid cell = inputsOf(source, field).cell;
                census.clause += static_cast<long long>(
                    !cell.surface.aborted && !onNearSurfacePath(cell) &&
                    cell.centreY >
                        javamath::wrappingAdd(cell.surface.cap, aquifer::kNearSurfaceFloorOffset));
            }
        }
    }
    return census;
}

// --- scoring -----------------------------------------------------------------

/// The analyzer's readout for one subset: blocks whose nearest source is in
/// it, and which of the two readings each block's server state agrees with.
struct Readout {
    long long total = 0;
    long long built = 0;
    long long rival = 0;
    long long both = 0;
    long long neither = 0;

    void record(const bool observedWet, const bool builtWet, const bool rivalWet) {
        ++total;
        const bool b = observedWet == builtWet;
        const bool r = observedWet == rivalWet;
        built += static_cast<long long>(b);
        rival += static_cast<long long>(r);
        both += static_cast<long long>(b && r);
        neither += static_cast<long long>(!b && !r);
    }

    void add(const Readout& other) {
        total += other.total;
        built += other.built;
        rival += other.rival;
        both += other.both;
        neither += other.neither;
    }
};

/// The decision for one subset's rival: the blocks where it and the build
/// place different categories (model-only), and which one the server holds.
struct Contest {
    long long blocks = 0;
    long long builtRight = 0;
    long long rivalRight = 0;
    /// Rival-right blocks that are flowing water (`level` above 0): the
    /// aquifer places sources only, so these moved.
    long long rivalRightFlowing = 0;
    /// Rival-right blocks within kContactRows of the sea's top or of lambda,
    /// where no wall, or the thinnest, stands between a sea source and an
    /// A_lava one.
    long long rivalRightAtContact = 0;
    /// Contested blocks the build has wrong, by what explains them.
    long long builtWrongFlow = 0;
    long long builtWrongUnexplained = 0;

    void add(const Contest& other) {
        blocks += other.blocks;
        builtRight += other.builtRight;
        rivalRight += other.rivalRight;
        rivalRightFlowing += other.rivalRightFlowing;
        rivalRightAtContact += other.rivalRightAtContact;
        builtWrongFlow += other.builtWrongFlow;
        builtWrongUnexplained += other.builtWrongUnexplained;
    }
};

/// What becomes of the analyzer readout's misses in a subset once the same
/// block is decided the filler's way.
struct Residual {
    long long misses = 0;        ///< bare readout of the build against the server
    long long decisionRight = 0; ///< the filler's decision holds the server's block
    long long flow = 0;          ///< the decision is wrong there, and flow explains it
    long long unexplained = 0;   ///< neither
    long long outside = 0;       ///< in the edge margin, or below lambda or above kTopY

    void add(const Residual& other) {
        misses += other.misses;
        decisionRight += other.decisionRight;
        flow += other.flow;
        unexplained += other.unexplained;
        outside += other.outside;
    }
};

/// One source's contested blocks where it is the nearest.
struct Territory {
    long long contested = 0;
    long long rivalRight = 0;
};

struct Score {
    std::array<Readout, 3> readout{};
    std::array<Contest, 3> contest{};
    std::array<Residual, 3> residual{};
    std::array<long long, 3> sources{};
    long long skipped = 0;  ///< blocks the analyzer readout skips: neither fluid nor air
    long long baseline = 0; ///< every block the analyzer readout scores
    long long baselineRight = 0;
    /// The decision against the server over every block it scores.
    long long decided = 0;
    long long decidedWrong = 0;
    long long decidedFlow = 0;
    long long decidedUnexplained = 0;
    long long nonAirAboveTop = 0; ///< server blocks above kTopY that are not air
    long long missingBlocks = 0;
    long long selectionChecked = 0;
    long long selectionMismatch = 0;
    long long libraryChecked = 0;
    long long libraryMismatch = 0; ///< the case's own statuses against computeSubstance's
    long long inconsistent = 0;
    long long clause = 0;
    /// Abort-subset sources with 64 or more sampled contested blocks as the
    /// nearest; those whose blocks the server holds the rival's way on most;
    /// and the largest share any of them gives the rival.
    long long territories = 0;
    long long territoriesLost = 0;
    double worstTerritory = 0.0;

    void add(const Score& other) {
        for (std::size_t i = 0; i < 3; ++i) {
            readout.at(i).add(other.readout.at(i));
            contest.at(i).add(other.contest.at(i));
            residual.at(i).add(other.residual.at(i));
            sources.at(i) += other.sources.at(i);
        }
        skipped += other.skipped;
        baseline += other.baseline;
        baselineRight += other.baselineRight;
        decided += other.decided;
        decidedWrong += other.decidedWrong;
        decidedFlow += other.decidedFlow;
        decidedUnexplained += other.decidedUnexplained;
        nonAirAboveTop += other.nonAirAboveTop;
        missingBlocks += other.missingBlocks;
        selectionChecked += other.selectionChecked;
        selectionMismatch += other.selectionMismatch;
        libraryChecked += other.libraryChecked;
        libraryMismatch += other.libraryMismatch;
        inconsistent += other.inconsistent;
        clause += other.clause;
        territories += other.territories;
        territoriesLost += other.territoriesLost;
        worstTerritory = std::max(worstTerritory, other.worstTerritory);
    }
};

[[nodiscard]] Category categoryOf(const aquifer::SubstanceAt& at) {
    switch (at.substance) {
        case aquifer::Substance::Air:
            return Category::Air;
        case aquifer::Substance::Solid:
            return Category::Solid;
        case aquifer::Substance::Fluid:
            return at.fluidType == aquifer::FluidType::Lava ? Category::Lava : Category::Water;
    }
    return Category::Solid;
}

/// The global picker, which decides above the chunk's y_skip.
[[nodiscard]] Category globalAt(const std::int32_t y) {
    if (aquifer::globalReadsLava(y, kSeaLevel)) {
        return Category::Lava;
    }
    return y < kSeaLevel ? Category::Water : Category::Air;
}

/// Scores one arm of one corpus: every block of the footprint, the
/// analyzer's way on every row, the filler's way on rows lambda..kTopY.
void scoreArm(const std::filesystem::path& world, const aquifer::CentreSource& centres,
              const Field& field, Score& score) {
    stratum::test::GoldenRegion golden(world);
    Sources sources(field);
    Ranker ranker(centres);
    aquifer::StatusCache cache;
    std::map<Key, Territory> territories;
    const std::int32_t lambda = aquifer::lambdaLevel(kSeaLevel);
    long long visited = 0;
    long long latticeVisited = 0;

    for (std::int32_t cz = 0; cz < kChunks; ++cz) {
        for (std::int32_t cx = 0; cx < kChunks; ++cx) {
            if (!golden.hasChunk(cx, cz)) {
                score.missingBlocks += 16LL * 16LL * (kMaxY - kMinY + 1);
                continue;
            }
            const stratum::chunk::Chunk& chunk = golden.chunk(cx, cz);
            const std::int32_t ySkipLevel = aquifer::chunkYSkip(field, cx * 16, cz * 16);
            for (std::int32_t lz = 0; lz < 16; ++lz) {
                for (std::int32_t lx = 0; lx < 16; ++lx) {
                    const std::int32_t x = (cx * 16) + lx;
                    const std::int32_t z = (cz * 16) + lz;
                    const bool inside = x >= kEdgeMargin && x < kSpan - kEdgeMargin &&
                                        z >= kEdgeMargin && z < kSpan - kEdgeMargin;
                    const bool sampled = javamath::floorMod(x, kColumnStride) == 0 &&
                                         javamath::floorMod(z, kColumnStride) == 0;
                    for (std::int32_t y = kMinY; y <= kMaxY; ++y) {
                        const auto* block = chunk.blockAt(lx, y, lz);
                        if (block == nullptr) {
                            ++score.missingBlocks;
                            continue;
                        }
                        const Category server = stratum::test::categoryOf(block->name);
                        if (y > kTopY) {
                            score.nonAirAboveTop += static_cast<long long>(server != Category::Air);
                        }
                        const bool readable =
                            stratum::test::isFluid(server) || block->name == "minecraft:air";
                        // Every row the decision can part on, in a sample of
                        // the columns: every second one on each axis.
                        const bool inRows = inside && y >= lambda && y <= kTopY;
                        const bool decided = inRows && sampled;
                        if (!readable && !decided) {
                            ++score.skipped;
                            continue;
                        }

                        const aquifer::Selection selection = ranker.rank(x, y, z);
                        // The split selection is selectSources: checked on a
                        // sample, which is enough to be sure of it.
                        if (++visited % 251 == 0) {
                            ++score.selectionChecked;
                            score.selectionMismatch += static_cast<long long>(
                                aquifer::selectSources(centres, x, y, z).ranked !=
                                selection.ranked);
                        }
                        const SourceInfo& nearest = sources.of(selection.ranked[0]);

                        // The filler's path: the global picker above the
                        // chunk's y_skip, computeSubstance at and below it.
                        const aquifer::AquiferQuery query{
                            .x = x, .y = y, .z = z, .density = -1.0, .seaLevel = kSeaLevel};
                        const bool lattice = aquifer::consultsLattice(y, ySkipLevel);
                        const auto decide = [&] {
                            return lattice ? categoryOf(aquifer::computeSubstance(
                                                 centres, query, cache, constant(kBarrier),
                                                 constant(kFloodedness), constant(0.0),
                                                 constant(0.0), field, aquifer::NoDeepDark{}))
                                           : globalAt(y);
                        };
                        Category built = Category::Air;
                        if (decided) {
                            // Each rival through the same decision, with its
                            // subset's statuses.
                            built = decide();
                            // A rival is weighed where a ranked source of
                            // its subset reads differently under it; where
                            // none does, the two decisions are one.
                            std::array<Subset, 3> parts{};
                            for (std::size_t r = 0; r < parts.size(); ++r) {
                                const SourceInfo& info = sources.of(selection.ranked.at(r));
                                parts.at(r) = info.rival.level != info.built.level ||
                                                      info.rival.type != info.built.type
                                                  ? info.subset
                                                  : Subset::None;
                            }
                            for (const Subset subset : kSubsets) {
                                if (!lattice || (parts[0] != subset && parts[1] != subset &&
                                                 parts[2] != subset)) {
                                    continue;
                                }
                                const auto rivalOf = [&](const aquifer::Source& source) {
                                    const SourceInfo& info = sources.of(source);
                                    return info.subset == subset ? info.rival : info.built;
                                };
                                const Category rival = categoryOf(aquifer::computeSubstanceWith(
                                    centres, query, rivalOf, constant(kBarrier)));
                                if (rival == built) {
                                    continue;
                                }
                                Contest& contest = score.contest.at(indexOf(subset));
                                ++contest.blocks;
                                contest.builtRight += static_cast<long long>(built == server);
                                if (rival == server) {
                                    ++contest.rivalRight;
                                    contest.rivalRightFlowing += static_cast<long long>(
                                        stratum::test::fluidLevel(block) > 0);
                                    contest.rivalRightAtContact += static_cast<long long>(
                                        y >= kSeaLevel - kContactRows || y < lambda + kContactRows);
                                }
                                if (built != server) {
                                    if (stratum::test::explainedByFlow(golden, x, y, z, server,
                                                                       built)) {
                                        ++contest.builtWrongFlow;
                                    } else {
                                        ++contest.builtWrongUnexplained;
                                    }
                                }
                                if (parts[0] == subset && subset != Subset::Floor) {
                                    Territory& territory =
                                        territories[keyOf(selection.ranked[0].centre)];
                                    ++territory.contested;
                                    territory.rivalRight += static_cast<long long>(rival == server);
                                }
                            }
                            // The case's own built statuses are the library's,
                            // so a rival differs from the build by its subset
                            // alone: checked on a sample.
                            if (lattice && ++latticeVisited % 64 == 0) {
                                const auto builtOf = [&](const aquifer::Source& source) {
                                    return sources.of(source).built;
                                };
                                ++score.libraryChecked;
                                score.libraryMismatch += static_cast<long long>(
                                    categoryOf(aquifer::computeSubstanceWith(
                                        centres, query, builtOf, constant(kBarrier))) != built);
                            }

                            ++score.decided;
                            if (built != server) {
                                ++score.decidedWrong;
                                if (stratum::test::explainedByFlow(golden, x, y, z, server,
                                                                   built)) {
                                    ++score.decidedFlow;
                                } else {
                                    ++score.decidedUnexplained;
                                }
                            }
                        }

                        if (!readable) {
                            ++score.skipped;
                            continue;
                        }
                        // The analyzer's readout: the nearest source alone,
                        // a bare `y < level`, fluid against air.
                        const bool observedWet = stratum::test::isFluid(server);
                        const bool builtWet = y < nearest.built.level;
                        ++score.baseline;
                        score.baselineRight += static_cast<long long>(builtWet == observedWet);
                        if (nearest.subset == Subset::None) {
                            continue;
                        }
                        const std::size_t i = indexOf(nearest.subset);
                        score.readout.at(i).record(observedWet, builtWet, y < nearest.rival.level);
                        if (builtWet == observedWet) {
                            continue;
                        }
                        Residual& residual = score.residual.at(i);
                        ++residual.misses;
                        if (!inRows) {
                            ++residual.outside;
                            continue;
                        }
                        if (!decided) {
                            built = decide(); // every miss is decided, sampled or not
                        }
                        if (built == server) {
                            ++residual.decisionRight;
                        } else if (stratum::test::explainedByFlow(golden, x, y, z, server, built)) {
                            ++residual.flow;
                        } else {
                            ++residual.unexplained;
                        }
                    }
                }
            }
        }
    }

    score.inconsistent += sources.inconsistent();
    for (const auto& [key, territory] : territories) {
        if (territory.contested < 64) {
            continue;
        }
        ++score.territories;
        const double share =
            static_cast<double>(territory.rivalRight) / static_cast<double>(territory.contested);
        score.worstTerritory = std::max(score.worstTerritory, share);
        score.territoriesLost +=
            static_cast<long long>(2 * territory.rivalRight > territory.contested);
    }
}

/// Present corpora: none (the case skips), or every seed's (a partial set
/// fails rather than scoring whatever it finds).
[[nodiscard]] bool corporaPresent() {
    std::size_t present = 0;
    for (const std::int64_t seed : kSeeds) {
        present += static_cast<std::size_t>(
            std::filesystem::is_regular_file(corpusDir(seed) / "manifest.json"));
    }
    INFO(present << " of " << kSeeds.size()
                 << " nearsurface corpora present; regenerate all with tools/probe-worlds "
                    "generate --only aquifer-nearsurface-probe.sh --accept-eula");
    REQUIRE((present == 0 || present == kSeeds.size()));
    return present == kSeeds.size();
}

/// What the model alone decides, per seed: the sources centred in the
/// footprint by subset, the blocks each rival moves in the scored sample,
/// and the sources Q5.3(a) off the ocean branch reaches.
struct Pinned {
    std::int64_t seed = 0;
    std::array<long long, 3> sources{};
    std::array<long long, 3> contested{};
    long long clause = 0;
};

constexpr std::array<Pinned, 2> kPinned{{
    {.seed = 42, .sources = {2584, 320, 191}, .contested = {143575, 116254, 143562}, .clause = 0},
    {.seed = 31337,
     .sources = {2480, 311, 162},
     .contested = {143662, 111167, 127025},
     .clause = 0},
}};

[[nodiscard]] double share(const long long part, const long long whole) {
    return whole == 0 ? 0.0 : static_cast<double>(part) / static_cast<double>(whole);
}

} // namespace

TEST_CASE("the near-surface floor reads cap and an aborted scan is refused the sea",
          "[conformance][aquifer]") {
    if (!corporaPresent()) {
        SKIP("no nearsurface_s* probes under " << probes() << "; generate them with " << kScript
                                               << " --accept-eula <seed> for seeds 42 and 31337");
    }
    std::array<Score, kSeeds.size()> perSeed{};
    Score total;
    for (std::size_t s = 0; s < kSeeds.size(); ++s) {
        const std::int64_t seed = kSeeds.at(s);
        const std::filesystem::path dir = corpusDir(seed);
        INFO("probe corpus " << dir);
        stratum::test::requireFrozen(dir, kScript);
        stratum::test::requireSeed(dir, seed);
        std::ifstream manifestFile(dir / "manifest.json");
        const nlohmann::json manifest = nlohmann::json::parse(manifestFile);
        std::ifstream specFile(dir / "spec.json");
        const nlohmann::json spec = nlohmann::json::parse(specFile);
        REQUIRE(spec.is_array());

        // The probe's noise as the manifest records it, not restated here.
        const nlohmann::json& declared = manifest.at("probe_noise");
        REQUIRE(declared.at("id").get<std::string>() == "stratum:probe_noise");
        const auto amplitudes = declared.at("amplitudes").get<std::vector<double>>();
        auto random = stratum::rng::XoroshiroPositionalFactory(seed).fromHashOf(
            declared.at("id").get<std::string>());
        const auto noise = stratum::noise::NormalNoise::create(
            random, declared.at("first_octave").get<int>(), amplitudes);
        const aquifer::CentreSource centres(seed, stratum::density::RandomSource::Xoroshiro);

        Score& corpus = perSeed.at(s);
        for (const Arm& arm : kArms) {
            INFO("arm " << arm.world);
            const Field field(noise, parseArm(spec, arm));
            long long corners = 0;
            const long long mismatches =
                readoutMismatches(dir / arm.readout / "r.0.0.mca", field, corners);
            REQUIRE(corners == static_cast<long long>(kSpan / 4) * (kSpan / 4));
            // The rebuilt field is the server's wherever the aquifer reads it.
            REQUIRE(mismatches == 0);
            const Census census = censusOf(centres, field);
            for (std::size_t i = 0; i < census.subsets.size(); ++i) {
                corpus.sources.at(i) += census.subsets.at(i);
            }
            corpus.clause += census.clause;
            scoreArm(dir / arm.world / "r.0.0.mca", centres, field, corpus);
        }
        for (const Subset subset : kSubsets) {
            const Readout& r = corpus.readout.at(indexOf(subset));
            const Contest& c = corpus.contest.at(indexOf(subset));
            WARN("seed " << seed << ", " << nameOf(subset) << ": sources "
                         << corpus.sources.at(indexOf(subset)) << "; analyzer readout " << r.built
                         << "/" << r.total << " (" << share(r.built, r.total) << ") against "
                         << r.rival << " (" << share(r.rival, r.total) << "), neither " << r.neither
                         << "; contested " << c.blocks << ", build right " << c.builtRight
                         << ", rival right " << c.rivalRight << " (flowing " << c.rivalRightFlowing
                         << ", at a contact row " << c.rivalRightAtContact << ")");
        }
        total.add(corpus);
    }

    for (const Subset subset : kSubsets) {
        const Residual& m = total.residual.at(indexOf(subset));
        const Contest& c = total.contest.at(indexOf(subset));
        WARN(nameOf(subset) << ": analyzer misses " << m.misses << " = decision right "
                            << m.decisionRight << " + flow " << m.flow << " + unexplained "
                            << m.unexplained << " + outside " << m.outside
                            << "; contested build wrong: flow " << c.builtWrongFlow
                            << ", unexplained " << c.builtWrongUnexplained);
    }
    WARN("baseline " << total.baselineRight << "/" << total.baseline << ", skipped "
                     << total.skipped << "; decided " << total.decided << ", wrong "
                     << total.decidedWrong << " (flow " << total.decidedFlow << ", unexplained "
                     << total.decidedUnexplained << "); non-air above row " << kTopY << ": "
                     << total.nonAirAboveTop << "; selection " << total.selectionMismatch << "/"
                     << total.selectionChecked << ", library " << total.libraryMismatch << "/"
                     << total.libraryChecked << "; Q5.3(a) sources " << total.clause
                     << "; territories " << total.territoriesLost << "/" << total.territories
                     << ", worst " << total.worstTerritory);

    // The harness is the library: the split selection, and the case's own
    // statuses, against the library's wherever they were compared.
    REQUIRE(total.missingBlocks == 0);
    REQUIRE(total.selectionChecked > 10000);
    CHECK(total.selectionMismatch == 0);
    REQUIRE(total.libraryChecked > 10000);
    CHECK(total.libraryMismatch == 0);
    // Every source a subset holds takes the branch the subset names, and its
    // rival parts from it there.
    CHECK(total.inconsistent == 0);
    // Above the sea every reading is air, and so is the server.
    CHECK(total.nonAirAboveTop == 0);

    for (std::size_t s = 0; s < kSeeds.size(); ++s) {
        const Score& corpus = perSeed.at(s);
        const Pinned& pinned = kPinned.at(s);
        INFO("seed " << kSeeds.at(s));
        REQUIRE(pinned.seed == kSeeds.at(s));
        // Model-only, so exact: which branch each source takes, how many
        // blocks each rival moves in the scored sample, and Q5.3(a).
        CHECK(corpus.sources == pinned.sources);
        for (const Subset subset : kSubsets) {
            INFO(nameOf(subset));
            CHECK(corpus.contest.at(indexOf(subset)).blocks ==
                  pinned.contested.at(indexOf(subset)));
        }
        // Q5.3(a) off the ocean branch reaches no source of this corpus at
        // all: a scan that does not abort here has no -70 in its window, so
        // off the near-surface path its whole window reads 96, which no
        // source does. So none of the residual below can be that clause.
        CHECK(corpus.clause == pinned.clause);

        // The analyzer's figures, re-derived. The floor's cap is exact: a
        // sea source's water cannot leave and none can rise past 62, and
        // what water meeting lava leaves the readout skips. Against it,
        // gate scores 0.9351-0.9358.
        const Readout& floorRead = corpus.readout.at(indexOf(Subset::Floor));
        CHECK(floorRead.built == floorRead.total);
        CHECK(floorRead.rival * 100 >= floorRead.total * 90);
        CHECK(floorRead.rival * 100 <= floorRead.total * 95);
        // Refusing the sea: 0.9979-0.9987 with the anchor at or above
        // sea-8, against 0.645-0.651 for ignoring the abort, and 0.9992-
        // 0.9996 below it, against 0.066-0.089. The bounds leave the frozen
        // remnant room to triple, and the unfrozen corpus's 0.9812-0.9829
        // and 0.9911-0.9941 fail them.
        const Readout& aboveRead = corpus.readout.at(indexOf(Subset::Above));
        CHECK(aboveRead.built * 1000 >= aboveRead.total * 993);
        CHECK(aboveRead.rival * 100 <= aboveRead.total * 70);
        const Readout& belowRead = corpus.readout.at(indexOf(Subset::Below));
        CHECK(belowRead.built * 1000 >= belowRead.total * 997);
        CHECK(belowRead.rival * 100 <= belowRead.total * 15);
    }

    // The decision, where the two readings part. The floor: the server holds
    // the build's water on every block, and nothing flow does can leave air
    // or stone where a sea source holds water (it can leave obsidian, where
    // that water meets lava, which neither reading writes).
    const Contest& floorContest = total.contest.at(indexOf(Subset::Floor));
    CHECK(floorContest.builtRight * 1000 >= floorContest.blocks * 999);
    CHECK(floorContest.rivalRight * 1000 <= floorContest.blocks);
    // Refusing the sea: the build holds on 99.6-99.97% of the blocks, and
    // every block it has wrong is fluid that moved — almost all of it on the
    // three unwalled rows at the sea's top and at lambda, where a sea
    // source's water meets an A_lava source's air.
    long long rivalRight = 0;
    long long rivalRightAtContact = 0;
    for (const Subset subset : {Subset::Above, Subset::Below}) {
        const Contest& contest = total.contest.at(indexOf(subset));
        INFO(nameOf(subset));
        CHECK(contest.builtWrongUnexplained == 0);
        CHECK(contest.builtRight * 100 >= contest.blocks * 99);
        rivalRight += contest.rivalRight;
        rivalRightAtContact += contest.rivalRightAtContact;
    }
    CHECK(rivalRightAtContact * 100 >= rivalRight * 95);
    // No source's own territory goes the rival's way: a guard that refused
    // the sea wrongly would lose whole territories, where flow wets slivers.
    REQUIRE(total.territories >= 200);
    CHECK(total.territoriesLost == 0);
    CHECK(total.worstTerritory < 0.5);

    // The analyzer's residual, attributed: every block it misses inside the
    // decision is fluid that moved; the rest sit in the edge margin.
    for (const Subset subset : kSubsets) {
        INFO(nameOf(subset));
        CHECK(total.residual.at(indexOf(subset)).unexplained == 0);
    }
    // And the decision over the whole sample: wrong only where fluid moved.
    CHECK(total.decidedUnexplained == 0);
    CHECK(total.decidedWrong * 1000 <= total.decided * 5);
}
