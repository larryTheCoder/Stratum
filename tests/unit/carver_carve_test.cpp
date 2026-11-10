// Stratum — carveChunk against a recording target: what the carvers ask of
// the generator, and what a pack's debug mode writes.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The carvers' output is confirmed on the server's own carving stage
// (tests/conformance/vanilla_carver_golden_test.cpp and
// vanilla_carver_variants_test.cpp). These cases pin, on a synthetic world,
// the parts of spec/carvers-a-spec.md a block comparison cannot single out
// (IMPLEMENTER-RULING-01, carvers P2: "implement these exactly, even though
// tests could not observe them") — the start-chunk order and the biome's
// sampling position (Q11.1, Q11.2) and the mask (Q4.4) — and the debug
// fill of Q13.4, answer by answer.
#include <stratum/carver/carve.hpp>
#include <stratum/carver/config.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/settings/noise_settings.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using stratum::carver::AquiferAnswer;
using stratum::carver::CarverSet;
using stratum::carver::CarverSources;
using stratum::carver::CarveTarget;
using stratum::data::ResourceLocation;
using stratum::settings::BlockState;

namespace {

constexpr std::int32_t kMinY = -64;
constexpr std::int32_t kHeight = 384;
constexpr std::int32_t kChunkX = 3;
constexpr std::int32_t kChunkZ = -2;
constexpr std::int64_t kSeed = 42;

// The synthetic world: obsidian (which no carver here may replace) in
// [30, 40), stone elsewhere below 100, air above.
constexpr std::int32_t kObsidianLow = 30;
constexpr std::int32_t kObsidianHigh = 40;
constexpr std::int32_t kSurface = 100;

// The synthetic aquifer, by height: water (flag set) below -20, lava to 0,
// the barrier verdict (flag set) to 20, air above.
[[nodiscard]] AquiferAnswer aquiferAt(const std::int32_t y) {
    if (y < -20) {
        return {.kind = AquiferAnswer::Kind::Water, .flag = true};
    }
    if (y < 0) {
        return {.kind = AquiferAnswer::Kind::Lava, .flag = false};
    }
    if (y < 20) {
        return {.kind = AquiferAnswer::Kind::Solid, .flag = true};
    }
    return {.kind = AquiferAnswer::Kind::Air, .flag = false};
}

[[nodiscard]] BlockState named(const char* name) {
    return BlockState{.name = ResourceLocation::parse(name), .properties = {}};
}

/// A cave (or nether cave) that starts in every chunk, replacing stone only.
[[nodiscard]] nlohmann::json carverJson(const char* type, const nlohmann::json& debug) {
    nlohmann::json json = nlohmann::json::parse(R"({"config": {
        "probability": 1.0,
        "y": {"type": "minecraft:uniform", "min_inclusive": {"above_bottom": 8},
              "max_inclusive": {"absolute": 90}},
        "yScale": 0.5,
        "lava_level": {"above_bottom": 8},
        "replaceable": ["minecraft:stone"],
        "horizontal_radius_multiplier": 1.0,
        "vertical_radius_multiplier": 1.0,
        "floor_level": -0.7}})");
    json["type"] = type;
    if (!debug.is_null()) {
        json["config"]["debug_settings"] = debug;
    }
    return json;
}

[[nodiscard]] CarverSet carverSet(const nlohmann::json& carver) {
    CarverSources sources;
    sources.configured.emplace(ResourceLocation::parse("stratum:carver"), carver);
    sources.biomeCarvers.emplace(ResourceLocation::parse("stratum:biome"),
                                 nlohmann::json::array({"stratum:carver"}));
    return stratum::carver::buildCarverSet(sources);
}

/// One chunk of the synthetic world, and everything the carvers did to it.
class Recording {
public:
    Recording() : cells_(static_cast<std::size_t>(kHeight) * 256U) {
        for (std::int32_t y = kMinY; y < kMinY + kHeight; ++y) {
            BlockState state = named("minecraft:air");
            if (y >= kObsidianLow && y < kObsidianHigh) {
                state = named("minecraft:obsidian");
            } else if (y < kSurface) {
                state = named("minecraft:stone");
            }
            for (int i = 0; i < 256; ++i) {
                cells_[(static_cast<std::size_t>(y - kMinY) * 256U) + static_cast<std::size_t>(i)] =
                    state;
            }
        }
        reads_.assign(cells_.size(), 0);
    }

