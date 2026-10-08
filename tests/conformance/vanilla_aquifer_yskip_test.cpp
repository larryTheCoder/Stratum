// Stratum — y_skip, the height above which the aquifer is not consulted.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Spec Q2.3/Q2.5: above a per-chunk height `y_skip` the local aquifer is not
// consulted and the global picker decides. The build computes it as
// `12 * (floorDiv(S_max + 20, 12) + 1) + 10`, S_max the highest floored
// `preliminary_surface_level`, read at y = 0, over the chunk's lattice
// rectangle at a stride of four (`chunkYSkip`, sampling.hpp). On every vanilla
// surface the cutoff is invisible — above it the lattice answers what the
// global picker does (aquifer_substance_test) — so the goldens cannot see it.
//
// `tools/analysis/aquifer-yskip-probe.sh` builds worlds where they part. Every
// psl value is -80 or lower, so every scan aborts and every source's status is
// fixed by its centre alone: wet to the sea (63, water) at or above lambda
// (-54), dry at lambda below it. The POCKETS — blocks whose nearest source is
// dry — are therefore the same blocks in every dimension of a seed, whatever
// its psl; only y_skip moves. At or below it a pocket is air or barrier stone,
// above it the global picker makes it water. Pockets reach from lambda up to
// -44, so rows -54..-44 are where a cutoff shows.
//
// Every dimension is scored three ways against one lattice model per seed:
//   * block for block — exactly on the 63 chunks that never ticked, and on the
//     81 that did, every disagreement fluid that moved (support/fluid_flow.hpp);
//   * the cutoff READ OFF the server with no model of y_skip at all: the
//     uniform cutoffs that leave no block contradicted;
//   * rival readings — no cutoff, other constants, a strict boundary,
//     truncation, other read heights, other rectangles and strides — each
//     scored on the blocks where it and the build part.
// And the server's post-processing lists, which only the lattice writes:
// exact on the untouched chunks, with no mark above the cutoff. A third case
// runs `ChunkFiller` itself on the probes' own noise settings.
//
// What they showed (SPEC §11): the closed form is the server's to the row,
// and the rectangle was not — the server reads offsets -16..+24 from the
// chunk's corner, where the build read -16..+16.
//
// ONE SEED PER PROBE DIRECTORY, read from its manifest and its name; every
// directory present is scored, and the cases SKIP when there is none. The
// fixtures are Mojang-derived and never committed (SPEC §12).
#include "support/fluid_flow.hpp"
#include "support/probe_corpus.hpp"
#include "support/probe_region.hpp"
#include "support/temp_path.hpp"

#include <stratum/aquifer/fluid_type.hpp>
#include <stratum/aquifer/lattice.hpp>
#include <stratum/aquifer/sampling.hpp>
#include <stratum/aquifer/selection.hpp>
#include <stratum/aquifer/substance.hpp>
#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/javamath.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/noise/perlin.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/terrain/filler.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace {

namespace aquifer = stratum::aquifer;
using stratum::test::Category;

constexpr std::int32_t kSeaLevel = 63;
constexpr std::int32_t kMinY = -64;
constexpr std::int32_t kLambda = aquifer::lambdaLevel(kSeaLevel);
/// Chunks 0..11 on both axes reached the noise stage (support/probe_region.hpp).
constexpr std::int32_t kChunks = 12;
constexpr std::int32_t kSpan = kChunks * 16;
/// From this row up every candidate source is centred at -48 or higher (the
/// window's lowest layer is the cell below the block's own, and cell -4's
/// centre is at least 12 * -4), so wet: the lattice answers water, unmarked,
/// exactly as the global picker does. The model REQUIREs it of its own top
/// rows rather than taking it on trust.
constexpr std::int32_t kAllWetFrom = -37;
/// The lattice model's top row. Rows above it read the same under every
/// reading, so only rows kLambda..kModelTop separate them.
constexpr std::int32_t kModelTop = -30;
constexpr std::int32_t kRows = kModelTop - kLambda + 1;
/// The highest row scored block for block; nothing reaches above the sea.
constexpr std::int32_t kTopY = 80;

/// The probe's constants, as the script writes them; each dimension's own
/// spec entry is checked against these before it is scored.
constexpr double kDensity = -1.0;
constexpr double kBarrier = -2.0;
constexpr double kFloodedness = 0.5;
constexpr double kSpread = 0.0;
constexpr double kLava = 0.0;
/// Any surface at or below -80 gives every source the same status; the model
/// is built at this one and every dimension's statuses are checked against it.
constexpr double kModelSurface = -85.0;

[[nodiscard]] std::filesystem::path fixtures() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11";
}

/// Every probe directory under the fixtures whose name starts with @p prefix,
/// sorted.
[[nodiscard]] std::vector<std::filesystem::path> corpora(const std::string& prefix) {
    const std::filesystem::path root = fixtures() / "probes";
    std::vector<std::filesystem::path> found;
    if (std::filesystem::is_directory(root)) {
        for (const auto& entry : std::filesystem::directory_iterator(root)) {
            if (entry.is_directory() && entry.path().filename().string().rfind(prefix, 0) == 0) {
                found.push_back(entry.path());
            }
        }
    }
    std::ranges::sort(found);
    return found;
}

/// The seed a corpus carries in its name ("..._s<seed>"), checked against the
/// one its manifest records.
[[nodiscard]] std::int64_t corpusSeed(const std::filesystem::path& dir) {
    const std::string name = dir.filename().string();
    const auto at = name.rfind("_s");
    INFO("corpus " << name);
    REQUIRE(at != std::string::npos);
    const std::int64_t seed = std::stoll(name.substr(at + 2));
    stratum::test::requireSeed(dir, seed);
    return seed;
}

/// One dimension's `preliminary_surface_level`, from its own spec.json entry:
/// a constant, or a `range_choice` between two constants over a
/// `y_clamped_gradient` or the probe's own noise — the shapes the probe
/// script writes. Any other shape fails the case, naming it.
class PslField {
public:
    PslField(const nlohmann::json& node, const stratum::noise::NormalNoise& probe)
        : probe_(&probe) {
        INFO("preliminary_surface_level " << node.dump());
        if (node.is_number()) {
            inRange_ = node.get<double>();
            return;
        }
        const std::string type = node.at("type").get<std::string>();
        if (type == "minecraft:constant") {
            inRange_ = node.at("argument").get<double>();
            return;
        }
        REQUIRE(type == "minecraft:range_choice");
        const nlohmann::json& input = node.at("input");
        const std::string inputType = input.at("type").get<std::string>();
        if (inputType == "minecraft:y_clamped_gradient") {
            input_ = Input::Gradient;
            fromY_ = input.at("from_y").get<double>();
            toY_ = input.at("to_y").get<double>();
            fromValue_ = input.at("from_value").get<double>();
            toValue_ = input.at("to_value").get<double>();
        } else {
            REQUIRE(inputType == "minecraft:noise");
            REQUIRE(input.at("noise").get<std::string>() == "stratum:probe_noise");
            input_ = Input::Noise;
            xzScale_ = input.at("xz_scale").get<double>();
            yScale_ = input.at("y_scale").get<double>();
        }
        minInclusive_ = node.at("min_inclusive").get<double>();
        maxExclusive_ = node.at("max_exclusive").get<double>();
        inRange_ = node.at("when_in_range").get<double>();
        outOfRange_ = node.at("when_out_of_range").get<double>();
    }

