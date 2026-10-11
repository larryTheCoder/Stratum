// Stratum — ore veins under `legacy_random_source`, over a non-stone
// default_block and with aquifers off, against the server.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// No vanilla dimension combines the legacy source with ore veins, so nothing
// on disk said how the vein source draws under it, and this build refused
// the pair by name. The server generates the pair when a datapack asks, and
// tools/analysis/legacy-orevein-probe.sh asks: one world per seed, eight
// dimensions, each a solid column whose vein inputs make every block of
// both vein ranges a candidate —
//
//   lv   legacy_random_source TRUE        mv   the same with the flag false
//   nv   mv over netherrack                dv   mv over deepslate[axis=x]
//   or   mv, copper only, an ore chance    lr   lv, the same, its own chance
//        built to part the ore roll's two comparisons at one position
//   la   lv with aquifers off              ma   mv with aquifers off
//
// What the worlds say (SPEC §11, "Ore veins under the legacy source"): the
// legacy vein source is the legacy positional primitive salted
// "minecraft:ore" with the modern three draws, each a java.util.Random
// nextFloat; a first draw of exactly 0.7F is a member and the ore roll
// compares in float, under both sources; a non-stone default_block takes the
// same vein blocks as stone; and veins do not need aquifers. Four seeds:
// 42, 31337 (whose legacy window holds a 0.7F membership draw), 42's 48-bit
// twin (the same legacy world, a different modern one) and 2 (whose modern
// window holds two).
//
// The fixtures are Mojang-derived and never committed (SPEC §12).
#include "support/fluid_flow.hpp"
#include "support/probe_corpus.hpp"
#include "support/probe_settings.hpp"
#include "support/probe_spec.hpp"
#include "support/temp_path.hpp"

#include <stratum/chunk/chunk.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/density/random_source.hpp>
#include <stratum/ore/vein.hpp>
#include <stratum/rng/java_random.hpp>
#include <stratum/rng/xoroshiro128.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/rule_graph.hpp>
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
#include <initializer_list>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using stratum::density::RandomSource;

constexpr const char* kScript = "tools/analysis/legacy-orevein-probe.sh";
constexpr std::int64_t kTwin = 42 ^ std::numeric_limits<std::int64_t>::min();
/// The seeds tools/probe-worlds generates the family at, in its order.
constexpr std::array<std::int64_t, 4> kSeeds{42, 31337, kTwin, 2};
constexpr std::int32_t kWindow = 8 * 16;
constexpr std::array<const char*, 8> kArms{"lv", "mv", "nv", "dv", "or", "lr", "la", "ma"};

[[nodiscard]] std::filesystem::path probes() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11" / "probes";
}

/// One arm's declared inputs, required to be the probe's shapes: anything
/// else is a world this case cannot replay.
struct Arm {
    bool legacy = false;
    bool aquifers = false;
    stratum::chunk::BlockState defaultBlock;
    double toggleFromY = 0.0;
    double toggleToY = 1.0;
    double toggleFromValue = 0.0;
    double toggleToValue = 0.0;
    double ridged = 0.0;
    double gap = 0.0;
};

[[nodiscard]] stratum::chunk::BlockState sorted(stratum::chunk::BlockState state) {
    std::ranges::sort(state.properties);
    return state;
}

