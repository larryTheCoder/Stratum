// Stratum — how the surface pass counts aquifer lava, read off the server.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The surface pass reads three things off the blocks the first pass left: a
// stone-depth run counted top down (`stone_depth` floor), the same counted
// bottom up (`stone_depth` ceiling), and the column's water height (`water`).
// In a dimension whose `default_fluid` is water the aquifer also writes
// literal `minecraft:lava`, and nothing had measured how any of the three
// treats it: every aquifer probe ran a surface rule that never fires, and in
// the shipped presets every run-reading condition sits under
// `above_preliminary_surface`, far above the goldens' lava. The filler had
// lava counting like stone, by elimination (not air, not `default_fluid`).
//
// `tools/analysis/aquifer-lavarun-probe.sh` builds the world that asks:
// flat terrain depending on y alone, an aquifer whose every source sits at
// -12 (lava, or water in the twin dimensions), and a ladder of sixteen
// wool-placing rungs per run, so every stone block shows its own 0-based
// depth, or how far below the water height it sits. Each kind of fluid —
// lava the lattice placed, the global lava sea below lambda, water — is a
// factor of its own, with three readings for a run (counts like stone,
// holds like documented water, resets like air) and two for the height
// (latches or not). Every reading of every factor is separated on whole
// columns by some dimension, and the dimensions of one ladder together leave
// exactly one reading standing.
//
// WHAT THE SERVER DOES, on 16 384 columns per dimension, every one exact:
//
//   * TOP DOWN, every fluid HOLDS the run: lava of either origin exactly as
//     water. The filler's lava counted, which every lava dimension refutes
//     outright (lava_floor: 16 of 24 discriminating blocks a column wrong).
//   * BOTTOM UP, every fluid RESETS the run, water included. That corrects
//     surface::Context's own doc, which called this run "the same counting
//     up" — so the filler's bottom-up run skipped water as the top-down one
//     does, and was wrong for every fluid, not only lava. The water twins,
//     meant as the positive control for the documented behaviour, are what
//     show it: water_ceiling's stone above an enclosed pool reads depth 0..7,
//     where a held run would leave it stone.
//   * Lava LATCHES the water height exactly as water does, the sea's lava
//     too: in a column whose only fluid is lava, `water` measures from one
//     above the topmost lava block.
//
// The lattice's lava and the sea's take the same reading in every respect,
// as the block alone says they should: the server does not tell them apart.
//
// The second case runs the shipped filler over the same spec and scores it
// block name for block name against the server, flow bounded. The filler
// before this measurement wrote 2 392 064 of the 74 973 184 blocks wrong,
// every one a marker, in nine of the twelve dimensions; now none. The
// corpus froze with no flow at all, as the script's layout predicts.
//
// The whole corpus is scored, every block of 8x8 chunks in twelve
// dimensions: about a minute for the first case and four and a half for the
// second in a Debug build, almost all of that the filler itself.
//
// The fixtures are Mojang-derived and never committed (SPEC §12); without the
// corpus this SKIPs, naming the script that produces it.
#include "support/fluid_flow.hpp"
#include "support/probe_corpus.hpp"
#include "support/probe_settings.hpp"
#include "support/probe_spec.hpp"
#include "support/temp_path.hpp"

#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/noise_registry.hpp>
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
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using namespace stratum;
using test::Category;

constexpr std::int32_t kChunks = 8;
constexpr std::int64_t kSeed = 42;
constexpr long long kColumns = 16LL * 16LL * kChunks * kChunks;
/// min(-54, sea_level -16): every fluid position below it is the global
/// lava sea (Q2.4), which the lattice never decides.
constexpr std::int32_t kLambda = -54;
constexpr std::string_view kScript = "tools/analysis/aquifer-lavarun-probe.sh";

[[nodiscard]] std::filesystem::path probeDir() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11" / "probes" / "lavarun_s42";
}

constexpr std::array<std::string_view, 16> kWool{
    "white",      "orange", "magenta", "light_blue", "yellow", "lime",  "pink", "gray",
    "light_gray", "cyan",   "purple",  "blue",       "brown",  "green", "red",  "black"};

constexpr int kNoMarker = -1; // the default block: no rung fired
constexpr int kForeign = -2;  // anything else