    [[nodiscard]] double operator()(std::int32_t x, std::int32_t y, std::int32_t z) const {
        if (input_ == Input::None) {
            return inRange_;
        }
        double value = 0.0;
        if (input_ == Input::Gradient) {
            const double t =
                std::clamp((static_cast<double>(y) - fromY_) / (toY_ - fromY_), 0.0, 1.0);
            value = fromValue_ + (t * (toValue_ - fromValue_));
        } else {
            value =
                probe_->sample(static_cast<double>(x) * xzScale_, static_cast<double>(y) * yScale_,
                               static_cast<double>(z) * xzScale_);
        }
        return (value >= minInclusive_ && value < maxExclusive_) ? inRange_ : outOfRange_;
    }

private:
    enum class Input : std::uint8_t { None, Gradient, Noise };
    const stratum::noise::NormalNoise* probe_;
    Input input_ = Input::None;
    double fromY_ = 0.0;
    double toY_ = 1.0;
    double fromValue_ = 0.0;
    double toValue_ = 0.0;
    double xzScale_ = 1.0;
    double yScale_ = 0.0;
    double minInclusive_ = 0.0;
    double maxExclusive_ = 0.0;
    double inRange_ = 0.0;
    double outOfRange_ = 0.0;
};

/// The lattice's own answer for one block, in rows kLambda..kModelTop.
struct ModelBlock {
    Category category = Category::Air;
    bool mark = false;   ///< spec Q8's post-processing flag
    bool pocket = false; ///< the nearest source is dry
    bool allWet = false; ///< all four ranked sources are wet
};

/// The full aquifer decision on rows kLambda..kModelTop of x, z in [0, 192),
/// for one seed, at the probe's constants — the shipped `computeSubstance`.
/// It holds for every dimension of the seed whose statuses are the model's
/// own (`premiseViolations`).
class LatticeModel {
public:
    explicit LatticeModel(const std::int64_t seed)
        : centres_(seed),
          blocks_(static_cast<std::size_t>(kSpan) * kSpan * static_cast<std::size_t>(kRows)) {
        const auto constant = [](const double value) {
            return [value](std::int32_t, std::int32_t, std::int32_t) { return value; };
        };
        aquifer::StatusCache cache;
        std::set<std::tuple<std::int32_t, std::int32_t, std::int32_t>> seen;
        for (std::int32_t z = 0; z < kSpan; ++z) {
            for (std::int32_t x = 0; x < kSpan; ++x) {
                for (std::int32_t y = kLambda; y <= kModelTop; ++y) {
                    const aquifer::SubstanceAt at = aquifer::computeSubstance(
                        centres_,
                        aquifer::AquiferQuery{
                            .x = x, .y = y, .z = z, .density = kDensity, .seaLevel = kSeaLevel},
                        cache, constant(kBarrier), constant(kFloodedness), constant(kSpread),
                        constant(kLava), constant(kModelSurface), aquifer::NoDeepDark{});
                    const aquifer::Selection selection = aquifer::selectSources(centres_, x, y, z);
                    ModelBlock& block = blocks_[index(x, y, z)];
                    switch (at.substance) {
                        case aquifer::Substance::Air:
                            block.category = Category::Air;
                            break;
                        case aquifer::Substance::Solid:
                            block.category = Category::Solid;
                            break;
                        case aquifer::Substance::Fluid:
                            block.category = at.fluidType == aquifer::FluidType::Lava
                                                 ? Category::Lava
                                                 : Category::Water;
                            break;
                    }
                    block.mark = at.fluidUpdate;
                    block.pocket = selection.nearest().centre.y < kLambda;
                    block.allWet = std::ranges::all_of(selection.ranked, [](const auto& source) {
                        return source.centre.y >= kLambda;
                    });
                    for (const aquifer::Source& source : selection.ranked) {
                        if (seen.emplace(source.centre.x, source.centre.y, source.centre.z)
                                .second) {
                            sources_.push_back(source);
                        }
                    }
                }
            }
        }
    }

    [[nodiscard]] const ModelBlock& at(std::int32_t x, std::int32_t y, std::int32_t z) const {
        return blocks_[index(x, y, z)];
    }

    /// Every source the selection ranks for some modelled block.
    [[nodiscard]] const std::vector<aquifer::Source>& sources() const { return sources_; }

private:
    [[nodiscard]] static std::size_t index(std::int32_t x, std::int32_t y, std::int32_t z) {
        return (((static_cast<std::size_t>(z) * kSpan) + static_cast<std::size_t>(x)) *
                static_cast<std::size_t>(kRows)) +
               static_cast<std::size_t>(y - kLambda);
    }

    aquifer::CentreSource centres_;
    std::vector<ModelBlock> blocks_;
    std::vector<aquifer::Source> sources_;
};

/// The model's own premise, checked rather than argued: in rows kAllWetFrom
/// and up no source is dry, so nothing is marked and every block is water.
void requireTopRowsAllWet(const LatticeModel& model) {
    long long notAllWet = 0;
    for (std::int32_t z = 0; z < kSpan; ++z) {
        for (std::int32_t x = 0; x < kSpan; ++x) {
            for (std::int32_t y = kAllWetFrom; y <= kModelTop; ++y) {
                const ModelBlock& block = model.at(x, y, z);
                notAllWet += static_cast<long long>(!block.allWet || block.mark ||
                                                    block.category != Category::Water);
            }
        }
    }
    REQUIRE(notAllWet == 0);
}

/// Sources whose status under @p psl is not the model's: wet to the sea at or
/// above lambda, dry at lambda below it. Zero means the dimension's lattice
/// is the model's, block for block.
[[nodiscard]] long long premiseViolations(const LatticeModel& model, const PslField& psl) {
    long long violations = 0;
    for (const aquifer::Source& source : model.sources()) {
        const aquifer::SourceStatus status =
            aquifer::sourceStatus(aquifer::CellFluid{.centreY = source.centre.y,
                                                     .surface = aquifer::readPreliminarySurface(
                                                         psl, source.centre, kSeaLevel),
                                                     .seaLevel = kSeaLevel,
                                                     .floodedness = kFloodedness,
                                                     .spread = kSpread},
                                  kLava);
        const bool ok = source.centre.y >= kLambda ? (status.level == kSeaLevel &&
                                                      status.type == aquifer::FluidType::Default)
                                                   : status.level == kLambda;
        violations += static_cast<long long>(!ok);
    }
    return violations;
}

/// What the global picker writes at @p y (Q1.2): lava below lambda, the
/// default fluid below the sea, air above.
[[nodiscard]] Category globalAt(std::int32_t y) {
    if (y < kLambda) {
        return Category::Lava;
    }
    return y < kSeaLevel ? Category::Water : Category::Air;
}

/// Whether column (@p x, @p z) of an untouched chunk shares a face with a
/// ticked one. Fluid there can be the neighbour's flow: measured, the only
/// moved fluid on any untouched chunk of these worlds sits in such a column
/// (x or z exactly 144, beside chunk 8), never one further in.
[[nodiscard]] bool besideTicked(std::int32_t x, std::int32_t z) {
    constexpr std::int32_t kFace = 9 * 16;
    return (x == kFace && z < kFace) || (z == kFace && x < kFace);
}

/// `explainedByFlow`'s rebuilt-source shape where one of the block's four
/// horizontal neighbours lies outside the region file (x or z is 0): a water
/// source where the lattice has air, beside a source the file does hold. The
/// unseen neighbour is the second source the rule needs; the file cannot say,
/// so this lets it through and nothing else.
[[nodiscard]] bool sourceAtRegionEdge(stratum::test::GoldenRegion& region, std::int32_t x,
                                      std::int32_t y, std::int32_t z, Category server,
                                      Category built) {
    if ((x != 0 && z != 0) || server != Category::Water || built != Category::Air ||
        stratum::test::fluidLevel(region.blockAt(x, y, z)) != 0) {
        return false;
    }
    int sources = 0;
    for (const auto& [dx, dz] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
        const auto* next = region.blockAt(x + dx, y, z + dz);
        sources +=
            (stratum::test::named(next, "minecraft:water") && stratum::test::fluidLevel(next) == 0)
                ? 1
                : 0;
    }
    return sources >= 1;
}

