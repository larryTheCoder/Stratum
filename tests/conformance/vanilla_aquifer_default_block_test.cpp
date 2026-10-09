// Stratum — the aquifer's barrier is the preset's default_block (spec Q6.7).
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Q6.7: where the aquifer decides "solid" against a non-positive density, the
// block written is the preset's own `default_block`, the same block positive
// density writes, properties and all. `ChunkFiller` does that, but nothing
// could tell it from a filler that wrote literal stone: every probe world ran
// stone, and the vanilla presets with another default block have aquifers
// off.
//
// `tools/analysis/aquifer-defaultblock-probe.sh` builds one world of four
// dimensions on `aquifer-barrier-probe.sh`'s `d_neg1_0` recipe (constant
// density -1, vanilla's barrier noises, no lava anywhere, a surface rule that
// never fires): the stone control, netherrack, deepslate[axis=x], and
// deepslate[axis=x] at density +1. Every solid block in the first three is a
// barrier, and with no lava nothing that flows can make or unmake one, so
// every solid count here is exact on any regeneration; only non-solid blocks
// (air, water, a water level) may differ between runs (SPEC §7), and those
// are bounded.
//
// The first case reads the server alone: each treatment's solids are its
// default_block at exactly the control's stone positions. The second ties
// the control to barrier3way's d_neg1_0, the same recipe in another world.
// The third runs the shipped filler over the corpus's own spec and scores it
// state for state against the server. About 12 s, 6 s and 75 s in a Debug
// build, the last almost all the filler itself.
//
// The fixtures are Mojang-derived and never committed (SPEC §12); without the
// corpus each case SKIPs, naming the script that produces it.
#include "support/fluid_flow.hpp"
#include "support/probe_corpus.hpp"
#include "support/probe_settings.hpp"
#include "support/probe_spec.hpp"

#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/registry.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/graph.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/terrain/filler.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace stratum;
using test::Category;

constexpr std::int64_t kSeed = 42;
constexpr std::int32_t kChunks = 8;
constexpr std::int32_t kMinY = -48;
constexpr std::int32_t kTopY = 271; // MIN_Y + HEIGHT - 1 in the script
constexpr long long kBlocks = 16LL * 16LL * kChunks * kChunks * (kTopY - kMinY + 1);
/// The barrier blocks of each density -1 dimension at seed 42, measured.
/// Pinned exactly: the server's generation alone decides them, and no flow
/// can move a solid in a world without lava. barrier3way's d_neg1_0, the
/// same recipe in another world, holds the same 4110.
constexpr long long kBarriers = 4110;
constexpr std::string_view kScript = "tools/analysis/aquifer-defaultblock-probe.sh";

[[nodiscard]] std::filesystem::path fixtures() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11";
}

[[nodiscard]] std::filesystem::path probeDir() {
    return fixtures() / "probes" / "defaultblock_s42";
}

struct Arm {
    std::string_view name;
    double density;
    chunk::BlockState block;
};

/// The script's four dimensions, as it writes them. The first is the control.
[[nodiscard]] const std::array<Arm, 4>& arms() {
    static const std::array<Arm, 4> kArms{
        Arm{.name = "db_stone", .density = -1.0, .block = {"minecraft:stone", {}}},
        Arm{.name = "db_netherrack", .density = -1.0, .block = {"minecraft:netherrack", {}}},
        Arm{.name = "db_deepslate_x",
            .density = -1.0,
            .block = {"minecraft:deepslate", {{"axis", "x"}}}},
        Arm{.name = "db_solid_deepslate_x",
            .density = 1.0,
            .block = {"minecraft:deepslate", {{"axis", "x"}}}}};
    return kArms;
}

[[nodiscard]] std::optional<nlohmann::json> loadSpec() {
    if (!std::filesystem::exists(probeDir() / "spec.json")) {
        return std::nullopt;
    }
    return test::readSpec(probeDir());
}

