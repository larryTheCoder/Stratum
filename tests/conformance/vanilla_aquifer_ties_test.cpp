// Stratum — the surface scan decides a source's status sample by sample.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Pipeline engine v12 reduced one scan of `preliminary_surface_level` to four
// values and carried three readings no probe had parted, because every field
// that reached them could not:
//
//   (a) the near-surface exemption of an aborting scan read the whole
//       window's minimum, `cap`; the rival was the aborting sample itself;
//   (b) Q5.3(a) for a scan that did not abort, off the ocean branch, read
//       `cap`; the clean-room spec's comparand is the anchor;
//   (c) the clean-room spec lets the FIRST submerged sample in scan order
//       decide (Q5.3(b)), where v12 read the abort flag and `cap`.
//
// `tools/analysis/aquifer-ties-probe.sh` builds the fields that part each
// pair (its header says which dimension parts which), with packed ice as the
// default fluid so that nothing the aquifer places can flow. The server took
// the clean-room spec's side on every one, and the library now reads Q5.3 as
// written (`aquifer::cellLevel`). aquifer-ties-rivals.hpp spells v12 as a
// rival, whole and one case at a time, and aquifer-ties-score.hpp scores
// each reading through `computeSubstanceWith`, the decision the filler runs,
// on the chunks the probe generated without ticking them
// (support/probe_region.hpp); the shipped filler runs end to end on five of
// them per dimension. About a minute for both seeds in a Debug build.
//
// The fixtures are Mojang-derived and never committed (SPEC §12).
#include "aquifer-ties-rivals.hpp"
#include "aquifer-ties-score.hpp"
#include "support/fluid_flow.hpp"
#include "support/probe_corpus.hpp"
#include "support/probe_region.hpp"
#include "support/probe_spec.hpp"

#include <stratum/aquifer/lattice.hpp>
#include <stratum/chunk/chunk.hpp>
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
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace stratum;
namespace ties = analysis::ties;

constexpr const char* kScript = "tools/analysis/aquifer-ties-probe.sh";

[[nodiscard]] std::filesystem::path fixtures() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11";
}

[[nodiscard]] std::vector<std::filesystem::path> corpora() {
    const std::filesystem::path root = fixtures() / "probes";
    std::vector<std::filesystem::path> found;
    if (std::filesystem::is_directory(root)) {
        for (const auto& entry : std::filesystem::directory_iterator(root)) {
            if (entry.is_directory() && entry.path().filename().string().rfind("ties_s", 0) == 0) {
                found.push_back(entry.path());
            }
        }
    }
    std::ranges::sort(found);
    return found;
}

/// The seed a corpus's name carries (`ties_s<seed>`), checked against the
/// manifest it was generated with.
[[nodiscard]] std::int64_t corpusSeed(const std::filesystem::path& dir) {
    const std::string name = dir.filename().string();
    const std::int64_t seed = std::stoll(name.substr(std::string_view{"ties_s"}.size()));
    test::requireSeed(dir, seed);
    return seed;
}

/// The manifest's probe noise is the one aquifer-ties-score.hpp replays.
void requireProbeNoise(const std::filesystem::path& dir) {
    const nlohmann::json manifest = nlohmann::json::parse(std::ifstream(dir / "manifest.json"));
    const nlohmann::json& declared = manifest.at("probe_noise");
    REQUIRE(declared.at("id").get<std::string>() == "stratum:probe_noise");
    REQUIRE(declared.at("first_octave") == ties::probeNoise().at("firstOctave"));
    REQUIRE(declared.at("amplitudes") == ties::probeNoise().at("amplitudes"));
}

/// What a dimension must show, reading by reading. The shipped reading is
/// always the server's, block and mark.
enum class Expect : std::uint8_t {
    Refuted, ///< parts from the shipped reading on scored blocks, and is wrong on every one
    Tied,    ///< the shipped reading's category at every position read
};

struct Arm {
    const char* name;
    /// Every `stride`-th column both ways.
    std::int32_t stride;
    /// Rows from `rowsFrom` to `rowsTo` above the sea (both inclusive),
    /// clipped to the rows the lattice decides; all of those when unset.
    std::optional<std::int32_t> rowsFrom;
    std::optional<std::int32_t> rowsTo;
    /// The fewest scored blocks each Refuted reading must part on, summed
    /// over both seeds.
    long long parts;
    std::array<Expect, ties::kReadingCount> expect;
};

