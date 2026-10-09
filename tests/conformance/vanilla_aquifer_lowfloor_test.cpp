// Stratum — an aborted scan's status, where the sea sits below the lava level.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// A cell whose scan of `preliminary_surface_level` aborts and that does not
// take the sea reads a floor. Through pipeline engine v9 `cellLevel` gave it
// lambda, typed as the cell's own fluid, on three paths: the aborting
// near-surface floor, an aborted cell off the near-surface path (engine v5)
// and an aborted cell under Q5.9's deep-dark override (v6). The clean-room
// spec's Q5.3(b) gives every one of them the global picker's status at the
// surface the scan met submerged in the lava sea — Q1.1's A_lava, -54 and
// lava. At any sea at or above -54 those are one level, and nothing can see
// the type (aquifer_substance_test.cpp pins that); below it lambda is
// `sea_level`, and the readings part in blocks.
//
// `tools/analysis/aquifer-lowfloor-probe.sh` builds the worlds that part
// them: at sea -70 a two-valued surface (-88 / -20) sends cells through all
// three paths, at floodedness 0, at 0.6 (water bodies beside the floors) and
// under the override; at sea -60 a flat -75 holds the near-surface floor
// alone, where the literal -54 and lambda + 16 part; and at sea 63 the same
// noise (-70 / 96) is the control, where the floor's readings are one. One
// more rival rides along: whether the near-surface return needs the scan's
// anchor to clear the abort threshold, which sampling.hpp carried as a tie.
//
// Scored on the chunks the probe generated without ticking them (63 per
// dimension, support/probe_region.hpp), whose blocks and post-processing
// lists are generation's own: the shipped filler end to end on five of them,
// and each rival reading through the shipped decision
// (`computeSubstanceWith`) on every fourth column of all 63, over the rows
// the floor can move. About 35 s for both seeds in a Debug build.
//
// The fixtures are Mojang-derived and never committed (SPEC §12).
#include "support/fluid_flow.hpp"
#include "support/probe_corpus.hpp"
#include "support/probe_region.hpp"
#include "support/probe_settings.hpp"
#include "support/temp_path.hpp"

#include <stratum/aquifer/fluid_type.hpp>
#include <stratum/aquifer/lattice.hpp>
#include <stratum/aquifer/sampling.hpp>
#include <stratum/aquifer/selection.hpp>
#include <stratum/aquifer/substance.hpp>
#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/javamath.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/terrain/filler.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <vector>

namespace {

using namespace stratum;

constexpr const char* kScript = "tools/analysis/aquifer-lowfloor-probe.sh";
/// The region's chunks the probe can have generated: the 8x8 window, its
/// ticked ring, and the untouched band out to chunk 11.
constexpr std::int32_t kRegionChunks = 12;
/// Rows scored for the rival readings, from lambda up (and never past the
/// chunk's y_skip). A floor at -54 reads fluid to -55 and weighs in the
/// barrier a few rows above it, where its pressure against a dry neighbour
/// fades: 24 rows hold both at sea -70, and at sea 63 the band the floors
/// share with their neighbours.
constexpr std::int32_t kRivalRows = 24;
/// The rival readings read every fourth column of an untouched chunk, both
/// ways (16 of 256), which keeps the case to seconds.
constexpr std::int32_t kColumnStride = 4;
/// The shipped filler runs on every thirteenth untouched chunk, in scan
/// order: 5 of the 63, whole.
constexpr int kFillerChunkStride = 13;
constexpr int kFillerChunks = 5;

[[nodiscard]] std::filesystem::path fixtures() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11";
}

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

/// The seed a corpus's name carries (`lowfloor_s<seed>`), checked against
/// the manifest it was generated with.
[[nodiscard]] std::int64_t corpusSeed(const std::filesystem::path& dir) {
    const std::string name = dir.filename().string();
    const auto at = name.rfind("_s");
    INFO("corpus " << name);
    REQUIRE(at != std::string::npos);
    const std::int64_t seed = std::stoll(name.substr(at + 2));
    test::requireSeed(dir, seed);
    return seed;
}

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

