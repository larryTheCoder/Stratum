// Stratum — Q5.8's lava override on the level rule's short-circuit seas.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// `fluidTypeOf` turns a source to lava when its level is at most -10 and
// `|lava| > 0.3`. Two of the level rule's outcomes are short-circuits that
// take `sea_level` from the surface rather than from the cell's own
// floodedness: the near-surface return (`LevelOrigin::NearSurfaceSea`) and an
// aborted scan's sea, taken by a cell more than twenty blocks above its
// surface. Neither can sit at or under -10 at the shipped sea, so no world
// had said whether the override reaches them.
//
// `tools/analysis/aquifer-fluidnear-probe.sh` builds worlds where nothing
// else holds fluid above lambda, with every router entry a constant: each
// lava arm puts one `lava` on every source, so R0 and R1 below disagree on
// every fluid block of a lava arm, and no block can tie. This case replays the
// substance decision against the blocks the server wrote (every second
// column inside the band where the type is decided, every fourth outside
// it), then sorts every fluid SOURCE the server holds in that band by the
// nearest source's class and scores five readings of the override on it:
//
//   R0  it reaches every short-circuit sea;
//   R1  it reaches none (the clean-room spec's Q5.3 read literally: both
//       short-circuits return the global picker's status);
//   R2  it reaches only a source centred more than twenty blocks above the
//       surface (Q5.3(a)'s class, which every aborted sea is in);
//   R3  it reaches only the others;
//   R4  it keys on the surface (psl <= -10) rather than on the level.
//
// The server takes R1, and the build now does (pipeline engine v6 and
// earlier built R0). Over seeds 42 and 31337 the two part on 742 856 server
// sources owned by a near-surface sea within twenty blocks of its surface,
// 155 533 owned by one higher up and 129 304 owned by an aborted scan's sea;
// the server holds water on every one, and the four rivals are each wrong
// on at least one whole class.
//
// ONE SEED PER CORPUS, read from its manifest; every `fluidnear_s*` corpus
// present is scored with its `fluidnearb_s*` twin, and the case SKIPs when
// there is none. The fixtures are Mojang-derived and never committed
// (SPEC §12).
#include "support/fluid_flow.hpp"
#include "support/probe_corpus.hpp"

#include <stratum/aquifer/fluid_type.hpp>
#include <stratum/aquifer/lattice.hpp>
#include <stratum/aquifer/sampling.hpp>
#include <stratum/aquifer/selection.hpp>
#include <stratum/aquifer/substance.hpp>
#include <stratum/chunk/chunk.hpp>
#include <stratum/javamath.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {

using namespace stratum;
using test::Category;

constexpr std::int32_t kChunks = 8;
constexpr std::int32_t kSpan = kChunks * 16;
/// Two blocks in from the force-loaded window's edge on every side, so that
/// a block's neighbours — which the flow classifier reads — were generated.
constexpr std::int32_t kEdgeMargin = 2;
/// Inside the band the type is decided in, every second column on each axis:
/// a quarter of the blocks, still hundreds of thousands per class.
constexpr std::int32_t kBandStride = 2;
/// Outside it, every fourth: there the replay only shows the arm is
/// otherwise the world it is meant to be.
constexpr std::int32_t kOuterStride = 4;

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

/// A router entry the probe declared, which this case requires to be a
/// constant: anything else is a world it cannot replay.
[[nodiscard]] double constantEntry(const nlohmann::json& router, const char* name) {
    INFO("router entry " << name);
    REQUIRE(router.at(name).is_number());
    return router.at(name).get<double>();
}

[[nodiscard]] Category categoryOf(const aquifer::Substance substance,
                                  const aquifer::FluidType type) {
    switch (substance) {
        case aquifer::Substance::Air:
            return Category::Air;
        case aquifer::Substance::Solid:
            return Category::Solid;
        case aquifer::Substance::Fluid:
            return type == aquifer::FluidType::Lava ? Category::Lava : Category::Water;
    }
    return Category::Solid;
}

