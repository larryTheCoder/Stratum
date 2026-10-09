// Stratum — cache markers inside the aquifer's router entries, against the server.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The aquifer reads its router entries at five kinds of point and only one of
// them, the barrier's, is the block being generated: a source's floodedness
// (and Q5.9's erosion and depth) at its jittered centre, its spread and lava
// at contracted indices, its surface at 4-aligned anchors. A datapack may wrap
// any entry in a cache marker, and what a marker means at a point that is not
// the generating block is documented nowhere. `tools/analysis/
// aquifer-markers-probe.sh` asks the server: 37 dimensions per seed, two
// seeds, each wrapping one entry in one marker over a field built to make the
// candidate readings part on whole bands (its header has the families).
//
// EVERY ARM NAMES THE READING THE SERVER TAKES — and the build must be that
// reading on every block (`measured`: its discrimination against the build is
// zero) — and the readings it refutes (`rivals`): each parts from the build
// on at least its floor of blocks on each seed, and the server never sides
// with it there. The build itself is ChunkFiller, tied block for block to the
// harness's own composition of the same reads (`tie`), and it must match the
// server everywhere but where fluid moved after generation (only the lava
// arms keep any), on every row of the world.
//
// The probe's default fluid is `minecraft:packed_ice`, which the aquifer
// places exactly where it places water and which never flows; ChunkFiller
// refuses a default fluid that is not water, so the case fills with water in
// its place. The bare controls are exact, which is what licenses that. Where
// a lava source meets a default-fluid one the barrier weighs Q6.4's
// lava-against-water constant, and whether packed_ice counts as water there is
// a separate, unmeasured question: those blocks are set aside and counted
// (`mixedPair`), and only the flat_cache lava arm has any.
//
// SKIPs when no markers_* corpus exists; once one does, every group needs both
// seeds. The fixtures are Mojang-derived and never committed (SPEC §12).
#include "support/fluid_flow.hpp"
#include "support/probe_corpus.hpp"
#include "support/temp_path.hpp"

#include <stratum/aquifer/barrier.hpp>
#include <stratum/aquifer/lattice.hpp>
#include <stratum/aquifer/sampling.hpp>
#include <stratum/aquifer/selection.hpp>
#include <stratum/aquifer/substance.hpp>
#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/graph.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/javamath.hpp>
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
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

using namespace stratum;
using density::Point;

constexpr std::int32_t kSeaLevel = 63;
constexpr std::int32_t kChunks = 8;
constexpr std::int32_t kMinY = -48;
constexpr std::int32_t kHeight = 320;
constexpr std::int32_t kCellHeight = 8; ///< size_vertical 2
/// The highest row the readings are scored on. Every level here is at most
/// 96 (sea 63, ladder capped by psl 96), and a barrier reaches at most five
/// blocks past a level; the case checks that no reading places anything but
/// air on the top few rows it scores.
constexpr std::int32_t kTopY = 110;
constexpr std::int32_t kTopCheckFrom = 105;
constexpr double kDensity = -1.0;
/// Fluid that moved after generation, per dimension: none can in a
/// packed_ice arm, and a frozen lava arm keeps a run-dependent remnant (2 on
/// every lava arm measured so far). Bounded, never pinned.
constexpr long long kFlowBound = 64;
/// Blocks of the flat_cache lava arm set aside as mixed pairs, per corpus.
constexpr long long kMixedBound = 4096;

enum class Entry : std::uint8_t { Barrier, Floodedness, Spread, Lava, Psl, Erosion };
enum class Marker : std::uint8_t {
    None,
    Interpolated,
    CacheAllInCell,
    FlatCache,
    CacheOnce,
    Cache2d
};

/// Where a marker's argument is read, for each reading under test.
enum class Reading : std::uint8_t {
    Built,        ///< what ChunkFiller does
    Exact,        ///< the argument at the read point
    Trilinear,    ///< blended over the noise cell holding the read point
    LowerCorner,  ///< the argument at that cell's lower corner
    BlockInterp,  ///< the generating block's own blended value
    BlockExact,   ///< the argument at the generating block
    Window20,     ///< flat_cache relocating inside [16c, 16c+19], (x, 0, z) outside
    KeepY16,      ///< relocating inside the chunk's own 16 columns, (x, y, z) outside
    KeepY20,      ///< ... inside [16c, 16c+19] (one quart past the chunk)
    KeepY24,      ///< ... inside [16c-4, 16c+19] (a quart on the low side too)
    KeepY24High,  ///< ... inside [16c, 16c+23] (two quarts past the chunk)
    KeepY28,      ///< ... inside [16c-8, 16c+19] (two quarts on the low side)
    CornerAlways, ///< flat_cache relocating every read
    ColumnY0,     ///< flat_cache never relocating, read at (x, 0, z)
    Memo,         ///< one value per generating block: the first read
};

[[nodiscard]] std::string_view readingName(Reading r) {
    switch (r) {
        case Reading::Built:
            return "built";
        case Reading::Exact:
            return "exact";
        case Reading::Trilinear:
            return "trilinear";
        case Reading::LowerCorner:
            return "lower-corner";
        case Reading::BlockInterp:
            return "block-interp";
        case Reading::BlockExact:
            return "block-exact";
        case Reading::Window20:
            return "window20-y0";
        case Reading::KeepY16:
            return "keep-y16";
        case Reading::KeepY20:
            return "keep-y20";
        case Reading::KeepY24:
            return "keep-y24";
        case Reading::KeepY24High:
            return "keep-y24-high";
        case Reading::KeepY28:
            return "keep-y28";
        case Reading::CornerAlways:
            return "corner-always";
        case Reading::ColumnY0:
            return "column-y0";
        case Reading::Memo:
            return "memo";
    }
    return "?";
}

[[nodiscard]] bool blockDependent(Reading r) {
    return r == Reading::BlockInterp || r == Reading::BlockExact || r == Reading::Memo;
}

[[nodiscard]] std::string_view routerKey(Entry e) {
    switch (e) {
        case Entry::Barrier:
            return "barrier";
        case Entry::Floodedness:
            return "fluid_level_floodedness";
        case Entry::Spread:
            return "fluid_level_spread";
        case Entry::Lava:
            return "lava";
        case Entry::Psl:
            return "preliminary_surface_level";
        case Entry::Erosion:
            return "erosion";
    }
    return "?";
}

[[nodiscard]] settings::RouterEntry routerEntry(Entry e) {
    switch (e) {
        case Entry::Barrier:
            return settings::RouterEntry::Barrier;
        case Entry::Floodedness:
            return settings::RouterEntry::FluidLevelFloodedness;
        case Entry::Spread:
            return settings::RouterEntry::FluidLevelSpread;
        case Entry::Lava:
            return settings::RouterEntry::Lava;
        case Entry::Psl:
            return settings::RouterEntry::PreliminarySurfaceLevel;
        case Entry::Erosion:
            return settings::RouterEntry::Erosion;
    }
    return settings::RouterEntry::Barrier;
}