/// One probe dimension as a data pack on disk: its noise settings as
/// density-probe.sh wrote them (support/probe_settings.hpp), the probe's
/// noise as its manifest records it, and vanilla's `aquifer_barrier` noise
/// from the fetched worldgen tree, which the dimensions' barrier reads.
/// Removed when it goes out of scope; nothing here is committed.
class ProbePack {
public:
    ProbePack(const nlohmann::json& entry, const std::filesystem::path& corpus)
        : path_(test::tempPath("stratum-lowfloor")), name_(entry.at("name").get<std::string>()) {
        std::filesystem::create_directories(path_);
        std::ofstream(path_ / "pack.mcmeta")
            << R"({"pack":{"description":"stratum low-floor replay",)"
               R"("min_format":[94,1],"max_format":94}})";
        write("stratum", "noise_settings", name_, test::probeNoiseSettings(entry));
        const nlohmann::json manifest =
            nlohmann::json::parse(std::ifstream(corpus / "manifest.json"));
        const nlohmann::json& declared = manifest.at("probe_noise");
        REQUIRE(declared.at("id").get<std::string>() == "stratum:probe_noise");
        write("stratum", "noise", "probe_noise",
              {{"firstOctave", declared.at("first_octave")},
               {"amplitudes", declared.at("amplitudes")}});
        write("minecraft", "noise", "aquifer_barrier",
              nlohmann::json::parse(
                  std::ifstream(fixtures() / "worldgen" / "noise" / "aquifer_barrier.json")));
    }

    ProbePack(const ProbePack&) = delete;
    ProbePack& operator=(const ProbePack&) = delete;
    ProbePack(ProbePack&&) = delete;
    ProbePack& operator=(ProbePack&&) = delete;

    ~ProbePack() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] data::Pack pack() const { return data::Pack::openDataPack(path_); }

    [[nodiscard]] data::ResourceLocation settingsId() const {
        return data::ResourceLocation{"stratum", name_};
    }

private:
    void write(const std::string& space, const std::string& registry, const std::string& name,
               const nlohmann::json& body) const {
        const std::filesystem::path file =
            path_ / "data" / space / "worldgen" / registry / (name + ".json");
        std::filesystem::create_directories(file.parent_path());
        std::ofstream(file) << body.dump();
    }

    std::filesystem::path path_;
    std::string name_;
};

/// A column an untouched chunk shares a face with a ticked one: the
/// neighbour's flow can reach it (vanilla_aquifer_yskip_test.cpp measured
/// that it reaches no further in).
[[nodiscard]] bool besideTicked(std::int32_t x, std::int32_t z) {
    constexpr std::int32_t kFace = 9 * 16;
    return (x == kFace && z < kFace) || (z == kFace && x < kFace);
}

/// How an aborted scan's status is read. Each rival but the last replaces
/// the status of a cell whose scan aborted and that took no sea — the floor
/// — and leaves every other status the shipped one.
enum class Reading : std::uint8_t {
    Shipped,         ///< A_lava: -54 and lava (spec Q1.1, Q5.3(b))
    Lambda,          ///< engine v9: lambda, and the cell's own type
    LambdaLava,      ///< lambda, lava
    OwnType,         ///< -54, and the cell's own type
    NearSurfaceOnly, ///< A_lava on the near-surface path, v9's lambda off it
    LambdaPlus16,    ///< lambda + 16, lava (the literal -54 at sea -70)
    /// The near-surface return taken only by a cell whose anchor clears the
    /// abort threshold: a low anchor sends a cell off the path, to A_lava,
    /// even where it sits more than twenty above the cap and the shipped
    /// reading gives it the sea. sampling.hpp carried this as a permanent
    /// tie, which it stopped being once the floor left lambda.
    AnchorClears,
    Count,
};

constexpr std::array<const char*, static_cast<std::size_t>(Reading::Count)> kReadingNames{
    "A_lava (-54, lava): shipped",
    "lambda, the cell's own type: engine v9",
    "lambda, lava",
    "-54, the cell's own type",
    "A_lava on the near-surface path only, v9 off it",
    "lambda + 16, lava",
    "the near-surface return needs an anchor above the abort threshold"};

[[nodiscard]] bool onNearSurfacePath(const aquifer::CellFluid& cell) {
    return cell.surface.gate < javamath::wrappingSub(cell.seaLevel, aquifer::kOceanGateOffset) &&
           javamath::wrappingSub(cell.surface.gate, cell.centreY) < aquifer::kNearSurfaceDepth;
}

