// Stratum — reads back tools/analysis/legacy-orevein-probe.sh's worlds.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The probe puts one solid, veins-on dimension in a world eight times: with
// `legacy_random_source` true (lv) and false (mv) over stone, with the flag
// false over netherrack (nv) and over deepslate[axis=x] (dv), two
// copper-only arms at an ore chance chosen to part the ore roll's two
// comparisons (or, lr), and lv and mv with aquifers off (la, ma). Every vein
// input is a constant or a y gradient chosen so that every block of the vein
// ranges is a candidate, so every such block is a readout of the three
// draws. This scores those worlds.
//
//   legacy-orevein-analyze score <probe-dir> <seed>
//       Every candidate block of the force-loaded 8x8 window of every arm,
//       name for name, against each derivation below; the first misses of
//       the shipped VeinSource under the arm's own source, with their draws;
//       per arm, the blocks outside the candidates (all must be the arm's
//       default_block); and lv against mv, raw.
//
//   legacy-orevein-analyze bucket <seed>...
//       Every window position, under each source, whose FIRST draw is
//       exactly 0.7F — the one 24-bit value at which the membership roll's
//       float and double comparisons part. From the RNG alone.
//
//   legacy-orevein-analyze same <probe-dir> <arm> <arm>
//       Two arms of one world, block for block over the window: how `la` and
//       `ma`, aquifers off, read against `lv` and `mv`.
//
//   legacy-orevein-analyze oreroll <seed>
//       Under each source, a member position whose SECOND draw f lies in
//       [0.25, 0.3), and the toggle whose ore chance sits a quarter of a
//       float ulp above f: the probe's or/lr arms. From the RNG alone.
//
// THE DERIVATIONS, and why each is there. One per-position generator gives
// three draws in this order: membership (< 0.7), the ore roll (< the mapped
// chance), the raw roll (< 0.02) after an ore. What is scored:
//
//   * the shipped `ore::VeinSource` under each source;
//   * H0 — java.util.Random through `legacyPositionalSourceFor(seed,
//     "minecraft:ore")`, the legacy positional primitive measured for the
//     aquifer and the gradient (SPEC §11), nextFloat thrice;
//   * H0 with each draw nextFloat or nextDouble, all eight assignments.
//     Under Xoroshiro both precisions come from one nextLong and could not be
//     told apart; under java.util.Random nextFloat takes one LCG step and
//     nextDouble two, so a draw's method moves every LCG step after it. Draws
//     1 and 2 are therefore observable here, through the draws that follow
//     them. Draw 3 is the last and is not: its two precisions agree on the
//     top 24 bits, so they part only on draws in the 24-bit bucket that
//     straddles 0.02 — none of these;
//   * the COMPARISON, per draw: a 24-bit draw f against a threshold c in
//     float (f < (float) c) or in double ((double) f < c). The two part only
//     where (float) c rounds down onto f's own grid: for membership that is
//     the bucket f = 0.7F (`bucket`), for the raw roll nowhere (0.02F is not
//     on the grid), and for the ore roll only at chances built to do it
//     (`oreroll`). All four pairings of the first two, under both sources.
//     What the server said: membership in double, the ore roll in float —
//     which is also what the library's VeinSource does;
//   * Xoroshiro with draw 1 nextDouble (the 53-bit value of the same
//     nextLong), which the 0.7F bucket parts from nextFloat;
//   * a wrong seed (seed + 1), and, where the world is not 42, 42 — which
//     for 42's 48-bit twin is the SAME legacy world (java.util.Random keeps
//     48 bits of a seed) and a different Xoroshiro one.
//
// Nothing in the space was chosen from Mojang's or deepslate's source
// (CLAUDE.md): the primitive is the one SPEC §11 measured, the draw order and
// thresholds are ore/vein.hpp's, and the two precisions are
// java.util.Random's documented nextFloat and nextDouble.
#include <stratum/chunk/chunk.hpp>
#include <stratum/density/random_source.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/ore/vein.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/rng/java_random.hpp>
#include <stratum/rng/xoroshiro128.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace stratum;