[[nodiscard]] std::string_view markerType(Marker m) {
    switch (m) {
        case Marker::None:
            return "";
        case Marker::Interpolated:
            return "minecraft:interpolated";
        case Marker::CacheAllInCell:
            return "minecraft:cache_all_in_cell";
        case Marker::FlatCache:
            return "minecraft:flat_cache";
        case Marker::CacheOnce:
            return "minecraft:cache_once";
        case Marker::Cache2d:
            return "minecraft:cache_2d";
    }
    return "";
}

/// A reading the server is shown NOT to take: on every corpus it parts from
/// the build on at least `floor` blocks, and the server sides with the build
/// on every one. A floor of 0 marks a reading that coincides with the
/// measured one on this arm BY CONSTRUCTION — asserted as a tie, so a design
/// change that made it discriminate would be noticed rather than credited.
struct Rival {
    Reading reading;
    long long floor;
};

/// One probe dimension: the entry under test, its marker, the reading the
/// server takes (and the build must), and the readings it refutes.
struct Arm {
    const char* name;
    Entry entry;
    Marker marker;
    std::int32_t sizeHorizontal;
    Reading measured;
    std::vector<Rival> rivals;
};

struct Group {
    const char* prefix;
    std::vector<Arm> arms;
};

/// Every arm the probe writes. Floors are about a tenth of the smaller of the
/// two seeds' counts at first measurement, which is given beside each.
[[nodiscard]] std::vector<Group> groups() {
    using R = Reading;
    const Marker in = Marker::Interpolated;
    const Marker cell = Marker::CacheAllInCell;
    const Marker flat = Marker::FlatCache;
    // The barrier is read at the block itself, so the block-valued readings
    // are the point-valued ones there and are not listed for it.
    return {
        {"markers_bare_s",
         {{"b", Entry::Barrier, Marker::None, 1, R::Exact, {}},
          {"f", Entry::Floodedness, Marker::None, 1, R::Exact, {}},
          {"s", Entry::Spread, Marker::None, 1, R::Exact, {}},
          {"l1", Entry::Lava, Marker::None, 1, R::Exact, {}},
          {"p", Entry::Psl, Marker::None, 1, R::Exact, {}}}},
        // interpolated: the blend at the block, the argument everywhere else.
        {"markers_int_s",
         {{"b1", Entry::Barrier, in, 1, R::Trilinear, {{R::Exact, 300}}}, // 3425
          {"b2", Entry::Barrier, in, 2, R::Trilinear, {{R::Exact, 300}}}, // 3740
          {"f1",
           Entry::Floodedness,
           in,
           1,
           R::Exact,
           {{R::Trilinear, 29000}, {R::BlockInterp, 33000}}}, // 299074, 330677
          {"f2",
           Entry::Floodedness,
           in,
           2,
           R::Exact,
           {{R::Trilinear, 33000}, {R::BlockInterp, 32000}}}, // 330325, 323528
          {"s1",
           Entry::Spread,
           in,
           1,
           R::Exact,
           {{R::Trilinear, 26000}, {R::BlockInterp, 25000}}}, // 266489, 257998
          {"s2",
           Entry::Spread,
           in,
           2,
           R::Exact,
           {{R::Trilinear, 26000}, {R::BlockInterp, 25000}}}, // 265528, 257117
          // The lava profiles read (lava, water, water) under l1 for (exact,
          // blend, block), (water, lava, water) under l2 and all water under
          // l3, whose arm is the cell's lower corner (below).
          {"l1",
           Entry::Lava,
           in,
           1,
           R::Exact,
           {{R::Trilinear, 28000}, {R::BlockInterp, 28000}}}, // 280661, 280661
          {"l2", Entry::Lava, in, 1, R::Exact, {{R::Trilinear, 28000}, {R::BlockInterp, 0}}},
          {"l3", Entry::Lava, in, 1, R::Exact, {{R::Trilinear, 0}, {R::BlockInterp, 0}}},
          // A 4-aligned anchor is a cell corner at size_horizontal 1.
          {"p1", Entry::Psl, in, 1, R::Exact, {{R::Trilinear, 0}}},
          {"p2", Entry::Psl, in, 2, R::Exact, {{R::Trilinear, 6000}}}, // 61295
          {"e1",
           Entry::Erosion,
           in,
           1,
           R::Exact,
           {{R::Trilinear, 71000}, {R::BlockInterp, 100000}}}}}, // 716199, 1062962
        // cache_all_in_cell: the argument, at the block and everywhere else.
        {"markers_cell_s",
         {{"b1",
           Entry::Barrier,
           cell,
           1,
           R::Exact,
           {{R::Trilinear, 300}, {R::LowerCorner, 400}}}, // 3425, 4900
          {"b2",
           Entry::Barrier,
           cell,
           2,
           R::Exact,
           {{R::Trilinear, 300}, {R::LowerCorner, 400}}}, // 3740, 4936
          // (blend, lower corner, block): f1 299074, 463596, 495781; f2
          // 330325, 472569, 495781; s1 266489, 424643, 259569; s2 265528,
          // 424354, 259569; every lava reading that parts, 280661; e1 716199,
          // 880080, 1068771.
          {"f1",
           Entry::Floodedness,
           cell,
           1,
           R::Exact,
           {{R::Trilinear, 29000}, {R::LowerCorner, 46000}, {R::BlockExact, 49000}}},
          {"f2",
           Entry::Floodedness,
           cell,
           2,
           R::Exact,
           {{R::Trilinear, 33000}, {R::LowerCorner, 47000}, {R::BlockExact, 49000}}},
          {"s1",
           Entry::Spread,
           cell,
           1,
           R::Exact,
           {{R::Trilinear, 26000}, {R::LowerCorner, 42000}, {R::BlockExact, 25000}}},
          {"s2",
           Entry::Spread,
           cell,
           2,
           R::Exact,
           {{R::Trilinear, 26000}, {R::LowerCorner, 42000}, {R::BlockExact, 25000}}},
          {"l1",
           Entry::Lava,
           cell,
           1,
           R::Exact,
           {{R::Trilinear, 28000}, {R::LowerCorner, 28000}, {R::BlockExact, 28000}}},
          {"l2",
           Entry::Lava,
           cell,
           1,
           R::Exact,
           {{R::Trilinear, 28000}, {R::LowerCorner, 0}, {R::BlockExact, 0}}},
          {"l3",
           Entry::Lava,
           cell,
           1,
           R::Exact,
           {{R::Trilinear, 0}, {R::LowerCorner, 28000}, {R::BlockExact, 0}}},
          {"p1", Entry::Psl, cell, 1, R::Exact, {{R::Trilinear, 0}, {R::LowerCorner, 0}}},
          {"p2",
           Entry::Psl,
           cell,
           2,
           R::Exact,
           {{R::Trilinear, 6000}, {R::LowerCorner, 6000}}}, // 61295, 62183
          {"e1",
           Entry::Erosion,
           cell,
           1,
           R::Exact,
           {{R::Trilinear, 71000}, {R::LowerCorner, 88000}, {R::BlockExact, 100000}}}}},
        // flat_cache: the 4x4 corner at y = 0 inside [16c, 16c + 19], the read
        // point itself (its own y) outside. No centre lands in the low quart
        // [16c - 4, 16c - 1], so the 24-column reading ties on the centre
        // arms; contracted indices at chunk (0, 0) do land there, and part it.
        // The barrier, always inside: the argument at the block parts on 4915
        // blocks, the column at y = 0 on 3890.
        {"markers_flat_s",
         {{"b", Entry::Barrier, flat, 1, R::KeepY20, {{R::Exact, 400}, {R::ColumnY0, 350}}},
          {"f",
           Entry::Floodedness,
           flat,
           1,
           R::KeepY20,
           {{R::Window20, 6000}, // 67771
            {R::KeepY16, 13000}, // 133463
            {R::KeepY24, 0},
            {R::KeepY24High, 5000},  // 53875
            {R::KeepY28, 600},       // 6650
            {R::CornerAlways, 7000}, // 76028
            {R::ColumnY0, 37000},    // 373267
            {R::Exact, 44000}}},     // 440023
          {"s",
           Entry::Spread,
           flat,
           1,
           R::KeepY20,
           {{R::Window20, 20000}, // 201732
            {R::KeepY16, 0},
            {R::KeepY24, 1}, // 4 (and 37)
            {R::KeepY24High, 0},
            {R::KeepY28, 1},          // 4 (and 37)
            {R::CornerAlways, 22000}, // 221531
            {R::ColumnY0, 20000},     // 202294
            {R::Exact, 300}}},        // 3126
          {"l1",
           Entry::Lava,
           flat,
           1,
           R::KeepY20,
           {{R::Window20, 27000}, // 276839
            {R::KeepY16, 0},
            {R::KeepY24, 1}, // 10 (and 47), on the set-aside blocks' fluid
            {R::KeepY24High, 0},
            {R::KeepY28, 1},          // 10 (and 47)
            {R::CornerAlways, 27000}, // 276839
            {R::ColumnY0, 27000},     // 276839
            {R::Exact, 300}}},        // 3822
          {"e",
           Entry::Erosion,
           flat,
           1,
           R::KeepY20,
           {{R::Window20, 15000}, // 157419
            {R::KeepY16, 23000},  // 239501
            {R::KeepY24, 0},
            {R::KeepY24High, 11000},  // 118345
            {R::KeepY28, 1300},       // 13285
            {R::CornerAlways, 14000}, // 148326
            {R::ColumnY0, 63000},     // 635692
            {R::Exact, 79000}}}}},    // 792344
        // cache_once and cache_2d: each source reads its own value (one per
        // block parts on 118927, 10441 and 103206).
        {"markers_memo_s",
         {{"f_once", Entry::Floodedness, Marker::CacheOnce, 1, R::Exact, {{R::Memo, 11000}}},
          {"s_once", Entry::Spread, Marker::CacheOnce, 1, R::Exact, {{R::Memo, 1000}}},
          {"f_2d", Entry::Floodedness, Marker::Cache2d, 1, R::Exact, {{R::Memo, 10000}}}}},
    };
}

