// Stratum — `aquifers_enabled` false against true, over open ground below a
// raised sea, with ore veins on in both: the flag's own control.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// density-probe.sh writes an entry's `aquifers_enabled` into the noise
// settings it generates, and nothing showed that line reaching the server:
// every aquifers-off arm a case read was a solid column, or left sea_level at
// the dimension's floor with an air default fluid — worlds the flag cannot
// change (SPEC §11, "Ore veins under the legacy source"). And veins with
// aquifers off had been measured over solid columns only.
// tools/analysis/aquifer-flag-probe.sh builds the world where the flag must
// show: open ground below a sea raised to 32, ore veins on with every
// default_block position of both vein ranges a candidate, every other input
// a constant. Two pairs that differ only in the flag:
//
//   mon / moff   the modern source over 3D caves (stratum:probe_noise)
//   lon / loff   the legacy source over a noise-free layering in y
//
// What the worlds say (SPEC §11, "`aquifers_enabled: false` reaches the
// server, pinned"): each off arm is the global picker on every open block of
// its window — lava below min(-54, sea_level), the default fluid below
// sea_level, air above — and each on arm is not, by hundreds of thousands of
// blocks; the shipped filler is each arm's own setting state for state, an
// off arm on every block and an on arm on all but the flow it cannot run,
// each block of which is one step of the server's fluid from a source of
// its own; the veins with aquifers off are exactly the on arm's over solid
// ground, each metal's inside its own range, and the on arm's barriers
// carry veins of their own. Three seeds: 42, 31337 and 8675309.
//
// The fixtures are Mojang-derived and never committed (SPEC §12).
#include "support/fluid_flow.hpp"
#include "support/probe_corpus.hpp"
#include "support/probe_region.hpp"
#include "support/probe_settings.hpp"
#include "support/probe_spec.hpp"
#include "support/temp_path.hpp"

#include <stratum/aquifer/lattice.hpp>
#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/density/random_source.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/ore/vein.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/rule_graph.hpp>
#include <stratum/terrain/filler.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using stratum::test::Category;

/// What regenerates the family, named alike by the SKIP and by a partial set.
constexpr const char* kRegenerate =
    "tools/probe-worlds generate --only aquifer-flag-probe.sh --accept-eula";
/// The seeds tools/probe-worlds generates the family at, in its order.
constexpr std::array<std::int64_t, 3> kSeeds{42, 31337, 8675309};

struct Pair {
    const char* on;
    const char* off;
};

constexpr std::array<Pair, 2> kPairs{{{"mon", "moff"}, {"lon", "loff"}}};
constexpr std::array<const char*, 4> kArms{"mon", "moff", "lon", "loff"};

/// The force-loaded window, in blocks.
constexpr std::int32_t kWindow = 8 * 16;

/// The chunks the shipped filler is scored on: three inside the window and
/// one in the ring around it, all of which ticked (their fluids may have
/// flowed, and their post-processing lists are empty), and two the server
/// generated without ticking (support/probe_region.hpp), whose blocks and
/// post-processing lists are generation's own — neither on a face a ticked
/// chunk's flow can reach.
constexpr std::array<std::pair<std::int32_t, std::int32_t>, 6> kChunks{
    {{1, 1}, {6, 2}, {3, 6}, {8, 5}, {10, 3}, {4, 10}}};

/// Which metal's vein placed a block, if any.
enum class Metal : std::uint8_t { None, Copper, Iron };

/// The three blocks each metal's vein places, and the y-range each metal
/// holds (SPEC §10, M3, "The deterministic gate"): copper in [0, 50], iron
/// in [-60, -8], so neither in the dead zone [-7, -1] between them. The
/// probe's surface rule never fires and its default block is stone, so
/// nothing else in its worlds places any of the six.
constexpr std::array<std::string_view, 3> kCopperVeinBlocks{
    "minecraft:copper_ore", "minecraft:raw_copper_block", "minecraft:granite"};
constexpr std::array<std::string_view, 3> kIronVeinBlocks{
    "minecraft:deepslate_iron_ore", "minecraft:raw_iron_block", "minecraft:tuff"};

struct VeinRange {
    std::int32_t lowest;
    std::int32_t highest;

    [[nodiscard]] constexpr bool contains(std::int32_t y) const {
        return y >= lowest && y <= highest;
    }
};