/// Which rung of a ladder painted this block. Called on every solid block of
/// twelve dimensions, so the sixteen names are built once.
[[nodiscard]] int markerOf(const std::string& name) {
    static const std::array<std::string, 16> kNames = [] {
        std::array<std::string, 16> names;
        for (std::size_t k = 0; k < kWool.size(); ++k) {
            names.at(k) = "minecraft:" + std::string(kWool.at(k)) + "_wool";
        }
        return names;
    }();
    if (name == "minecraft:stone") {
        return kNoMarker;
    }
    if (!name.ends_with("_wool")) {
        return kForeign;
    }
    for (std::size_t k = 0; k < kNames.size(); ++k) {
        if (name == kNames.at(k)) {
            return static_cast<int>(k);
        }
    }
    return kForeign;
}

/// `id` spelled `namespace:path` is @p name — without building the string,
/// which the second case would otherwise do for every one of 75 million
/// blocks.
[[nodiscard]] bool namedAs(const data::ResourceLocation& id, const std::string& name) {
    const std::string& space = id.namespaceName();
    const std::string& path = id.path();
    return name.size() == space.size() + 1 + path.size() &&
           name.compare(0, space.size(), space) == 0 && name[space.size()] == ':' &&
           name.compare(space.size() + 1, path.size(), path) == 0;
}

/// What the first pass left at a position the server shows @p name at. A
/// marker replaces only the default block (SPEC §11), so wool is solid.
[[nodiscard]] Category layoutCategoryOf(const std::string& name) {
    return markerOf(name) == kForeign ? test::categoryOf(name) : Category::Solid;
}

[[nodiscard]] Category categoryNamed(const std::string& name) {
    if (name == "solid") {
        return Category::Solid;
    }
    if (name == "air") {
        return Category::Air;
    }
    if (name == "lava") {
        return Category::Lava;
    }
    if (name == "water") {
        return Category::Water;
    }
    throw std::runtime_error("expected_layout names an unknown category: " + name);
}

/// A column's worth of something, indexed by y over one dimension's extent.
template<typename T>
class ByY {
public:
    ByY(std::int32_t minY, std::int32_t height, T fill)
        : minY_(minY), values_(static_cast<std::size_t>(height), fill) {}

    [[nodiscard]] std::int32_t minY() const { return minY_; }

    [[nodiscard]] std::int32_t topY() const {
        return minY_ + static_cast<std::int32_t>(values_.size()) - 1;
    }

    [[nodiscard]] T& operator[](std::int32_t y) {
        return values_.at(static_cast<std::size_t>(y - minY_));
    }

    [[nodiscard]] const T& operator[](std::int32_t y) const {
        return values_.at(static_cast<std::size_t>(y - minY_));
    }

private:
    std::int32_t minY_;
    std::vector<T> values_;
};

using Layout = ByY<Category>;
using Markers = ByY<int>;

/// The script's own `expected_layout`: [top, bottom, kind] runs, top down,
/// covering the entry's world exactly once.
[[nodiscard]] Layout expectedLayout(const nlohmann::json& entry) {
    const auto minY = entry.at("min_y").get<std::int32_t>();
    const auto height = entry.at("height").get<std::int32_t>();
    Layout layout(minY, height, Category::Air);
    std::set<std::int32_t> seen;
    for (const auto& run : entry.at("expected_layout")) {
        const auto top = run.at(0).get<std::int32_t>();
        const auto bottom = run.at(1).get<std::int32_t>();
        const Category category = categoryNamed(run.at(2).get<std::string>());
        for (std::int32_t y = bottom; y <= top; ++y) {
            layout[y] = category;
            seen.insert(y);
        }
    }
    if (seen.size() != static_cast<std::size_t>(height)) {
        throw std::runtime_error("expected_layout does not cover the world exactly once");
    }
    return layout;
}

// ------------------------------------------------------------- the readings

/// What one kind of fluid does to a stone-depth run.
enum class Effect : std::uint8_t {
    Count, // like the default block: the run goes on
    Hold,  // like water top down: neither counts nor breaks it
    Reset, // like air: the run starts again past it
};
constexpr std::array<Effect, 3> kEffects{Effect::Count, Effect::Hold, Effect::Reset};

[[nodiscard]] std::string_view nameOf(Effect effect) {
    switch (effect) {
        case Effect::Count:
            return "count";
        case Effect::Hold:
            return "hold";
        case Effect::Reset:
            return "reset";
    }
    return "?";
}