/// A block state as a spec writes one: {"Name": ..., "Properties": {...}}.
[[nodiscard]] nlohmann::json specState(const chunk::BlockState& state) {
    nlohmann::json out{{"Name", state.name}};
    if (!state.properties.empty()) {
        out["Properties"] = nlohmann::json::object();
        for (const auto& [key, value] : state.properties) {
            out["Properties"][key] = value;
        }
    }
    return out;
}

/// The corpus is the experiment the script describes: the four entries in
/// order, each the control's object but for its name, its default_block and,
/// for the last, its density. Checked rather than assumed, because a
/// treatment that differed in anything else would not be a treatment.
void requireDesign(const nlohmann::json& spec) {
    REQUIRE(spec.is_array());
    REQUIRE(spec.size() == arms().size());
    nlohmann::json control = spec.at(0);
    control.erase("name");
    control.erase("default_block");
    control.erase("raw_final_density");
    for (std::size_t i = 0; i < arms().size(); ++i) {
        const Arm& arm = arms().at(i);
        const nlohmann::json& entry = spec.at(i);
        INFO("spec entry " << i);
        REQUIRE(entry.at("name").get<std::string>() == arm.name);
        CHECK(entry.at("default_block") == specState(arm.block));
        CHECK(entry.at("raw_final_density") ==
              nlohmann::json{{"type", "minecraft:constant"}, {"argument", arm.density}});
        nlohmann::json rest = entry;
        rest.erase("name");
        rest.erase("default_block");
        rest.erase("raw_final_density");
        CHECK(rest == control);
    }
    CHECK(control.at("aquifers_enabled") == true);
    CHECK(control.at("min_y") == kMinY);
    CHECK(control.at("router").at("lava") == 0.0);
}

[[nodiscard]] const chunk::BlockState& blockOrAir(const chunk::BlockState* block) {
    static const chunk::BlockState kAir{"minecraft:air", {}};
    return block == nullptr ? kAir : *block;
}

[[nodiscard]] bool solid(const chunk::BlockState& block) {
    return test::categoryOf(block.name) == Category::Solid;
}

/// `id` spelled `namespace:path` is @p name, without building the string.
[[nodiscard]] bool namedAs(const data::ResourceLocation& id, const std::string& name) {
    const std::string& space = id.namespaceName();
    const std::string& path = id.path();
    return name.size() == space.size() + 1 + path.size() &&
           name.compare(0, space.size(), space) == 0 && name[space.size()] == ':' &&
           name.compare(space.size() + 1, path.size(), path) == 0;
}

/// The whole state, name and every property.
[[nodiscard]] bool sameState(const settings::BlockState& ours, const chunk::BlockState& served) {
    if (!namedAs(ours.name, served.name) || ours.properties.size() != served.properties.size()) {
        return false;
    }
    for (const auto& [key, value] : served.properties) {
        const auto found = ours.properties.find(key);
        if (found == ours.properties.end() || found->second != value) {
            return false;
        }
    }
    return true;
}

/// How one treatment's blocks sit against the control's, position by
/// position. `atBarrier` counts the treatment's own default_block where the
/// control has stone; every other solid outcome is a separate reading.
struct Versus {
    long long atBarrier = 0;
    long long literalStone = 0;      // stone where the control has stone
    long long otherMaterial = 0;     // any other solid there (bare deepslate...)
    long long openAtBarrier = 0;     // not solid where the control has stone
    long long solidElsewhere = 0;    // solid where the control is not
    long long nonSolidDiffering = 0; // air, water, a water level: flow, bounded
};

} // namespace