/// Which outcome of the level rule the nearest source took, from the
/// branches' own documented conditions — spelled out here rather than read
/// from `LevelOrigin`, so the classes do not move if the build's origins do.
enum class SeaClass : std::uint8_t {
    /// The near-surface return, centred at most twenty above the surface.
    NearB,
    /// The near-surface return, centred more than twenty above it.
    NearA,
    /// An aborted scan's sea: centred at or above lambda and more than
    /// twenty above the scan's minimum.
    AbortedSea,
    /// Everything else: the cell's own floodedness, or a dry or floored level.
    Cell,
};
constexpr std::size_t kClasses = 4;
constexpr std::array<const char*, kClasses> kClassNames{"near<=+20", "near>+20", "aborted sea",
                                                        "cell"};

/// The five readings of the override on a short-circuit sea.
enum Reading : std::uint8_t { R0, R1, R2, R3, R4, kReadings };

constexpr std::array<const char*, kReadings> kReadingNames{"R0", "R1", "R2", "R3", "R4"};

/// The reading this build implements; the case requires it, and requires the
/// server to agree with it wherever the readings part.
constexpr Reading kBuilt = R1;

[[nodiscard]] bool onNearSurfacePath(const aquifer::CellFluid& cell) {
    const std::int32_t oceanGate = javamath::wrappingSub(cell.seaLevel, aquifer::kOceanGateOffset);
    return cell.surface.gate < oceanGate &&
           javamath::wrappingSub(cell.surface.gate, cell.centreY) < aquifer::kNearSurfaceDepth;
}

[[nodiscard]] SeaClass classify(const aquifer::CellFluid& cell) {
    const std::int32_t lambda = aquifer::lambdaLevel(cell.seaLevel);
    if (!onNearSurfacePath(cell)) {
        return SeaClass::Cell;
    }
    const std::int32_t twentyAbove =
        javamath::wrappingAdd(cell.surface.cap, aquifer::kNearSurfaceFloorOffset);
    if (!cell.surface.aborted) {
        return cell.centreY > twentyAbove ? SeaClass::NearA : SeaClass::NearB;
    }
    return (cell.centreY >= lambda && cell.centreY > twentyAbove) ? SeaClass::AbortedSea
                                                                  : SeaClass::Cell;
}

/// The type reading @p reading gives a source of class @p sea whose status the
/// build computed as @p built, in a world of constant @p lava and floored
/// surface @p surface.
[[nodiscard]] aquifer::FluidType readingType(const Reading reading, const SeaClass sea,
                                             const aquifer::SourceStatus& built, const double lava,
                                             const std::int32_t surface) {
    if (sea == SeaClass::Cell) {
        return built.type; // no reading here touches the cell's own outcomes
    }
    const bool pastThreshold = (lava < 0.0 ? -lava : lava) > aquifer::kLavaThreshold;
    const bool overridden = built.level != aquifer::kNeverLevel &&
                            built.level <= aquifer::kLavaLevelCeiling && pastThreshold;
    const auto lavaIf = [](bool yes) {
        return yes ? aquifer::FluidType::Lava : aquifer::FluidType::Default;
    };
    switch (reading) {
        case R0:
            return lavaIf(overridden);
        case R1:
            return aquifer::FluidType::Default;
        case R2:
            return lavaIf(overridden && sea != SeaClass::NearB);
        case R3:
            return lavaIf(overridden && sea == SeaClass::NearB);
        case R4:
            return lavaIf(surface <= aquifer::kLavaLevelCeiling && pastThreshold);
        case kReadings:
            break;
    }
    return built.type;
}

struct Tally {
    long long blocks = 0;
    long long agree = 0;
    long long flow = 0;
    long long unexplained = 0;
    std::string samples; ///< the first few unexplained blocks, for the failure

    /// In the band: fluid SOURCES the server holds where the build fills
    /// fluid, by the nearest source's class.
    std::array<long long, kClasses> scored{};
    /// ... and of those, where each reading types the source differently from
    /// the server's block, and how many of those are flow-shaped.
    std::array<std::array<long long, kReadings>, kClasses> wrong{};
    std::array<std::array<long long, kReadings>, kClasses> wrongFlow{};
    /// Where R0 and R1 part: the item's own question.
    std::array<long long, kClasses> contested{};
    /// Sources the build types differently from `kBuilt`'s reading.
    long long builtNotReading = 0;
    /// In the band: lava SOURCES the server holds, the build's lava, the
    /// build's lava the server shows as water's contact, and that contact.
    long long serverLava = 0;
    long long oursLava = 0;
    long long oursLavaQuenched = 0;
    long long contact = 0;
};