constexpr Expect R = Expect::Refuted;
constexpr Expect T = Expect::Tied;
constexpr std::optional<std::int32_t> kAll;

// Readings in `Reading`'s order: shipped, engine v12, (a) v12's exemption on
// the aborting sample, then v12 on the abort-anchor / submerged-first /
// land-prefix / none-fires / land-anchor sources only. The shipped reading
// is asserted exact separately. Strides and rows: every other column of the
// rows the lattice decides where the readings part in bulk; every column of
// the lid rows, sea - 3 to sea + 8, where they part through the barrier's lid
// alone; and row lambda, every column, where the none-fires kind parts.
constexpr std::array<Arm, 12> kArms{{
    // (a): an anchor at -63 with -100 in its window. v12's exemption read
    // -100 and gave the sea; the anchor (and so the aborting sample, which
    // it is) gives A_lava below -43.
    {"ta", 2, kAll, kAll, 10000, {T, R, T, R, T, T, T, T}},
    {"tar", 2, kAll, kAll, 10000, {T, R, T, R, T, T, T, T}},
    // (c): -60 fires before -64 aborts, so the sea, where v12 gave A_lava.
    {"tb", 2, kAll, kAll, 10000, {T, R, R, T, R, T, T, T}},
    {"tbr", 2, kAll, kAll, 10000, {T, R, R, T, R, T, T, T}},
    // Both, with -100 after the -64 that aborts: the aborting sample's
    // exemption parts from the spec here as well.
    {"tab", 2, kAll, kAll, 1000, {T, R, R, R, R, T, T, T}},
    // (b): Q5.3(a) on the anchor, seen through the sea's lid.
    {"tl35", 1, -3, 8, 300, {T, R, T, T, T, T, T, R}},
    {"tl43", 1, -3, 8, 300, {T, R, T, T, T, T, T, R}},
    // Q5.3(a) on the anchor over an aborted scan's land prefix, the same
    // way, and in blocks at sea -70.
    {"tp35", 1, -3, 8, 300, {T, R, T, T, T, R, T, T}},
    {"tp43", 1, -3, 8, 300, {T, R, T, T, T, R, T, T}},
    {"tc", 2, kAll, kAll, 5000, {T, R, T, T, T, R, T, T}},
    {"tcr", 2, kAll, kAll, 5000, {T, R, T, T, T, R, T, T}},
    // No sample fires: the level rule, where v12 gave A_lava. Row lambda,
    // where every block the model parts lies, and only a few.
    {"td", 1, -117, -117, 10, {T, R, T, T, T, T, R, T}},
}};

/// The readout dimensions: the field as a step indicator, so terrain height
/// names the noise's range at every column the probe pins (flat_cache's
/// corners, multiples of four). density-probe.sh's surface sits where
/// 0.35 * F + g(y) = 0, which puts the steps -1, -1/3, 1/3 and 1 near 61,
/// 106, 150 and 195.
[[nodiscard]] int stepOfHeight(const int height) {
    constexpr std::array<int, 3> kCuts{83, 128, 173};
    return static_cast<int>(std::ranges::count_if(kCuts, [&](int cut) { return height > cut; }));
}

/// The shipped filler, end to end, on every thirteenth untouched chunk: the
/// server's category at every block but the ice world's row lambda (where
/// Q6.3 is measured for water alone), and the server's marks at every
/// position the filler puts a fluid the server marks (lava, or water in a
/// water world; packed ice holds no fluid to mark).
struct FillScore {
    int chunks = 0;
    long long blocks = 0;
    long long wrong = 0;
    long long moved = 0;
    long long marksExtra = 0;
    long long marksMissing = 0;
};