/// Whether the server's @p server block refutes a prediction @p predicted.
/// Where nothing can have moved, any difference does. Elsewhere only
/// non-water where water was predicted: fluid that moves fills an air pocket
/// but never empties water, and nothing here can turn water to stone.
[[nodiscard]] bool refutes(Category predicted, Category server, bool exact) {
    if (exact) {
        return predicted != server;
    }
    return predicted == Category::Water && server != Category::Water;
}

/// A reading of y_skip: the closed form with its parameters exposed, so each
/// rival is one field away from the build. Offsets are from the chunk's
/// minimum corner.
struct Reading {
    const char* name = "";
    std::int32_t addend = 20; ///< S_max + addend, floorDiv'd by 12 (the build: +8 +12)
    std::int32_t from = -16;  ///< the rectangle, as offsets from the chunk's corner
    std::int32_t to = 25;
    std::int32_t stride = 4;
    std::int32_t readY = 0;
    bool truncate = false; ///< S_max truncated toward zero instead of floored
    bool strict = false;   ///< the lattice below y_skip only, not at it
    bool none = false;     ///< no cutoff at all
};

/// @p reading's cutoff for chunk (@p cx, @p cz): the last row the lattice is
/// consulted at, clamped to [kLambda - 1, kModelTop] — every cutoff below that
/// range reads the same, and so does every one above it.
[[nodiscard]] std::int32_t cutoffOf(const Reading& reading, const PslField& psl, std::int32_t cx,
                                    std::int32_t cz) {
    if (reading.none) {
        return kModelTop;
    }
    const std::int32_t baseX = cx * 16;
    const std::int32_t baseZ = cz * 16;
    std::int32_t maxSurface = std::numeric_limits<std::int32_t>::min();
    for (std::int32_t dz = reading.from; dz <= reading.to; dz += reading.stride) {
        for (std::int32_t dx = reading.from; dx <= reading.to; dx += reading.stride) {
            const double value = psl(baseX + dx, reading.readY, baseZ + dz);
            maxSurface =
                std::max(maxSurface, reading.truncate ? stratum::javamath::doubleToInt(value)
                                                      : stratum::javamath::floorToInt(value));
        }
    }
    std::int32_t level =
        (12 * (stratum::javamath::floorDiv(maxSurface + reading.addend, 12) + 1)) + 10;
    if (reading.strict) {
        --level;
    }
    return std::clamp(level, kLambda - 1, kModelTop);
}

/// One dimension, read once: the block-for-block score of the build, and per
/// chunk and row how many blocks each of the two answers — the lattice's and
/// the global picker's — is refuted on, so any reading's score is a sum.
struct ArmScore {
    int chunks = 0;
    int untouchedChunks = 0;
    long long missingBlocks = 0; ///< scored positions with no section stored: must be 0
    /// Untouched chunks off the face beside a ticked one: nothing moved.
    long long exactBlocks = 0;
    long long exactWrong = 0;    ///< the build there: must be 0
    long long exactFlowing = 0;  ///< fluid at level > 0 there: must be 0
    long long exactContact = 0;  ///< obsidian on the lava sea's top row there
    long long contactOnFull = 0; ///< of which on a chunk promoted to full
    /// Ticked chunks, and the face of an untouched one beside them.
    long long flowBlocks = 0;
    long long flowMoved = 0;        ///< disagreements that are fluid that moved
    long long flowUnexplained = 0;  ///< the rest: must be 0
    long long faceMoved = 0;        ///< of flowMoved, on an untouched chunk's face
    long long pocketsInLattice = 0; ///< exact blocks: pockets at or below the build's cutoff
    long long serverStone = 0;      ///< exact blocks: barrier stone, rows kLambda..kModelTop
    long long serverMarks = 0;      ///< post-processing marks on untouched chunks
    long long marksExtra = 0;       ///< the build's, not the server's
    long long marksMissing = 0;     ///< the server's, not the build's
    long long marksAboveCutoff = 0; ///< server marks above the build's cutoff
    std::vector<std::int32_t> builtCutoff;
    /// [chunk][row - kLambda]: blocks refuting the lattice's answer and the
    /// global picker's, and the post-processing marks each gets wrong.
    std::vector<std::array<long long, kRows>> latticeRefuted;
    std::vector<std::array<long long, kRows>> globalRefuted;
    std::vector<std::array<long long, kRows>> latticeMarksWrong;
    std::vector<std::array<long long, kRows>> globalMarksWrong;

    /// Blocks refuting @p cutoff(chunk), and marks it gets wrong.
    template<typename CutoffOf>
    [[nodiscard]] std::pair<long long, long long> score(CutoffOf&& cutoff) const {
        long long blocks = 0;
        long long marks = 0;
        for (std::size_t chunk = 0; chunk < latticeRefuted.size(); ++chunk) {
            const std::int32_t level = cutoff(chunk);
            for (std::int32_t y = kLambda; y <= kModelTop; ++y) {
                const auto row = static_cast<std::size_t>(y - kLambda);
                const bool lattice = aquifer::consultsLattice(y, level);
                blocks += lattice ? latticeRefuted[chunk][row] : globalRefuted[chunk][row];
                marks += lattice ? latticeMarksWrong[chunk][row] : globalMarksWrong[chunk][row];
            }
        }
        return {blocks, marks};
    }

    /// Blocks of chunk @p chunk refuting the cutoff @p level.
    [[nodiscard]] long long refutedIn(std::size_t chunk, std::int32_t level) const {
        long long blocks = 0;
        for (std::int32_t y = kLambda; y <= kModelTop; ++y) {
            const auto row = static_cast<std::size_t>(y - kLambda);
            blocks += aquifer::consultsLattice(y, level) ? latticeRefuted[chunk][row]
                                                         : globalRefuted[chunk][row];
        }
        return blocks;
    }

    /// Chunks with at least one block refuting @p cutoff(chunk).
    template<typename CutoffOf>
    [[nodiscard]] int chunksRefuting(CutoffOf&& cutoff) const {
        int refuted = 0;
        for (std::size_t chunk = 0; chunk < latticeRefuted.size(); ++chunk) {
            refuted += refutedIn(chunk, cutoff(chunk)) > 0 ? 1 : 0;
        }
        return refuted;
    }
};

/// Checks that @p entry is the probe's aquifer dimension, as the model
/// assumes, before anything is scored against it. Bits, not values: the
/// project builds with -Wfloat-equal, and these are exact by construction.
void requireProbeConstants(const nlohmann::json& entry) {
    INFO("dimension " << entry.at("name").get<std::string>());
    const auto same = [](const nlohmann::json& value, double expected) {
        return std::bit_cast<std::uint64_t>(value.get<double>()) ==
               std::bit_cast<std::uint64_t>(expected);
    };
    REQUIRE(entry.at("min_y").get<std::int32_t>() == kMinY);
    REQUIRE(entry.at("sea_level").get<std::int32_t>() == kSeaLevel);
    REQUIRE(entry.at("aquifers_enabled").get<bool>());
    REQUIRE(entry.at("default_fluid").at("Name").get<std::string>() == "minecraft:water");
    REQUIRE(same(entry.at("raw_final_density").at("argument"), kDensity));
    const nlohmann::json& router = entry.at("router");
    REQUIRE(same(router.at("barrier"), kBarrier));
    REQUIRE(same(router.at("fluid_level_floodedness"), kFloodedness));
    REQUIRE(same(router.at("fluid_level_spread"), kSpread));
    REQUIRE(same(router.at("lava"), kLava));
    REQUIRE(!router.contains("erosion"));
    REQUIRE(!router.contains("depth"));
}