/// The three kinds of fluid the first pass leaves, told apart: lava the
/// lattice placed, lava of the global sea below lambda — the same block, so
/// only a server that does not read the block could treat them apart, which
/// is why they are scored apart — and water.
enum class Fluid : std::uint8_t { None, LatticeLava, SeaLava, Water };

[[nodiscard]] Fluid fluidAt(Category category, std::int32_t y) {
    if (category == Category::Water) {
        return Fluid::Water;
    }
    if (category == Category::Lava) {
        return y < kLambda ? Fluid::SeaLava : Fluid::LatticeLava;
    }
    return Fluid::None;
}

/// One reading of a run: an Effect per kind of fluid.
struct RunReading {
    Effect latticeLava = Effect::Count;
    Effect seaLava = Effect::Count;
    Effect water = Effect::Count;

    [[nodiscard]] Effect of(Fluid fluid) const {
        if (fluid == Fluid::Water) {
            return water;
        }
        return fluid == Fluid::SeaLava ? seaLava : latticeLava;
    }

    [[nodiscard]] std::string label() const {
        return "lattice lava " + std::string(nameOf(latticeLava)) + ", sea lava " +
               std::string(nameOf(seaLava)) + ", water " + std::string(nameOf(water));
    }

    [[nodiscard]] bool operator==(const RunReading&) const = default;
};

/// One reading of the water height: which kinds of fluid latch it.
struct LatchReading {
    bool latticeLava = false;
    bool seaLava = false;
    bool water = false;

    [[nodiscard]] bool latches(Fluid fluid) const {
        if (fluid == Fluid::Water) {
            return water;
        }
        return fluid == Fluid::SeaLava ? seaLava : latticeLava;
    }

    [[nodiscard]] std::string label() const {
        const auto says = [](bool latches) { return latches ? "latches" : "does not"; };
        return std::string("lattice lava ") + says(latticeLava) + ", sea lava " + says(seaLava) +
               ", water " + says(water);
    }

    [[nodiscard]] bool operator==(const LatchReading&) const = default;
};

[[nodiscard]] std::vector<RunReading> allRunReadings() {
    std::vector<RunReading> readings;
    for (const Effect lattice : kEffects) {
        for (const Effect sea : kEffects) {
            for (const Effect water : kEffects) {
                readings.push_back(
                    RunReading{.latticeLava = lattice, .seaLava = sea, .water = water});
            }
        }
    }
    return readings;
}

[[nodiscard]] std::vector<LatchReading> allLatchReadings() {
    std::vector<LatchReading> readings;
    for (const bool lattice : {false, true}) {
        for (const bool sea : {false, true}) {
            for (const bool water : {false, true}) {
                readings.push_back(
                    LatchReading{.latticeLava = lattice, .seaLava = sea, .water = water});
            }
        }
    }
    return readings;
}

enum class Ladder : std::uint8_t { Floor, Ceiling, Water };

[[nodiscard]] Ladder ladderNamed(const std::string& name) {
    if (name == "floor") {
        return Ladder::Floor;
    }
    if (name == "ceiling") {
        return Ladder::Ceiling;
    }
    if (name == "water") {
        return Ladder::Water;
    }
    throw std::runtime_error("spec entry names an unknown ladder: " + name);
}

/// A stone-depth ladder's marker: the 0-based depth, for depths 0..15.
[[nodiscard]] int rungFor(std::int32_t run) {
    return run <= 16 ? run - 1 : kNoMarker;
}

/// The marker every solid position of @p layout shows under @p reading, for
/// a floor ladder (the run counted top down) or a ceiling one (bottom up).
[[nodiscard]] Markers predictRun(const Layout& layout, Ladder ladder, const RunReading& reading) {
    Markers markers(layout.minY(), layout.topY() - layout.minY() + 1, kNoMarker);
    std::int32_t run = 0;
    const auto step = [&](std::int32_t y) {
        const Category category = layout[y];
        if (category == Category::Solid) {
            ++run;
            markers[y] = rungFor(run);
            return;
        }
        if (category == Category::Air) {
            run = 0;
            return;
        }
        switch (reading.of(fluidAt(category, y))) {
            case Effect::Count:
                ++run;
                break;
            case Effect::Hold:
                break;
            case Effect::Reset:
                run = 0;
                break;
        }
    };
    if (ladder == Ladder::Floor) {
        for (std::int32_t y = layout.topY(); y >= layout.minY(); --y) {
            step(y);
        }
    } else {
        for (std::int32_t y = layout.minY(); y <= layout.topY(); ++y) {
            step(y);
        }
    }
    return markers;
}