    [[nodiscard]] CarveTarget target() {
        CarveTarget target;
        target.chunkX = kChunkX;
        target.chunkZ = kChunkZ;
        target.worldSeed = kSeed;
        target.geometry = stratum::settings::NoiseGeometry{
            .minY = kMinY, .height = kHeight, .sizeHorizontal = 1, .sizeVertical = 2};
        target.biomeAt = [this](std::int32_t x, std::int32_t z) -> const ResourceLocation& {
            static const ResourceLocation kBiome = ResourceLocation::parse("stratum:biome");
            biomeAsked.emplace_back(x, z);
            return kBiome;
        };
        target.blockAt = [this](int lx, std::int32_t y, int lz) -> const ResourceLocation& {
            ++reads_[index(lx, y, lz)];
            return cells_[index(lx, y, lz)].name;
        };
        target.write = [this](int lx, std::int32_t y, int lz, CarveTarget::Write state) {
            ++writes;
            switch (state) {
                case CarveTarget::Write::Air:
                    cells_[index(lx, y, lz)] = named("minecraft:air");
                    break;
                case CarveTarget::Write::CaveAir:
                    cells_[index(lx, y, lz)] = named("minecraft:cave_air");
                    break;
                case CarveTarget::Write::Lava:
                    cells_[index(lx, y, lz)] = named("minecraft:lava");
                    break;
                case CarveTarget::Write::Water:
                default:
                    cells_[index(lx, y, lz)] = named("minecraft:water");
                    break;
            }
        };
        target.writeState = [this](int lx, std::int32_t y, int lz, const BlockState& state) {
            ++writes;
            ++stateWrites;
            cells_[index(lx, y, lz)] = state;
        };
        target.aquifer = [this](std::int32_t x, std::int32_t y, std::int32_t z) {
            // World coordinates, inside this chunk.
            CHECK(floor16(x) == kChunkX);
            CHECK(floor16(z) == kChunkZ);
            ++aquiferAsked;
            return aquiferAt(y);
        };
        target.topMaterial = [](int, std::int32_t, int, bool) { return false; };
        target.mark = [this](int lx, std::int32_t y, int lz) { marks.emplace_back(lx, y, lz); };
        return target;
    }

    [[nodiscard]] const BlockState& at(int lx, std::int32_t y, int lz) const {
        return cells_[index(lx, y, lz)];
    }

    /// The most times any one cell's block was read.
    [[nodiscard]] int mostReads() const {
        int most = 0;
        for (const int count : reads_) {
            most = count > most ? count : most;
        }
        return most;
    }

    /// How many cells in [yLow, yHigh) hold a block named @p name.
    [[nodiscard]] long long count(const char* name, std::int32_t yLow, std::int32_t yHigh) const {
        const ResourceLocation id = ResourceLocation::parse(name);
        long long total = 0;
        for (std::int32_t y = yLow; y < yHigh; ++y) {
            for (int i = 0; i < 256; ++i) {
                total += at(i % 16, y, i / 16).name == id ? 1 : 0;
            }
        }
        return total;
    }

    std::vector<std::pair<std::int32_t, std::int32_t>> biomeAsked;
    std::vector<std::tuple<int, std::int32_t, int>> marks;
    long long writes = 0;
    long long stateWrites = 0;
    long long aquiferAsked = 0;

private:
    [[nodiscard]] static std::int32_t floor16(const std::int32_t value) {
        return value >= 0 ? value / 16 : -((-value + 15) / 16);
    }

    [[nodiscard]] static std::size_t index(int lx, std::int32_t y, int lz) {
        REQUIRE((lx >= 0 && lx < 16 && lz >= 0 && lz < 16 && y >= kMinY && y < kMinY + kHeight));
        return (static_cast<std::size_t>(y - kMinY) * 256U) + (static_cast<std::size_t>(lz) * 16U) +
               static_cast<std::size_t>(lx);
    }

    std::vector<BlockState> cells_;
    std::vector<int> reads_;
};

} // namespace

TEST_CASE("a chunk's carving asks each start chunk's biome once, in window order, at y 0's quart",
          "[carver][carve]") {
    // A-Q11.1: Sx - Tx outer, Sz - Tz inner, each -8..8. A-Q11.2: the biome
    // at quart (4 Sx, 0, 4 Sz), which the target is asked for by that
    // quart's lowest block corner (16 Sx, 16 Sz); the pipeline's own
    // callback samples it at y 0.
    Recording world;
    const CarverSet carvers = carverSet(carverJson("minecraft:cave", nullptr));
    stratum::carver::carveChunk(carvers, world.target());

    std::vector<std::pair<std::int32_t, std::int32_t>> expected;
    for (std::int32_t dx = -8; dx <= 8; ++dx) {
        for (std::int32_t dz = -8; dz <= 8; ++dz) {
            expected.emplace_back((kChunkX + dx) * 16, (kChunkZ + dz) * 16);
        }
    }
    CHECK(world.biomeAsked == expected);
}