/// Scores one dimension against @p model, the build's cutoff from the
/// shipped `chunkYSkip` over @p psl.
[[nodiscard]] ArmScore scoreArm(const std::filesystem::path& region, const LatticeModel& model,
                                const PslField& psl) {
    ArmScore score;
    const auto file = stratum::region::RegionFile::open(region);
    stratum::test::GoldenRegion neighbours{region};
    constexpr std::size_t kChunkCount = static_cast<std::size_t>(kChunks) * kChunks;
    score.builtCutoff.assign(kChunkCount, 0);
    score.latticeRefuted.assign(kChunkCount, {});
    score.globalRefuted.assign(kChunkCount, {});
    score.latticeMarksWrong.assign(kChunkCount, {});
    score.globalMarksWrong.assign(kChunkCount, {});
    for (std::int32_t cz = 0; cz < kChunks; ++cz) {
        for (std::int32_t cx = 0; cx < kChunks; ++cx) {
            if (!file.hasChunk(cx, cz)) {
                continue; // counted short by the caller's REQUIRE on `chunks`
            }
            const auto doc = stratum::nbt::read(file.readChunk(cx, cz));
            const std::string status = doc.root.at("Status").asString();
            const bool untouched = stratum::test::untouched(cx, cz, status);
            // Every chunk of the 12x12 reached the noise stage: ticked ones
            // are full, the rest are generation's own.
            if (!untouched && !(stratum::test::ticked(cx, cz) && status == "minecraft:full")) {
                continue; // counted short likewise
            }
            const auto chunk = stratum::chunk::Chunk::decode(doc.root);
            const auto marks = stratum::test::postProcessingMarks(doc.root, kMinY);
            const auto index = static_cast<std::size_t>((cz * kChunks) + cx);
            // The build's own cutoff — the filler's call, on this
            // dimension's own field.
            const std::int32_t cutoff = aquifer::chunkYSkip(psl, cx * 16, cz * 16);
            score.builtCutoff[index] = cutoff;
            ++score.chunks;
            score.untouchedChunks += untouched ? 1 : 0;

            if (untouched) {
                for (const auto& mark : marks) {
                    ++score.serverMarks;
                    score.marksAboveCutoff += static_cast<long long>(std::get<1>(mark) > cutoff);
                }
            }
            for (std::int32_t lz = 0; lz < 16; ++lz) {
                for (std::int32_t lx = 0; lx < 16; ++lx) {
                    const std::int32_t x = (cx * 16) + lx;
                    const std::int32_t z = (cz * 16) + lz;
                    for (std::int32_t y = kMinY; y <= kTopY; ++y) {
                        const auto* block = chunk.blockAt(lx, y, lz);
                        if (block == nullptr) {
                            ++score.missingBlocks;
                            continue;
                        }
                        const Category server = stratum::test::categoryOf(block->name);
                        const bool modelled = y >= kLambda && y <= kModelTop;
                        const ModelBlock* lattice = modelled ? &model.at(x, y, z) : nullptr;
                        const Category global = globalAt(y);
                        const bool inLattice = aquifer::consultsLattice(y, cutoff) && modelled;
                        const Category built = inLattice ? lattice->category : global;
                        const bool builtMark = inLattice && lattice->mark;
                        const bool serverMark = untouched && marks.contains({lx, y, lz});
                        // The post-processing list is generation's on every
                        // untouched chunk, its face included: a neighbour's
                        // flow sets blocks, not marks.
                        if (untouched) {
                            score.marksExtra += static_cast<long long>(builtMark && !serverMark);
                            score.marksMissing += static_cast<long long>(!builtMark && serverMark);
                        }
                        const bool exact = untouched && !besideTicked(x, z);
                        if (exact) {
                            ++score.exactBlocks;
                            if (built != server) {
                                // Obsidian on the lava sea's top row, under
                                // water: lava that met water when a chunk
                                // was promoted to full. Fluid that moved, not
                                // generation's answer; counted apart.
                                const bool contact =
                                    y == kLambda - 1 && stratum::test::fluidContactBlock(block);
                                score.exactContact += contact ? 1 : 0;
                                score.exactWrong += contact ? 0 : 1;
                                if (contact) {
                                    score.contactOnFull +=
                                        static_cast<long long>(status == "minecraft:full");
                                }
                            }
                            score.exactFlowing +=
                                static_cast<long long>(stratum::test::isFluid(server) &&
                                                       stratum::test::fluidLevel(block) != 0);
                            score.pocketsInLattice +=
                                static_cast<long long>(inLattice && lattice->pocket);
                            score.serverStone +=
                                static_cast<long long>(modelled && server == Category::Solid);
                        } else {
                            ++score.flowBlocks;
                            if (built != server) {
                                const bool moved =
                                    stratum::test::explainedByFlow(neighbours, x, y, z, server,
                                                                   built) ||
                                    sourceAtRegionEdge(neighbours, x, y, z, server, built);
                                score.flowMoved += moved ? 1 : 0;
                                score.faceMoved += (moved && untouched) ? 1 : 0;
                                score.flowUnexplained += moved ? 0 : 1;
                            }
                        }
                        if (modelled) {
                            const auto row = static_cast<std::size_t>(y - kLambda);
                            score.latticeRefuted[index][row] +=
                                static_cast<long long>(refutes(lattice->category, server, exact));
                            score.globalRefuted[index][row] +=
                                static_cast<long long>(refutes(global, server, exact));
                            score.latticeMarksWrong[index][row] +=
                                static_cast<long long>(untouched && lattice->mark != serverMark);
                            score.globalMarksWrong[index][row] +=
                                static_cast<long long>(serverMark);
                        }
                    }
                }
            }
        }
    }
    return score;
}

/// Chunks where @p reading's cutoff is not the build's own (`chunkYSkip`).
[[nodiscard]] int genericDiffersFromBuild(const Reading& reading, const PslField& psl,
                                          const ArmScore& score) {
    int differ = 0;
    for (std::int32_t cz = 0; cz < kChunks; ++cz) {
        for (std::int32_t cx = 0; cx < kChunks; ++cx) {
            const std::int32_t built =
                score.builtCutoff[static_cast<std::size_t>((cz * kChunks) + cx)];
            differ +=
                cutoffOf(reading, psl, cx, cz) == std::clamp(built, kLambda - 1, kModelTop) ? 0 : 1;
        }
    }
    return differ;
}

/// The uniform cutoffs in [kLambda - 1, kModelTop] that @p score leaves no
/// block refuting — y_skip read off the server, with no model of it.
[[nodiscard]] std::vector<std::int32_t> unrefutedCutoffs(const ArmScore& score) {
    std::vector<std::int32_t> zero;
    for (std::int32_t level = kLambda - 1; level <= kModelTop; ++level) {
        if (score.score([level](std::size_t) { return level; }).first == 0) {
            zero.push_back(level);
        }
    }
    return zero;
}

[[nodiscard]] std::string describe(const std::vector<std::int32_t>& levels) {
    if (levels.empty()) {
        return "none";
    }
    std::ostringstream out;
    out << "[" << levels.front() << ", " << levels.back() << "]";
    if (static_cast<std::int32_t>(levels.size()) != levels.back() - levels.front() + 1) {
        out << " (with gaps)";
    }
    return out.str();
}

/// One dimension of a step corpus, and the cutoff the closed form gives it.
struct StepArm {
    const char* name;
    std::int32_t cutoff; ///< the build's, from the table in the probe script
};