TEST_CASE("the server writes the preset's default_block at every aquifer barrier",
          "[conformance][aquifer]") {
    const std::optional<nlohmann::json> spec = loadSpec();
    if (!spec.has_value()) {
        SKIP("no default-block probe under " << probeDir() << "; generate it with " << kScript
                                             << " --accept-eula 42");
    }
    test::requireFrozen(probeDir(), kScript);
    test::requireSeed(probeDir(), kSeed);
    requireDesign(*spec);

    std::vector<region::RegionFile> files;
    for (const Arm& arm : arms()) {
        files.push_back(region::RegionFile::open(probeDir() / arm.name / "r.0.0.mca"));
    }
    long long controlStone = 0;
    long long controlOtherSolid = 0;
    std::array<Versus, 2> versus{}; // netherrack, deepslate[axis=x]
    long long solidArmExact = 0;
    long long solidArmOther = 0;
    for (std::int32_t cz = 0; cz < kChunks; ++cz) {
        for (std::int32_t cx = 0; cx < kChunks; ++cx) {
            std::vector<chunk::Chunk> chunks;
            for (const auto& file : files) {
                REQUIRE(file.hasChunk(cx, cz));
                chunks.push_back(chunk::Chunk::decode(nbt::read(file.readChunk(cx, cz)).root));
            }
            for (int lz = 0; lz < 16; ++lz) {
                for (int lx = 0; lx < 16; ++lx) {
                    for (std::int32_t y = kMinY; y <= kTopY; ++y) {
                        const chunk::BlockState& control =
                            blockOrAir(chunks.at(0).blockAt(lx, y, lz));
                        const bool barrier = control == arms().at(0).block;
                        controlStone += barrier ? 1 : 0;
                        controlOtherSolid += (!barrier && solid(control)) ? 1 : 0;
                        for (std::size_t t = 0; t < versus.size(); ++t) {
                            const Arm& arm = arms().at(t + 1);
                            const chunk::BlockState& here =
                                blockOrAir(chunks.at(t + 1).blockAt(lx, y, lz));
                            Versus& score = versus.at(t);
                            if (barrier) {
                                if (here == arm.block) {
                                    ++score.atBarrier;
                                } else if (here == arms().at(0).block) {
                                    ++score.literalStone;
                                } else if (solid(here)) {
                                    ++score.otherMaterial;
                                } else {
                                    ++score.openAtBarrier;
                                }
                            } else if (solid(here)) {
                                ++score.solidElsewhere;
                            } else if (!(here == control)) {
                                ++score.nonSolidDiffering;
                            }
                        }
                        const chunk::BlockState& full = blockOrAir(chunks.at(3).blockAt(lx, y, lz));
                        if (full == arms().at(3).block) {
                            ++solidArmExact;
                        } else {
                            ++solidArmOther;
                        }
                    }
                }
            }
        }
    }

    INFO("control: " << controlStone << " stone, " << controlOtherSolid << " other solid, of "
                     << kBlocks);
    // The control, exactly: a solid count no flow can move.
    REQUIRE(controlStone == kBarriers);
    CHECK(controlOtherSolid == 0);
    for (std::size_t t = 0; t < versus.size(); ++t) {
        const Versus& score = versus.at(t);
        INFO(arms().at(t + 1).name
             << ": " << score.atBarrier << " at the barrier, " << score.literalStone
             << " literal stone, " << score.otherMaterial << " another solid, "
             << score.openAtBarrier << " open, " << score.solidElsewhere << " solid off it, "
             << score.nonSolidDiffering << " non-solid differing");
        // Q6.7: the preset's block, whole state, at exactly the control's
        // barrier positions — so default_block feeds nothing back into the
        // aquifer's decision either.
        CHECK(score.atBarrier == controlStone);
        CHECK(score.literalStone == 0);
        CHECK(score.otherMaterial == 0);
        CHECK(score.openAtBarrier == 0);
        CHECK(score.solidElsewhere == 0);
        // Two non-solid blocks that differ between dimensions of one frozen
        // world (air, water, a water level) are the run's flow remnant, never
        // the material: bounded, not pinned.
        CHECK(score.nonSolidDiffering * 1000 <= kBlocks);
    }
    // Positive density with aquifers on: the same state, everywhere.
    CHECK(solidArmExact == kBlocks);
    CHECK(solidArmOther == 0);
}