[[nodiscard]] Arm armOf(const nlohmann::json& entry) {
    REQUIRE(entry.at("raw_final_density").at("type") == "minecraft:constant");
    REQUIRE(entry.at("raw_final_density").at("argument").get<double>() > 0.0);
    REQUIRE(entry.at("ore_veins_enabled") == true);
    const nlohmann::json& router = entry.at("router");
    const nlohmann::json& toggle = router.at("vein_toggle");
    REQUIRE(router.at("vein_ridged").is_number());
    REQUIRE(router.at("vein_gap").is_number());
    Arm arm;
    arm.legacy = entry.at("legacy_random_source").get<bool>();
    arm.aquifers = entry.at("aquifers_enabled").get<bool>();
    const nlohmann::json& state = entry.at("default_block");
    arm.defaultBlock.name = state.at("Name").get<std::string>();
    if (state.contains("Properties")) {
        for (const auto& [key, value] : state.at("Properties").items()) {
            arm.defaultBlock.properties.emplace_back(key, value.get<std::string>());
        }
    }
    arm.defaultBlock = sorted(arm.defaultBlock);
    if (toggle.is_number()) {
        arm.toggleFromValue = toggle.get<double>();
        arm.toggleToValue = arm.toggleFromValue;
    } else {
        REQUIRE(toggle.at("type") == "minecraft:y_clamped_gradient");
        arm.toggleFromY = toggle.at("from_y").get<double>();
        arm.toggleToY = toggle.at("to_y").get<double>();
        arm.toggleFromValue = toggle.at("from_value").get<double>();
        arm.toggleToValue = toggle.at("to_value").get<double>();
    }
    arm.ridged = router.at("vein_ridged").get<double>();
    arm.gap = router.at("vein_gap").get<double>();
    return arm;
}

/// vein_toggle at @p y: the y_clamped_gradient the spec declares, or its
/// constant (whose two values are equal).
[[nodiscard]] stratum::ore::VeinInputs inputsAt(const Arm& arm, std::int32_t y) {
    const double t = std::clamp(
        (static_cast<double>(y) - arm.toggleFromY) / (arm.toggleToY - arm.toggleFromY), 0.0, 1.0);
    return stratum::ore::VeinInputs{.toggle = arm.toggleFromValue +
                                              (t * (arm.toggleToValue - arm.toggleFromValue)),
                                    .ridged = arm.ridged,
                                    .gap = arm.gap};
}

[[nodiscard]] stratum::chunk::BlockState veinState(const stratum::ore::Vein& vein,
                                                   const stratum::chunk::BlockState& otherwise) {
    const bool copper = vein.type == stratum::ore::VeinType::Copper;
    const auto named = [](const char* name) {
        return stratum::chunk::BlockState{.name = name, .properties = {}};
    };
    switch (vein.block) {
        case stratum::ore::VeinBlock::Filler:
            return named(copper ? "minecraft:granite" : "minecraft:tuff");
        case stratum::ore::VeinBlock::Ore:
            return named(copper ? "minecraft:copper_ore" : "minecraft:deepslate_iron_ore");
        case stratum::ore::VeinBlock::RawBlock:
            return named(copper ? "minecraft:raw_copper_block" : "minecraft:raw_iron_block");
        case stratum::ore::VeinBlock::None:
            break;
    }
    return otherwise;
}

/// The server's block at a window position, properties sorted. A position
/// the region does not hold reads as a name no block has, so it can only
/// disagree; the window's chunks are REQUIREd present before any is read.
[[nodiscard]] stratum::chunk::BlockState servedAt(stratum::test::GoldenRegion& region,
                                                  std::int32_t x, std::int32_t y, std::int32_t z) {
    const auto* state = region.blockAt(x, y, z);
    if (state == nullptr) {
        return stratum::chunk::BlockState{.name = "(absent)", .properties = {}};
    }
    return sorted(*state);
}

struct Tally {
    long long candidates = 0;
    long long exact = 0;
};

/// Every candidate of every @p stride-th column of the window: the server's
/// block against @p veins.
[[nodiscard]] Tally replay(stratum::test::GoldenRegion& region, const Arm& arm,
                           const stratum::ore::VeinSource& veins, std::int32_t stride = 1) {
    Tally tally;
    for (std::int32_t z = 0; z < kWindow; z += stride) {
        for (std::int32_t x = 0; x < kWindow; x += stride) {
            for (std::int32_t y = stratum::ore::kLowestVeinY; y <= stratum::ore::kHighestVeinY;
                 ++y) {
                const stratum::ore::VeinInputs inputs = inputsAt(arm, y);
                if (!stratum::ore::clearsRichness(y, inputs.toggle) || inputs.ridged >= 0.0) {
                    continue;
                }
                ++tally.candidates;
                tally.exact +=
                    static_cast<long long>(veinState(veins.at(x, y, z, inputs), arm.defaultBlock) ==
                                           servedAt(region, x, y, z));
            }
        }
    }
    return tally;
}