constexpr VeinRange kCopperRange{.lowest = 0, .highest = 50};
constexpr VeinRange kIronRange{.lowest = -60, .highest = -8};
static_assert(kIronRange.lowest == stratum::ore::kLowestVeinY &&
              kCopperRange.highest == stratum::ore::kHighestVeinY);

[[nodiscard]] std::filesystem::path probes() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11" / "probes";
}

[[nodiscard]] Metal metalOf(std::string_view name) {
    if (std::ranges::find(kCopperVeinBlocks, name) != kCopperVeinBlocks.end()) {
        return Metal::Copper;
    }
    if (std::ranges::find(kIronVeinBlocks, name) != kIronVeinBlocks.end()) {
        return Metal::Iron;
    }
    return Metal::None;
}

[[nodiscard]] bool isVein(const stratum::chunk::BlockState* block) {
    return block != nullptr && metalOf(block->name) != Metal::None;
}

[[nodiscard]] bool isOpen(Category category) {
    return category != Category::Solid;
}

/// What the global picker puts in an open block at @p y: lava below
/// min(-54, sea_level), the default fluid below sea_level, air above (spec
/// Q1.2, Q2.1, Q2.3). The probe's default fluid is water.
[[nodiscard]] Category picker(std::int32_t y, std::int32_t seaLevel) {
    if (y < stratum::aquifer::lambdaLevel(seaLevel)) {
        return Category::Lava;
    }
    return y < seaLevel ? Category::Water : Category::Air;
}

/// The family's corpora, checked: empty when none was generated, a failure
/// when only some were (support/probe_corpus.hpp).
[[nodiscard]] std::vector<std::filesystem::path> corpora() {
    return stratum::test::seededCorpora(probes(), "aqflag_s", kSeeds, kRegenerate);
}

/// One arm's region, REQUIREd present first. Constructed straight from a
/// missing file, GoldenRegion throws, and Catch2 reports the exception
/// without the seed and arm the case was reading.
[[nodiscard]] std::filesystem::path armRegion(const std::filesystem::path& dir, const char* name) {
    const std::filesystem::path path = dir / name / "r.0.0.mca";
    INFO("region " << path);
    REQUIRE(std::filesystem::is_regular_file(path));
    return path;
}

/// The arms are the one recipe but for the flag, each pair over its own
/// terrain and source, below a sea above the floor with veins on.
void requireRecipe(const std::filesystem::path& dir) {
    const nlohmann::json spec = stratum::test::readSpec(dir);
    const auto without = [&](const char* name, std::initializer_list<const char*> keys) {
        nlohmann::json entry = stratum::test::specEntry(spec, dir, name);
        entry.erase("name");
        for (const char* key : keys) {
            entry.erase(key);
        }
        return entry;
    };
    for (const Pair& pair : kPairs) {
        INFO("pair " << pair.on << " / " << pair.off);
        CHECK(without(pair.on, {"aquifers_enabled"}) == without(pair.off, {"aquifers_enabled"}));
        CHECK(stratum::test::specEntry(spec, dir, pair.on).at("aquifers_enabled") == true);
        CHECK(stratum::test::specEntry(spec, dir, pair.off).at("aquifers_enabled") == false);
    }
    // The two pairs part on their terrain and their source, and on nothing
    // the aquifer or the veins read.
    CHECK(without("mon", {"legacy_random_source", "raw_final_density"}) ==
          without("lon", {"legacy_random_source", "raw_final_density"}));
    CHECK(stratum::test::specEntry(spec, dir, "mon").at("legacy_random_source") == false);
    CHECK(stratum::test::specEntry(spec, dir, "lon").at("legacy_random_source") == true);
    const nlohmann::json& mon = stratum::test::specEntry(spec, dir, "mon");
    CHECK(mon.at("ore_veins_enabled") == true);
    CHECK(mon.at("default_fluid").at("Name") == "minecraft:water");
    CHECK(mon.at("default_block").at("Name") == "minecraft:stone");
    // Raised: inside copper's range, so vein blocks meet the sea and the air
    // above it with aquifers off.
    CHECK(mon.at("sea_level").get<std::int32_t>() > 0);
    CHECK(mon.at("sea_level").get<std::int32_t>() < stratum::ore::kHighestVeinY);
}