FillScore scoreFiller(const std::filesystem::path& region, const ties::Dimension& dimension) {
    constexpr int kEvery = 13;
    const settings::NoiseSettings& settings = dimension.settings();
    const std::int32_t minY = settings.geometry.minY;
    const std::int32_t topY = minY + settings.geometry.height - 1;
    const bool water = dimension.fluid() == "minecraft:water";
    const std::int32_t lambda = aquifer::lambdaLevel(settings.seaLevel);
    FillScore score;
    int untouched = 0;
    test::GoldenRegion golden(region);
    const auto file = region::RegionFile::open(region);
    for (std::int32_t cz = 0; cz < ties::kRegionChunks; ++cz) {
        for (std::int32_t cx = 0; cx < ties::kRegionChunks; ++cx) {
            if (!file.hasChunk(cx, cz)) {
                continue;
            }
            const auto doc = nbt::read(file.readChunk(cx, cz));
            if (!test::untouched(cx, cz, doc.root.at("Status").asString()) ||
                untouched++ % kEvery != 0) {
                continue;
            }
            ++score.chunks;
            const std::set<test::LocalPosition> marks = test::postProcessingMarks(doc.root, minY);
            terrain::ChunkBuffer buffer(settings.geometry);
            dimension.filler().fill(cx, cz, buffer);
            std::set<test::LocalPosition> ours;
            for (const auto& update : buffer.fluidUpdates()) {
                const std::string built =
                    buffer.at(update.localX, update.y, update.localZ).name.toString();
                if (water || built == "minecraft:lava") {
                    ours.emplace(update.localX, update.y, update.localZ);
                }
            }
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
                    if (ties::besideTicked(x, z)) {
                        continue;
                    }
                    for (std::int32_t y = minY; y <= topY; ++y) {
                        if (!water && y == lambda) {
                            continue;
                        }
                        const chunk::BlockState* block = golden.blockAt(x, y, z);
                        const std::optional<test::Category> served =
                            ties::servedOf(block, dimension.fluid());
                        // The filler writes the replay's water where the
                        // server wrote the dimension's default fluid.
                        const test::Category built =
                            test::categoryOf(buffer.at(lx, y, lz).name.toString());
                        ++score.blocks;
                        if (served.has_value()) {
                            score.wrong += *served == built ? 0 : 1;
                        } else if (test::explainedByFlow(golden, x, y, z,
                                                         test::categoryOf(block->name), built)) {
                            ++score.moved;
                        } else {
                            ++score.wrong;
                        }
                    }
                }
            }
        }
    }
    REQUIRE(untouched == 63);
    return score;
}

} // namespace