TEST_CASE("the default-block probe's control is barrier3way's d_neg1_0, block for block",
          "[conformance][aquifer]") {
    // The two are one recipe in two worlds of one seed; nothing that shapes
    // a barrier is seeded from the dimension's name (SPEC §11, the deepfloor
    // control), so every solid block must agree.
    const std::filesystem::path other = fixtures() / "probes" / "barrier3way";
    if (!loadSpec().has_value() || !std::filesystem::is_regular_file(other / "manifest.json")) {
        SKIP("needs both " << probeDir() << " (" << kScript << ") and " << other
                           << " (tools/analysis/aquifer-barrier-probe.sh)");
    }
    test::requireFrozen(probeDir(), kScript);
    test::requireSeed(probeDir(), kSeed);
    test::requireFrozen(other, "tools/analysis/aquifer-barrier-probe.sh");
    test::requireSeed(other, kSeed);
    // The same recipe: the d_neg1_0 entry is the control's but for the name
    // and the default_block it leaves to the script's default (stone).
    nlohmann::json theirs = test::specEntry(other, "d_neg1_0");
    nlohmann::json ours = test::specEntry(probeDir(), "db_stone");
    REQUIRE(ours.at("default_block") == nlohmann::json{{"Name", "minecraft:stone"}});
    ours.erase("default_block");
    ours.erase("name");
    theirs.erase("name");
    REQUIRE(ours == theirs);

    const auto mine = region::RegionFile::open(probeDir() / "db_stone" / "r.0.0.mca");
    const auto reference = region::RegionFile::open(other / "d_neg1_0" / "r.0.0.mca");
    long long solids = 0;
    long long differ = 0;
    for (std::int32_t cz = 0; cz < kChunks; ++cz) {
        for (std::int32_t cx = 0; cx < kChunks; ++cx) {
            REQUIRE(mine.hasChunk(cx, cz));
            REQUIRE(reference.hasChunk(cx, cz));
            const auto a = chunk::Chunk::decode(nbt::read(mine.readChunk(cx, cz)).root);
            const auto b = chunk::Chunk::decode(nbt::read(reference.readChunk(cx, cz)).root);
            for (int lz = 0; lz < 16; ++lz) {
                for (int lx = 0; lx < 16; ++lx) {
                    for (std::int32_t y = kMinY; y <= kTopY; ++y) {
                        const chunk::BlockState& left = blockOrAir(a.blockAt(lx, y, lz));
                        const chunk::BlockState& right = blockOrAir(b.blockAt(lx, y, lz));
                        solids += solid(left) ? 1 : 0;
                        differ += (solid(left) || solid(right)) && !(left == right) ? 1 : 0;
                    }
                }
            }
        }
    }
    INFO(solids << " solid in the control, " << differ << " solid positions differing");
    REQUIRE(solids == kBarriers);
    CHECK(differ == 0);
}