/// Reads the spec entry named @p name out of a corpus's spec.json.
[[nodiscard]] nlohmann::json specEntry(const std::filesystem::path& dir, const std::string& name) {
    std::ifstream in(dir / "spec.json");
    const nlohmann::json spec = nlohmann::json::parse(in);
    // REQUIRE rather than FAIL-then-return: MSVC sees the return after an
    // unconditional FAIL as unreachable (C4702), and warnings are errors.
    const nlohmann::json* named = nullptr;
    for (const auto& entry : spec) {
        if (entry.at("name").get<std::string>() == name) {
            named = &entry;
            break;
        }
    }
    INFO("no dimension " << name << " in " << (dir / "spec.json"));
    REQUIRE(named != nullptr);
    return *named;
}

} // namespace

TEST_CASE("y_skip's cutoff row against the server on flat surfaces from -200 to -80",
          "[conformance][aquifer]") {
    const std::vector<std::filesystem::path> step = corpora("yskip_s");
    const std::vector<std::filesystem::path> step2 = corpora("yskip2_s");
    if (step.empty() || !std::filesystem::is_directory(fixtures() / "worldgen")) {
        SKIP("no yskip_s* aquifer probe under "
             << (fixtures() / "probes")
             << "; generate them with tools/analysis/aquifer-yskip-probe.sh --group step and "
                "--group step2");
    }
    INFO("yskip2_s* corpora are written by the same script; regenerate with "
         "tools/analysis/aquifer-yskip-probe.sh --group step2");
    REQUIRE(step2.size() == step.size());

    // Every rival reads the same S_max as the build except where it says
    // otherwise (and the `none` one reads nothing).
    const std::vector<Reading> readings{
        Reading{.name = "built"},
        Reading{.name = "no cutoff", .none = true},
        Reading{.name = "+8 missing (S_max + 12)", .addend = 12},
        Reading{.name = "+8 twice (S_max + 28)", .addend = 28},
        Reading{.name = "+12 missing (S_max + 8)", .addend = 8},
        Reading{.name = "strict: lattice below y_skip only", .strict = true},
        Reading{.name = "S_max truncated toward zero", .truncate = true},
        Reading{.name = "psl read at y = 1", .readY = 1},
        Reading{.name = "psl read at min_y", .readY = kMinY},
        Reading{.name = "psl read at sea_level", .readY = kSeaLevel},
    };
    std::vector<long long> refutedBlocks(readings.size(), 0);
    std::vector<long long> wrongMarks(readings.size(), 0);
    long long serverMarks = 0;

    std::ostringstream report;
    for (std::size_t corpus = 0; corpus < step.size(); ++corpus) {
        const std::int64_t seed = corpusSeed(step[corpus]);
        INFO("seed " << seed);
        REQUIRE(corpusSeed(step2[corpus]) == seed);
        const LatticeModel model(seed);
        requireTopRowsAllWet(model);
        long long pockets = 0;
        for (std::int32_t z = 0; z < kSpan; ++z) {
            for (std::int32_t x = 0; x < kSpan; ++x) {
                for (std::int32_t y = kLambda; y <= kModelTop; ++y) {
                    pockets += static_cast<long long>(model.at(x, y, z).pocket);
                }
            }
        }
        report << "seed " << seed << ": " << pockets << " pockets in rows " << kLambda << ".."
               << kModelTop << ", " << model.sources().size() << " sources\n";
        // The pockets are the seed's, whatever the psl: pinned for the two
        // seeds the probe script writes.
        const std::map<std::int64_t, long long> kPockets{{42, 73573}, {31337, 90764}};
        if (kPockets.contains(seed)) {
            CHECK(pockets == kPockets.at(seed));
        }

        for (const auto& [dir, arms] :
             {std::pair{
                  step[corpus],
                  std::vector<StepArm>{{"ys93", -62}, {"ys92", -50}, {"ys81", -50}, {"ys80", -38}}},
              std::pair{step2[corpus],
                        std::vector<StepArm>{
                            {"ys200", -158}, {"ys925", -62}, {"ys85", -50}, {"ysy0", -50}}}}) {
            stratum::test::requireFrozen(dir, "tools/analysis/aquifer-yskip-probe.sh");
            const stratum::noise::NormalNoise probe = stratum::test::probeNoise(dir);
            for (const StepArm& arm : arms) {
                INFO("dimension " << arm.name);
                const nlohmann::json entry = specEntry(dir, arm.name);
                requireProbeConstants(entry);
                const PslField psl(entry.at("router").at("preliminary_surface_level"), probe);
                // The lattice is the model's, whatever this dimension's psl.
                REQUIRE(premiseViolations(model, psl) == 0);

                const ArmScore score = scoreArm(dir / arm.name / "r.0.0.mca", model, psl);
                const std::vector<std::int32_t> unrefuted = unrefutedCutoffs(score);
                report << "  " << arm.name << ": built cutoff " << score.builtCutoff.front()
                       << ", unrefuted uniform cutoffs " << describe(unrefuted)
                       << "; exact blocks wrong " << score.exactWrong << " of " << score.exactBlocks
                       << " (obsidian on row " << (kLambda - 1) << ": " << score.exactContact
                       << ", " << score.contactOnFull << " on full chunks), pockets in the lattice "
                       << score.pocketsInLattice << ", stone " << score.serverStone << "; moved "
                       << score.flowMoved << " (" << score.faceMoved
                       << " on an untouched face), unexplained " << score.flowUnexplained << " of "
                       << score.flowBlocks << "; marks " << score.serverMarks << " (extra "
                       << score.marksExtra << ", missing " << score.marksMissing
                       << ", above the cutoff " << score.marksAboveCutoff << ")\n";

                // The premise of everything below: the 12x12 is all there, 63
                // chunks never ticked, and nothing on them moved.
                REQUIRE(score.chunks == kChunks * kChunks);
                REQUIRE(score.missingBlocks == 0);
                REQUIRE(score.untouchedChunks == 63);
                REQUIRE(score.exactFlowing == 0);

                // The table: one cutoff per dimension, as the closed form says.
                CHECK(std::ranges::all_of(score.builtCutoff,
                                          [&](std::int32_t level) { return level == arm.cutoff; }));
                // The build, block for block: exact where nothing ticked, and
                // where it did, every difference is fluid that moved.
                CHECK(score.exactWrong == 0);
                CHECK(score.flowUnexplained == 0);
                // Obsidian on the lava sea's top row of an untouched chunk
                // comes only with the lattice's water over it, and only on a
                // chunk promoted to full: none where the global picker owns
                // row lambda.
                CHECK(score.exactContact == score.contactOnFull);
                if (arm.cutoff < kLambda) {
                    CHECK(score.exactContact == 0);
                }
                // The server's own post-processing lists, which only the
                // lattice writes: exact, and empty above the cutoff.
                CHECK(score.marksExtra == 0);
                CHECK(score.marksMissing == 0);
                CHECK(score.marksAboveCutoff == 0);

                // y_skip read off the server: the uniform cutoffs no block
                // refutes. Below lambda every cutoff reads the same (the lava
                // sea), and from kAllWetFrom up so does every one.
                REQUIRE(!unrefuted.empty());
                const std::int32_t expected = std::clamp(arm.cutoff, kLambda - 1, kModelTop);
                CHECK(std::ranges::find(unrefuted, expected) != unrefuted.end());
                if (arm.cutoff < kLambda) {
                    // The lattice is never consulted above the lava sea: a
                    // cutoff one row higher is refuted on row lambda.
                    CHECK(unrefuted.front() == kLambda - 1);
                    CHECK(unrefuted.back() == kLambda - 1);
                } else if (arm.cutoff == -50) {
                    // Exactly row -50: the rows the -92..-81 step leaves to the
                    // lattice end there, to the block.
                    CHECK(unrefuted.front() == -50);
                    CHECK(unrefuted.back() == -50);
                } else {
                    // -80 opens the next step: pockets reach about -44, so
                    // everything from there up reads alike.
                    CHECK(unrefuted.front() > -50);
                    CHECK(unrefuted.back() == kModelTop);
                }

                for (std::size_t r = 0; r < readings.size(); ++r) {
                    const auto [blocks, marksWrong] = score.score([&](std::size_t chunk) {
                        const auto cx = static_cast<std::int32_t>(chunk % kChunks);
                        const auto cz = static_cast<std::int32_t>(chunk / kChunks);
                        return cutoffOf(readings[r], psl, cx, cz);
                    });
                    refutedBlocks[r] += blocks;
                    wrongMarks[r] += marksWrong;
                }
                // The generic reading at the build's parameters IS the build.
                CHECK(genericDiffersFromBuild(readings.front(), psl, score) == 0);
                serverMarks += score.serverMarks;
            }
        }
    }
    for (std::size_t r = 0; r < readings.size(); ++r) {
        report << readings[r].name << ": " << refutedBlocks[r] << " blocks refute it, "
               << wrongMarks[r] << " post-processing marks wrong\n";
    }
    report << "server post-processing marks on untouched chunks: " << serverMarks << "\n";
    INFO(report.str());
    CHECK(refutedBlocks.front() == 0);
    CHECK(wrongMarks.front() == 0);
    // Generation's own lists, so the same every run: 135 321 over the two
    // seeds' sixteen dimensions when first run, every one the build's.
    if (step.size() == 2 && step.front().filename() == "yskip_s31337" &&
        step.back().filename() == "yskip_s42") {
        CHECK(serverMarks == 135321);
    }
    for (std::size_t r = 1; r < readings.size(); ++r) {
        INFO("rival " << readings[r].name);
        CHECK(refutedBlocks[r] >= 1000);
    }
}