[[nodiscard]] aquifer::SourceStatus statusUnder(const Reading reading,
                                                const aquifer::RankedCell& inputs) {
    const aquifer::SourceStatus shipped = aquifer::sourceStatus(inputs.cell, inputs.lava);
    // The trailing guard's A_lava (a cell centred below lambda that took the
    // sea) did not abort, and is not in question.
    if (reading == Reading::Shipped || !inputs.cell.surface.aborted) {
        return shipped;
    }
    constexpr aquifer::SourceStatus kALava{.level = aquifer::kLavaLevel,
                                           .type = aquifer::FluidType::Lava};
    if (reading == Reading::AnchorClears) {
        const bool lowAnchor = static_cast<double>(inputs.cell.surface.anchor) <
                               aquifer::abortThreshold(inputs.cell.seaLevel);
        return onNearSurfacePath(inputs.cell) && lowAnchor ? kALava : shipped;
    }
    if (aquifer::cellLevel(inputs.cell).origin != aquifer::LevelOrigin::GlobalLava) {
        return shipped; // the near-surface sea of a cell twenty above its cap
    }
    const std::int32_t lambda = aquifer::lambdaLevel(inputs.cell.seaLevel);
    const auto own = [&](const std::int32_t at) {
        return aquifer::SourceStatus{
            .level = at,
            .type = aquifer::fluidTypeOf(aquifer::FluidTypeAt{.centreY = inputs.cell.centreY,
                                                              .level = at,
                                                              .seaLevel = inputs.cell.seaLevel,
                                                              .lava = inputs.lava})};
    };
    switch (reading) {
        case Reading::Lambda:
            return own(lambda);
        case Reading::LambdaLava:
            return aquifer::SourceStatus{.level = lambda, .type = aquifer::FluidType::Lava};
        case Reading::OwnType:
            return own(aquifer::kLavaLevel);
        case Reading::NearSurfaceOnly:
            return onNearSurfacePath(inputs.cell) ? shipped : own(lambda);
        case Reading::LambdaPlus16:
            return aquifer::SourceStatus{.level = lambda + 16, .type = aquifer::FluidType::Lava};
        case Reading::Shipped:
        case Reading::AnchorClears:
        case Reading::Count:
            break;
    }
    return shipped;
}

/// One aquifer dimension of a corpus, loaded the way the filler loads it.
class Dimension {
public:
    Dimension(const std::filesystem::path& corpus, const std::string& name, std::int64_t seed)
        : probe_(specEntry(corpus, name), corpus), pack_(probe_.pack()),
          loaded_(settings::loadAll(pack_)), settings_(loaded_.settings.at(probe_.settingsId())),
          noises_(density::NoiseRegistry::create(pack_, loaded_.graph.referencedNoises(), seed,
                                                 density::RandomSource::Xoroshiro)),
          interpreter_(loaded_.graph, noises_),
          filler_(terrain::ChunkFiller::compile(loaded_.graph, noises_, settings_)),
          centres_(seed) {}

    [[nodiscard]] const settings::NoiseSettings& settings() const { return settings_; }

    [[nodiscard]] const terrain::ChunkFiller& filler() const { return filler_; }

    [[nodiscard]] const aquifer::CentreSource& centres() const { return centres_; }

    /// A router entry at a point, uncached (every entry here is constant in
    /// y or a flat function of the column).
    [[nodiscard]] double read(settings::RouterEntry entry, std::int32_t x, std::int32_t y,
                              std::int32_t z) const {
        return interpreter_.evaluate(settings_.router.at(entry),
                                     density::Point{.x = x, .y = y, .z = z});
    }

private:
    ProbePack probe_;
    data::Pack pack_;
    settings::LoadedSettings loaded_;
    const settings::NoiseSettings& settings_;
    density::NoiseRegistry noises_;
    density::Interpreter interpreter_;
    terrain::ChunkFiller filler_;
    aquifer::CentreSource centres_;
};

/// Everything one dimension's untouched chunks say about the readings.
struct ArmScore {
    int chunks = 0;
    long long blocks = 0;      ///< blocks the readings are scored on
    long long moved = 0;       ///< server fluid that moved, left out of every reading's score
    long long unexplained = 0; ///< of those, not shaped like flow
    long long marks = 0;       ///< marks the server holds at the scored positions
    std::array<long long, static_cast<std::size_t>(Reading::Count)> wrongBlocks{};
    std::array<long long, static_cast<std::size_t>(Reading::Count)> wrongMarks{};
    /// Positions where a reading predicts anything different from the
    /// shipped one — block or mark — whatever the server holds.
    std::array<long long, static_cast<std::size_t>(Reading::Count)> differsFromShipped{};
    long long floorLava = 0; ///< scored server lava whose nearest source takes the floor
};