TEST_CASE("the shipped filler writes the server's default_block at every aquifer barrier",
          "[conformance][aquifer][terrain]") {
    const std::optional<nlohmann::json> spec = loadSpec();
    if (!spec.has_value() || !std::filesystem::is_directory(fixtures() / "worldgen")) {
        SKIP("no default-block probe under " << probeDir() << "; generate it with " << kScript
                                             << " --accept-eula 42");
    }
    test::requireFrozen(probeDir(), kScript);
    test::requireSeed(probeDir(), kSeed);
    requireDesign(*spec);

    // The corpus's own spec, rebuilt into the settings the server was given
    // (density-probe.sh's defaults included), resolved against vanilla's
    // worldgen for the three aquifer noises the router names.
    const data::Pack pack = data::Pack::open(fixtures() / "worldgen");
    density::Graph::Builder builder(pack);
    std::vector<settings::NoiseSettings> dimensions;
    for (const auto& entry : *spec) {
        const std::string name = entry.at("name").get<std::string>();
        const data::PackEntry settingsEntry{
            .registry = data::Registry::NoiseSettings,
            .id = data::ResourceLocation{"stratum", name},
            .file = probeDir() / "spec.json",
            .json = test::probeNoiseSettings(entry),
        };
        dimensions.push_back(settings::NoiseSettings::load(settingsEntry, builder));
    }
    const density::Graph graph = builder.release();
    const density::NoiseRegistry noises = density::NoiseRegistry::create(
        pack, graph.referencedNoises(), kSeed, density::RandomSource::Xoroshiro);

    long long flowTotal = 0;
    for (std::size_t i = 0; i < arms().size(); ++i) {
        const Arm& arm = arms().at(i);
        const settings::NoiseSettings& dimension = dimensions.at(i);
        INFO("dimension " << arm.name);
        // No surface rules: the probe's one never fires (y_above 2000).
        const terrain::ChunkFiller filler = terrain::ChunkFiller::compile(graph, noises, dimension);
        REQUIRE(dimension.geometry.minY == kMinY);
        REQUIRE(dimension.geometry.maxY() == kTopY + 1);
        test::GoldenRegion region(probeDir() / std::string(arm.name) / "r.0.0.mca");
        terrain::ChunkBuffer buffer(dimension.geometry);
        long long serverSolid = 0;
        long long coSolid = 0;
        long long exact = 0;
        long long flow = 0;
        long long fluidLevel = 0;
        long long wrong = 0;
        for (std::int32_t cz = 0; cz < kChunks; ++cz) {
            for (std::int32_t cx = 0; cx < kChunks; ++cx) {
                REQUIRE(region.hasChunk(cx, cz));
                filler.fill(cx, cz, buffer);
                const chunk::Chunk& served = region.chunk(cx, cz);
                for (int lz = 0; lz < 16; ++lz) {
                    for (int lx = 0; lx < 16; ++lx) {
                        for (std::int32_t y = kMinY; y <= kTopY; ++y) {
                            const chunk::BlockState& theirs = blockOrAir(served.blockAt(lx, y, lz));
                            const settings::BlockState& ours = buffer.at(lx, y, lz);
                            const bool theirsSolid = solid(theirs);
                            serverSolid += theirsSolid ? 1 : 0;
                            if (sameState(ours, theirs)) {
                                coSolid += theirsSolid ? 1 : 0;
                                ++exact;
                                continue;
                            }
                            const Category theirCategory = test::categoryOf(theirs.name);
                            const Category ourCategory = test::categoryOf(ours.name.toString());
                            // Only flow tells two non-solid blocks apart
                            // here: a fluid at another level, or air against
                            // fluid in a shape flow leaves. A solid never is
                            // flow's, where nothing can make or unmake one.
                            if (!theirsSolid && ourCategory != Category::Solid) {
                                if (theirCategory == ourCategory) {
                                    ++fluidLevel;
                                    continue;
                                }
                                if (test::explainedByFlow(region, (cx * 16) + lx, y, (cz * 16) + lz,
                                                          theirCategory, ourCategory)) {
                                    ++flow;
                                    continue;
                                }
                            }
                            ++wrong;
                            if (wrong <= 20) {
                                UNSCOPED_INFO(arm.name << " (" << ((cx * 16) + lx) << ", " << y
                                                       << ", " << ((cz * 16) + lz) << "): server "
                                                       << theirs.toString() << ", ours "
                                                       << ours.name.toString());
                            }
                        }
                    }
                }
            }
        }
        INFO("server solid " << serverSolid << ", agreed solid " << coSolid << ", exact " << exact
                             << ", flow-shaped " << flow << ", fluid level only " << fluidLevel
                             << ", wrong " << wrong << " of " << kBlocks);
        flowTotal += flow + fluidLevel;
        REQUIRE(serverSolid == (arm.density > 0.0 ? kBlocks : kBarriers));
        CHECK(coSolid == serverSolid);
        CHECK(wrong == 0);
        CHECK(exact + flow + fluidLevel == kBlocks);
    }
    INFO("flow over every dimension: " << flowTotal);
    // Bounded, not pinned: the run's own remnant.
    CHECK(flowTotal * 1000 <= kBlocks * static_cast<long long>(arms().size()));
}