/// The water ladder's marker under @p reading: the height latches one above
/// the first latching fluid met descending; a block shows max(0, height - y)
/// while that is at most 15, and 0 wherever the column has no height.
[[nodiscard]] Markers predictWater(const Layout& layout, const LatchReading& reading) {
    std::optional<std::int32_t> height;
    for (std::int32_t y = layout.topY(); y >= layout.minY(); --y) {
        const Fluid fluid = fluidAt(layout[y], y);
        if (fluid != Fluid::None && reading.latches(fluid)) {
            height = y + 1;
            break;
        }
    }
    Markers markers(layout.minY(), layout.topY() - layout.minY() + 1, kNoMarker);
    for (std::int32_t y = layout.minY(); y <= layout.topY(); ++y) {
        if (layout[y] != Category::Solid) {
            continue;
        }
        const std::int32_t below = height.has_value() ? *height - y : 0;
        if (below <= 0) {
            markers[y] = 0;
        } else if (below <= 15) {
            markers[y] = below;
        }
    }
    return markers;
}

// --------------------------------------------------------- reading the world

[[nodiscard]] std::optional<nlohmann::json> loadSpec() {
    if (!std::filesystem::exists(probeDir() / "spec.json")) {
        return std::nullopt;
    }
    return test::readSpec(probeDir());
}

/// One dimension, scored: its layout against the script's, and the marker
/// every reading predicts against the server's.
struct DimensionScore {
    long long columns = 0;     // scored
    long long flowColumns = 0; // dropped: the layout differs only as flow does
    long long flowBlocks = 0;
    long long unexplained = 0; // layout differences flow does not explain
    long long foreign = 0;     // a solid position holding neither stone nor wool
    long long controlTotal = 0;
    long long controlAgree = 0;
    long long controlWool = 0; // control positions where every reading predicts a marker
    long long discriminatingTotal = 0;
    std::vector<long long> agree; // per reading, on the discriminating positions
};

/// Scores @p dimension against @p predictions (one Markers per reading,
/// computed from the expected layout). A column is scored only where the
/// server's layout is exactly the script's: the markers were written before
/// any fluid moved, so a column flow has touched is dropped whole rather
/// than read against a layout the surface pass never saw.
[[nodiscard]] DimensionScore scoreDimension(const std::string& dimension, const Layout& expected,
                                            const std::vector<Markers>& predictions) {
    DimensionScore score;
    score.agree.assign(predictions.size(), 0);
    const std::int32_t minY = expected.minY();
    const std::int32_t topY = expected.topY();
    // Discriminating: a solid position where two readings differ.
    // (`std::uint8_t`, not `bool`: `std::vector<bool>` hands out no references.)
    ByY<std::uint8_t> discriminating(minY, topY - minY + 1, 0U);
    for (std::int32_t y = minY; y <= topY; ++y) {
        for (const Markers& markers : predictions) {
            if (markers[y] != predictions.front()[y]) {
                discriminating[y] = 1U;
            }
        }
    }
    test::GoldenRegion region(probeDir() / dimension / "r.0.0.mca");
    // Pointers into the region's own decoded chunks, which it keeps.
    static const std::string kAir = "minecraft:air";
    ByY<const std::string*> names(minY, topY - minY + 1, &kAir);
    for (std::int32_t chunkZ = 0; chunkZ < kChunks; ++chunkZ) {
        for (std::int32_t chunkX = 0; chunkX < kChunks; ++chunkX) {
            INFO("chunk " << chunkX << ", " << chunkZ);
            REQUIRE(region.hasChunk(chunkX, chunkZ));
            const chunk::Chunk& decoded = region.chunk(chunkX, chunkZ);
            for (int localZ = 0; localZ < 16; ++localZ) {
                for (int localX = 0; localX < 16; ++localX) {
                    const std::int32_t x = (chunkX * 16) + localX;
                    const std::int32_t z = (chunkZ * 16) + localZ;
                    long long flow = 0;
                    long long wrong = 0;
                    for (std::int32_t y = minY; y <= topY; ++y) {
                        const chunk::BlockState* block = decoded.blockAt(localX, y, localZ);
                        names[y] = block == nullptr ? &kAir : &block->name;
                        const Category served = layoutCategoryOf(*names[y]);
                        if (served == expected[y]) {
                            continue;
                        }
                        if (test::explainedByFlow(region, x, y, z, served, expected[y])) {
                            ++flow;
                        } else {
                            ++wrong;
                            if (score.unexplained + wrong <= 20) {
                                UNSCOPED_INFO(dimension << " (" << x << ", " << y << ", " << z
                                                        << ") holds " << *names[y]);
                            }
                        }
                    }
                    if (wrong > 0) {
                        score.unexplained += wrong;
                        continue;
                    }
                    if (flow > 0) {
                        ++score.flowColumns;
                        score.flowBlocks += flow;
                        continue;
                    }
                    ++score.columns;
                    for (std::int32_t y = minY; y <= topY; ++y) {
                        if (expected[y] != Category::Solid) {
                            continue;
                        }
                        const int served = markerOf(*names[y]);
                        if (served == kForeign) {
                            ++score.foreign;
                            continue;
                        }
                        if (discriminating[y] == 0U) {
                            ++score.controlTotal;
                            score.controlAgree += served == predictions.front()[y] ? 1 : 0;
                            score.controlWool += predictions.front()[y] >= 0 ? 1 : 0;
                            continue;
                        }
                        ++score.discriminatingTotal;
                        for (std::size_t r = 0; r < predictions.size(); ++r) {
                            score.agree[r] += served == predictions[r][y] ? 1 : 0;
                        }
                    }
                }
            }
        }
    }
    return score;
}