void add(Tally& into, const Tally& from) {
    into.blocks += from.blocks;
    into.agree += from.agree;
    into.flow += from.flow;
    into.unexplained += from.unexplained;
    for (std::size_t c = 0; c < kClasses; ++c) {
        into.scored[c] += from.scored[c];
        into.contested[c] += from.contested[c];
        for (std::size_t r = 0; r < kReadings; ++r) {
            into.wrong[c][r] += from.wrong[c][r];
            into.wrongFlow[c][r] += from.wrongFlow[c][r];
        }
    }
    into.builtNotReading += from.builtNotReading;
    into.serverLava += from.serverLava;
    into.oursLava += from.oursLava;
    into.oursLavaQuenched += from.oursLavaQuenched;
    into.contact += from.contact;
}

[[nodiscard]] std::string describe(const Tally& t) {
    std::string out = "blocks " + std::to_string(t.blocks) + ", agree " + std::to_string(t.agree) +
                      ", flow " + std::to_string(t.flow) + ", unexplained " +
                      std::to_string(t.unexplained) + "; band lava: server " +
                      std::to_string(t.serverLava) + ", ours " + std::to_string(t.oursLava) +
                      " (quenched " + std::to_string(t.oursLavaQuenched) + "), contact " +
                      std::to_string(t.contact) + ", built off its reading " +
                      std::to_string(t.builtNotReading);
    for (std::size_t c = 0; c < kClasses; ++c) {
        if (t.scored[c] == 0) {
            continue;
        }
        out += "; " + std::string(kClassNames[c]) + ": scored " + std::to_string(t.scored[c]) +
               ", R0/R1 contested " + std::to_string(t.contested[c]) + ", wrong";
        for (std::size_t r = 0; r < kReadings; ++r) {
            out += " " + std::string(kReadingNames[r]) + "=" + std::to_string(t.wrong[c][r]) +
                   "(flow " + std::to_string(t.wrongFlow[c][r]) + ")";
        }
    }
    return out;
}