TEST_CASE("the mask decides a cell once; replaceability and the barrier leave it alone",
          "[carver][carve]") {
    // A-Q4.4: a decided cell is never decided again, whatever it then
    // holds. With a start in every chunk of the window the ellipsoids
    // overlap heavily, and no cell's block is read twice (this world has no
    // grass, so the fill reads nothing but the cell it decides).
    Recording world;
    stratum::carver::carveChunk(carverSet(carverJson("minecraft:cave", nullptr)), world.target());

    CHECK(world.writes > 0);
    CHECK(world.mostReads() == 1);
    CHECK(world.stateWrites == 0);
    // A-Q12.1: not replaceable, unchanged; the barrier verdict, unchanged.
    CHECK(world.count("minecraft:obsidian", kObsidianLow, kObsidianHigh) == 256LL * 10);
    CHECK(world.count("minecraft:stone", 0, 20) == 256LL * 20);
    // Each answer, where the synthetic aquifer gives it; lava at or below
    // the lava height (y_min + 8), the aquifer not asked.
    CHECK(world.count("minecraft:air", 40, kSurface) > 0);
    CHECK(world.count("minecraft:lava", -20, 0) > 0);
    CHECK(world.count("minecraft:water", -55, -20) > 0);
    CHECK(world.count("minecraft:water", kMinY, -55) == 0);
    CHECK(world.count("minecraft:lava", kMinY, -55) > 0);
    // A-Q12.4: every water cell is marked (its own decision's flag is set).
    long long waterMarks = 0;
    for (const auto& [lx, y, lz] : world.marks) {
        waterMarks +=
            world.at(lx, y, lz).name == ResourceLocation::parse("minecraft:water") ? 1 : 0;
    }
    CHECK(waterMarks == world.count("minecraft:water", -55, -20));
}

TEST_CASE("a cave's debug mode lifts the mask and replaceability and writes the debug states",
          "[carver][carve][debug]") {
    // A-Q13.4, confirmed on probes/carverdebug_s42.
    const nlohmann::json debug = {
        {"debug_mode", true},
        {"air_state", {{"Name", "minecraft:red_stained_glass"}}},
        {"water_state",
         {{"Name", "minecraft:oak_slab"},
          {"Properties", {{"type", "bottom"}, {"waterlogged", "false"}}}}},
        {"lava_state", {{"Name", "minecraft:magenta_wool"}}},
        {"barrier_state", {{"Name", "minecraft:glass"}}}};

    Recording plain;
    stratum::carver::carveChunk(carverSet(carverJson("minecraft:cave", nullptr)), plain.target());
    Recording world;
    stratum::carver::carveChunk(carverSet(carverJson("minecraft:cave", debug)), world.target());

    // The mask lifted: cells are decided again.
    CHECK(world.mostReads() > 1);
    CHECK(world.writes > plain.writes);
    // Replaceability ignored: obsidian is carved, to the air state.
    CHECK(world.count("minecraft:obsidian", kObsidianLow, kObsidianHigh) < 256LL * 10);
    CHECK(world.count("minecraft:red_stained_glass", kObsidianLow, kObsidianHigh) > 0);
    // Each answer's state, and nothing else, in its band.
    CHECK(world.count("minecraft:red_stained_glass", 40, kSurface) > 0);
    CHECK(world.count("minecraft:air", kMinY, kSurface) == 0);
    CHECK(world.count("minecraft:glass", 0, 20) > 0);
    CHECK(world.count("minecraft:glass", 0, 20) + world.count("minecraft:stone", 0, 20) ==
          256LL * 20);
    CHECK(world.count("minecraft:magenta_wool", -20, 0) > 0);
    CHECK(world.count("minecraft:lava", -55, kSurface) == 0);
    const long long slabs = world.count("minecraft:oak_slab", -55, -20);
    CHECK(slabs > 0);
    CHECK(world.count("minecraft:water", kMinY, kSurface) == 0);
    // At or below the lava height: plain lava still, no debug state.
    CHECK(world.count("minecraft:lava", kMinY, -55) > 0);
    CHECK(world.count("minecraft:oak_slab", kMinY, -55) == 0);
    CHECK(world.count("minecraft:magenta_wool", kMinY, -55) == 0);

    // The water state is written waterlogged, its other properties kept,
    // and — holding fluid — marked on its decision's flag.
    const std::set<std::tuple<int, std::int32_t, int>> marked(world.marks.begin(),
                                                              world.marks.end());
    long long slabsMarked = 0;
    for (std::int32_t y = -55; y < -20; ++y) {
        for (int i = 0; i < 256; ++i) {
            const BlockState& state = world.at(i % 16, y, i / 16);
            if (state.name == ResourceLocation::parse("minecraft:oak_slab")) {
                CHECK(state.properties.at("waterlogged") == "true");
                CHECK(state.properties.at("type") == "bottom");
                slabsMarked += marked.contains({i % 16, y, i / 16}) ? 1 : 0;
            }
        }
    }
    CHECK(slabsMarked == slabs);
    // The barrier state holds no fluid: unmarked, though its flag is set.
    for (const auto& [lx, y, lz] : world.marks) {
        CHECK(world.at(lx, y, lz).name != ResourceLocation::parse("minecraft:glass"));
    }

    SECTION("a water state with no waterlogged property holds no fluid, and is not marked") {
        nlohmann::json dry = debug;
        dry["water_state"] = {{"Name", "minecraft:blue_wool"}};
        Recording other;
        stratum::carver::carveChunk(carverSet(carverJson("minecraft:cave", dry)), other.target());
        CHECK(other.count("minecraft:blue_wool", -55, -20) == slabs);
        for (const auto& [lx, y, lz] : other.marks) {
            CHECK(other.at(lx, y, lz).name != ResourceLocation::parse("minecraft:blue_wool"));
        }
    }
    SECTION("debug_mode off writes none of it") {
        nlohmann::json off = debug;
        off["debug_mode"] = false;
        Recording other;
        stratum::carver::carveChunk(carverSet(carverJson("minecraft:cave", off)), other.target());
        CHECK(other.stateWrites == 0);
        CHECK(other.writes == plain.writes);
        CHECK(other.marks == plain.marks);
    }
}