namespace {

/// What the server shows of one chunk of a two-valued dimension, whose
/// cutoff can only be -50 (S_max -85) or -38 (S_max -80).
enum class ChunkClass : std::uint8_t {
    Unseen,       ///< both cutoffs fit: no pocket the two part on is visible
    Global,       ///< -50 fits and -38 is refuted
    Lattice,      ///< -38 fits and -50 is refuted
    Inconsistent, ///< both are refuted: the cutoff is neither
};

[[nodiscard]] ChunkClass observedClass(const ArmScore& score, std::size_t chunk) {
    const bool low = score.refutedIn(chunk, -50) == 0;
    const bool high = score.refutedIn(chunk, -38) == 0;
    if (low && high) {
        return ChunkClass::Unseen;
    }
    if (low) {
        return ChunkClass::Global;
    }
    return high ? ChunkClass::Lattice : ChunkClass::Inconsistent;
}

/// One dimension's observed chunk classes, and where its field reads -80 at
/// y = 0, over every column a candidate rectangle below can reach.
struct RectObservation {
    static constexpr std::int32_t kLow = -48;
    static constexpr std::int32_t kSide = kSpan + 96;

    std::vector<ChunkClass> classes;
    std::vector<bool> high = std::vector<bool>(static_cast<std::size_t>(kSide) * kSide, false);

    [[nodiscard]] bool highAt(std::int32_t x, std::int32_t z) const {
        return high[(static_cast<std::size_t>(z - kLow) * kSide) +
                    static_cast<std::size_t>(x - kLow)];
    }
};

/// A candidate sample set: [fromX, toX] x [fromZ, toZ] from a chunk's corner,
/// every `stride` blocks from the low corner, both ends included.
struct Rectangle {
    std::int32_t fromX = 0;
    std::int32_t toX = 0;
    std::int32_t fromZ = 0;
    std::int32_t toZ = 0;
    std::int32_t stride = 1;
};

/// Chunks whose observed class @p rectangle contradicts, over @p observed.
[[nodiscard]] int misfits(const std::vector<RectObservation>& observed,
                          const Rectangle& rectangle) {
    int wrong = 0;
    for (const RectObservation& one : observed) {
        for (std::size_t chunk = 0; chunk < one.classes.size(); ++chunk) {
            const ChunkClass seen = one.classes[chunk];
            if (seen != ChunkClass::Global && seen != ChunkClass::Lattice) {
                continue;
            }
            const auto baseX = static_cast<std::int32_t>(chunk % kChunks) * 16;
            const auto baseZ = static_cast<std::int32_t>(chunk / kChunks) * 16;
            bool high = false;
            for (std::int32_t dz = rectangle.fromZ; dz <= rectangle.toZ && !high;
                 dz += rectangle.stride) {
                for (std::int32_t dx = rectangle.fromX; dx <= rectangle.toX && !high;
                     dx += rectangle.stride) {
                    high = one.highAt(baseX + dx, baseZ + dz);
                }
            }
            wrong += high == (seen == ChunkClass::Lattice) ? 0 : 1;
        }
    }
    return wrong;
}

} // namespace