namespace {

constexpr std::int32_t kWindowChunks = 8;
constexpr std::int32_t kWindow = kWindowChunks * 16;
constexpr std::string_view kVeinSalt = "minecraft:ore";
/// The one 24-bit membership draw whose two comparisons part: next(24) of
/// 11744051 is 0.7F exactly, which is below the double 0.7 and not below the
/// float 0.7F.
constexpr std::int32_t kMembershipBucket = 11744051;
constexpr int kListed = 10;

// --- the arm, from the corpus's own spec.json ----------------------------------

/// An arm's vein inputs and default block, read from spec.json rather than
/// restated, and refused unless they are the shapes this scores: vein_toggle
/// a constant or a y_clamped_gradient, the other two constants.
struct Arm {
    std::string name;
    bool legacy = false;
    /// False for `la` and `ma`, which score as their aquifers-on twins.
    bool aquifers = true;
    chunk::BlockState defaultBlock;
    std::int32_t minY = 0;
    std::int32_t height = 0;
    double toggleFromY = 0.0;
    double toggleToY = 1.0;
    double toggleFromValue = 0.0;
    double toggleToValue = 0.0;
    double ridged = 0.0;
    double gap = 0.0;
};

chunk::BlockState stateOf(const nlohmann::json& state) {
    chunk::BlockState out{.name = state.at("Name").get<std::string>(), .properties = {}};
    if (state.contains("Properties")) {
        for (const auto& [key, value] : state.at("Properties").items()) {
            out.properties.emplace_back(key, value.get<std::string>());
        }
    }
    std::sort(out.properties.begin(), out.properties.end());
    return out;
}

std::optional<std::vector<Arm>> readArms(const std::filesystem::path& probe) {
    std::ifstream in(probe / "spec.json");
    if (!in) {
        std::fprintf(stderr, "no %s\n", (probe / "spec.json").string().c_str());
        return std::nullopt;
    }
    const nlohmann::json spec = nlohmann::json::parse(in);
    std::vector<Arm> arms;
    for (const auto& entry : spec) {
        const nlohmann::json& router = entry.at("router");
        const nlohmann::json& toggle = router.at("vein_toggle");
        const bool gradient =
            toggle.is_object() && toggle.at("type") == "minecraft:y_clamped_gradient";
        if ((!gradient && !toggle.is_number()) || !router.at("vein_ridged").is_number() ||
            !router.at("vein_gap").is_number()) {
            std::fprintf(stderr, "arm %s: the vein inputs are not the probe's shapes\n",
                         entry.at("name").get<std::string>().c_str());
            return std::nullopt;
        }
        Arm arm{
            .name = entry.at("name").get<std::string>(),
            .legacy = entry.value("legacy_random_source", false),
            .aquifers = entry.value("aquifers_enabled", false),
            .defaultBlock =
                stateOf(entry.value("default_block", nlohmann::json{{"Name", "minecraft:stone"}})),
            .minY = entry.value("min_y", -64),
            .height = entry.value("height", 384),
            .ridged = router.at("vein_ridged").get<double>(),
            .gap = router.at("vein_gap").get<double>(),
        };
        if (gradient) {
            arm.toggleFromY = toggle.at("from_y").get<double>();
            arm.toggleToY = toggle.at("to_y").get<double>();
            arm.toggleFromValue = toggle.at("from_value").get<double>();
            arm.toggleToValue = toggle.at("to_value").get<double>();
        } else {
            arm.toggleFromValue = toggle.get<double>();
            arm.toggleToValue = arm.toggleFromValue;
        }
        arms.push_back(arm);
    }
    return arms;
}

double clampedMap(double v, double inLo, double inHi, double outLo, double outHi) {
    const double t = std::clamp((v - inLo) / (inHi - inLo), 0.0, 1.0);
    return outLo + (t * (outHi - outLo));
}

ore::VeinInputs inputsAt(const Arm& arm, std::int32_t y) {
    return ore::VeinInputs{.toggle =
                               clampedMap(static_cast<double>(y), arm.toggleFromY, arm.toggleToY,
                                          arm.toggleFromValue, arm.toggleToValue),
                           .ridged = arm.ridged,
                           .gap = arm.gap};
}

// --- the derivations -------------------------------------------------------------

enum class Method : std::uint8_t { Float, Double };
/// How a nextFloat draw meets its threshold: against the float the threshold
/// rounds to, or widened to double against the double itself. A nextDouble
/// draw is always compared in double.
enum class Compare : std::uint8_t { InFloat, InDouble };

/// Defaults to the measured rule: nextFloat thrice, membership in double,
/// the ore roll in float.
struct Model {
    bool legacy = false;
    Method membership = Method::Float;
    Method ore = Method::Float;
    Method raw = Method::Float;
    Compare membershipCompare = Compare::InDouble;
    Compare oreCompare = Compare::InFloat;
};

std::string spell(const Model& model) {
    const auto one = [](Method m) { return m == Method::Float ? "F" : "D"; };
    const auto cmp = [](Compare c) { return c == Compare::InFloat ? "float" : "double"; };
    return std::string(model.legacy ? "legacyPositionalSourceFor" : "positionalSourceFor") +
           ", draws " + one(model.membership) + one(model.ore) + one(model.raw) +
           ", membership in " + cmp(model.membershipCompare) + ", ore roll in " +
           cmp(model.oreCompare);
}

/// One draw against one threshold.
template<typename Generator>
bool below(Generator& generator, Method method, Compare compare, double threshold) {
    if (method == Method::Double) {
        return generator.nextDouble() < threshold;
    }
    const float draw = generator.nextFloat();
    return compare == Compare::InFloat ? draw < static_cast<float>(threshold)
                                       : static_cast<double>(draw) < threshold;
}

/// The three draws, from one generator, in ore/vein.hpp's order. The
/// deterministic gate is the library's own (`clearsRichness`, ridged < 0).
template<typename Generator>
ore::Vein decide(Generator generator, const Model& model, std::int32_t y,
                 const ore::VeinInputs& inputs) {
    if (!ore::clearsRichness(y, inputs.toggle) || inputs.ridged >= 0.0) {
        return {};
    }
    const ore::VeinType type = y >= 0 ? ore::VeinType::Copper : ore::VeinType::Iron;
    if (!below(generator, model.membership, model.membershipCompare, 0.7)) {
        return {};
    }
    const double oreChance = clampedMap(std::abs(inputs.toggle), 0.4, 0.6, 0.1, 0.3);
    const bool oreWon = below(generator, model.ore, model.oreCompare, oreChance);
    if (!oreWon || inputs.gap <= -0.3) {
        return {.block = ore::VeinBlock::Filler, .type = type};
    }
    // The raw roll's two comparisons cannot part (see the header).
    if (below(generator, model.raw, Compare::InDouble, 0.02)) {
        return {.block = ore::VeinBlock::RawBlock, .type = type};
    }
    return {.block = ore::VeinBlock::Ore, .type = type};
}

using Derivation =
    std::function<ore::Vein(std::int32_t, std::int32_t, std::int32_t, const ore::VeinInputs&)>;

struct Named {
    std::string name;
    Derivation derive;
};

Named shipped(std::int64_t seed, density::RandomSource source) {
    return {std::string("shipped ore::VeinSource, ") +
                (source == density::RandomSource::Legacy ? "Legacy" : "Xoroshiro") + ", seed " +
                std::to_string(seed),
            [veins = ore::VeinSource{seed, source}](std::int32_t x, std::int32_t y, std::int32_t z,
                                                    const ore::VeinInputs& inputs) {
                return veins.at(x, y, z, inputs);
            }};
}

Named modelled(std::int64_t seed, const Model& model) {
    std::string name = spell(model) + ", seed " + std::to_string(seed);
    if (model.legacy) {
        return {std::move(name),
                [source = rng::legacyPositionalSourceFor(seed, kVeinSalt), model](
                    std::int32_t x, std::int32_t y, std::int32_t z, const ore::VeinInputs& inputs) {
                    return decide(source.at(x, y, z), model, y, inputs);
                }};
    }
    return {std::move(name),
            [source = rng::positionalSourceFor(seed, kVeinSalt),
             model](std::int32_t x, std::int32_t y, std::int32_t z, const ore::VeinInputs& inputs) {
                return decide(source.at(x, y, z), model, y, inputs);
            }};
}

/// Where derivations() puts the shipped source under each flag.
constexpr std::size_t kShippedModern = 0;
constexpr std::size_t kShippedLegacy = 1;

std::vector<Named> derivations(std::int64_t seed) {
    using density::RandomSource;
    std::vector<Named> out;
    out.push_back(shipped(seed, RandomSource::Xoroshiro));
    out.push_back(shipped(seed, RandomSource::Legacy));
    for (const bool legacy : {false, true}) {
        // The measured rule; engine v13's comparisons (both in float); both
        // in double; and the measured rule's two comparisons swapped.
        out.push_back(modelled(seed, Model{.legacy = legacy}));
        out.push_back(
            modelled(seed, Model{.legacy = legacy, .membershipCompare = Compare::InFloat}));
        out.push_back(modelled(seed, Model{.legacy = legacy, .oreCompare = Compare::InDouble}));
        out.push_back(modelled(seed, Model{.legacy = legacy,
                                           .membershipCompare = Compare::InFloat,
                                           .oreCompare = Compare::InDouble}));
    }
    // Xoroshiro's draw 1 as the 53-bit nextDouble of the same nextLong.
    out.push_back(modelled(seed, Model{.membership = Method::Double}));
    // H0's draw methods, every assignment but FFF (above).
    for (int bits = 1; bits < 8; ++bits) {
        const auto method = [bits](int bit) {
            return ((bits >> bit) & 1) != 0 ? Method::Double : Method::Float;
        };
        out.push_back(modelled(
            seed,
            Model{.legacy = true, .membership = method(2), .ore = method(1), .raw = method(0)}));
    }
    out.push_back(modelled(seed + 1, Model{.legacy = true}));
    out.push_back(modelled(seed + 1, Model{}));
    if (seed != 42) {
        out.push_back(modelled(42, Model{.legacy = true}));
        out.push_back(modelled(42, Model{}));
    }
    return out;
}

/// A position's first three draws as 24-bit integers, and the first as the
/// 53-bit double its generator would give, for a miss.
void traceDraws(bool legacy, std::int64_t seed, std::int32_t x, std::int32_t y, std::int32_t z) {
    if (legacy) {
        const rng::LegacyPositionalSource source = rng::legacyPositionalSourceFor(seed, kVeinSalt);
        rng::JavaRandom steps = source.at(x, y, z);
        const std::int32_t a = steps.next(24);
        const std::int32_t b = steps.next(24);
        const std::int32_t c = steps.next(24);
        rng::JavaRandom twice = source.at(x, y, z);
        std::printf("    next(24) x3: %d %d %d; first as nextDouble %.17g\n", a, b, c,
                    twice.nextDouble());
        return;
    }
    const rng::PositionalSource source = rng::positionalSourceFor(seed, kVeinSalt);
    rng::Xoroshiro128PlusPlus steps = source.at(x, y, z);
    const auto top24 = [](std::int64_t v) {
        return static_cast<std::int32_t>(static_cast<std::uint64_t>(v) >> 40U);
    };
    const std::int32_t a = top24(steps.nextLong());
    const std::int32_t b = top24(steps.nextLong());
    const std::int32_t c = top24(steps.nextLong());
    rng::Xoroshiro128PlusPlus once = source.at(x, y, z);
    std::printf("    nextLong>>>40 x3: %d %d %d; first as nextDouble %.17g\n", a, b, c,
                once.nextDouble());
}

// --- the readout -------------------------------------------------------------------

chunk::BlockState veinState(const ore::Vein& vein, const chunk::BlockState& defaultBlock) {
    const bool copper = vein.type == ore::VeinType::Copper;
    const auto named = [](const char* name) {
        return chunk::BlockState{.name = name, .properties = {}};
    };
    switch (vein.block) {
        case ore::VeinBlock::Filler:
            return named(copper ? "minecraft:granite" : "minecraft:tuff");
        case ore::VeinBlock::Ore:
            return named(copper ? "minecraft:copper_ore" : "minecraft:deepslate_iron_ore");
        case ore::VeinBlock::RawBlock:
            return named(copper ? "minecraft:raw_copper_block" : "minecraft:raw_iron_block");
        case ore::VeinBlock::None:
            break;
    }
    return defaultBlock;
}

/// One arm's window, decoded once.
class Window {
public:
    explicit Window(const std::filesystem::path& region) {
        const auto file = region::RegionFile::open(region);
        for (std::int32_t cz = 0; cz < kWindowChunks; ++cz) {
            for (std::int32_t cx = 0; cx < kWindowChunks; ++cx) {
                if (!file.hasChunk(cx, cz)) {
                    throw std::runtime_error(region.string() + " is missing window chunk " +
                                             std::to_string(cx) + ", " + std::to_string(cz));
                }
                chunks_.push_back(chunk::Chunk::decode(nbt::read(file.readChunk(cx, cz)).root));
            }
        }
    }