/// One arm's settings as a data pack on disk: its noise settings as
/// density-probe.sh wrote them (support/probe_settings.hpp) and the probe's
/// noise as its manifest records it. Removed when it goes out of scope.
class ArmPack {
public:
    ArmPack(const nlohmann::json& entry, const std::filesystem::path& corpus)
        : path_(stratum::test::tempPath("stratum-aqflag")),
          name_(entry.at("name").get<std::string>()) {
        std::filesystem::create_directories(path_);
        std::ofstream(path_ / "pack.mcmeta")
            << R"({"pack":{"description":"stratum aquifer-flag replay",)"
               R"("min_format":[94,1],"max_format":94}})";
        write("noise_settings", name_, stratum::test::probeNoiseSettings(entry));
        const nlohmann::json manifest =
            nlohmann::json::parse(std::ifstream(corpus / "manifest.json"));
        const nlohmann::json& declared = manifest.at("probe_noise");
        REQUIRE(declared.at("id").get<std::string>() == "stratum:probe_noise");
        write("noise", "probe_noise",
              {{"firstOctave", declared.at("first_octave")},
               {"amplitudes", declared.at("amplitudes")}});
    }

    ArmPack(const ArmPack&) = delete;
    ArmPack& operator=(const ArmPack&) = delete;
    ArmPack(ArmPack&&) = delete;
    ArmPack& operator=(ArmPack&&) = delete;