constexpr std::array<std::int64_t, 2> kSeeds{42, 31337};

[[nodiscard]] std::filesystem::path fixtures() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11";
}

// --- The world, rebuilt -----------------------------------------------------

/// A one-dimension data pack holding exactly what density-probe.sh wrote for
/// @p entry: its noise settings and the probe's own noise, from the manifest.
class ProbePack {
public:
    ProbePack(const nlohmann::json& entry, const nlohmann::json& manifest)
        : root_(test::tempPath("stratum-markers-pack")) {
        std::filesystem::remove_all(root_);
        const std::filesystem::path data = root_ / "data" / "stratum" / "worldgen";
        std::filesystem::create_directories(data / "noise_settings");
        std::filesystem::create_directories(data / "noise");
        write(root_ / "pack.mcmeta",
              nlohmann::json{{"pack", {{"pack_format", 94}, {"description", "markers"}}}});
        const auto& declared = manifest.at("probe_noise");
        REQUIRE(declared.at("id").get<std::string>() == "stratum:probe_noise");
        write(data / "noise" / "probe_noise.json",
              nlohmann::json{{"firstOctave", declared.at("first_octave")},
                             {"amplitudes", declared.at("amplitudes")}});

        // density-probe.sh's own defaults, then the entry's overrides.
        nlohmann::json router = {{"barrier", 0},
                                 {"fluid_level_floodedness", 0},
                                 {"fluid_level_spread", 0},
                                 {"lava", 0},
                                 {"temperature", 0},
                                 {"vegetation", 0},
                                 {"continents", 0},
                                 {"erosion", 0},
                                 {"depth", 0},
                                 {"ridges", 0},
                                 {"preliminary_surface_level", 0},
                                 {"vein_toggle", 0},
                                 {"vein_ridged", 0},
                                 {"vein_gap", 0}};
        for (const auto& [key, value] : entry.at("router").items()) {
            router[key] = value;
        }
        router["final_density"] = entry.at("raw_final_density");
        const nlohmann::json settings = {{"sea_level", entry.at("sea_level")},
                                         {"disable_mob_generation", true},
                                         {"aquifers_enabled", entry.at("aquifers_enabled")},
                                         {"ore_veins_enabled", false},
                                         {"legacy_random_source", false},
                                         {"default_block", {{"Name", "minecraft:stone"}}},
                                         {"default_fluid", entry.at("default_fluid")},
                                         {"noise",
                                          {{"min_y", entry.at("min_y")},
                                           {"height", entry.at("height")},
                                           {"size_horizontal", entry.value("size_horizontal", 1)},
                                           {"size_vertical", entry.value("size_vertical", 1)}}},
                                         {"spawn_target", nlohmann::json::array()},
                                         {"surface_rule", entry.at("surface_rule")},
                                         {"noise_router", router}};
        write(data / "noise_settings" / (entry.at("name").get<std::string>() + ".json"), settings);
    }

    ProbePack(const ProbePack&) = delete;
    ProbePack& operator=(const ProbePack&) = delete;
    ProbePack(ProbePack&&) = delete;
    ProbePack& operator=(ProbePack&&) = delete;

    ~ProbePack() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

private:
    static void write(const std::filesystem::path& path, const nlohmann::json& body) {
        std::ofstream out(path);
        out << body.dump();
        REQUIRE(out.good());
    }

    std::filesystem::path root_;
};

// --- Categories ---------------------------------------------------------------