/// What the script's layout implies per column, worked by hand from the
/// layouts in its header: how many solid positions separate some pair of
/// readings, how many of the rest show a marker, and — pinned after the run
/// — how many readings this one dimension leaves exact, which is the readings
/// of the factors it cannot see times those it cannot separate. The case
/// recomputes all three from the layout and the server, and fails if the
/// world the script wrote has less power than this.
struct Power {
    long long discriminating;
    long long controlWool;
    std::size_t exactReadings;
};

const std::map<std::string, Power, std::less<>> kPower{
    // Top down. lava_floor and water_floor separate all three readings of
    // their own fluid (the sea, at the world's floor, is past every stone);
    // sea_open separates counting from the other two, sea_roof resetting.
    {"lava_floor", {24, 16, 9}},
    {"water_floor", {24, 16, 9}},
    {"sea_open", {4, 16, 18}},
    {"sea_roof", {4, 16, 18}},
    // Bottom up. The G1 pair cannot separate holding from resetting for the
    // sea at the world's floor (the run starts at zero either way); the
    // pockets and sea_ceiling separate all three.
    {"lava_ceiling", {24, 16, 6}},
    {"water_ceiling", {24, 16, 6}},
    {"lava_pocket_ceiling", {16, 12, 9}},
    {"water_pocket_ceiling", {16, 12, 9}},
    {"sea_ceiling", {16, 4, 9}},
    // The height. Every stone above the topmost fluid shows 0 whatever
    // latches; each dimension separates its own fluid's two readings.
    {"lava_water", {26, 316, 4}},
    {"water_water", {26, 316, 4}},
    {"sea_water", {4, 369, 4}},
};