TEST_CASE("a nether cave's debug mode lifts only the mask, which changes nothing",
          "[carver][carve][debug]") {
    // A-Q13.4: replaceability stands, the fill is A-Q12.6's, and a cell
    // decided again holds cave air or lava, neither replaceable.
    const nlohmann::json debug = {{"debug_mode", true},
                                  {"air_state", {{"Name", "minecraft:red_stained_glass"}}}};
    Recording plain;
    stratum::carver::carveChunk(carverSet(carverJson("minecraft:nether_cave", nullptr)),
                                plain.target());
    Recording world;
    stratum::carver::carveChunk(carverSet(carverJson("minecraft:nether_cave", debug)),
                                world.target());

    CHECK(plain.writes > 0);
    CHECK(plain.mostReads() == 1);
    CHECK(world.mostReads() > 1);
    CHECK(world.writes == plain.writes);
    CHECK(world.stateWrites == 0);
    CHECK(world.aquiferAsked == 0);
    CHECK(world.marks.empty());
    CHECK(world.count("minecraft:obsidian", kObsidianLow, kObsidianHigh) == 256LL * 10);
    for (std::int32_t y = kMinY; y < kMinY + kHeight; ++y) {
        for (int i = 0; i < 256; ++i) {
            REQUIRE(world.at(i % 16, y, i / 16) == plain.at(i % 16, y, i / 16));
        }
    }
    // A-Q12.6: lava at or below y_min + 31, cave air above.
    CHECK(world.count("minecraft:lava", kMinY, kMinY + 32) > 0);
    CHECK(world.count("minecraft:lava", kMinY + 32, kMinY + kHeight) == 0);
    CHECK(world.count("minecraft:cave_air", kMinY + 32, kSurface) > 0);
}

TEST_CASE("the debug states a carver set can write are listed, as written", "[carver][debug]") {
    const nlohmann::json debug = {
        {"debug_mode", true},
        {"air_state", {{"Name", "minecraft:red_stained_glass"}}},
        {"water_state",
         {{"Name", "minecraft:oak_slab"},
          {"Properties", {{"type", "bottom"}, {"waterlogged", "false"}}}}},
        {"lava_state", {{"Name", "minecraft:red_stained_glass"}}}};

    const std::vector<BlockState> states =
        stratum::carver::debugStates(carverSet(carverJson("minecraft:cave", debug)));
    // Air, water (waterlogged), lava (the air state again, listed once), and
    // the barrier state left out: the button.
    REQUIRE(states.size() == 3);
    CHECK(states[0] == named("minecraft:red_stained_glass"));
    CHECK(states[1].name == ResourceLocation::parse("minecraft:oak_slab"));
    CHECK(states[1].properties.at("waterlogged") == "true");
    CHECK(states[2].name == ResourceLocation::parse("minecraft:acacia_button"));

    // Debug off, and a nether cave's debug mode, write none.
    nlohmann::json off = debug;
    off["debug_mode"] = false;
    CHECK(stratum::carver::debugStates(carverSet(carverJson("minecraft:cave", off))).empty());
    CHECK(stratum::carver::debugStates(carverSet(carverJson("minecraft:nether_cave", debug)))
              .empty());
    CHECK(stratum::carver::debugStates(carverSet(carverJson("minecraft:cave", nullptr))).empty());
}