/// Whether the server's block is one generation writes in these worlds —
/// air, stone, or a water or lava SOURCE — rather than what moving fluid
/// left behind.
[[nodiscard]] bool generationsOwn(const chunk::BlockState* block) {
    if (block == nullptr || block->name == "minecraft:air" || block->name == "minecraft:stone") {
        return true;
    }
    return (block->name == "minecraft:water" || block->name == "minecraft:lava") &&
           test::fluidLevel(block) == 0;
}

[[nodiscard]] test::Category categoryOf(const aquifer::SubstanceAt& at) {
    switch (at.substance) {
        case aquifer::Substance::Air:
            return test::Category::Air;
        case aquifer::Substance::Solid:
            return test::Category::Solid;
        case aquifer::Substance::Fluid:
            return at.fluidType == aquifer::FluidType::Lava ? test::Category::Lava
                                                            : test::Category::Water;
    }
    return test::Category::Solid;
}

/// Scores every reading on @p dimension's untouched chunks.
ArmScore scoreReadings(const std::filesystem::path& region, const Dimension& dimension) {
    const settings::NoiseSettings& settings = dimension.settings();
    const std::int32_t sea = settings.seaLevel;
    const std::int32_t lambda = aquifer::lambdaLevel(sea);
    const std::int32_t minY = settings.geometry.minY;
    const auto router = [&](settings::RouterEntry entry) {
        return [&dimension, entry](std::int32_t x, std::int32_t y, std::int32_t z) {
            return dimension.read(entry, x, y, z);
        };
    };
    const auto psl = router(settings::RouterEntry::PreliminarySurfaceLevel);
    const auto barrier = router(settings::RouterEntry::Barrier);
    const auto floodedness = router(settings::RouterEntry::FluidLevelFloodedness);
    const auto spread = router(settings::RouterEntry::FluidLevelSpread);
    const auto lava = router(settings::RouterEntry::Lava);
    const auto deepDark = [&](std::int32_t x, std::int32_t y, std::int32_t z) {
        return aquifer::isDeepDark(dimension.read(settings::RouterEntry::Erosion, x, y, z),
                                   dimension.read(settings::RouterEntry::Depth, x, y, z));
    };
    // Each source's inputs, read once and kept by centre: every reading's
    // status is a function of them alone.
    using Key = std::tuple<std::int32_t, std::int32_t, std::int32_t>;
    std::map<Key, aquifer::RankedCell> inputsOf;
    const auto inputs = [&](const aquifer::Source& ranked) -> const aquifer::RankedCell& {
        const Key key{ranked.centre.x, ranked.centre.y, ranked.centre.z};
        auto found = inputsOf.find(key);
        if (found == inputsOf.end()) {
            found = inputsOf
                        .emplace(key, aquifer::rankedCellOf(ranked, sea, psl, floodedness, spread,
                                                            lava, deepDark))
                        .first;
        }
        return found->second;
    };

    ArmScore score;
    test::GoldenRegion golden(region);
    const auto file = region::RegionFile::open(region);
    for (std::int32_t cz = 0; cz < kRegionChunks; ++cz) {
        for (std::int32_t cx = 0; cx < kRegionChunks; ++cx) {
            if (!file.hasChunk(cx, cz)) {
                continue;
            }
            const auto doc = nbt::read(file.readChunk(cx, cz));
            if (!test::untouched(cx, cz, doc.root.at("Status").asString())) {
                continue;
            }
            ++score.chunks;
            const std::set<test::LocalPosition> marks = test::postProcessingMarks(doc.root, minY);
            const std::int32_t ySkip = aquifer::chunkYSkip(psl, cx * 16, cz * 16);
            const std::int32_t top = std::min(ySkip, lambda + kRivalRows);
            for (std::int32_t lz = 0; lz < 16; lz += kColumnStride) {
                for (std::int32_t lx = 0; lx < 16; lx += kColumnStride) {
                    const std::int32_t x = (cx * 16) + lx;
                    const std::int32_t z = (cz * 16) + lz;
                    for (std::int32_t y = lambda; y <= top; ++y) {
                        const aquifer::AquiferQuery query{
                            .x = x, .y = y, .z = z, .density = -1.0, .seaLevel = sea};
                        // A rival differs from the shipped reading only
                        // through a ranked source whose scan aborted.
                        const aquifer::Selection selection =
                            aquifer::selectSources(dimension.centres(), x, y, z);
                        const bool floorRanked =
                            std::ranges::any_of(selection.ranked, [&](const aquifer::Source& s) {
                                return inputs(s).cell.surface.aborted;
                            });
                        std::optional<double> barrierHere;
                        const auto barrierOnce = [&](std::int32_t bx, std::int32_t by,
                                                     std::int32_t bz) {
                            if (!barrierHere.has_value()) {
                                barrierHere = barrier(bx, by, bz);
                            }
                            return *barrierHere;
                        };
                        std::array<aquifer::SubstanceAt, static_cast<std::size_t>(Reading::Count)>
                            out{};
                        for (std::size_t r = 0; r < out.size(); ++r) {
                            if (r > 0 && !floorRanked) {
                                out[r] = out[0];
                                continue;
                            }
                            const auto reading = static_cast<Reading>(r);
                            out[r] = aquifer::computeSubstanceWith(
                                dimension.centres(), query,
                                [&](const aquifer::Source& ranked) {
                                    return statusUnder(reading, inputs(ranked));
                                },
                                barrierOnce);
                        }
                        const bool marked = marks.contains(test::LocalPosition{lx, y, lz});
                        score.marks += marked ? 1 : 0;
                        for (std::size_t r = 0; r < out.size(); ++r) {
                            score.wrongMarks[r] += out[r].fluidUpdate == marked ? 0 : 1;
                            const bool differs = categoryOf(out[r]) != categoryOf(out[0]) ||
                                                 out[r].fluidUpdate != out[0].fluidUpdate;
                            score.differsFromShipped[r] += differs ? 1 : 0;
                        }
                        if (besideTicked(x, z)) {
                            continue; // a ticked neighbour's flow can reach the blocks here
                        }
                        const chunk::BlockState* block = golden.blockAt(x, y, z);
                        const test::Category served =
                            block == nullptr ? test::Category::Air : test::categoryOf(block->name);
                        if (!generationsOwn(block)) {
                            ++score.moved;
                            score.unexplained +=
                                test::explainedByFlow(golden, x, y, z, served, categoryOf(out[0]))
                                    ? 0
                                    : 1;
                            continue;
                        }
                        ++score.blocks;
                        for (std::size_t r = 0; r < out.size(); ++r) {
                            score.wrongBlocks[r] += categoryOf(out[r]) == served ? 0 : 1;
                        }
                        const aquifer::RankedCell& nearest = inputs(selection.nearest());
                        score.floorLava += static_cast<long long>(
                            served == test::Category::Lava && nearest.cell.surface.aborted &&
                            aquifer::cellLevel(nearest.cell).origin ==
                                aquifer::LevelOrigin::GlobalLava);
                    }
                }
            }
        }
    }
    return score;
}