/// Scores one dimension of one corpus.
Tally scoreDimension(const std::filesystem::path& probeDir, const nlohmann::json& entry,
                     const aquifer::CentreSource& centres) {
    const std::string name = entry.at("name").get<std::string>();
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
    const std::int32_t lambda = aquifer::lambdaLevel(seaLevel);
    const std::int32_t surface = javamath::floorToInt(psl);
    // Q2.5: above this the chunk never consults its aquifer. The surface is
    // constant, so every chunk's maximum is that constant.
    const std::int32_t ySkipLevel = aquifer::ySkip(surface);
    // The band the type is decided in: fluid the aquifer places, above the
    // global lava sea and under both the sea and y_skip.
    const std::int32_t bandTop = std::min(seaLevel - 1, ySkipLevel);
    // The scan itself, on a constant field: it aborts when the constant is
    // below the threshold, which ab20 is built to do.
    const aquifer::PslRead read =
        aquifer::readPreliminarySurface(constant(psl), aquifer::CellIndex{}, seaLevel);

    const std::filesystem::path region = probeDir / name / "r.0.0.mca";
    REQUIRE(std::filesystem::is_regular_file(region));
    test::GoldenRegion golden(region);
    aquifer::StatusCache statuses; // every input constant: one per world
    Tally tally;

    for (std::int32_t z = kEdgeMargin; z < kSpan - kEdgeMargin; z += kBandStride) {
        for (std::int32_t x = kEdgeMargin; x < kSpan - kEdgeMargin; x += kBandStride) {
            REQUIRE(golden.hasChunk(x / 16, z / 16));
            const bool outerColumn =
                (x - kEdgeMargin) % kOuterStride == 0 && (z - kEdgeMargin) % kOuterStride == 0;
            for (std::int32_t y = minY; y < minY + height; ++y) {
                const bool inBand = y >= lambda && y <= bandTop;
                if (!inBand && !outerColumn) {
                    continue;
                }
                const auto* theirs = golden.blockAt(x, y, z);
                const Category g = test::categoryOf(
                    theirs != nullptr ? theirs->name : std::string("minecraft:air"));
                Category r = Category::Air;
                if (y > ySkipLevel) {
                    // Q2.3: the global picker, as the filler does.
                    if (y < lambda) {
                        r = Category::Lava;
                    } else if (y < seaLevel) {
                        r = Category::Water;
                    }
                } else {
                    const aquifer::SubstanceAt ours = aquifer::computeSubstance(
                        centres,
                        aquifer::AquiferQuery{
                            .x = x, .y = y, .z = z, .density = density, .seaLevel = seaLevel},
                        statuses, constant(barrier), constant(floodedness), constant(spread),
                        constant(lava), constant(psl),
                        // Erosion and depth are 0 in a probe: Q5.9 cannot fire.
                        aquifer::NoDeepDark{});
                    r = categoryOf(ours.substance, ours.fluidType);
                }
                ++tally.blocks;
                if (g == r) {
                    ++tally.agree;
                } else if (test::explainedByFlow(golden, x, y, z, g, r)) {
                    ++tally.flow;
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
                if (!inBand) {
                    continue;
                }

                const bool serverSource = test::isFluid(g) && test::fluidLevel(theirs) == 0;
                const bool contact = test::fluidContactBlock(theirs);
                tally.serverLava += static_cast<long long>(serverSource && g == Category::Lava);
                tally.oursLava += static_cast<long long>(r == Category::Lava);
                tally.oursLavaQuenched += static_cast<long long>(r == Category::Lava && contact);
                tally.contact += static_cast<long long>(contact && test::isFluid(r));
                if (!serverSource || !test::isFluid(r)) {
                    continue;
                }

                // The nearest source's class and status, as the build sees it.
                const aquifer::Selection sel = aquifer::selectSources(centres, x, y, z);
                const aquifer::Source& nearest = sel.ranked[0];
                const aquifer::CellFluid cell{.centreY = nearest.centre.y,
                                              .surface = read,
                                              .seaLevel = seaLevel,
                                              .floodedness = floodedness,
                                              .spread = spread};
                const aquifer::SourceStatus built = aquifer::sourceStatus(cell, lava);
                const SeaClass sea = classify(cell);
                const auto c = static_cast<std::size_t>(sea);
                tally.builtNotReading += static_cast<long long>(
                    readingType(kBuilt, sea, built, lava, surface) != built.type);
                ++tally.scored[c];
                tally.contested[c] +=
                    static_cast<long long>(readingType(R0, sea, built, lava, surface) !=
                                           readingType(R1, sea, built, lava, surface));
                for (std::size_t k = 0; k < kReadings; ++k) {
                    const aquifer::FluidType type =
                        readingType(static_cast<Reading>(k), sea, built, lava, surface);
                    const Category claimed =
                        type == aquifer::FluidType::Lava ? Category::Lava : Category::Water;
                    if (claimed != g) {
                        ++tally.wrong[c][k];
                        tally.wrongFlow[c][k] += static_cast<long long>(
                            test::explainedByFlow(golden, x, y, z, g, claimed));
                    }
                }
            }
        }
    }
    return tally;
}

} // namespace