enum class Cat : std::uint8_t { Air, Fluid, Lava, Solid, Other };

/// The server's block. `packed_ice` is the probe's default fluid, placed
/// exactly where water would be.
[[nodiscard]] Cat serverCat(const chunk::BlockState* block) {
    if (block == nullptr) {
        return Cat::Other;
    }
    const std::string& name = block->name;
    if (name == "minecraft:air" || name == "minecraft:cave_air" || name == "minecraft:void_air") {
        return Cat::Air;
    }
    if (name == "minecraft:packed_ice") {
        return Cat::Fluid;
    }
    if (name == "minecraft:lava") {
        return Cat::Lava;
    }
    if (name == "minecraft:stone") {
        return Cat::Solid;
    }
    return Cat::Other;
}

/// The filler's block, with the default fluid overridden to water.
[[nodiscard]] Cat fillerCat(const settings::BlockState& block) {
    const std::string name = block.name.toString();
    if (name == "minecraft:air") {
        return Cat::Air;
    }
    if (name == "minecraft:water") {
        return Cat::Fluid;
    }
    if (name == "minecraft:lava") {
        return Cat::Lava;
    }
    if (name == "minecraft:stone") {
        return Cat::Solid;
    }
    return Cat::Other;
}

[[nodiscard]] test::Category flowCategory(Cat c) {
    switch (c) {
        case Cat::Air:
            return test::Category::Air;
        case Cat::Fluid:
            return test::Category::Water;
        case Cat::Lava:
            return test::Category::Lava;
        default:
            return test::Category::Solid;
    }
}

/// The aquifer's substance decision past source selection, from the lib's
/// own pieces in computeSubstance's order. The case ties it, with the
/// as-built samplers, to ChunkFiller on every scored block.
[[nodiscard]] Cat decide(const aquifer::Selection& selection,
                         const std::array<aquifer::SourceStatus, 3>& status, double barrier,
                         std::int32_t y) {
    if (aquifer::globalReadsLava(y, kSeaLevel)) {
        return Cat::Lava;
    }
    const auto asFluid = [&] {
        return status[0].type == aquifer::FluidType::Lava ? Cat::Lava : Cat::Fluid;
    };
    if (aquifer::waterOverLava(y, kSeaLevel, status[0].level, status[0].type)) {
        return asFluid();
    }
    const auto source = [&](std::size_t r) {
        return aquifer::BarrierSource{.level = status[r].level,
                                      .distanceSq = selection.ranked[r].distanceSq,
                                      .type = status[r].type};
    };
    const aquifer::BarrierAt at{.y = y,
                                .density = kDensity,
                                .nearest = source(0),
                                .second = source(1),
                                .third = source(2),
                                .barrier = barrier};
    if (aquifer::placesBarrier(at)) {
        return Cat::Solid;
    }
    if (y >= status[0].level) {
        return Cat::Air;
    }
    return asFluid();
}

// --- Readings -----------------------------------------------------------------

/// One source's inputs to its status, as read for it.
struct Inputs {
    aquifer::PslRead psl{};
    double floodedness = 0.0;
    double spread = 0.0;
    double lava = 0.0;
    double erosion = 0.0;
    double depth = 0.0;
};

[[nodiscard]] aquifer::SourceStatus statusFrom(const aquifer::Source& source, const Inputs& in) {
    const aquifer::CellFluid cell{.centreY = source.centre.y,
                                  .surface = in.psl,
                                  .seaLevel = kSeaLevel,
                                  .floodedness = in.floodedness,
                                  .spread = in.spread,
                                  .deepDark = aquifer::isDeepDark(in.erosion, in.depth)};
    return aquifer::sourceStatus(cell, in.lava);
}

[[nodiscard]] std::uint64_t key(std::int32_t x, std::int32_t y, std::int32_t z) {
    const auto part = [](std::int32_t v) {
        return static_cast<std::uint64_t>(static_cast<std::uint32_t>(v) & 0x1FFFFFU);
    };
    return (part(x) << 42U) | (part(y) << 21U) | part(z);
}

/// One probe dimension, rebuilt: the lib's own filler, and an interpreter the
/// readings evaluate the marker's argument with.
class Dimension {
public:
    Dimension(const nlohmann::json& entry, const nlohmann::json& manifest, const Arm& arm,
              std::int64_t seed)
        : arm_(arm), pack_(entry, manifest),
          loaded_(settings::loadAll(data::Pack::openDataPack(pack_.root()))),
          id_(data::ResourceLocation::parse("stratum:" + std::string(arm.name))),
          settings_(loaded_.settings.at(id_)),
          noises_(density::NoiseRegistry::create(data::Pack::openDataPack(pack_.root()),
                                                 loaded_.graph.referencedNoises(), seed,
                                                 density::RandomSource::Xoroshiro)),
          interp_(loaded_.graph, noises_,
                  density::CellGeometry{.width = settings_.geometry.cellWidth(),
                                        .height = settings_.geometry.cellHeight()}),
          centres_(seed) {
        // ChunkFiller refuses a default fluid other than water; the server's
        // packed_ice stands where its water would.
        settings_.defaultFluid =
            settings::BlockState{.name = data::ResourceLocation::parse("minecraft:water"),
                                 .properties = {{"level", "0"}}};
        root_ = settings_.router.at(routerEntry(arm.entry));
        const density::Node& node = loaded_.graph.node(root_);
        argument_ = arm.marker == Marker::None ? root_ : node.arguments.at(0);
    }

    [[nodiscard]] terrain::ChunkFiller filler() const {
        return terrain::ChunkFiller::compile(loaded_.graph, noises_, settings_);
    }

    [[nodiscard]] const settings::NoiseSettings& settings() const { return settings_; }

    [[nodiscard]] const aquifer::CentreSource& centres() const { return centres_; }

    [[nodiscard]] const Arm& arm() const { return arm_; }

    [[nodiscard]] std::size_t cacheSize() const { return interp_.cacheSize(); }

    /// What ChunkFiller reads for router entry @p e at @p p, through the
    /// generating chunk's @p window: the barrier at the block itself, every
    /// other entry detached.
    [[nodiscard]] double builtNode(settings::RouterEntry e, Point p,
                                   density::Interpreter::CornerCache& cache,
                                   const density::FlatCacheWindow& window) const {
        return interp_.evaluate(settings_.router.at(e), p, cache, window, contextOf(e));
    }

    [[nodiscard]] static density::ReadContext contextOf(settings::RouterEntry e) {
        return e == settings::RouterEntry::Barrier ? density::ReadContext::Block
                                                   : density::ReadContext::Detached;
    }