/// The shipped filler, end to end, on every `kFillerChunkStride`th untouched
/// chunk: block names exact, and the post-processing list exact.
struct FillScore {
    int untouched = 0;
    int chunks = 0;
    long long blocks = 0;
    long long wrong = 0;
    long long moved = 0; ///< server blocks fluid moved to, all of them flow-shaped
    long long unexplained = 0;
    long long marks = 0;
    long long marksExtra = 0;
    long long marksMissing = 0;
};

FillScore scoreFiller(const std::filesystem::path& region, const Dimension& dimension) {
    const settings::NoiseSettings& settings = dimension.settings();
    const std::int32_t minY = settings.geometry.minY;
    const std::int32_t topY = minY + settings.geometry.height - 1;
    FillScore score;
    test::GoldenRegion golden(region);
    const auto file = region::RegionFile::open(region);
    for (std::int32_t cz = 0; cz < kRegionChunks; ++cz) {
        for (std::int32_t cx = 0; cx < kRegionChunks; ++cx) {
            if (!file.hasChunk(cx, cz)) {
                continue;
            }
            const auto doc = nbt::read(file.readChunk(cx, cz));
            if (!test::untouched(cx, cz, doc.root.at("Status").asString())) {
                continue;
            }
            if (score.untouched++ % kFillerChunkStride != 0) {
                continue;
            }
            ++score.chunks;
            const std::set<test::LocalPosition> marks = test::postProcessingMarks(doc.root, minY);
            // A buffer per chunk: `fill` rewrites every block of a buffer it
            // is given, but appends to its fluid updates.
            terrain::ChunkBuffer buffer(settings.geometry);
            dimension.filler().fill(cx, cz, buffer);
            std::set<test::LocalPosition> ours;
            for (const auto& update : buffer.fluidUpdates()) {
                ours.emplace(update.localX, update.y, update.localZ);
            }
            score.marks += static_cast<long long>(marks.size());
            for (const auto& mark : ours) {
                score.marksExtra += marks.contains(mark) ? 0 : 1;
            }
            for (const auto& mark : marks) {
                score.marksMissing += ours.contains(mark) ? 0 : 1;
            }
            for (std::int32_t lz = 0; lz < 16; ++lz) {
                for (std::int32_t lx = 0; lx < 16; ++lx) {
                    const std::int32_t x = (cx * 16) + lx;
                    const std::int32_t z = (cz * 16) + lz;
                    if (besideTicked(x, z)) {
                        continue;
                    }
                    for (std::int32_t y = minY; y <= topY; ++y) {
                        const chunk::BlockState* block = golden.blockAt(x, y, z);
                        const std::string built = buffer.at(lx, y, lz).name.toString();
                        const std::string served =
                            block == nullptr ? std::string("minecraft:air") : block->name;
                        ++score.blocks;
                        if (served == built) {
                            // The right block; flowing, if fluid moved here.
                            score.moved += test::fluidLevel(block) > 0 ? 1 : 0;
                            continue;
                        }
                        const test::Category servedCategory = test::categoryOf(served);
                        const test::Category builtCategory = test::categoryOf(built);
                        if (servedCategory != builtCategory &&
                            test::explainedByFlow(golden, x, y, z, servedCategory, builtCategory)) {
                            ++score.moved;
                            continue;
                        }
                        ++score.wrong;
                    }
                }
            }
        }
    }
    return score;
}