TEST_CASE("y_skip's rectangle and stride against the server on a two-valued surface",
          "[conformance][aquifer]") {
    const std::vector<std::filesystem::path> rect = corpora("yskiprect_s");
    if (rect.empty() || !std::filesystem::is_directory(fixtures() / "worldgen")) {
        SKIP("no yskiprect_s* aquifer probe under "
             << (fixtures() / "probes")
             << "; generate them with tools/analysis/aquifer-yskip-probe.sh --group rect");
    }

    // Offsets from the chunk's corner. The build reads every point a source
    // centre of Q3.5's lattice extent can occupy; the rest are what Q2.5's
    // "lattice-covered rectangle" could also mean.
    const std::vector<Reading> readings{
        Reading{.name = "built: [-16, +25] stride 4"},
        Reading{.name = "the cells' origins: [-16, +16] (pipeline engine v6)", .to = 16},
        Reading{.name = "far end at +20", .to = 20},
        Reading{.name = "the cells' full extent: [-16, +31]", .to = 31},
        Reading{.name = "low end at -20", .from = -20},
        Reading{.name = "low end at -12", .from = -12},
        Reading{.name = "shifted origins 16i + 5: [-11, +21]", .from = -11, .to = 21},
        Reading{.name = "the chunk alone: [0, +15]", .from = 0, .to = 15},
        Reading{.name = "stride 1", .stride = 1},
        Reading{.name = "stride 2", .stride = 2},
        Reading{.name = "stride 8", .stride = 8},
    };
    std::vector<long long> refutedBlocks(readings.size(), 0);
    std::vector<int> refutedChunks(readings.size(), 0);
    std::vector<RectObservation> observed;

    std::ostringstream report;
    for (const auto& dir : rect) {
        const std::int64_t seed = corpusSeed(dir);
        INFO("seed " << seed);
        stratum::test::requireFrozen(dir, "tools/analysis/aquifer-yskip-probe.sh");
        const stratum::noise::NormalNoise probe = stratum::test::probeNoise(dir);
        const LatticeModel model(seed);
        requireTopRowsAllWet(model);
        for (const auto& [world, readout] : {std::pair{"ysf_a", "ysr_a"}, {"ysf_b", "ysr_b"}}) {
            INFO("dimension " << world);
            const nlohmann::json entry = specEntry(dir, world);
            requireProbeConstants(entry);
            const PslField psl(entry.at("router").at("preliminary_surface_level"), probe);
            REQUIRE(premiseViolations(model, psl) == 0);

            // The rebuilt field against the server's own reading of it: the
            // readout dimension's terrain names the arm at every column the
            // probe pins (multiples of four, flat_cache's corners) — which
            // are all the columns any stride-4 reading here reads.
            const PslField indicator(specEntry(dir, readout).at("function"), probe);
            const auto file = stratum::region::RegionFile::open(dir / readout / "r.0.0.mca");
            long long columns = 0;
            long long agree = 0;
            long long high = 0;
            for (std::int32_t cz = 0; cz < kChunks; ++cz) {
                for (std::int32_t cx = 0; cx < kChunks; ++cx) {
                    if (!file.hasChunk(cx, cz)) {
                        continue; // counted short below
                    }
                    const auto chunk = stratum::chunk::Chunk::decode(
                        stratum::nbt::read(file.readChunk(cx, cz)).root);
                    for (std::int32_t lz = 0; lz < 16; lz += 4) {
                        for (std::int32_t lx = 0; lx < 16; lx += 4) {
                            const std::optional<int> top = chunk.highestNonAir(lx, lz);
                            // The indicator's arms are -1 and +1: surfaces
                            // near 61 and 195.
                            const bool serverHigh = top.value_or(kMinY) >= 128;
                            const std::int32_t x = (cx * 16) + lx;
                            const std::int32_t z = (cz * 16) + lz;
                            ++columns;
                            agree +=
                                static_cast<long long>((indicator(x, 0, z) > 0.0) == serverHigh);
                            high += static_cast<long long>(serverHigh);
                        }
                    }
                }
            }
            INFO("readout " << readout << ": " << agree << " of " << columns << " columns, " << high
                            << " at -80");
            REQUIRE(columns == 48 * 48);
            REQUIRE(agree == columns);
            REQUIRE(high > 0);

            const ArmScore score = scoreArm(dir / world / "r.0.0.mca", model, psl);
            RectObservation one;
            std::array<int, 4> classes{};
            for (std::size_t chunk = 0; chunk < score.latticeRefuted.size(); ++chunk) {
                one.classes.push_back(observedClass(score, chunk));
                ++classes[static_cast<std::size_t>(one.classes.back())];
            }
            for (std::int32_t z = RectObservation::kLow;
                 z < RectObservation::kLow + RectObservation::kSide; ++z) {
                for (std::int32_t x = RectObservation::kLow;
                     x < RectObservation::kLow + RectObservation::kSide; ++x) {
                    one.high[(static_cast<std::size_t>(z - RectObservation::kLow) *
                              RectObservation::kSide) +
                             static_cast<std::size_t>(x - RectObservation::kLow)] =
                        psl(x, 0, z) > -82.0;
                }
            }
            observed.push_back(std::move(one));

            const auto lattice38 =
                std::ranges::count(score.builtCutoff, static_cast<std::int32_t>(-38));
            report << "seed " << seed << " " << world << ": the build puts " << lattice38
                   << " chunks at -38 and the rest at -50; the server shows "
                   << classes[static_cast<std::size_t>(ChunkClass::Lattice)] << " at -38, "
                   << classes[static_cast<std::size_t>(ChunkClass::Global)] << " at -50, "
                   << classes[static_cast<std::size_t>(ChunkClass::Unseen)] << " unseen; "
                   << "exact blocks wrong " << score.exactWrong << " of " << score.exactBlocks
                   << " (obsidian on row " << (kLambda - 1) << ": " << score.exactContact
                   << "); moved " << score.flowMoved << " (" << score.faceMoved
                   << " on an untouched face), unexplained " << score.flowUnexplained << "; marks "
                   << score.serverMarks << " (extra " << score.marksExtra << ", missing "
                   << score.marksMissing << ")\n";
            REQUIRE(score.chunks == kChunks * kChunks);
            REQUIRE(score.missingBlocks == 0);
            REQUIRE(score.untouchedChunks == 63);
            REQUIRE(score.exactFlowing == 0);
            // Both cutoffs occur, or the field separates nothing; and every
            // chunk the server shows is one or the other.
            REQUIRE(lattice38 > 0);
            REQUIRE(lattice38 < kChunks * kChunks);
            CHECK(classes[static_cast<std::size_t>(ChunkClass::Inconsistent)] == 0);
            CHECK(score.exactWrong == 0);
            CHECK(score.exactContact == score.contactOnFull);
            CHECK(score.flowUnexplained == 0);
            CHECK(score.marksExtra == 0);
            CHECK(score.marksMissing == 0);
            CHECK(score.marksAboveCutoff == 0);

            for (std::size_t r = 0; r < readings.size(); ++r) {
                const auto cutoff = [&](std::size_t chunk) {
                    const auto cx = static_cast<std::int32_t>(chunk % kChunks);
                    const auto cz = static_cast<std::int32_t>(chunk / kChunks);
                    return cutoffOf(readings[r], psl, cx, cz);
                };
                refutedBlocks[r] += score.score(cutoff).first;
                refutedChunks[r] += score.chunksRefuting(cutoff);
            }
            CHECK(genericDiffersFromBuild(readings.front(), psl, score) == 0);
        }
    }

    // The rectangle read off the server: every square from -28..-4 to
    // +12..+36 at strides 1, 2, 4 and 8, then each axis alone at stride 4
    // with the other held at the build's, scored on the chunks whose class
    // the server shows. Only a low end of -16 and a high end of +24..+27
    // (which read the same samples) at stride 4 leave none wrong.
    std::vector<std::string> square;
    for (const std::int32_t stride : {1, 2, 4, 8}) {
        for (std::int32_t from = -28; from <= -4; ++from) {
            for (std::int32_t to = 12; to <= 36; ++to) {
                if (misfits(observed, Rectangle{from, to, from, to, stride}) == 0) {
                    square.push_back(std::to_string(stride) + ":" + std::to_string(from) + ".." +
                                     std::to_string(to));
                }
            }
        }
    }
    std::vector<std::string> alongX;
    std::vector<std::string> alongZ;
    for (std::int32_t from = -28; from <= -4; ++from) {
        for (std::int32_t to = 12; to <= 36; ++to) {
            if (misfits(observed, Rectangle{from, to, -16, 25, 4}) == 0) {
                alongX.push_back(std::to_string(from) + ".." + std::to_string(to));
            }
            if (misfits(observed, Rectangle{-16, 25, from, to, 4}) == 0) {
                alongZ.push_back(std::to_string(from) + ".." + std::to_string(to));
            }
        }
    }
    const auto joined = [](const std::vector<std::string>& items) {
        std::string out;
        for (const auto& item : items) {
            out += (out.empty() ? "" : ", ") + item;
        }
        return out;
    };
    report << "unrefuted squares (stride:from..to): " << joined(square) << "\n"
           << "unrefuted along x: " << joined(alongX) << "\nunrefuted along z: " << joined(alongZ)
           << "\n";
    for (std::size_t r = 0; r < readings.size(); ++r) {
        report << readings[r].name << ": " << refutedBlocks[r] << " blocks on " << refutedChunks[r]
               << " chunks refute it\n";
    }
    INFO(report.str());
    const std::vector<std::string> ends{"-16..24", "-16..25", "-16..26", "-16..27"};
    CHECK(square == std::vector<std::string>{"4:-16..24", "4:-16..25", "4:-16..26", "4:-16..27"});
    CHECK(alongX == ends);
    CHECK(alongZ == ends);
    CHECK(refutedBlocks.front() == 0);
    for (std::size_t r = 1; r < readings.size(); ++r) {
        INFO("rival " << readings[r].name);
        CHECK(refutedChunks[r] >= 5);
    }
}