    [[nodiscard]] chunk::BlockState at(std::int32_t x, std::int32_t y, std::int32_t z) const {
        const chunk::Chunk& chunk =
            chunks_.at(static_cast<std::size_t>(((z / 16) * kWindowChunks) + (x / 16)));
        const chunk::BlockState* state = chunk.blockAt(x % 16, y, z % 16);
        if (state == nullptr) {
            return chunk::BlockState{.name = "minecraft:air", .properties = {}};
        }
        chunk::BlockState sorted = *state;
        std::sort(sorted.properties.begin(), sorted.properties.end());
        return sorted;
    }

private:
    std::vector<chunk::Chunk> chunks_;
};

struct Score {
    long long exact = 0;
    long long total = 0;
};

int score(const std::filesystem::path& probe, std::int64_t seed) {
    const std::optional<std::vector<Arm>> arms = readArms(probe);
    if (!arms) {
        return 1;
    }
    const std::vector<Named> all = derivations(seed);
    std::map<std::string, std::unique_ptr<Window>> windows;
    for (const Arm& arm : *arms) {
        windows.emplace(arm.name, std::make_unique<Window>(probe / arm.name / "r.0.0.mca"));
    }

    for (const Arm& arm : *arms) {
        const Window& window = *windows.at(arm.name);
        std::printf("\n== arm %s (legacy_random_source %s, default_block %s)\n", arm.name.c_str(),
                    arm.legacy ? "true" : "false", arm.defaultBlock.toString().c_str());
        const std::size_t own = arm.legacy ? kShippedLegacy : kShippedModern;
        int listed = 0;
        long long outside = 0;
        long long outsideOther = 0;
        long long untouched = 0;
        std::map<std::string, long long> census;
        std::vector<Score> scores(all.size());
        for (std::int32_t z = 0; z < kWindow; ++z) {
            for (std::int32_t x = 0; x < kWindow; ++x) {
                for (std::int32_t y = arm.minY; y < arm.minY + arm.height; ++y) {
                    const chunk::BlockState served = window.at(x, y, z);
                    const ore::VeinInputs inputs = inputsAt(arm, y);
                    const bool candidate = ore::inAnyVeinRange(y) &&
                                           ore::clearsRichness(y, inputs.toggle) &&
                                           inputs.ridged < 0.0;
                    if (!candidate) {
                        ++outside;
                        outsideOther += static_cast<long long>(!(served == arm.defaultBlock));
                        continue;
                    }
                    ++census[served.toString()];
                    untouched += static_cast<long long>(served == arm.defaultBlock);
                    for (std::size_t i = 0; i < all.size(); ++i) {
                        ++scores[i].total;
                        const chunk::BlockState predicted =
                            veinState(all[i].derive(x, y, z, inputs), arm.defaultBlock);
                        const bool exact = predicted == served;
                        scores[i].exact += static_cast<long long>(exact);
                        if (!exact && i == own && ++listed <= kListed) {
                            std::printf("  shipped miss at (%d, %d, %d): server %s, ours %s\n", x,
                                        y, z, served.toString().c_str(),
                                        predicted.toString().c_str());
                            traceDraws(arm.legacy, seed, x, y, z);
                        }
                    }
                }
            }
        }
        std::printf("  outside the candidates: %lld blocks, %lld not %s\n", outside, outsideOther,
                    arm.defaultBlock.toString().c_str());
        std::printf("  aquifers_enabled %s; candidates left %s: %lld\n",
                    arm.aquifers ? "true" : "false", arm.defaultBlock.toString().c_str(),
                    untouched);
        std::printf("  candidates, by the block the server wrote:\n");
        for (const auto& [name, count] : census) {
            std::printf("    %-34s %9lld\n", name.c_str(), count);
        }
        std::printf("  derivations, exact / candidates:\n");
        for (std::size_t i = 0; i < all.size(); ++i) {
            const Score& s = scores[i];
            std::printf("    %9lld / %9lld  %8.4f%%  %s%s\n", s.exact, s.total,
                        s.total > 0
                            ? 100.0 * static_cast<double>(s.exact) / static_cast<double>(s.total)
                            : 0.0,
                        all[i].name.c_str(), s.exact == s.total ? "  EXACT" : "");
        }
    }

    // lv against mv, raw: the flag reached the vein source at all.
    if (windows.contains("lv") && windows.contains("mv")) {
        const Window& lv = *windows.at("lv");
        const Window& mv = *windows.at("mv");
        long long differ = 0;
        long long blocks = 0;
        for (std::int32_t z = 0; z < kWindow; ++z) {
            for (std::int32_t x = 0; x < kWindow; ++x) {
                for (std::int32_t y = ore::kLowestVeinY; y <= ore::kHighestVeinY; ++y) {
                    ++blocks;
                    differ += static_cast<long long>(!(lv.at(x, y, z) == mv.at(x, y, z)));
                }
            }
        }
        std::printf("\nlv against mv, raw: %lld of %lld blocks in y [%d, %d] differ\n", differ,
                    blocks, ore::kLowestVeinY, ore::kHighestVeinY);
    }
    return 0;
}

/// Two arms of one world, block for block over the whole window and every y:
/// the census of where they differ. How `la`/`ma` (aquifers off) were read
/// against `lv`/`mv`, and how any other pair can be.
int same(const std::filesystem::path& probe, const std::string& first, const std::string& second) {
    const Window a(probe / first / "r.0.0.mca");
    const Window b(probe / second / "r.0.0.mca");
    long long blocks = 0;
    long long differ = 0;
    long long veinBlocksA = 0;
    std::map<std::string, long long> pairs;
    for (std::int32_t z = 0; z < kWindow; ++z) {
        for (std::int32_t x = 0; x < kWindow; ++x) {
            for (std::int32_t y = -64; y < 320; ++y) {
                ++blocks;
                const chunk::BlockState left = a.at(x, y, z);
                const chunk::BlockState right = b.at(x, y, z);
                veinBlocksA += static_cast<long long>(left.name != "minecraft:stone" &&
                                                      left.name != "minecraft:air");
                if (!(left == right)) {
                    ++differ;
                    ++pairs[left.toString() + " / " + right.toString()];
                }
            }
        }
    }
    std::printf("%s against %s: %lld of %lld blocks differ (%s holds %lld blocks that are "
                "neither stone nor air)\n",
                first.c_str(), second.c_str(), differ, blocks, first.c_str(), veinBlocksA);
    for (const auto& [pair, count] : pairs) {
        std::printf("  %-60s %lld\n", pair.c_str(), count);
    }
    return 0;
}

// --- choosing positions, from the RNG alone ----------------------------------------

/// A position's first two draws under one source, as 24-bit integers.
std::pair<std::int32_t, std::int32_t> firstTwo(bool legacy, std::int64_t seed, std::int32_t x,
                                               std::int32_t y, std::int32_t z) {
    if (legacy) {
        rng::JavaRandom random = rng::legacyPositionalSourceFor(seed, kVeinSalt).at(x, y, z);
        const std::int32_t first = random.next(24);
        const std::int32_t second = random.next(24);
        return {first, second};
    }
    rng::Xoroshiro128PlusPlus random = rng::positionalSourceFor(seed, kVeinSalt).at(x, y, z);
    const auto first =
        static_cast<std::int32_t>(static_cast<std::uint64_t>(random.nextLong()) >> 40U);
    const auto second =
        static_cast<std::int32_t>(static_cast<std::uint64_t>(random.nextLong()) >> 40U);
    return {first, second};
}

int bucket(const std::vector<std::int64_t>& seeds) {
    for (const std::int64_t seed : seeds) {
        for (const bool legacy : {true, false}) {
            for (std::int32_t y = ore::kLowestVeinY; y <= ore::kHighestVeinY; ++y) {
                if (y > -8 && y < 0) {
                    continue; // the dead zone: no candidate
                }
                for (std::int32_t z = 0; z < kWindow; ++z) {
                    for (std::int32_t x = 0; x < kWindow; ++x) {
                        if (firstTwo(legacy, seed, x, y, z).first == kMembershipBucket) {
                            std::printf("seed %lld %s (%d, %d, %d)\n", static_cast<long long>(seed),
                                        legacy ? "legacy" : "xoroshiro", x, y, z);
                        }
                    }
                }
            }
        }
    }
    return 0;
}

int oreRoll(std::int64_t seed) {
    for (const bool legacy : {false, true}) {
        std::optional<std::int32_t> chosen;
        for (std::int32_t y = 20; y <= 30 && !chosen; ++y) {
            for (std::int32_t z = 0; z < kWindow && !chosen; ++z) {
                for (std::int32_t x = 0; x < kWindow && !chosen; ++x) {
                    const auto [first, second] = firstTwo(legacy, seed, x, y, z);
                    const double f = static_cast<double>(second) * 0x1.0P-24;
                    if (first < kMembershipBucket && f >= 0.25 && f < 0.3) {
                        chosen = second;
                        std::printf("%s seed %lld: first at (%d, %d, %d), second draw %d (%.17g)\n",
                                    legacy ? "legacy" : "xoroshiro", static_cast<long long>(seed),
                                    x, y, z, second, f);
                    }
                }
            }
        }
        if (!chosen) {
            continue;
        }
        const double f = static_cast<double>(*chosen) * 0x1.0P-24;
        const double toggle = (f + 0x1.0P-27) + 0.3;
        const double chance = clampedMap(toggle, 0.4, 0.6, 0.1, 0.3);
        const auto rounded = static_cast<double>(static_cast<float>(chance));
        std::printf("  toggle %.17g -> chance %.17g; (float) chance %.17g is f: %s; chance > f: "
                    "%s\n",
                    toggle, chance, rounded, !(rounded < f) && !(rounded > f) ? "yes" : "no",
                    chance > f ? "yes" : "no");
        for (std::int32_t y = 0; y <= 50; ++y) {
            for (std::int32_t z = 0; z < kWindow; ++z) {
                for (std::int32_t x = 0; x < kWindow; ++x) {
                    const auto [first, second] = firstTwo(legacy, seed, x, y, z);
                    if (second == *chosen && ore::clearsRichness(y, toggle)) {
                        std::printf("  second draw f at (%d, %d, %d), first %d (%s)\n", x, y, z,
                                    first, first <= kMembershipBucket ? "member" : "not");
                    }
                }
            }
        }
    }
    return 0;
}

int usage() {
    std::fprintf(stderr, "usage: legacy-orevein-analyze score <probe-dir> <seed>\n"
                         "       legacy-orevein-analyze bucket <seed>...\n"
                         "       legacy-orevein-analyze oreroll <seed>\n"
                         "       legacy-orevein-analyze same <probe-dir> <arm> <arm>\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::vector<std::string_view> args(argv + 1, argv + argc);
        if (args.size() == 3 && args[0] == "score") {
            return score(std::filesystem::path{args[1]}, std::stoll(std::string{args[2]}));
        }
        if (args.size() == 4 && args[0] == "same") {
            return same(std::filesystem::path{args[1]}, std::string{args[2]}, std::string{args[3]});
        }
        if (args.size() == 2 && args[0] == "oreroll") {
            return oreRoll(std::stoll(std::string{args[1]}));
        }
        if (args.size() >= 2 && args[0] == "bucket") {
            std::vector<std::int64_t> seeds;
            for (std::size_t i = 1; i < args.size(); ++i) {
                seeds.push_back(std::stoll(std::string{args[i]}));
            }
            return bucket(seeds);
        }
        return usage();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
}