/// What each arm must show, per reading: refuted (the server's blocks say
/// otherwise on at least `kRefuted` of the sampled ones, where the shipped
/// reading is wrong on none), or tied with the shipped one on every block
/// and mark, whatever the server holds.
enum class Expect : std::uint8_t { Refuted, Tied };
constexpr long long kRefuted = 200;

struct Arm {
    const char* name;
    std::array<Expect, static_cast<std::size_t>(Reading::Count)> expect;
};

constexpr Expect R = Expect::Refuted;
constexpr Expect T = Expect::Tied;

// Readings in `Reading`'s order: shipped, lambda+own, lambda+lava, -54+own,
// near-surface only, lambda+16, anchor clears.
constexpr std::array<Arm, 5> kArms{{
    // Sea -70: lambda and lambda + 16 are -70 and -54, so lambda + 16 ties.
    {"lf_v5", {T, R, R, R, R, T, R}},
    {"lf_v5w", {T, R, R, R, R, T, R}},
    {"lf_dd", {T, R, R, R, R, T, R}},
    // Sea -60, every scan aborting at its anchor: the near-surface floor
    // alone, so the reading that changes only the cells off that path ties.
    {"lf_f60", {T, R, R, R, T, R, R}},
    // Sea 63: every floor reading's level is -54 (lambda + 16 is -38) and
    // the type is unseen; a low anchor that refused the sea is not.
    {"lf_s63", {T, T, T, T, T, R, R}},
}};

} // namespace