TEST_CASE("Q5.8's lava override on the near-surface and aborted seas", "[conformance][aquifer]") {
    const std::vector<std::filesystem::path> near = corpora("fluidnear_s");
    const std::vector<std::filesystem::path> low = corpora("fluidnearb_s");
    if (near.empty()) {
        SKIP("no fluidnear_s* aquifer probe under "
             << fixtures() / "probes"
             << "; generate one with tools/analysis/aquifer-fluidnear-probe.sh --accept-eula");
    }
    INFO("fluidnearb_s* corpora are written by the same script; regenerate with "
         "tools/analysis/aquifer-fluidnear-probe.sh");
    REQUIRE(low.size() == near.size());

    std::vector<std::filesystem::path> all = near;
    all.insert(all.end(), low.begin(), low.end());
    std::map<std::string, Tally> byArm; // pooled over seeds
    Tally overall;
    for (const auto& probe : all) {
        test::requireFrozen(probe, "tools/analysis/aquifer-fluidnear-probe.sh");
        std::ifstream manifestFile(probe / "manifest.json");
        const auto seed = nlohmann::json::parse(manifestFile).at("seed").get<std::int64_t>();
        std::ifstream specFile(probe / "spec.json");
        const nlohmann::json spec = nlohmann::json::parse(specFile);
        const aquifer::CentreSource centres{seed, stratum::density::RandomSource::Xoroshiro};
        std::size_t read = 0;
        for (const auto& entry : spec) {
            const std::string name = entry.at("name").get<std::string>();
            INFO("corpus " << probe.filename().string() << ", arm " << name);
            const Tally tally = scoreDimension(probe, entry, centres);
            INFO(describe(tally) << "; first unexplained:" << tally.samples);
            // Each arm is the world it is meant to be: the replay matches the
            // server on every block but the frozen world's flow remnant.
            CHECK(tally.unexplained == 0);
            CHECK(tally.agree > tally.blocks * 9 / 10);
            CHECK(tally.builtNotReading == 0);
            // The built reading is right on every server source the arm
            // scores, but where fluid moved.
            for (std::size_t c = 0; c < kClasses; ++c) {
                INFO("class " << kClassNames[c]);
                CHECK(tally.wrong[c][kBuilt] == tally.wrongFlow[c][kBuilt]);
            }
            add(byArm[name], tally);
            add(overall, tally);
            ++read;
        }
        // Every arm the probe declares, read: a missing one failed above.
        REQUIRE(read == spec.size());
    }
    INFO("overall: " << describe(overall));

    // The arms the case names, all present.
    for (const char* arm : {"nb20", "nb20z", "nb20n", "nb10", "nb9", "nab12", "nb40", "nb40z",
                            "ab20", "ab20z", "q20"}) {
        INFO("arm " << arm);
        REQUIRE(byArm.contains(arm));
    }

    // Live: each class of short-circuit sea is reached, on blocks where R0
    // and R1 type it differently — 742 856, 155 533 and 129 304 over seeds 42
    // and 31337 when first run.
    const auto nearB = static_cast<std::size_t>(SeaClass::NearB);
    const auto nearA = static_cast<std::size_t>(SeaClass::NearA);
    const auto aborted = static_cast<std::size_t>(SeaClass::AbortedSea);
    const auto perSeed = static_cast<long long>(near.size());
    REQUIRE(overall.contested[nearB] >= 250000 * perSeed);
    REQUIRE(overall.contested[nearA] >= 20000 * perSeed);
    REQUIRE(overall.contested[aborted] >= 20000 * perSeed);

    // The decision: on every class, the built reading is right on every
    // server source but the flow-shaped ones (none when first run), and those
    // few.
    for (std::size_t c = 0; c < kClasses; ++c) {
        INFO("class " << kClassNames[c]);
        CHECK(overall.wrong[c][kBuilt] == overall.wrongFlow[c][kBuilt]);
        CHECK(overall.wrong[c][kBuilt] * 100 <= overall.scored[c]);
    }
    // And every rival is wrong where it parts from the build — on the classes
    // that can separate it at all.
    for (std::size_t k = 0; k < kReadings; ++k) {
        if (k == kBuilt) {
            continue;
        }
        long long rivalWrong = 0;
        for (std::size_t c = 0; c < kClasses; ++c) {
            rivalWrong += overall.wrong[c][k] - overall.wrongFlow[c][k];
        }
        INFO("rival " << kReadingNames[k]);
        CHECK(rivalWrong >= 1000 * perSeed);
    }

    // Controls. nb9's sea sits above the ceiling over a surface below it: no
    // lava under any reading but R4's, which keys on the surface.
    CHECK(byArm.at("nb9").serverLava == 0);
    CHECK(byArm.at("nb9").oursLava == 0);
    // `lava` 0.0: nothing past the threshold, so no lava above lambda.
    for (const char* arm : {"nb20z", "nb40z", "ab20z"}) {
        INFO("arm " << arm);
        CHECK(byArm.at(arm).serverLava == 0);
        CHECK(byArm.at(arm).oursLava == 0);
    }
    // The cell's own sea at -20 is lava under every reading: the override is
    // in force on these worlds at all (241 896 sources when first run).
    CHECK(byArm.at("q20").serverLava > 100000 * perSeed);
    CHECK(byArm.at("q20").oursLava ==
          byArm.at("q20").serverLava + byArm.at("q20").oursLavaQuenched);
    // The absolute value, on this path too: -0.5 gives the verdict 0.5 does.
    CHECK((byArm.at("nb20n").serverLava > 0) == (byArm.at("nb20").serverLava > 0));

    // Flow is bounded, never pinned: water's contact with lava stays a small
    // fraction of the blocks the type is scored on.
    for (const auto& [arm, tally] : byArm) {
        long long scored = 0;
        for (const long long s : tally.scored) {
            scored += s;
        }
        INFO("arm " << arm << ": contact " << tally.contact << " of " << scored);
        CHECK(tally.contact * 100 <= scored);
    }
}