    /// The tested entry's value at read point @p p under reading @p r, for a
    /// block @p b of chunk (@p cx, @p cz). @p cache serves the built read and
    /// the block's own blended value. Memo is the caller's: here it is the
    /// read it memoises, at @p p.
    [[nodiscard]] double read(Reading r, Point p, Point b, std::int32_t cx, std::int32_t cz,
                              density::Interpreter::CornerCache& cache) const {
        const std::int32_t w = settings_.geometry.cellWidth();
        const auto arg = [&](Point at) { return interp_.evaluate(argument_, at); };
        const auto corner4 = [](std::int32_t v) { return javamath::floorDiv(v, 4) * 4; };
        const auto windowed = [&](std::int32_t lowExtra, std::int32_t columns, bool keepY) {
            const density::FlatCacheWindow window{.minX = (cx * 16) - lowExtra,
                                                  .maxX = (cx * 16) - lowExtra + columns - 1,
                                                  .minZ = (cz * 16) - lowExtra,
                                                  .maxZ = (cz * 16) - lowExtra + columns - 1};
            if (window.covers(p.x, p.z)) {
                return arg(Point{.x = corner4(p.x), .y = 0, .z = corner4(p.z)});
            }
            return arg(keepY ? p : Point{.x = p.x, .y = 0, .z = p.z});
        };
        switch (r) {
            case Reading::Built:
                return interp_.evaluate(root_, p, cache,
                                        terrain::ChunkFiller::flatCacheWindow(cx, cz),
                                        contextOf(routerEntry(arm_.entry)));
            case Reading::Exact:
                return arg(p);
            case Reading::Trilinear:
                return trilinear(p, w);
            case Reading::LowerCorner:
                return arg(Point{.x = javamath::floorDiv(p.x, w) * w,
                                 .y = javamath::floorDiv(p.y, kCellHeight) * kCellHeight,
                                 .z = javamath::floorDiv(p.z, w) * w});
            case Reading::BlockInterp:
                // Only listed for `interpolated`, whose own node is the blend.
                return interp_.evaluate(root_, b, cache);
            case Reading::BlockExact:
                return arg(b);
            case Reading::Window20:
                return windowed(0, 20, false);
            case Reading::KeepY16:
                return windowed(0, 16, true);
            case Reading::KeepY20:
                return windowed(0, 20, true);
            case Reading::KeepY24:
                return windowed(4, 24, true);
            case Reading::KeepY24High:
                return windowed(0, 24, true);
            case Reading::KeepY28:
                return windowed(8, 28, true);
            case Reading::CornerAlways:
                return arg(Point{.x = corner4(p.x), .y = 0, .z = corner4(p.z)});
            case Reading::ColumnY0:
                return arg(Point{.x = p.x, .y = 0, .z = p.z});
            case Reading::Memo:
                return arg(p);
        }
        return 0.0;
    }

private:
    /// The argument blended over the noise cell holding @p p: y first, then
    /// x, then z, as the interpreter does.
    [[nodiscard]] double trilinear(Point p, std::int32_t w) const {
        const std::int32_t x0 = javamath::floorDiv(p.x, w) * w;
        const std::int32_t y0 = javamath::floorDiv(p.y, kCellHeight) * kCellHeight;
        const std::int32_t z0 = javamath::floorDiv(p.z, w) * w;
        const double tx = static_cast<double>(p.x - x0) / static_cast<double>(w);
        const double ty = static_cast<double>(p.y - y0) / static_cast<double>(kCellHeight);
        const double tz = static_cast<double>(p.z - z0) / static_cast<double>(w);
        const auto v = [&](std::int32_t dx, std::int32_t dy, std::int32_t dz) {
            return interp_.evaluate(
                argument_,
                Point{.x = x0 + (dx * w), .y = y0 + (dy * kCellHeight), .z = z0 + (dz * w)});
        };
        const auto lerp = [](double t, double a, double b) { return a + (t * (b - a)); };
        const double x0z0 = lerp(ty, v(0, 0, 0), v(0, 1, 0));
        const double x1z0 = lerp(ty, v(1, 0, 0), v(1, 1, 0));
        const double x0z1 = lerp(ty, v(0, 0, 1), v(0, 1, 1));
        const double x1z1 = lerp(ty, v(1, 0, 1), v(1, 1, 1));
        const double nearZ = lerp(tx, x0z0, x1z0);
        const double farZ = lerp(tx, x0z1, x1z1);
        return lerp(tz, nearZ, farZ);
    }

    Arm arm_;
    ProbePack pack_;
    settings::LoadedSettings loaded_;
    data::ResourceLocation id_;
    settings::NoiseSettings settings_;
    density::NoiseRegistry noises_;
    density::Interpreter interp_;
    aquifer::CentreSource centres_;
    density::NodeIndex root_{};
    density::NodeIndex argument_{};
};

// --- Scoring ------------------------------------------------------------------

struct ReadingScore {
    long long discriminating = 0;     ///< blocks where this reading and the build part
    long long readingRight = 0;       ///< ... and the server sides with this reading
    long long readingRightByFlow = 0; ///< ... only where the build's miss is flow
    long long builtRight = 0;         ///< ... and the server sides with the build
    long long unexplained = 0;        ///< this reading against the server, not flow
    long long flow = 0;               ///< ... and flow-shaped
    long long mixed = 0;              ///< set aside: a mixed pair under it or the build
    /// On those blocks, only the server's fluid scored: where this reading's
    /// nearest source gives another fluid than the build's, and whose the
    /// server's is.
    long long fluidDiscriminating = 0;
    long long fluidReadingRight = 0;
    long long fluidBuiltRight = 0;
};

struct DimScore {
    long long blocks = 0; ///< scored by the readings
    long long tie = 0;    ///< the harness's as-built reading against ChunkFiller
    long long fillerBlocks = 0;
    long long fillerUnexplained = 0;
    long long fillerFlow = 0;
    long long serverOther = 0;
    long long serverLava = 0;
    long long serverFluid = 0;
    long long serverSolid = 0;
    long long topRowsNonAir = 0; ///< any reading placing non-air at rows >= kTopCheckFrom
    /// Blocks set aside because the build weighs a lava source against a
    /// default-fluid one there (see `mixedPair`).
    long long mixed = 0;
    /// ... of which the server holds a fluid the build's nearest source does
    /// not give.
    long long mixedFluidWrong = 0;
    std::map<Reading, ReadingScore> readings;
};

/// One block under one reading.
struct Outcome {
    Cat cat = Cat::Air;
    bool mixed = false;
    /// What the nearest source alone gives here: its fluid where it reads
    /// one, air otherwise. A FLUID block is always this, whatever the barrier
    /// weighed — the barrier only ever turns fluid into stone — so a server
    /// fluid block tests a reading even where the barrier cannot be scored.
    Cat nearest = Cat::Air;
};