TEST_CASE("the surface scan decides a source's status sample by sample", "[conformance][aquifer]") {
    const std::vector<std::filesystem::path> probes = corpora();
    if (probes.empty() || !std::filesystem::is_directory(fixtures() / "worldgen")) {
        SKIP("no ties_s* aquifer probe under " << (fixtures() / "probes") << "; generate them with "
                                               << kScript);
    }
    std::ostringstream report;
    // Each Refuted reading's parting blocks, summed over the seeds.
    std::array<std::array<long long, ties::kReadingCount>, kArms.size()> parted{};
    for (const auto& dir : probes) {
        const std::int64_t seed = corpusSeed(dir);
        INFO("corpus " << dir.filename().string());
        test::requireFrozen(dir, kScript);
        requireProbeNoise(dir);
        const nlohmann::json spec = test::readSpec(dir);

        // The rebuilt field against the server's own reading of it, at both
        // scales and every cut: the readout's terrain names the range at
        // every pinned column, and an aquifer dimension's surface, rebuilt,
        // agrees. tab's arms -100 / -64 / -60 are tr's -1 / -1/3 or 1/3 / 1;
        // ta's -100 / -63 split it at 0; tl35's 35 / 47 are trs's -1 / 1.
        struct Readout {
            const char* readout;
            const char* aquifer;
            std::size_t steps;
            bool (*agrees)(double psl, int step);
        };

        constexpr std::array<Readout, 3> kReadouts{{
            {"tr", "tab", 4,
             [](double psl, int step) {
                 return (psl < -90.0) == (step == 0) && (psl > -62.0) == (step == 3);
             }},
            {"tr", "ta", 4, [](double psl, int step) { return (psl < -90.0) == (step <= 1); }},
            {"trs", "tl35", 2, [](double psl, int step) { return (psl < 40.0) == (step == 0); }},
        }};
        for (const Readout& check : kReadouts) {
            INFO("readout " << check.readout << " against " << check.aquifer);
            const ties::Dimension field(test::specEntry(spec, dir, check.aquifer),
                                        ties::probeNoise(), fixtures() / "worldgen", seed);
            const auto file = region::RegionFile::open(dir / check.readout / "r.0.0.mca");
            long long columns = 0;
            long long agree = 0;
            std::set<int> steps;
            for (std::int32_t cz = 0; cz < 8; ++cz) {
                for (std::int32_t cx = 0; cx < 8; ++cx) {
                    REQUIRE(file.hasChunk(cx, cz));
                    const auto ch = chunk::Chunk::decode(nbt::read(file.readChunk(cx, cz)).root);
                    for (std::int32_t lz = 0; lz < 16; lz += 4) {
                        for (std::int32_t lx = 0; lx < 16; lx += 4) {
                            const int step = stepOfHeight(ch.highestNonAir(lx, lz).value_or(-64));
                            const double psl =
                                field.read(settings::RouterEntry::PreliminarySurfaceLevel,
                                           (cx * 16) + lx, 0, (cz * 16) + lz);
                            ++columns;
                            agree += check.agrees(psl, step) ? 1 : 0;
                            steps.insert(step);
                        }
                    }
                }
            }
            INFO("readout: " << agree << " of " << columns << " columns");
            REQUIRE(columns == 32 * 32);
            CHECK(agree == columns);
            CHECK(steps.size() == check.steps);
        }

        for (std::size_t a = 0; a < kArms.size(); ++a) {
            const Arm& arm = kArms.at(a);
            INFO("dimension " << arm.name);
            const nlohmann::json entry = test::specEntry(spec, dir, arm.name);
            const ties::Dimension dimension(entry, ties::probeNoise(), fixtures() / "worldgen",
                                            seed);
            const std::filesystem::path region = dir / arm.name / "r.0.0.mca";
            REQUIRE(std::filesystem::is_regular_file(region));
            const std::int32_t sea = dimension.settings().seaLevel;
            const ties::Score score = ties::scoreReadings(
                dimension, region, arm.stride,
                arm.rowsFrom.has_value() ? sea + *arm.rowsFrom
                                         : std::numeric_limits<std::int32_t>::min(),
                arm.rowsTo.has_value() ? sea + *arm.rowsTo
                                       : std::numeric_limits<std::int32_t>::max());
            const FillScore fill = scoreFiller(region, dimension);
            report << "seed " << seed << " " << arm.name << ": " << score.blocks
                   << " blocks scored (moved " << score.moved << "), marks " << score.marks
                   << ", drift " << score.drift << "; filler " << fill.wrong << " wrong of "
                   << fill.blocks << " (moved " << fill.moved << "), marks extra "
                   << fill.marksExtra << " missing " << fill.marksMissing << "\n";
            for (std::size_t r = 0; r < ties::kReadingCount; ++r) {
                report << "    " << ties::kReadingNames.at(r) << ": parts on "
                       << score.differsScored.at(r) << ", wrong " << score.wrongBlocks.at(r)
                       << ", marks wrong " << score.wrongMarks.at(r) << "\n";
            }
            INFO(report.str());

            // The readings are the library's, and the library is the
            // server's, block and mark, by the scorer and end to end.
            REQUIRE(score.chunks == 63);
            CHECK(score.drift == 0);
            CHECK(score.unexplained == 0);
            CHECK(score.moved * 1000 <= score.blocks);
            CHECK(score.wrongBlocks[0] == 0);
            CHECK(score.wrongMarks[0] == 0);
            CHECK(fill.chunks == 5);
            CHECK(fill.wrong == 0);
            CHECK(fill.marksExtra == 0);
            CHECK(fill.marksMissing == 0);
            CHECK(fill.moved * 1000 <= fill.blocks);
            for (std::size_t r = 1; r < ties::kReadingCount; ++r) {
                INFO("reading " << ties::kReadingNames.at(r));
                if (arm.expect.at(r) == Expect::Tied) {
                    // Model against model: exact whatever the world did.
                    CHECK(score.differsFromShipped.at(r) == 0);
                } else {
                    // Wherever it parts, the server holds the shipped block.
                    CHECK(score.wrongBlocks.at(r) == score.differsScored.at(r));
                    parted.at(a).at(r) += score.differsScored.at(r);
                }
            }
        }
    }
    INFO(report.str());
    for (std::size_t a = 0; a < kArms.size(); ++a) {
        for (std::size_t r = 1; r < ties::kReadingCount; ++r) {
            if (kArms.at(a).expect.at(r) == Expect::Refuted) {
                INFO("dimension " << kArms.at(a).name << ", reading " << ties::kReadingNames.at(r));
                CHECK(parted.at(a).at(r) >= kArms.at(a).parts);
            }
        }
    }
}