/// The family's corpora, checked: empty when none was generated, a failure
/// when only some were (support/probe_corpus.hpp).
[[nodiscard]] std::vector<std::filesystem::path> corpora() {
    return stratum::test::seededCorpora(probes(), "orevlegacy_s", kSeeds, kScript);
}

/// The arms are the one recipe but for what each varies.
void requireRecipe(const std::filesystem::path& dir) {
    const nlohmann::json spec = stratum::test::readSpec(dir);
    const auto without = [&](const char* name, std::initializer_list<const char*> keys) {
        nlohmann::json entry = stratum::test::specEntry(spec, dir, name);
        entry.erase("name");
        for (const char* key : keys) {
            entry.erase(key);
            if (entry.contains("router")) {
                entry["router"].erase(key);
            }
        }
        return entry;
    };
    for (const char* name : kArms) {
        static_cast<void>(stratum::test::specEntry(spec, dir, name));
    }
    CHECK(without("lv", {"legacy_random_source"}) == without("mv", {"legacy_random_source"}));
    CHECK(without("nv", {"default_block"}) == without("mv", {"default_block"}));
    CHECK(without("dv", {"default_block"}) == without("mv", {"default_block"}));
    CHECK(without("or", {"vein_toggle"}) == without("mv", {"vein_toggle"}));
    CHECK(without("lr", {"vein_toggle"}) == without("lv", {"vein_toggle"}));
    CHECK(without("la", {"aquifers_enabled"}) == without("lv", {"aquifers_enabled"}));
    CHECK(without("ma", {"aquifers_enabled"}) == without("mv", {"aquifers_enabled"}));
    CHECK(stratum::test::specEntry(spec, dir, "lv").at("legacy_random_source") == true);
    CHECK(stratum::test::specEntry(spec, dir, "mv").at("legacy_random_source") == false);
    CHECK(stratum::test::specEntry(spec, dir, "lv").at("aquifers_enabled") == true);
    CHECK(stratum::test::specEntry(spec, dir, "la").at("aquifers_enabled") == false);
}

[[nodiscard]] RandomSource sourceOf(const Arm& arm) {
    return arm.legacy ? RandomSource::Legacy : RandomSource::Xoroshiro;
}

/// A position's first two nextFloat draws under @p source.
[[nodiscard]] std::pair<float, float> firstDraws(RandomSource source, std::int64_t seed,
                                                 std::int32_t x, std::int32_t y, std::int32_t z) {
    if (source == RandomSource::Legacy) {
        auto random = stratum::rng::legacyPositionalSourceFor(seed, "minecraft:ore").at(x, y, z);
        const float first = random.nextFloat();
        const float second = random.nextFloat();
        return {first, second};
    }
    auto random = stratum::rng::positionalSourceFor(seed, "minecraft:ore").at(x, y, z);
    const float first = random.nextFloat();
    const float second = random.nextFloat();
    return {first, second};
}