/// Whether two of the three ranked sources read fluid at @p y and differ in
/// type — where Q6.4's lava-against-water constant enters the barrier. The
/// probe's default fluid is packed_ice, and how the server weighs lava
/// against a default fluid that is not water is a separate question no
/// probe has measured (ChunkFiller refuses it by name), so such a block says
/// nothing about markers and is set aside, counted.
[[nodiscard]] bool mixedPair(const std::array<aquifer::SourceStatus, 3>& status, std::int32_t y) {
    const auto pair = [&](std::size_t a, std::size_t b) {
        return y < status[a].level && y < status[b].level && status[a].type != status[b].type;
    };
    return pair(0, 1) || pair(0, 2) || pair(1, 2);
}

/// One chunk's worth of the readings' state: statuses per source, cached
/// wherever the reading depends on the source alone.
class ChunkReadings {
public:
    ChunkReadings(const Dimension& dim, std::int32_t cx, std::int32_t cz)
        : dim_(dim), cx_(cx), cz_(cz), window_(terrain::ChunkFiller::flatCacheWindow(cx, cz)),
          builtCache_(dim.cacheSize()), blockCache_(dim.cacheSize()) {}

    /// Q2.5's cutoff for this chunk, with the psl read as @p r reads it.
    [[nodiscard]] std::int32_t ySkip(Reading r) {
        const bool tested = dim_.arm().entry == Entry::Psl && r != Reading::Built;
        const aquifer::YSkipRectangle rect = aquifer::ySkipRectangle(cx_ * 16, cz_ * 16);
        std::int32_t maxSurface = std::numeric_limits<std::int32_t>::min();
        for (std::int32_t z = rect.minZ; z <= rect.maxZ; z += aquifer::kYSkipSampleStride) {
            for (std::int32_t x = rect.minX; x <= rect.maxX; x += aquifer::kYSkipSampleStride) {
                const Point p{.x = x, .y = aquifer::kPreliminarySurfaceSampleY, .z = z};
                const double psl =
                    tested ? dim_.read(r, p, p, cx_, cz_, builtCache_)
                           : dim_.builtNode(settings::RouterEntry::PreliminarySurfaceLevel, p,
                                            builtCache_, window_);
                maxSurface = std::max(maxSurface, javamath::floorToInt(psl));
            }
        }
        return aquifer::ySkip(maxSurface);
    }

    /// The barrier at block @p b as @p r reads it.
    [[nodiscard]] double barrier(Reading r, Point b) {
        if (dim_.arm().entry == Entry::Barrier && r != Reading::Built) {
            return dim_.read(r, b, b, cx_, cz_, blockCache_);
        }
        return dim_.builtNode(settings::RouterEntry::Barrier, b, builtCache_, window_);
    }

    /// The three nearest sources' statuses at block @p b, as @p r reads them.
    [[nodiscard]] std::array<aquifer::SourceStatus, 3>
    statuses(Reading r, const aquifer::Selection& selection, Point b) {
        std::array<aquifer::SourceStatus, 3> out{};
        const Entry e = dim_.arm().entry;
        if (r == Reading::Built || e == Entry::Barrier || !blockDependent(r)) {
            auto& cache = statusCache_[r];
            for (std::size_t k = 0; k < 3; ++k) {
                const aquifer::Source& source = selection.ranked[k];
                const std::uint64_t id = key(source.centre.x, source.centre.y, source.centre.z);
                const auto found = cache.find(id);
                if (found != cache.end()) {
                    out[k] = found->second;
                    continue;
                }
                Inputs in = builtInputs(source);
                if (r != Reading::Built && e != Entry::Barrier) {
                    replace(in, e, source, r, b);
                }
                out[k] = statusFrom(source, in);
                cache.emplace(id, out[k]);
            }
            return out;
        }
        // One value per block: the block's own (BlockInterp, BlockExact), or
        // the first read of the block, which is the nearest source's (Memo).
        const double value = r == Reading::Memo
                                 ? dim_.read(Reading::Exact, readPoint(e, selection.ranked[0]), b,
                                             cx_, cz_, blockCache_)
                                 : dim_.read(r, b, b, cx_, cz_, blockCache_);
        for (std::size_t k = 0; k < 3; ++k) {
            Inputs in = builtInputs(selection.ranked[k]);
            set(in, e, value);
            out[k] = statusFrom(selection.ranked[k], in);
        }
        return out;
    }

private:
    [[nodiscard]] static Point readPoint(Entry e, const aquifer::Source& source) {
        aquifer::SamplePos pos{};
        switch (e) {
            case Entry::Spread:
                pos = aquifer::spreadSample(source.cell, source.centre);
                break;
            case Entry::Lava:
                pos = aquifer::lavaSample(source.centre);
                break;
            default:
                pos = aquifer::floodednessSample(source.centre);
                break;
        }
        return Point{.x = pos.x, .y = pos.y, .z = pos.z};
    }

    static void set(Inputs& in, Entry e, double value) {
        switch (e) {
            case Entry::Floodedness:
                in.floodedness = value;
                break;
            case Entry::Spread:
                in.spread = value;
                break;
            case Entry::Lava:
                in.lava = value;
                break;
            case Entry::Erosion:
                in.erosion = value;
                break;
            case Entry::Barrier:
            case Entry::Psl:
                break;
        }
    }

    void replace(Inputs& in, Entry e, const aquifer::Source& source, Reading r, Point b) {
        if (e == Entry::Psl) {
            in.psl = aquifer::readPreliminarySurface(
                [&](std::int32_t x, std::int32_t y, std::int32_t z) {
                    const Point p{.x = x, .y = y, .z = z};
                    return dim_.read(r, p, b, cx_, cz_, blockCache_);
                },
                source.centre, kSeaLevel);
            return;
        }
        set(in, e, dim_.read(r, readPoint(e, source), b, cx_, cz_, blockCache_));
    }

    /// A source's inputs as ChunkFiller reads them — rankedStatusOf's reads,
    /// at its positions.
    [[nodiscard]] Inputs builtInputs(const aquifer::Source& source) {
        const std::uint64_t id = key(source.centre.x, source.centre.y, source.centre.z);
        const auto found = inputs_.find(id);
        if (found != inputs_.end()) {
            return found->second;
        }
        const auto at = [&](settings::RouterEntry e, aquifer::SamplePos pos) {
            return dim_.builtNode(e, Point{.x = pos.x, .y = pos.y, .z = pos.z}, builtCache_,
                                  window_);
        };
        Inputs in;
        in.psl = aquifer::readPreliminarySurface(
            [&](std::int32_t x, std::int32_t y, std::int32_t z) {
                return dim_.builtNode(settings::RouterEntry::PreliminarySurfaceLevel,
                                      Point{.x = x, .y = y, .z = z}, builtCache_, window_);
            },
            source.centre, kSeaLevel);
        const aquifer::SamplePos flood = aquifer::floodednessSample(source.centre);
        in.floodedness = at(settings::RouterEntry::FluidLevelFloodedness, flood);
        in.spread = at(settings::RouterEntry::FluidLevelSpread,
                       aquifer::spreadSample(source.cell, source.centre));
        in.erosion = at(settings::RouterEntry::Erosion, flood);
        in.depth = at(settings::RouterEntry::Depth, flood);
        in.lava = at(settings::RouterEntry::Lava, aquifer::lavaSample(source.centre));
        inputs_.emplace(id, in);
        return in;
    }