TEST_CASE("an aborted scan's floor is A_lava where the sea sits below the lava level",
          "[conformance][aquifer]") {
    const std::vector<std::filesystem::path> probes = corpora("lowfloor_s");
    if (probes.empty() || !std::filesystem::is_directory(fixtures() / "worldgen")) {
        SKIP("no lowfloor_s* aquifer probe under " << (fixtures() / "probes")
                                                   << "; generate them with " << kScript);
    }

    std::ostringstream report;
    for (const auto& dir : probes) {
        const std::int64_t seed = corpusSeed(dir);
        INFO("seed " << seed);
        test::requireFrozen(dir, kScript);

        // The rebuilt field against the server's own reading of it: the
        // readout dimension's terrain names the arm at every column the probe
        // pins (multiples of four, flat_cache's corners).
        {
            const Dimension v5(dir, "lf_v5", seed);
            const auto file = region::RegionFile::open(dir / "lfr" / "r.0.0.mca");
            long long columns = 0;
            long long agree = 0;
            long long low = 0;
            for (std::int32_t cz = 0; cz < 8; ++cz) {
                for (std::int32_t cx = 0; cx < 8; ++cx) {
                    REQUIRE(file.hasChunk(cx, cz));
                    const auto ch = chunk::Chunk::decode(nbt::read(file.readChunk(cx, cz)).root);
                    for (std::int32_t lz = 0; lz < 16; lz += 4) {
                        for (std::int32_t lx = 0; lx < 16; lx += 4) {
                            // The indicator's arms are -1 and +1: surfaces
                            // near 61 and 195.
                            const bool serverLow = ch.highestNonAir(lx, lz).value_or(-64) < 128;
                            const bool builtLow =
                                v5.read(settings::RouterEntry::PreliminarySurfaceLevel,
                                        (cx * 16) + lx, 0, (cz * 16) + lz) < -78.0;
                            ++columns;
                            agree += serverLow == builtLow ? 1 : 0;
                            low += serverLow ? 1 : 0;
                        }
                    }
                }
            }
            INFO("readout: " << agree << " of " << columns << " columns, " << low << " at -88");
            REQUIRE(columns == 32 * 32);
            REQUIRE(agree == columns);
            REQUIRE(low > 0);
            REQUIRE(low < columns);
        }

        for (const Arm& arm : kArms) {
            INFO("dimension " << arm.name);
            const Dimension dimension(dir, arm.name, seed);
            const std::filesystem::path region = dir / arm.name / "r.0.0.mca";
            REQUIRE(std::filesystem::is_regular_file(region));

            const FillScore fill = scoreFiller(region, dimension);
            const ArmScore readings = scoreReadings(region, dimension);
            report << "seed " << seed << " " << arm.name << ": filler " << fill.wrong
                   << " wrong of " << fill.blocks << " (moved " << fill.moved << "), marks "
                   << fill.marks << " (extra " << fill.marksExtra << ", missing "
                   << fill.marksMissing << "); readings on " << readings.blocks << " blocks ("
                   << readings.moved << " moved, " << readings.unexplained << " unexplained) and "
                   << readings.marks << " marks, floor lava " << readings.floorLava << "\n";
            for (std::size_t r = 0; r < kReadingNames.size(); ++r) {
                report << "    " << kReadingNames[r] << ": blocks wrong " << readings.wrongBlocks[r]
                       << ", marks wrong " << readings.wrongMarks[r] << ", differs from shipped "
                       << readings.differsFromShipped[r] << "\n";
            }
            INFO(report.str());

            // The shipped filler writes the server's block and marks on every
            // untouched chunk it fills; what fluid moved is shaped like flow,
            // and little (a bound, since it differs from run to run).
            REQUIRE(fill.untouched == 63);
            REQUIRE(fill.chunks == kFillerChunks);
            CHECK(fill.wrong == 0);
            CHECK(fill.marksExtra == 0);
            CHECK(fill.marksMissing == 0);
            CHECK(fill.moved * 100 < fill.blocks);

            REQUIRE(readings.chunks == 63);
            REQUIRE(readings.blocks > 10000);
            CHECK(readings.unexplained == 0);
            CHECK(readings.moved * 1000 < readings.blocks);
            for (std::size_t r = 0; r < kReadingNames.size(); ++r) {
                INFO("reading " << kReadingNames[r]);
                if (arm.expect[r] == Expect::Tied) {
                    // Model against model: exact whatever the world did.
                    CHECK(readings.differsFromShipped[r] == 0);
                    CHECK(readings.wrongBlocks[r] == readings.wrongBlocks[0]);
                    CHECK(readings.wrongMarks[r] == readings.wrongMarks[0]);
                } else {
                    CHECK(readings.wrongBlocks[r] >= kRefuted);
                }
            }
            // The shipped reading is the server's, block and mark.
            CHECK(readings.wrongBlocks[0] == 0);
            CHECK(readings.wrongMarks[0] == 0);
            if (std::string_view{arm.name} != "lf_s63") {
                // The floor holds lava the server shows, not merely a level.
                CHECK(readings.floorLava >= kRefuted);
            }
        }
    }
}