/// A worldgen tree of the probe's own noise settings, removed on exit.
class ScratchTree {
public:
    ScratchTree() : path_(stratum::test::tempPath("stratum-legacy-orevein")) {
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

TEST_CASE("ore veins under the legacy source are the ones the server drew",
          "[conformance][ore][legacy]") {
    const std::vector<std::filesystem::path> dirs = corpora();
    if (dirs.empty()) {
        SKIP("no legacy ore-vein probe under " << probes() << "; generate it with "
                                               << "tools/probe-worlds generate --accept-eula "
                                                  "--only 'orevlegacy_*'");
    }
    const auto bits = [](float value) { return std::bit_cast<std::uint32_t>(value); };
    for (std::size_t i = 0; i < kSeeds.size(); ++i) {
        const std::int64_t seed = kSeeds.at(i);
        const std::filesystem::path& dir = dirs.at(i);
        INFO("seed " << seed);
        requireRecipe(dir);
        const nlohmann::json spec = stratum::test::readSpec(dir);

        for (const char* name : kArms) {
            INFO("arm " << name);
            const Arm arm = armOf(stratum::test::specEntry(spec, dir, name));
            stratum::test::GoldenRegion region(dir / name / "r.0.0.mca");
            for (std::int32_t cz = 0; cz < 8; ++cz) {
                for (std::int32_t cx = 0; cx < 8; ++cx) {
                    REQUIRE(region.hasChunk(cx, cz));
                }
            }
            // Exact under the arm's own source, whatever the default block and
            // whether or not aquifers are on.
            const Tally own = replay(region, arm, stratum::ore::VeinSource{seed, sourceOf(arm)});
            INFO("own source: " << own.exact << " of " << own.candidates);
            CHECK(own.candidates >= 737280);
            CHECK(own.exact == own.candidates);
            // And at chance under the other: what makes the exact replay a
            // reading of the flag (measured 37.2-38.1% over the whole window;
            // read here on every fourth column, a sixteenth of it).
            const RandomSource other = arm.legacy ? RandomSource::Xoroshiro : RandomSource::Legacy;
            const Tally wrong = replay(region, arm, stratum::ore::VeinSource{seed, other}, 4);
            INFO("other source: " << wrong.exact << " of " << wrong.candidates);
            CHECK(wrong.exact * 10 < wrong.candidates * 4);

            // Off the candidates, the default block and nothing else: on every
            // fourth column, every y.
            long long stray = 0;
            for (std::int32_t z = 1; z < kWindow; z += 4) {
                for (std::int32_t x = 2; x < kWindow; x += 4) {
                    for (std::int32_t y = -64; y < 320; ++y) {
                        const stratum::ore::VeinInputs inputs = inputsAt(arm, y);
                        if (stratum::ore::inAnyVeinRange(y) &&
                            stratum::ore::clearsRichness(y, inputs.toggle)) {
                            continue;
                        }
                        stray += static_cast<long long>(
                            !(servedAt(region, x, y, z) == arm.defaultBlock));
                    }
                }
            }
            CHECK(stray == 0);
        }

        // The 48-bit twin: seed 42's legacy world, not its modern one
        // (measured 49.9% for the latter).
        if (seed == kTwin) {
            const Arm lv = armOf(stratum::test::specEntry(spec, dir, "lv"));
            stratum::test::GoldenRegion lvRegion(dir / "lv" / "r.0.0.mca");
            const Tally asFortyTwo =
                replay(lvRegion, lv, stratum::ore::VeinSource{42, RandomSource::Legacy});
            CHECK(asFortyTwo.exact == asFortyTwo.candidates);
            const Arm mv = armOf(stratum::test::specEntry(spec, dir, "mv"));
            stratum::test::GoldenRegion mvRegion(dir / "mv" / "r.0.0.mca");
            const Tally modernAsFortyTwo =
                replay(mvRegion, mv, stratum::ore::VeinSource{42, RandomSource::Xoroshiro});
            CHECK(modernAsFortyTwo.exact * 10 < modernAsFortyTwo.candidates * 6);
        }

        // The single positions that settled each comparison, read off the
        // server directly so that neither replay above can hide one.
        //
        // A first draw of exactly 0.7F is a member (engine v13 compared in
        // float and left these as the default block).
        if (seed == 31337) {
            stratum::test::GoldenRegion lv(dir / "lv" / "r.0.0.mca");
            CHECK(bits(firstDraws(RandomSource::Legacy, seed, 38, -31, 7).first) == bits(0.7F));
            CHECK(servedAt(lv, 38, -31, 7).name == "minecraft:deepslate_iron_ore");
        }
        if (seed == 2) {
            for (const char* name : {"mv", "nv", "dv", "ma"}) {
                INFO("arm " << name);
                stratum::test::GoldenRegion region(dir / name / "r.0.0.mca");
                for (const std::int32_t y : {-45, 45}) {
                    CHECK(bits(firstDraws(RandomSource::Xoroshiro, seed, 24, y, 22).first) ==
                          bits(0.7F));
                    CHECK(servedAt(region, 24, y, 22).name ==
                          (y < 0 ? "minecraft:deepslate_iron_ore" : "minecraft:copper_ore"));
                }
            }
        }
        // The ore roll compares in float: a second draw equal to the chance
        // rounded to float, below the chance itself, is filler.
        const auto oreRollPin = [&](const char* name, RandomSource source, std::int32_t x) {
            INFO("arm " << name);
            const Arm arm = armOf(stratum::test::specEntry(spec, dir, name));
            const double chance = 0.1 + (((arm.toggleFromValue - 0.4) / (0.6 - 0.4)) * (0.3 - 0.1));
            const auto [first, second] = firstDraws(source, seed, x, 20, 0);
            CHECK(static_cast<double>(first) < 0.7);
            CHECK(bits(second) == bits(static_cast<float>(chance)));
            CHECK(static_cast<double>(second) < chance);
            stratum::test::GoldenRegion region(dir / name / "r.0.0.mca");
            CHECK(servedAt(region, x, 20, 0).name == "minecraft:granite");
        };
        if (seed == 42) {
            oreRollPin("or", RandomSource::Xoroshiro, 58);
        }
        if (seed == 42 || seed == kTwin) {
            oreRollPin("lr", RandomSource::Legacy, 20);
        }

        // lv and mv, raw: the flag reached the vein source at all (measured
        // 1068829-1069573 of 1818624 blocks in y -60..50 differ).
        {
            stratum::test::GoldenRegion lv(dir / "lv" / "r.0.0.mca");
            stratum::test::GoldenRegion mv(dir / "mv" / "r.0.0.mca");
            long long differ = 0;
            long long blocks = 0;
            for (std::int32_t z = 0; z < kWindow; z += 2) {
                for (std::int32_t x = 0; x < kWindow; x += 2) {
                    for (std::int32_t y = stratum::ore::kLowestVeinY;
                         y <= stratum::ore::kHighestVeinY; ++y) {
                        ++blocks;
                        differ += static_cast<long long>(
                            !(servedAt(lv, x, y, z) == servedAt(mv, x, y, z)));
                    }
                }
            }
            INFO("lv against mv: " << differ << " of " << blocks);
            CHECK(differ * 2 > blocks);
        }
    }
}

TEST_CASE("the shipped filler generates the legacy ore-vein probe block for block",
          "[conformance][ore][legacy]") {
    // The wiring, end to end: each arm's own settings, rebuilt from its
    // spec.json the way density-probe.sh wrote them, through the registry
    // the dimension's flag selects and ChunkFiller::compile — the call
    // world::CompiledDimension makes — against the server, state for state
    // (deepslate's axis included). Before this change ChunkFiller::compile
    // refused lv, lr and la by name, and engine v13 left la and ma as bare
    // stone and the 0.7F positions as the default block.
    const std::vector<std::filesystem::path> dirs = corpora();
    if (dirs.empty()) {
        SKIP("no legacy ore-vein probe under " << probes() << "; generate it with "
                                               << "tools/probe-worlds generate --accept-eula "
                                                  "--only 'orevlegacy_*'");
    }
    // Every chunk holding a position that settled a comparison — (1, 0)
    // lr's, (2, 0) 31337's 0.7F draw, (3, 0) or's, (1, 1) seed 2's two —
    // and one more besides.
    constexpr std::array<std::pair<std::int32_t, std::int32_t>, 5> kChunks{
        {{1, 0}, {2, 0}, {3, 0}, {1, 1}, {6, 5}}};
    for (std::size_t i = 0; i < kSeeds.size(); ++i) {
        const std::int64_t seed = kSeeds.at(i);
        const std::filesystem::path& dir = dirs.at(i);
        INFO("seed " << seed);
        const nlohmann::json spec = stratum::test::readSpec(dir);

        const ScratchTree scratch;
        for (const char* name : kArms) {
            std::ofstream out(scratch.path() / "noise_settings" / (std::string(name) + ".json"));
            out << stratum::test::probeNoiseSettings(stratum::test::specEntry(spec, dir, name))
                       .dump();
        }
        const stratum::data::Pack pack = stratum::data::Pack::open(scratch.path());
        const stratum::settings::LoadedSettings loaded = stratum::settings::loadAll(pack);

        for (const char* name : kArms) {
            INFO("dimension " << name);
            const stratum::data::ResourceLocation id{"minecraft", name};
            const stratum::settings::NoiseSettings& dimension = loaded.settings.at(id);
            // The source the dimension declares, exactly as
            // world::CompiledDimension chooses it.
            const RandomSource source =
                dimension.legacyRandomSource ? RandomSource::Legacy : RandomSource::Xoroshiro;
            const auto noises = stratum::density::NoiseRegistry::create(
                pack, loaded.graph.referencedNoises(), seed, source);
            const stratum::surface::RuleGraph rules =
                stratum::surface::RuleGraph::resolve(dimension.surfaceRule, id);
            const stratum::terrain::ChunkFiller filler =
                stratum::terrain::ChunkFiller::compile(loaded.graph, noises, dimension, &rules);
            REQUIRE(filler.runsSurfaceRules());

            const stratum::chunk::BlockState defaultBlock =
                armOf(stratum::test::specEntry(spec, dir, name)).defaultBlock;
            stratum::test::GoldenRegion region(dir / name / "r.0.0.mca");
            stratum::terrain::ChunkBuffer buffer(dimension.geometry);
            long long blocks = 0;
            long long wrong = 0;
            long long veinBlocks = 0;
            for (const auto& [chunkX, chunkZ] : kChunks) {
                REQUIRE(region.hasChunk(chunkX, chunkZ));
                filler.fill(chunkX, chunkZ, buffer);
                for (int localZ = 0; localZ < 16; ++localZ) {
                    for (int localX = 0; localX < 16; ++localX) {
                        const std::int32_t x = (chunkX * 16) + localX;
                        const std::int32_t z = (chunkZ * 16) + localZ;
                        for (std::int32_t y = dimension.geometry.minY;
                             y < dimension.geometry.minY + dimension.geometry.height; ++y) {
                            ++blocks;
                            const stratum::chunk::BlockState served = servedAt(region, x, y, z);
                            // A std::map, so its properties come out sorted.
                            const auto& ours = buffer.at(localX, y, localZ);
                            stratum::chunk::BlockState oursState{.name = ours.name.toString(),
                                                                 .properties = {}};
                            for (const auto& [key, value] : ours.properties) {
                                oursState.properties.emplace_back(key, value);
                            }
                            veinBlocks += static_cast<long long>(!(served == defaultBlock));
                            if (!(served == oursState)) {
                                ++wrong;
                                if (wrong <= 8) {
                                    UNSCOPED_INFO(name << " (" << x << ", " << y << ", " << z
                                                       << "): server " << served.toString()
                                                       << ", ours " << oursState.toString());
                                }
                            }
                        }
                    }
                }
            }
            INFO("wrong " << wrong << " of " << blocks << "; vein blocks " << veinBlocks);
            CHECK(wrong == 0);
            CHECK(veinBlocks > 10000);
        }
    }
}