namespace {

/// One probe dimension's own noise settings as a data pack on disk — the
/// entry density-probe.sh writes from the same spec entry — so the shipped
/// `ChunkFiller` can generate it. Pack opens a directory, nothing else.
class ProbePack {
public:
    ProbePack(const nlohmann::json& entry, const std::filesystem::path& corpus)
        : path_(std::filesystem::temp_directory_path() / stratum::test::tempName("stratum-yskip")),
          name_(entry.at("name").get<std::string>()) {
        std::filesystem::create_directories(path_);
        std::ofstream(path_ / "pack.mcmeta") << R"({"pack":{"description":"stratum y_skip replay",)"
                                                R"("min_format":[94,1],"max_format":94}})";
        // The router's fourteen other entries are 0, as density-probe.sh
        // writes them; the probe's own carry over.
        nlohmann::json router = nlohmann::json::object();
        for (const char* key :
             {"barrier", "fluid_level_floodedness", "fluid_level_spread", "lava", "temperature",
              "vegetation", "continents", "erosion", "depth", "ridges", "preliminary_surface_level",
              "vein_toggle", "vein_ridged", "vein_gap"}) {
            router[key] = 0;
        }
        for (const auto& [key, value] : entry.at("router").items()) {
            router[key] = value;
        }
        router["final_density"] = entry.at("raw_final_density");
        const nlohmann::json settings{{"sea_level", entry.at("sea_level")},
                                      {"disable_mob_generation", true},
                                      {"aquifers_enabled", entry.at("aquifers_enabled")},
                                      {"ore_veins_enabled", false},
                                      {"legacy_random_source", false},
                                      {"default_block", {{"Name", "minecraft:stone"}}},
                                      {"default_fluid", entry.at("default_fluid")},
                                      {"noise",
                                       {{"min_y", entry.at("min_y")},
                                        {"height", entry.at("height")},
                                        {"size_horizontal", 1},
                                        {"size_vertical", entry.at("size_vertical")}}},
                                      {"spawn_target", nlohmann::json::array()},
                                      {"surface_rule", entry.at("surface_rule")},
                                      {"noise_router", router}};
        write("noise_settings", name_, settings);
        // The probe's noise, as its manifest records it.
        const nlohmann::json manifest =
            nlohmann::json::parse(std::ifstream(corpus / "manifest.json"));
        const nlohmann::json& declared = manifest.at("probe_noise");
        REQUIRE(declared.at("id").get<std::string>() == "stratum:probe_noise");
        write("noise", "probe_noise",
              {{"firstOctave", declared.at("first_octave")},
               {"amplitudes", declared.at("amplitudes")}});
    }

    ProbePack(const ProbePack&) = delete;
    ProbePack& operator=(const ProbePack&) = delete;
    ProbePack(ProbePack&&) = delete;
    ProbePack& operator=(ProbePack&&) = delete;

    ~ProbePack() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] stratum::data::Pack pack() const {
        return stratum::data::Pack::openDataPack(path_);
    }

    [[nodiscard]] stratum::data::ResourceLocation settingsId() const {
        return stratum::data::ResourceLocation{"stratum", name_};
    }

private:
    void write(const std::string& registry, const std::string& name,
               const nlohmann::json& body) const {
        const std::filesystem::path file =
            path_ / "data" / "stratum" / "worldgen" / registry / (name + ".json");
        std::filesystem::create_directories(file.parent_path());
        std::ofstream(file) << body.dump();
    }

    std::filesystem::path path_;
    std::string name_;
};

} // namespace

TEST_CASE("the filler writes the server's block on every untouched chunk of the y_skip probes",
          "[conformance][aquifer][terrain]") {
    // End to end: the probe's own noise settings through `ChunkFiller`, which
    // computes y_skip, decides which rows reach the lattice and runs the
    // aquifer on the rest — scored on the untouched chunks, where the server's
    // blocks and post-processing lists are generation's own. One dimension
    // per y_skip it can show: -62 (the lattice never above the lava sea), -50,
    // -38, and the two-valued fields where it varies by chunk.
    std::vector<std::pair<std::filesystem::path, std::string>> arms;
    for (const auto& dir : corpora("yskip_s")) {
        for (const char* name : {"ys93", "ys92", "ys80"}) {
            arms.emplace_back(dir, name);
        }
    }
    for (const auto& dir : corpora("yskiprect_s")) {
        for (const char* name : {"ysf_a", "ysf_b"}) {
            arms.emplace_back(dir, name);
        }
    }
    if (arms.empty() || !std::filesystem::is_directory(fixtures() / "worldgen")) {
        SKIP("no yskip_s* or yskiprect_s* aquifer probe under "
             << (fixtures() / "probes")
             << "; generate them with tools/analysis/aquifer-yskip-probe.sh");
    }

    long long blocks = 0;
    long long wrong = 0;
    long long contact = 0;
    long long serverMarks = 0;
    long long marksExtra = 0;
    long long marksMissing = 0;
    int chunks = 0;
    for (const auto& [dir, name] : arms) {
        INFO("probe " << dir.filename().string() << ", dimension " << name);
        const std::int64_t seed = corpusSeed(dir);
        stratum::test::requireFrozen(dir, "tools/analysis/aquifer-yskip-probe.sh");
        const ProbePack probe(specEntry(dir, name), dir);
        const stratum::data::Pack pack = probe.pack();
        const auto loaded = stratum::settings::loadAll(pack);
        const auto& settings = loaded.settings.at(probe.settingsId());
        const auto noises = stratum::density::NoiseRegistry::create(
            pack, loaded.graph.referencedNoises(), seed, stratum::density::RandomSource::Xoroshiro);
        const auto filler = stratum::terrain::ChunkFiller::compile(loaded.graph, noises, settings);

        const auto file = stratum::region::RegionFile::open(dir / name / "r.0.0.mca");
        for (std::int32_t cz = 0; cz < kChunks; ++cz) {
            for (std::int32_t cx = 0; cx < kChunks; ++cx) {
                if (!file.hasChunk(cx, cz)) {
                    continue;
                }
                const auto doc = stratum::nbt::read(file.readChunk(cx, cz));
                if (!stratum::test::untouched(cx, cz, doc.root.at("Status").asString())) {
                    continue;
                }
                ++chunks;
                const auto chunk = stratum::chunk::Chunk::decode(doc.root);
                const auto marks = stratum::test::postProcessingMarks(doc.root, kMinY);
                stratum::terrain::ChunkBuffer buffer(settings.geometry);
                filler.fill(cx, cz, buffer);
                std::set<stratum::test::LocalPosition> ours;
                for (const auto& update : buffer.fluidUpdates()) {
                    ours.emplace(update.localX, update.y, update.localZ);
                }
                serverMarks += static_cast<long long>(marks.size());
                for (const auto& mark : ours) {
                    marksExtra += marks.contains(mark) ? 0 : 1;
                }
                for (const auto& mark : marks) {
                    marksMissing += ours.contains(mark) ? 0 : 1;
                }
                for (std::int32_t lz = 0; lz < 16; ++lz) {
                    for (std::int32_t lx = 0; lx < 16; ++lx) {
                        if (besideTicked((cx * 16) + lx, (cz * 16) + lz)) {
                            continue; // a ticked neighbour's flow can reach here
                        }
                        for (std::int32_t y = kMinY; y <= kTopY; ++y) {
                            const auto* server = chunk.blockAt(lx, y, lz);
                            const std::string built = buffer.at(lx, y, lz).name.toString();
                            ++blocks;
                            if (server == nullptr || server->name != built) {
                                const bool obsidian =
                                    y == kLambda - 1 && stratum::test::fluidContactBlock(server);
                                contact += obsidian ? 1 : 0;
                                wrong += obsidian ? 0 : 1;
                            }
                        }
                    }
                }
            }
        }
    }
    INFO("chunks " << chunks << ", blocks " << blocks << ", wrong " << wrong << ", obsidian on row "
                   << (kLambda - 1) << " " << contact << "; marks " << serverMarks << ", extra "
                   << marksExtra << ", missing " << marksMissing);
    REQUIRE(chunks == static_cast<int>(arms.size()) * 63);
    CHECK(wrong == 0);
    CHECK(marksExtra == 0);
    CHECK(marksMissing == 0);
    REQUIRE(serverMarks > 0);
}