    ~ArmPack() {
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

/// The distinct block states a pair's windows hold, each read once: a window
/// holds an index into this per block, so a scan over every block of it
/// compares and classifies small integers rather than block names.
class States {
public:
    /// A position no window holds: outside r.0.0, or past the window's
    /// one-block margin.
    static constexpr std::uint16_t kAbsent = 0xFFFF;

    /// The index of @p state, its properties in sorted order.
    [[nodiscard]] std::uint16_t intern(const stratum::chunk::BlockState& state) {
        stratum::chunk::BlockState key = state;
        std::ranges::sort(key.properties);
        const auto [found, added] =
            ids_.try_emplace(key.toString(), static_cast<std::uint16_t>(categories_.size()));
        if (added) {
            REQUIRE(categories_.size() < kAbsent);
            categories_.push_back(stratum::test::categoryOf(key.name));
            metals_.push_back(metalOf(key.name));
            flowing_.push_back(stratum::test::fluidLevel(&key) > 0);
        }
        return found->second;
    }

    [[nodiscard]] Category category(std::uint16_t id) const { return categories_.at(id); }

    [[nodiscard]] Metal metal(std::uint16_t id) const { return metals_.at(id); }

    [[nodiscard]] bool vein(std::uint16_t id) const { return metal(id) != Metal::None; }

    [[nodiscard]] bool flowing(std::uint16_t id) const { return flowing_.at(id); }

private:
    std::map<std::string, std::uint16_t> ids_;
    std::vector<Category> categories_;
    std::vector<Metal> metals_;
    std::vector<bool> flowing_;
};

/// One arm's force-loaded window and a one-block margin around it, decoded
/// once. The margin's x = -1 and z = -1 lie outside r.0.0 and read as
/// absent, never as air; x = 128 and z = 128 come from the ring's chunks.
class Window {
public:
    Window(stratum::test::GoldenRegion& region, States& states, std::int32_t minY,
           std::int32_t height)
        : minY_(minY), height_(height),
          ids_(static_cast<std::size_t>(kSpan) * kSpan * static_cast<std::size_t>(height),
               States::kAbsent) {
        constexpr std::int32_t kChunksWithMargin = (kWindow / 16) + 1;
        for (std::int32_t chunkZ = 0; chunkZ < kChunksWithMargin; ++chunkZ) {
            for (std::int32_t chunkX = 0; chunkX < kChunksWithMargin; ++chunkX) {
                INFO("chunk (" << chunkX << ", " << chunkZ << ")");
                REQUIRE(region.hasChunk(chunkX, chunkZ));
                for (const stratum::chunk::Section& section :
                     region.chunk(chunkX, chunkZ).sections()) {
                    read(section, chunkX, chunkZ, states);
                }
            }
        }
    }

    [[nodiscard]] std::uint16_t at(std::int32_t x, std::int32_t y, std::int32_t z) const {
        if (x < -1 || x > kWindow || z < -1 || z > kWindow || y < minY_ || y >= minY_ + height_) {
            return States::kAbsent;
        }
        return ids_[index(x, y, z)];
    }

private:
    static constexpr std::int32_t kSpan = kWindow + 2;

    void read(const stratum::chunk::Section& section, std::int32_t chunkX, std::int32_t chunkZ,
              States& states) {
        std::vector<std::uint16_t> palette;
        palette.reserve(section.palette.size());
        for (const stratum::chunk::BlockState& state : section.palette) {
            palette.push_back(states.intern(state));
        }
        for (int localY = 0; localY < 16; ++localY) {
            const std::int32_t y = (section.y * 16) + localY;
            if (y < minY_ || y >= minY_ + height_) {
                continue;
            }
            for (int localZ = 0; localZ < 16; ++localZ) {
                for (int localX = 0; localX < 16; ++localX) {
                    const std::int32_t x = (chunkX * 16) + localX;
                    const std::int32_t z = (chunkZ * 16) + localZ;
                    if (x <= kWindow && z <= kWindow) {
                        // Section::blocks is YZX, a palette index per block.
                        ids_[index(x, y, z)] =
                            palette.at(section.blocks.at((static_cast<std::size_t>(localY) * 256U) +
                                                         (static_cast<std::size_t>(localZ) * 16U) +
                                                         static_cast<std::size_t>(localX)));
                    }
                }
            }
        }
    }

    [[nodiscard]] std::size_t index(std::int32_t x, std::int32_t y, std::int32_t z) const {
        return (((static_cast<std::size_t>(y - minY_) * kSpan) + static_cast<std::size_t>(z + 1)) *
                kSpan) +
               static_cast<std::size_t>(x + 1);
    }

    std::int32_t minY_;
    std::int32_t height_;
    std::vector<std::uint16_t> ids_;
};

/// What the server's window holds for one arm, read off its region alone.
struct WindowCount {
    long long blocks = 0;
    long long open = 0;           ///< air, water or lava, flowing or not
    long long pickerMisses = 0;   ///< open blocks the global picker does not explain
    long long flowing = 0;        ///< fluid with a level above 0
    long long veins = 0;          ///< vein blocks
    long long veinsByFluid = 0;   ///< of those, beside water or lava
    long long veinsByAir = 0;     ///< of those, beside air
    long long copperOffRange = 0; ///< copper-vein blocks outside [0, 50]
    long long ironOffRange = 0;   ///< iron-vein blocks outside [-60, -8]
};

/// Each pair, block for block, over the window.
struct PairCount {
    long long differ = 0;            ///< positions whose states differ
    long long onSolidOverOpen = 0;   ///< solid with aquifers on, open with them off
    long long onVeinOverOpen = 0;    ///< of those, a vein block
    long long offSolidElsewhere = 0; ///< an off arm's solid block the on arm does not hold
};

/// One block of one arm into @p count.
void tally(WindowCount& count, const Window& window, const States& states, std::int32_t x,
           std::int32_t y, std::int32_t z, std::int32_t sea) {
    constexpr std::array<std::array<std::int32_t, 3>, 6> kNeighbours{
        {{1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}, {0, 1, 0}, {0, -1, 0}}};
    ++count.blocks;
    const std::uint16_t id = window.at(x, y, z);
    const Category category = states.category(id);
    if (isOpen(category)) {
        ++count.open;
        count.pickerMisses += static_cast<long long>(category != picker(y, sea));
        count.flowing += static_cast<long long>(states.flowing(id));
        return;
    }
    const Metal metal = states.metal(id);
    if (metal == Metal::None) {
        return;
    }
    ++count.veins;
    // Each metal against its own range, so a block in the dead zone between
    // them, or one metal's block in the other's range, counts here.
    if (metal == Metal::Copper) {
        count.copperOffRange += static_cast<long long>(!kCopperRange.contains(y));
    } else {
        count.ironOffRange += static_cast<long long>(!kIronRange.contains(y));
    }
    bool byFluid = false;
    bool byAir = false;
    for (const auto& [dx, dy, dz] : kNeighbours) {
        const std::uint16_t next = window.at(x + dx, y + dy, z + dz);
        if (next == States::kAbsent) {
            continue;
        }
        byFluid = byFluid || stratum::test::isFluid(states.category(next));
        byAir = byAir || states.category(next) == Category::Air;
    }
    count.veinsByFluid += static_cast<long long>(byFluid);
    count.veinsByAir += static_cast<long long>(byAir);
}

/// What one pair's window holds, both arms read together.
struct PairWindow {
    WindowCount on;
    WindowCount off;
    PairCount both;
};

/// Every block of the window, both arms of a pair at once.
[[nodiscard]] PairWindow countWindow(const Window& on, const Window& off, const States& states,
                                     std::int32_t minY, std::int32_t height, std::int32_t sea) {
    PairWindow count;
    for (std::int32_t y = minY; y < minY + height; ++y) {
        for (std::int32_t z = 0; z < kWindow; ++z) {
            for (std::int32_t x = 0; x < kWindow; ++x) {
                tally(count.on, on, states, x, y, z, sea);
                tally(count.off, off, states, x, y, z, sea);
                const std::uint16_t a = on.at(x, y, z);
                const std::uint16_t b = off.at(x, y, z);
                const bool same = a == b;
                const bool aOpen = isOpen(states.category(a));
                const bool bOpen = isOpen(states.category(b));
                count.both.differ += static_cast<long long>(!same);
                count.both.onSolidOverOpen += static_cast<long long>(!aOpen && bOpen);
                count.both.onVeinOverOpen += static_cast<long long>(states.vein(a) && bOpen);
                count.both.offSolidElsewhere += static_cast<long long>(!bOpen && !same);
            }
        }
    }
    return count;
}

/// One arm's filler score over kChunks.
struct FillScore {
    long long blocks = 0;
    long long exact = 0;
    long long flow = 0;          ///< differing, and shaped like fluid that moved
    long long flowUntouched = 0; ///< of those, in a chunk that never ticked
    long long flowUnfed = 0;     ///< of those, not one step from a source
    long long wrong = 0;         ///< differing, and not
    long long veins = 0;         ///< the server's vein blocks, all of them exact
    long long veinsWrong = 0;    ///< positions where either side holds a vein block, differing
    long long marks = 0;         ///< the server's post-processing marks, untouched chunks
    long long marksExtra = 0;
    long long marksMissing = 0;
    /// The first few blocks counted wrong or unfed, for the failure message:
    /// carried out rather than UNSCOPED_INFO'd, which the next chunk's
    /// passing REQUIRE would consume.
    std::vector<std::string> examples;
};

[[nodiscard]] stratum::chunk::BlockState sorted(stratum::chunk::BlockState state) {
    std::ranges::sort(state.properties);
    return state;
}

/// Whether the server's @p served at (x, y, z) is fluid one step from a
/// source of its own: flowing (a level above 0), with a source of the same
/// fluid directly above it or beside it — fluid falls and spreads, never
/// climbs. What a frozen world's remnant of flow is: measured, of the 24
/// blocks the shared rule forgives over all 144 generated chunks of the six
/// on arms (none on an off arm), 19 are falling (level 8) directly under a
/// source of their own fluid, and 3 level-1 water and 2 level-2 lava lie
/// beside one. A source is never one step of anything, so the shared rule's
/// water source between two others — which would forgive a small pool of
/// wrong water whole — is not this shape.
[[nodiscard]] bool oneStepFromSource(stratum::test::GoldenRegion& region, std::int32_t x,
                                     std::int32_t y, std::int32_t z,
                                     const stratum::chunk::BlockState& served) {
    if (!stratum::test::isFluid(stratum::test::categoryOf(served.name)) ||
        stratum::test::fluidLevel(&served) <= 0) {
        return false;
    }
    constexpr std::array<std::array<std::int32_t, 3>, 5> kFeeders{
        {{1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}, {0, 1, 0}}};
    return std::ranges::any_of(kFeeders, [&](const std::array<std::int32_t, 3>& step) {
        const stratum::chunk::BlockState* next =
            region.blockAt(x + step[0], y + step[1], z + step[2]);
        return stratum::test::named(next, served.name) && stratum::test::fluidLevel(next) == 0;
    });
}

/// The shipped filler, compiled from @p entry the way world::CompiledDimension
/// compiles a dimension, against the arm's region on every chunk of kChunks.
///
/// Only an aquifers-on arm is scored with the shared flow rule. Only an
/// aquifer marks fluid to tick, and the world is frozen, so in an
/// aquifers-off arm nothing moved (measured: no flowing block in any off
/// arm's window, nor in any of its 144 chunks), and every block that differs
/// is wrong. That is what holds the off arms' default fluid below sea_level
/// block for block: under the shared rule a missing water source between two
/// others reads as flow, so a filler that placed no default fluid below
/// sea_level with aquifers off left each legacy off arm 0 blocks wrong, held
/// only by the bounds on flow; scored this way, 75 264.
[[nodiscard]] FillScore scoreFiller(const nlohmann::json& entry, const std::filesystem::path& dir,
                                    std::int64_t seed) {
    const std::string name = entry.at("name").get<std::string>();
    const bool flowPossible = entry.at("aquifers_enabled").get<bool>();
    const ArmPack armPack(entry, dir);
    const stratum::data::Pack pack = armPack.pack();
    const stratum::settings::LoadedSettings loaded = stratum::settings::loadAll(pack);
    const stratum::settings::NoiseSettings& dimension = loaded.settings.at(armPack.settingsId());
    // The source the dimension declares, exactly as world::CompiledDimension
    // chooses it. The legacy arm names no noise, so its registry is empty.
    const stratum::density::RandomSource source = dimension.legacyRandomSource
                                                      ? stratum::density::RandomSource::Legacy
                                                      : stratum::density::RandomSource::Xoroshiro;
    const auto noises = stratum::density::NoiseRegistry::create(
        pack, loaded.graph.referencedNoises(), seed, source);
    const stratum::surface::RuleGraph rules =
        stratum::surface::RuleGraph::resolve(dimension.surfaceRule, armPack.settingsId());
    const stratum::terrain::ChunkFiller filler =
        stratum::terrain::ChunkFiller::compile(loaded.graph, noises, dimension, &rules);
    REQUIRE(filler.runsSurfaceRules());

    const std::filesystem::path path = armRegion(dir, name.c_str());
    stratum::test::GoldenRegion region(path);
    const auto file = stratum::region::RegionFile::open(path);
    const std::int32_t minY = dimension.geometry.minY;
    FillScore score;
    for (const auto& [chunkX, chunkZ] : kChunks) {
        INFO("chunk (" << chunkX << ", " << chunkZ << ")");
        REQUIRE(file.hasChunk(chunkX, chunkZ));
        const auto doc = stratum::nbt::read(file.readChunk(chunkX, chunkZ));
        const std::string& status = doc.root.at("Status").asString();
        const bool ticked = stratum::test::ticked(chunkX, chunkZ);
        REQUIRE((ticked || stratum::test::untouched(chunkX, chunkZ, status)));
        // A buffer per chunk: `fill` rewrites every block of a buffer it is
        // given, but appends to its fluid updates.
        stratum::terrain::ChunkBuffer buffer(dimension.geometry);
        filler.fill(chunkX, chunkZ, buffer);
        if (!ticked) {
            // A ticked chunk empties its list; an untouched one keeps what
            // generation wrote, and the filler writes the same.
            const std::set<stratum::test::LocalPosition> marks =
                stratum::test::postProcessingMarks(doc.root, minY);
            std::set<stratum::test::LocalPosition> ours;
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
        }
        for (int localZ = 0; localZ < 16; ++localZ) {
            for (int localX = 0; localX < 16; ++localX) {
                const std::int32_t x = (chunkX * 16) + localX;
                const std::int32_t z = (chunkZ * 16) + localZ;
                for (std::int32_t y = minY; y < minY + dimension.geometry.height; ++y) {
                    ++score.blocks;
                    const stratum::chunk::BlockState* theirs = region.blockAt(x, y, z);
                    const stratum::chunk::BlockState served =
                        theirs != nullptr
                            ? sorted(*theirs)
                            : stratum::chunk::BlockState{.name = "(absent)", .properties = {}};
                    // A std::map, so its properties come out sorted.
                    const auto& built = buffer.at(localX, y, localZ);
                    stratum::chunk::BlockState ours{.name = built.name.toString(),
                                                    .properties = {}};
                    for (const auto& [key, value] : built.properties) {
                        ours.properties.emplace_back(key, value);
                    }
                    score.veins += static_cast<long long>(isVein(&served));
                    if (served == ours) {
                        ++score.exact;
                        continue;
                    }
                    score.veinsWrong += static_cast<long long>(isVein(&served) || isVein(&ours));
                    const Category servedCategory = stratum::test::categoryOf(served.name);
                    const Category oursCategory = stratum::test::categoryOf(ours.name);
                    const auto example = [&](std::string_view what) {
                        if (score.examples.size() < 8) {
                            score.examples.push_back(
                                std::string{what} + " (" + std::to_string(x) + ", " +
                                std::to_string(y) + ", " + std::to_string(z) + "): server " +
                                served.toString() + ", ours " + ours.toString());
                        }
                    };
                    // The shared rule (support/fluid_flow.hpp), which never
                    // forgives a solid block on either side but the few that
                    // water meeting lava leaves — so never a vein block —
                    // and, on top of it, the shape a frozen world's flow has.
                    if (flowPossible && stratum::test::explainedByFlow(
                                            region, x, y, z, servedCategory, oursCategory)) {
                        ++score.flow;
                        score.flowUntouched += static_cast<long long>(!ticked);
                        if (!oneStepFromSource(region, x, y, z, served)) {
                            ++score.flowUnfed;
                            example("flow not one step from a source");
                        }
                        continue;
                    }
                    ++score.wrong;
                    example("wrong");
                }
            }
        }
    }
    return score;
}

} // namespace

TEST_CASE("aquifers_enabled reaches the server: off is the global picker and on is not",
          "[conformance][aquifer][ore]") {
    // Library-free: the server's own regions, read against the global
    // picker's three rows and against each other. Each pair differs only in
    // the flag (requireRecipe), so whatever separates them, the flag did.
    const std::vector<std::filesystem::path> dirs = corpora();
    if (dirs.empty()) {
        SKIP("no aquifer-flag probe under " << probes() << "; generate it with " << kRegenerate);
    }
    for (std::size_t i = 0; i < kSeeds.size(); ++i) {
        const std::filesystem::path& dir = dirs.at(i);
        INFO("seed " << kSeeds.at(i));
        stratum::test::requireFrozen(dir, kRegenerate);
        requireRecipe(dir);
        const nlohmann::json& mon = stratum::test::specEntry(dir, "mon");
        const auto sea = mon.at("sea_level").get<std::int32_t>();
        const auto minY = mon.at("min_y").get<std::int32_t>();
        const auto height = mon.at("height").get<std::int32_t>();
        for (const Pair& pair : kPairs) {
            INFO("pair " << pair.on << " / " << pair.off);
            stratum::test::GoldenRegion onRegion(armRegion(dir, pair.on));
            stratum::test::GoldenRegion offRegion(armRegion(dir, pair.off));
            States states;
            const Window on(onRegion, states, minY, height);
            const Window off(offRegion, states, minY, height);
            const PairWindow count = countWindow(on, off, states, minY, height, sea);
            const WindowCount& onCount = count.on;
            const WindowCount& offCount = count.off;
            const PairCount& both = count.both;
            INFO("on: open " << onCount.open << ", picker misses " << onCount.pickerMisses
                             << ", flowing " << onCount.flowing << ", veins " << onCount.veins
                             << " (" << onCount.veinsByFluid << " beside fluid, "
                             << onCount.veinsByAir << " beside air; out of range: copper "
                             << onCount.copperOffRange << ", iron " << onCount.ironOffRange
                             << ") of " << onCount.blocks);
            INFO("off: open " << offCount.open << ", picker misses " << offCount.pickerMisses
                              << ", flowing " << offCount.flowing << ", veins " << offCount.veins
                              << " (" << offCount.veinsByFluid << " beside fluid, "
                              << offCount.veinsByAir << " beside air; out of range: copper "
                              << offCount.copperOffRange << ", iron " << offCount.ironOffRange
                              << ") of " << offCount.blocks);
            INFO("pair: differ " << both.differ << ", on solid over open " << both.onSolidOverOpen
                                 << " (veins " << both.onVeinOverOpen << "), off solid elsewhere "
                                 << both.offSolidElsewhere);

            // With aquifers off, the global picker and nothing else: every
            // open block of the window, at every height, is its row's lava,
            // water or air, and none of it moved.
            CHECK(offCount.pickerMisses == 0);
            CHECK(offCount.flowing == 0);
            // With them on, the lattice: the positive control that the flag
            // reached the world. Measured 755 404-910 599 picker misses of
            // the window's 6 291 456 blocks, and 843 148-1 025 059 blocks
            // between the twins, over the three seeds and both pairs.
            CHECK(onCount.pickerMisses > 500000);
            CHECK(both.differ > 500000);
            // What moved after generation, on the lattice's side only
            // (measured 1-6 blocks).
            CHECK(onCount.flowing <= 32);
            // The aquifer only ever adds solid — its barriers — and veins
            // read no aquifer: over the off arm's solid ground the on arm
            // holds the same block, vein blocks included, so the on arm's
            // only other vein blocks stand on its barriers (measured
            // 33 475-38 403 of 79 901-114 459 barrier blocks).
            CHECK(both.offSolidElsewhere == 0);
            CHECK(onCount.veins - offCount.veins == both.onVeinOverOpen);
            CHECK(both.onSolidOverOpen > 50000);
            CHECK(both.onVeinOverOpen > 20000);
            // Veins over open ground, each metal inside its own range and
            // none in the dead zone between: beside the fluid and beside the
            // air of both arms (measured, of 527 203-628 915 vein blocks an
            // arm: on, 70 248-84 523 beside fluid and 60 503-93 354 beside
            // air; off, 84 241-126 185 and 18 479-23 038; none outside its
            // metal's range).
            for (const WindowCount* arm : {&onCount, &offCount}) {
                CHECK(arm->copperOffRange == 0);
                CHECK(arm->ironOffRange == 0);
                CHECK(arm->veins > 400000);
                CHECK(arm->veinsByFluid > 50000);
                CHECK(arm->veinsByAir > 10000);
            }
        }
    }
}

TEST_CASE("the shipped filler generates the aquifer-flag probe block for block",
          "[conformance][aquifer][ore]") {
    // Each arm's own settings, rebuilt from its spec.json the way
    // density-probe.sh wrote them, through the registry its source selects
    // and ChunkFiller::compile, against the server state for state — an off
    // arm on every block, an on arm's fluid flow classified by the shared
    // rule, held to one step from a source, counted and bounded — and every
    // vein block exact. Measured on all 144 chunks the server generated of
    // every arm at every seed (14 155 776 blocks an arm): not one block
    // wrong; 1-9 blocks of flow an on arm, all in ticked chunks and each one
    // step from a source of its own fluid, none on an off arm; every
    // post-processing mark of the 63 untouched chunks exact (28 246-46 386
    // an on arm, none an off arm). The case reads six chunks an arm, where
    // the filler is the slow part; none of the 24 blocks of flow is in them.
    const std::vector<std::filesystem::path> dirs = corpora();
    if (dirs.empty()) {
        SKIP("no aquifer-flag probe under " << probes() << "; generate it with " << kRegenerate);
    }
    for (std::size_t i = 0; i < kSeeds.size(); ++i) {
        const std::int64_t seed = kSeeds.at(i);
        const std::filesystem::path& dir = dirs.at(i);
        INFO("seed " << seed);
        stratum::test::requireFrozen(dir, kRegenerate);
        const nlohmann::json spec = stratum::test::readSpec(dir);
        for (const char* name : kArms) {
            INFO("arm " << name);
            const nlohmann::json& entry = stratum::test::specEntry(spec, dir, name);
            const bool aquifers = entry.at("aquifers_enabled").get<bool>();
            const FillScore score = scoreFiller(entry, dir, seed);
            INFO("exact " << score.exact << ", flow " << score.flow << " (" << score.flowUntouched
                          << " untouched, " << score.flowUnfed << " not one step from a source)"
                          << ", wrong " << score.wrong << " of " << score.blocks << "; vein blocks "
                          << score.veins << ", vein positions wrong " << score.veinsWrong
                          << "; marks " << score.marks << " (extra " << score.marksExtra
                          << ", missing " << score.marksMissing << ")");
            std::string examples;
            for (const std::string& line : score.examples) {
                examples += "\n  " + line;
            }
            INFO("first blocks wrong or unfed:" << (examples.empty() ? " none" : examples));
            CHECK(score.wrong == 0);
            CHECK(score.exact + score.flow == score.blocks);
            CHECK(score.veinsWrong == 0);
            CHECK(score.veins > 10000);
            CHECK(score.marksExtra == 0);
            CHECK(score.marksMissing == 0);
            if (aquifers) {
                CHECK(score.marks > 0);
                // Only a chunk that ticked can hold moved fluid; and what
                // moved is fluid that took one step from a source of its
                // own, never a source, so never a pool of wrong water.
                CHECK(score.flowUntouched == 0);
                CHECK(score.flowUnfed == 0);
                CHECK(score.flow <= 32);
            } else {
                // Scored with no flow rule (scoreFiller): every block exact,
                // the default fluid below sea_level included.
                CHECK(score.marks == 0);
                CHECK(score.exact == score.blocks);
            }
        }
    }
}