/// A worldgen tree of the probe's own noise settings, removed on exit.
class ScratchTree {
public:
    ScratchTree() : path_(test::tempPath("stratum-lavarun")) {
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

TEST_CASE("the stone-depth run and the water height through aquifer lava, read off the server",
          "[conformance][aquifer][surface]") {
    const std::optional<nlohmann::json> spec = loadSpec();
    if (!spec.has_value()) {
        SKIP("no lava-run probe under " << probeDir() << "; generate it with " << kScript
                                        << " --accept-eula 42");
    }
    test::requireFrozen(probeDir(), kScript);
    test::requireSeed(probeDir(), kSeed);

    const std::vector<RunReading> runReadings = allRunReadings();
    const std::vector<LatchReading> latchReadings = allLatchReadings();
    // Per ladder: the readings every dimension of that ladder leaves exact.
    std::map<Ladder, std::set<std::size_t>> survivors;
    long long flowColumns = 0;
    long long flowBlocks = 0;
    REQUIRE(spec->size() == kPower.size());
    for (const auto& entry : *spec) {
        const std::string name = entry.at("name").get<std::string>();
        INFO("dimension " << name);
        REQUIRE(kPower.count(name) == 1U);
        const Ladder ladder = ladderNamed(entry.at("ladder").get<std::string>());
        const Layout expected = expectedLayout(entry);
        std::vector<Markers> predictions;
        std::vector<std::string> labels;
        if (ladder == Ladder::Water) {
            for (const LatchReading& reading : latchReadings) {
                predictions.push_back(predictWater(expected, reading));
                labels.push_back(reading.label());
            }
        } else {
            for (const RunReading& reading : runReadings) {
                predictions.push_back(predictRun(expected, ladder, reading));
                labels.push_back(reading.label());
            }
        }
        const DimensionScore score = scoreDimension(name, expected, predictions);
        flowColumns += score.flowColumns;
        flowBlocks += score.flowBlocks;
        INFO("scored " << score.columns << " columns, " << score.flowColumns << " dropped for "
                       << score.flowBlocks << " blocks of flow; control " << score.controlAgree
                       << " / " << score.controlTotal << ", discriminating "
                       << score.discriminatingTotal);
        CHECK(score.unexplained == 0);
        CHECK(score.foreign == 0);
        // Flow drops whole columns and is bounded, never pinned (SPEC §7).
        CHECK(score.flowColumns * 100 <= kColumns);
        CHECK(score.columns + score.flowColumns == kColumns);
        // (a) The control is exact: where every reading agrees the server
        // does too. A missing marker, or a cutoff on where the rules apply,
        // would show here before any reading is scored.
        CHECK(score.controlAgree == score.controlTotal);
        // (b) The power the layout promises, per scored column.
        const Power& power = kPower.at(name);
        CHECK(score.discriminatingTotal == power.discriminating * score.columns);
        CHECK(score.controlWool == power.controlWool * score.columns);
        std::set<std::size_t> exact;
        for (std::size_t r = 0; r < predictions.size(); ++r) {
            UNSCOPED_INFO(name << ": " << labels[r] << " — " << score.agree[r] << " / "
                               << score.discriminatingTotal);
            if (score.agree[r] == score.discriminatingTotal) {
                exact.insert(r);
            }
        }
        CHECK(exact.size() == power.exactReadings);
        if (survivors.count(ladder) == 0U) {
            survivors[ladder] = exact;
        } else {
            std::set<std::size_t> both;
            for (const std::size_t r : exact) {
                if (survivors[ladder].count(r) != 0U) {
                    both.insert(r);
                }
            }
            survivors[ladder] = both;
        }
    }
    INFO("flow: " << flowColumns << " columns, " << flowBlocks << " blocks");
    // (c) Each ladder's dimensions together leave exactly one reading.
    REQUIRE(survivors[Ladder::Floor].size() == 1U);
    REQUIRE(survivors[Ladder::Ceiling].size() == 1U);
    REQUIRE(survivors[Ladder::Water].size() == 1U);
    const RunReading floor = runReadings[*survivors[Ladder::Floor].begin()];
    const RunReading ceiling = runReadings[*survivors[Ladder::Ceiling].begin()];
    const LatchReading latch = latchReadings[*survivors[Ladder::Water].begin()];
    INFO("floor: " << floor.label() << "; ceiling: " << ceiling.label()
                   << "; water height: " << latch.label());
    // (d) The documented water behaviour, re-measured in the same run: water
    // holds the top-down run and latches the height (surface::Context).
    CHECK(floor.water == Effect::Hold);
    CHECK(latch.water);
    // (e) What the server does, pinned: lava is a fluid like water in every
    // respect — and bottom up, every fluid resets the run.
    CHECK(floor ==
          RunReading{.latticeLava = Effect::Hold, .seaLava = Effect::Hold, .water = Effect::Hold});
    CHECK(ceiling == RunReading{.latticeLava = Effect::Reset,
                                .seaLava = Effect::Reset,
                                .water = Effect::Reset});
    CHECK(latch == LatchReading{.latticeLava = true, .seaLava = true, .water = true});
}

TEST_CASE("the shipped filler paints the lava-run probe name-exact",
          "[conformance][aquifer][surface]") {
    const std::optional<nlohmann::json> spec = loadSpec();
    if (!spec.has_value()) {
        SKIP("no lava-run probe under " << probeDir() << "; generate it with " << kScript
                                        << " --accept-eula 42");
    }
    test::requireFrozen(probeDir(), kScript);
    test::requireSeed(probeDir(), kSeed);

    // The corpus's own spec, rebuilt into the settings the server was given
    // (density-probe.sh's defaults included) — never a copy kept here.
    const ScratchTree scratch;
    for (const auto& entry : *spec) {
        std::ofstream out(scratch.path() / "noise_settings" /
                          (entry.at("name").get<std::string>() + ".json"));
        out << test::probeNoiseSettings(entry, {"ladder", "expected_layout"}).dump();
    }
    const data::Pack pack = data::Pack::open(scratch.path());
    const settings::LoadedSettings loaded = settings::loadAll(pack);
    const density::NoiseRegistry noises = density::NoiseRegistry::create(
        pack, loaded.graph.referencedNoises(), kSeed, density::RandomSource::Xoroshiro);

    long long flowTotal = 0;
    long long blocksTotal = 0;
    for (const auto& entry : *spec) {
        const std::string name = entry.at("name").get<std::string>();
        INFO("dimension " << name);
        const data::ResourceLocation id{"minecraft", name};
        const settings::NoiseSettings& dimension = loaded.settings.at(id);
        const surface::RuleGraph rules = surface::RuleGraph::resolve(dimension.surfaceRule, id);
        const terrain::ChunkFiller filler =
            terrain::ChunkFiller::compile(loaded.graph, noises, dimension, &rules);
        REQUIRE(filler.runsSurfaceRules());
        const std::int32_t minY = dimension.geometry.minY;
        const std::int32_t topY = minY + dimension.geometry.height - 1;
        test::GoldenRegion region(probeDir() / name / "r.0.0.mca");
        long long exact = 0;
        long long flow = 0;
        long long wrong = 0;
        terrain::ChunkBuffer buffer(dimension.geometry);
        for (std::int32_t chunkZ = 0; chunkZ < kChunks; ++chunkZ) {
            for (std::int32_t chunkX = 0; chunkX < kChunks; ++chunkX) {
                REQUIRE(region.hasChunk(chunkX, chunkZ));
                filler.fill(chunkX, chunkZ, buffer);
                const chunk::Chunk& served = region.chunk(chunkX, chunkZ);
                for (int localZ = 0; localZ < 16; ++localZ) {
                    for (int localX = 0; localX < 16; ++localX) {
                        for (std::int32_t y = minY; y <= topY; ++y) {
                            const chunk::BlockState* block = served.blockAt(localX, y, localZ);
                            static const std::string kAir = "minecraft:air";
                            const std::string& servedName = block == nullptr ? kAir : block->name;
                            const data::ResourceLocation& ourId = buffer.at(localX, y, localZ).name;
                            if (namedAs(ourId, servedName)) {
                                ++exact;
                                continue;
                            }
                            const std::string ours = ourId.toString();
                            // A marker is written before any fluid moves, so
                            // only a CATEGORY change can be flow; a wrong
                            // marker never is.
                            const Category servedCategory = layoutCategoryOf(servedName);
                            const Category ourCategory = layoutCategoryOf(ours);
                            if (servedCategory != ourCategory &&
                                test::explainedByFlow(region, (chunkX * 16) + localX, y,
                                                      (chunkZ * 16) + localZ, servedCategory,
                                                      ourCategory)) {
                                ++flow;
                                continue;
                            }
                            ++wrong;
                            if (wrong <= 20) {
                                UNSCOPED_INFO(name << " (" << ((chunkX * 16) + localX) << ", " << y
                                                   << ", " << ((chunkZ * 16) + localZ)
                                                   << "): server " << servedName << ", ours "
                                                   << ours);
                            }
                        }
                    }
                }
            }
        }
        const long long blocks = kColumns * dimension.geometry.height;
        INFO("exact " << exact << ", flow " << flow << ", wrong " << wrong << " of " << blocks);
        flowTotal += flow;
        blocksTotal += blocks;
        CHECK(wrong == 0);
        CHECK(exact + flow == blocks);
    }
    INFO("flow over every dimension: " << flowTotal << " of " << blocksTotal);
    // Bounded, not pinned: nothing in this world is expected to move.
    CHECK(flowTotal * 1000 <= blocksTotal);
}