    const Dimension& dim_;
    std::int32_t cx_;
    std::int32_t cz_;
    density::FlatCacheWindow window_;
    density::Interpreter::CornerCache builtCache_;
    density::Interpreter::CornerCache blockCache_;
    std::unordered_map<std::uint64_t, Inputs> inputs_;
    std::map<Reading, std::unordered_map<std::uint64_t, aquifer::SourceStatus>> statusCache_;
};

/// Scores one dimension: the filler against the server on every row, and every
/// reading the arm lists against the build and the server on rows up to kTopY.
DimScore scoreDimension(const Dimension& dim, const std::filesystem::path& region) {
    DimScore score;
    const Arm& arm = dim.arm();
    std::vector<Reading> readings{arm.measured};
    for (const Rival& rival : arm.rivals) {
        readings.push_back(rival.reading);
    }
    for (const Reading r : readings) {
        score.readings[r] = ReadingScore{};
    }
    const terrain::ChunkFiller filler = dim.filler();
    const auto file = region::RegionFile::open(region);
    test::GoldenRegion golden(region);

    for (std::int32_t cz = 0; cz < kChunks; ++cz) {
        for (std::int32_t cx = 0; cx < kChunks; ++cx) {
            REQUIRE(file.hasChunk(cx, cz));
            terrain::ChunkBuffer buffer(dim.settings().geometry);
            filler.fill(cx, cz, buffer);
            ChunkReadings chunk(dim, cx, cz);
            const std::int32_t builtSkip = chunk.ySkip(Reading::Built);
            std::map<Reading, std::int32_t> skip;
            for (const Reading r : readings) {
                skip[r] = chunk.ySkip(r);
            }

            for (std::int32_t lz = 0; lz < 16; ++lz) {
                for (std::int32_t lx = 0; lx < 16; ++lx) {
                    const std::int32_t x = (cx * 16) + lx;
                    const std::int32_t z = (cz * 16) + lz;
                    for (std::int32_t y = kMinY; y < kMinY + kHeight; ++y) {
                        const Cat server = serverCat(golden.blockAt(x, y, z));
                        const Cat filled = fillerCat(buffer.at(lx, y, lz));
                        ++score.fillerBlocks;
                        score.serverOther += static_cast<long long>(server == Cat::Other);
                        score.serverLava += static_cast<long long>(server == Cat::Lava);
                        score.serverFluid += static_cast<long long>(server == Cat::Fluid);
                        score.serverSolid += static_cast<long long>(server == Cat::Solid);

                        std::optional<aquifer::Selection> selection;
                        const Point b{.x = x, .y = y, .z = z};
                        const auto outcomeUnder = [&](Reading r, std::int32_t ySkip) {
                            if (y > ySkip) {
                                const Cat global = aquifer::globalReadsLava(y, kSeaLevel)
                                                       ? Cat::Lava
                                                       : (y < kSeaLevel ? Cat::Fluid : Cat::Air);
                                return Outcome{.cat = global, .nearest = global};
                            }
                            if (!selection.has_value()) {
                                selection = aquifer::selectSources(dim.centres(), x, y, z);
                            }
                            const auto status = chunk.statuses(r, *selection, b);
                            const Cat nearest =
                                y >= status[0].level
                                    ? Cat::Air
                                    : (status[0].type == aquifer::FluidType::Lava ? Cat::Lava
                                                                                  : Cat::Fluid);
                            return Outcome{.cat =
                                               decide(*selection, status, chunk.barrier(r, b), y),
                                           .mixed = mixedPair(status, y),
                                           .nearest = nearest};
                        };
                        // Rows above kTopY hold air under every reading (the
                        // case checks the rows just below it), so only the
                        // filler is scored there.
                        const bool scored = y <= kTopY;
                        const Outcome built =
                            scored ? outcomeUnder(Reading::Built, builtSkip) : Outcome{};
                        const bool serverFluid = server == Cat::Fluid || server == Cat::Lava;
                        // A mixed block's fluid is still its nearest source's.
                        const auto scoreFluidOnly = [&](Reading r, const Outcome& under) {
                            ReadingScore& rs = score.readings[r];
                            ++rs.mixed;
                            if (!serverFluid || under.nearest == built.nearest) {
                                return;
                            }
                            ++rs.fluidDiscriminating;
                            rs.fluidReadingRight += static_cast<long long>(under.nearest == server);
                            rs.fluidBuiltRight += static_cast<long long>(built.nearest == server);
                        };
                        if (built.mixed) {
                            ++score.mixed;
                            score.mixedFluidWrong +=
                                static_cast<long long>(serverFluid && built.nearest != server);
                            for (const Reading r : readings) {
                                scoreFluidOnly(r, outcomeUnder(r, skip.at(r)));
                            }
                            continue;
                        }
                        if (server != filled) {
                            if (test::explainedByFlow(golden, x, y, z, flowCategory(server),
                                                      flowCategory(filled))) {
                                ++score.fillerFlow;
                            } else {
                                ++score.fillerUnexplained;
                            }
                        }
                        if (!scored) {
                            continue;
                        }

                        ++score.blocks;
                        score.tie += static_cast<long long>(built.cat != filled);
                        // Where the build and the server part only because
                        // fluid moved, a reading the server "sides with"
                        // there is not evidence for it.
                        const bool builtFlow =
                            built.cat != server &&
                            test::explainedByFlow(golden, x, y, z, flowCategory(server),
                                                  flowCategory(built.cat));
                        for (const Reading r : readings) {
                            const Outcome under = outcomeUnder(r, skip.at(r));
                            ReadingScore& rs = score.readings[r];
                            if (under.mixed) {
                                scoreFluidOnly(r, under);
                                continue;
                            }
                            if (y >= kTopCheckFrom && under.cat != Cat::Air) {
                                ++score.topRowsNonAir;
                            }
                            if (under.cat != built.cat) {
                                ++rs.discriminating;
                                if (under.cat == server) {
                                    ++(builtFlow ? rs.readingRightByFlow : rs.readingRight);
                                }
                                rs.builtRight += static_cast<long long>(built.cat == server);
                            }
                            if (under.cat != server) {
                                if (test::explainedByFlow(golden, x, y, z, flowCategory(server),
                                                          flowCategory(under.cat))) {
                                    ++rs.flow;
                                } else {
                                    ++rs.unexplained;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return score;
}

/// Whether any corpus of any group is present: one is, and every group must
/// be, for both seeds — a partial set would let a missing marker pass as
/// "measured".
[[nodiscard]] bool anyCorpus() {
    for (const Group& group : groups()) {
        for (const std::int64_t seed : kSeeds) {
            if (std::filesystem::is_regular_file(
                    fixtures() / "probes" / (std::string(group.prefix) + std::to_string(seed)) /
                    "manifest.json")) {
                return true;
            }
        }
    }
    return false;
}

/// Every corpus of one group, scored arm by arm.
void scoreGroup(const Group& group) {
    if (!anyCorpus()) {
        SKIP("no markers_* corpus under " << (fixtures() / "probes")
                                          << "; generate them with "
                                             "tools/analysis/aquifer-markers-probe.sh "
                                             "--accept-eula 42 (and 31337)");
    }

    for (const std::int64_t seed : kSeeds) {
        const std::filesystem::path dir =
            fixtures() / "probes" / (std::string(group.prefix) + std::to_string(seed));
        INFO("corpus " << dir << " — generate it with tools/analysis/aquifer-markers-probe.sh "
                       << "--accept-eula " << seed);
        REQUIRE(std::filesystem::is_regular_file(dir / "manifest.json"));
        test::requireFrozen(dir, "tools/analysis/aquifer-markers-probe.sh");
        test::requireSeed(dir, seed);
        const nlohmann::json manifest = nlohmann::json::parse(std::ifstream(dir / "manifest.json"));
        const nlohmann::json spec = nlohmann::json::parse(std::ifstream(dir / "spec.json"));
        REQUIRE(spec.size() == group.arms.size());

        for (const Arm& arm : group.arms) {
            INFO("dimension " << arm.name);
            const nlohmann::json* entry = nullptr;
            for (const auto& candidate : spec) {
                if (candidate.at("name").get<std::string>() == arm.name) {
                    entry = &candidate;
                }
            }
            REQUIRE(entry != nullptr);
            // The world on disk is the world this case believes it is.
            REQUIRE(entry->at("default_fluid").at("Name").get<std::string>() ==
                    "minecraft:packed_ice");
            REQUIRE(entry->at("min_y").get<std::int32_t>() == kMinY);
            REQUIRE(entry->at("height").get<std::int32_t>() == kHeight);
            REQUIRE(entry->at("sea_level").get<std::int32_t>() == kSeaLevel);
            REQUIRE(entry->value("size_horizontal", 1) == arm.sizeHorizontal);
            REQUIRE(entry->at("size_vertical").get<std::int32_t>() == 2);
            REQUIRE(entry->at("raw_final_density") ==
                    nlohmann::json{{"type", "minecraft:constant"}, {"argument", kDensity}});
            const nlohmann::json& tested =
                entry->at("router").at(std::string(routerKey(arm.entry)));
            if (arm.marker == Marker::None) {
                REQUIRE((!tested.is_object() || !tested.contains("argument")));
            } else {
                REQUIRE(tested.at("type").get<std::string>() == markerType(arm.marker));
            }

            const std::filesystem::path region = dir / arm.name / "r.0.0.mca";
            REQUIRE(std::filesystem::is_regular_file(region));
            const Dimension dim(*entry, manifest, arm, seed);
            const DimScore s = scoreDimension(dim, region);

            std::ostringstream line;
            line << dir.filename().string() << "/" << arm.name << ": filler " << s.fillerBlocks
                 << " unexplained " << s.fillerUnexplained << " flow " << s.fillerFlow
                 << " server-other " << s.serverOther << " (fluid " << s.serverFluid << " lava "
                 << s.serverLava << " solid " << s.serverSolid << "); scored " << s.blocks
                 << " tie " << s.tie << " top-nonair " << s.topRowsNonAir << " mixed " << s.mixed
                 << " (fluid wrong " << s.mixedFluidWrong << ")";
            for (const auto& [r, rs] : s.readings) {
                line << "\n    " << readingName(r) << ": disc " << rs.discriminating
                     << " reading-right " << rs.readingRight << " (by flow "
                     << rs.readingRightByFlow << ") built-right " << rs.builtRight
                     << " unexplained " << rs.unexplained << " flow " << rs.flow << " mixed "
                     << rs.mixed << " (fluid disc " << rs.fluidDiscriminating << " right "
                     << rs.fluidReadingRight << " built-right " << rs.fluidBuiltRight << ")";
            }
            WARN(line.str());

            // The harness's own as-built reading is the lib's, block for block,
            // and the lib's world is the server's but for fluid that moved —
            // which only the lava arms have, and only a little of.
            CHECK(s.tie == 0);
            CHECK(s.serverOther == 0);
            CHECK(s.topRowsNonAir == 0);
            CHECK(s.fillerUnexplained == 0);
            CHECK(s.fillerFlow <= kFlowBound);
            if (arm.entry != Entry::Lava) {
                CHECK(s.fillerFlow == 0);
            }
            // Mixed lava/default-fluid pairs arise only where the reading
            // under test types neighbouring sources differently — the
            // flat_cache lava arm, whose window splits chunk (0, 0)'s sources
            // — and stay a sliver of it.
            if (arm.entry == Entry::Lava && arm.marker == Marker::FlatCache) {
                CHECK(s.mixed <= kMixedBound);
            } else {
                CHECK(s.mixed == 0);
            }
            // The build IS the measured reading.
            CHECK(s.readings.at(arm.measured).discriminating == 0);
            CHECK(s.readings.at(arm.measured).fluidDiscriminating == 0);
            // On the blocks set aside, the server's fluid is still the build's
            // nearest source's.
            CHECK(s.mixedFluidWrong == 0);
            for (const Rival& rival : arm.rivals) {
                const ReadingScore& rs = s.readings.at(rival.reading);
                INFO("rival " << readingName(rival.reading));
                const long long parts = rs.discriminating + rs.fluidDiscriminating;
                if (rival.floor == 0) {
                    CHECK(parts == 0);
                    continue;
                }
                CHECK(parts >= rival.floor);
                CHECK(rs.readingRight == 0);
                CHECK(rs.fluidReadingRight == 0);
                CHECK(rs.builtRight + rs.fluidBuiltRight + kFlowBound >= parts);
            }
        }
    }
}

} // namespace

TEST_CASE("cache markers in aquifer entries: controls", "[conformance][aquifer]") {
    scoreGroup(groups()[0]);
}

TEST_CASE("cache markers in aquifer entries: interpolated", "[conformance][aquifer]") {
    scoreGroup(groups()[1]);
}

TEST_CASE("cache markers in aquifer entries: cache_all_in_cell", "[conformance][aquifer]") {
    scoreGroup(groups()[2]);
}

TEST_CASE("cache markers in aquifer entries: flat_cache", "[conformance][aquifer]") {
    scoreGroup(groups()[3]);
}

TEST_CASE("cache markers in aquifer entries: cache_once and cache_2d", "[conformance][aquifer]") {
    scoreGroup(groups()[4]);
}
