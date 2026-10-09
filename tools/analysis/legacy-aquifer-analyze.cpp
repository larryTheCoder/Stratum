// Stratum — reads back tools/analysis/legacy-aquifer-probe.sh's worlds.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The probe puts one open-void aquifer dimension in a world twice, once with
// `legacy_random_source` true (lj) and once false (mj), every router input a
// constant. This scores those worlds.
//
//   legacy-aquifer-analyze replay <probe-dir> <arm> <seed> [--source legacy|xoroshiro]
//                                 [--stride N]
//       Every block of every N-th column (default 1), two blocks in from the
//       window's edge, against `aquifer::computeSubstance` with the library's
//       centre source for that random source (default xoroshiro), classified as
//       agree / fluid that moved (tests/support/fluid_flow.hpp, the
//       conformance cases' own classifier) / unexplained, by 10-block band.
//
//   legacy-aquifer-analyze null <seed> [--stride N]
//       The same replay with no server in it: the modern source at <seed>
//       scored against a synthetic world drawn by the modern source at
//       <seed> + 1, through the same classifier (see `nullReplay`). What a
//       wrong centre field leaves UNEXPLAINED, as opposed to merely different.
//
//   legacy-aquifer-analyze control <probe-dir>
//       The positive control: lc against mc, a named noise read with the
//       flag on and off, column by column. And lj against mj, raw.
//
//   legacy-aquifer-analyze scan <probe-dir>:<arm>:<seed> ...
//   legacy-aquifer-analyze plant <rule> <seed> ...
//       The rule space below, scored on three one-bit readouts per cell
//       column (see "the readout"), every rule and the library's two
//       derivations alike.
//       `plant` scores it against readouts synthesised from rule <rule>,
//       which the scan must then return, alone, with its equivalence class.
//
// THE RULE SPACE, and where each axis comes from. Nothing in it was chosen
// from Mojang's or deepslate's source (CLAUDE.md): the base, salt, combine
// and fork axes are `tools/analysis/legacy-seed-analyze.cpp`'s seed-rule
// space as it stood before this tool, minus its two ordinal salts (the
// aquifer has no declaration ordinal); the position mix, the draw order and
// the bounds (10, 9, 10) are clean-room spec Q3.4, which says only the
// "combination step" differs under the legacy source; the bounded draws are
// java.util.Random's documented nextInt(bound), this project's own
// Xoroshiro128++ nextInt (Lemire's method) applied to the LCG's 32-bit
// draws, the power-of-two multiply-shift `(bound * next(31)) >> 31` applied
// to bounds that are not powers of two, which java.util.Random does NOT do
// and is here as a speculative generalisation only, and Xoroshiro128++
// itself seeded from the 64-bit result. The position may be XORed or added
// to the stream seed, and the result forked once more or not. 5 bases x 10
// salts x 3 combines x 3 fork counts x 2 position combines x 2 post-forks x
// 4 draws = 7200 rules. Rule outputs that coincide on every cell a scan
// reads are one equivalence class (a fingerprint of the draws), so a
// survivor is reported as a class, never as one index.
//
// WHAT IT FOUND (SPEC §11, "The aquifer under the legacy source"): on lj at
// seed 42 alone one class survives, rule 2464 (lcgLong xor hashCodeId,
// forks 1; position xor; nextInt(bound)), 192/192 against a next best of
// 148/192; on mj the shipped Xoroshiro source alone. Frozen, then confirmed
// on 31337 and on 42's 48-bit twin, which were generated afterwards; the
// library's legacy CentreSource is in the same class.
#include "support/fluid_flow.hpp"

#include <stratum/aquifer/lattice.hpp>
#include <stratum/aquifer/substance.hpp>
#include <stratum/density/random_source.hpp>
#include <stratum/hash/md5.hpp>
#include <stratum/javamath.hpp>
#include <stratum/rng/java_random.hpp>
#include <stratum/rng/xoroshiro128.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace stratum;
using test::Category;

namespace {

// --- the arm's constants ------------------------------------------------------

/// The aquifer arm's declared constants, read from the corpus's own spec.json
/// rather than restated, and refused unless every input is a constant.
struct Arm {
    double density = 0.0;
    std::int32_t seaLevel = 0;
    std::int32_t minY = 0;
    std::int32_t height = 0;
    double barrier = 0.0;
    double floodedness = 0.0;
    double spread = 0.0;
    double lava = 0.0;
    double psl = 0.0;
};

/// The probe's lj/mj constants, for the modes that have no corpus to read.
constexpr Arm kProbeArm{.density = -1.0,
                        .seaLevel = 63,
                        .minY = -64,
                        .height = 384,
                        .barrier = -2.0,
                        .floodedness = 0.5,
                        .spread = 0.0,
                        .lava = -1.0,
                        .psl = 96.0};

std::optional<Arm> readArm(const std::filesystem::path& probe, std::string_view name) {
    std::ifstream in(probe / "spec.json");
    if (!in) {
        std::fprintf(stderr, "no %s\n", (probe / "spec.json").string().c_str());
        return std::nullopt;
    }
    const nlohmann::json spec = nlohmann::json::parse(in);
    for (const auto& entry : spec) {
        if (entry.at("name").get<std::string>() != name) {
            continue;
        }
        const nlohmann::json& router = entry.at("router");
        for (const char* key : {"barrier", "fluid_level_floodedness", "fluid_level_spread", "lava",
                                "preliminary_surface_level"}) {
            if (!router.at(key).is_number()) {
                std::fprintf(stderr, "router entry %s of %s is not a constant\n", key,
                             std::string(name).c_str());
                return std::nullopt;
            }
        }
        return Arm{.density = entry.at("raw_final_density").at("argument").get<double>(),
                   .seaLevel = entry.at("sea_level").get<std::int32_t>(),
                   .minY = entry.at("min_y").get<std::int32_t>(),
                   .height = entry.at("height").get<std::int32_t>(),
                   .barrier = router.at("barrier").get<double>(),
                   .floodedness = router.at("fluid_level_floodedness").get<double>(),
                   .spread = router.at("fluid_level_spread").get<double>(),
                   .lava = router.at("lava").get<double>(),
                   .psl = router.at("preliminary_surface_level").get<double>()};
    }
    std::fprintf(stderr, "%s names no arm %s\n", probe.string().c_str(), std::string(name).c_str());
    return std::nullopt;
}

/// A cell's level from the library's own level rule, at a constant router.
std::int32_t levelAt(const Arm& arm, std::int32_t centreY) {
    return aquifer::cellFluidLevel(
        aquifer::CellFluid{.centreY = centreY,
                           .surface = aquifer::constantSurface(javamath::floorToInt(arm.psl)),
                           .seaLevel = arm.seaLevel,
                           .floodedness = arm.floodedness,
                           .spread = arm.spread});
}

// --- the rule space -----------------------------------------------------------

enum class Base : std::uint8_t { WorldSeed, LcgLong, XoroLo, Scrambled, Zero };
enum class Salt : std::uint8_t {
    Md5FirstBe,
    Md5FirstLe,
    Md5LastBe,
    Md5LastLe,
    Md5LoXorHi,
    Md5LoPlusHi,
    Md5PathFirstBe,
    HashCodeId,
    HashCodePath,
    None
};
enum class Combine : std::uint8_t { Xor, Add, Sub };
enum class Position : std::uint8_t { Xor, Add };
enum class Draw : std::uint8_t { JdkNextInt, Lemire32, MulShift31, Xoroshiro };

constexpr std::size_t kBases = 5;
constexpr std::size_t kSalts = 10;
constexpr std::size_t kCombines = 3;
constexpr std::size_t kForks = 3;
constexpr std::size_t kPositions = 2;
constexpr std::size_t kPostForks = 2;
constexpr std::size_t kDraws = 4;
constexpr std::size_t kRules =
    kBases * kSalts * kCombines * kForks * kPositions * kPostForks * kDraws;
/// Past the rule space: the library's own two derivations, scored beside the
/// rules so that one pass over a world says which of them it is — and that
/// the shipped legacy one is the same field as the rule it was taken from.
constexpr std::size_t kModern = kRules;
constexpr std::size_t kShippedLegacy = kRules + 1;
constexpr std::size_t kDerivations = kRules + 2;

struct Rule {
    Base base = Base::WorldSeed;
    Salt salt = Salt::None;
    Combine combine = Combine::Xor;
    int forks = 0;
    Position position = Position::Xor;
    int postForks = 0;
    Draw draw = Draw::JdkNextInt;
};

Rule ruleAt(std::size_t index) {
    Rule rule;
    rule.draw = static_cast<Draw>(index % kDraws);
    index /= kDraws;
    rule.postForks = static_cast<int>(index % kPostForks);
    index /= kPostForks;
    rule.position = static_cast<Position>(index % kPositions);
    index /= kPositions;
    rule.forks = static_cast<int>(index % kForks);
    index /= kForks;
    rule.combine = static_cast<Combine>(index % kCombines);
    index /= kCombines;
    rule.salt = static_cast<Salt>(index % kSalts);
    index /= kSalts;
    rule.base = static_cast<Base>(index % kBases);
    return rule;
}

std::string describe(std::size_t index) {
    if (index == kModern) {
        return "shipped CentreSource, RandomSource::Xoroshiro";
    }
    if (index == kShippedLegacy) {
        return "shipped CentreSource, RandomSource::Legacy";
    }
    static constexpr std::array<const char*, kBases> kBaseNames{"worldSeed", "lcgLong", "xoroLo",
                                                                "scrambled", "zero"};
    static constexpr std::array<const char*, kSalts> kSaltNames{
        "md5FirstBE",  "md5FirstLE",     "md5LastBE",  "md5LastLE",    "md5LoXorHi",
        "md5LoPlusHi", "md5PathFirstBE", "hashCodeId", "hashCodePath", "none"};
    static constexpr std::array<const char*, kCombines> kCombineNames{"xor", "add", "sub"};
    static constexpr std::array<const char*, kDraws> kDrawNames{
        "lcg nextInt(bound)", "lcg lemire(next(32))", "lcg (bound*next(31))>>31",
        "xoroshiro nextInt"};
    const Rule rule = ruleAt(index);
    return std::string{kBaseNames[static_cast<std::size_t>(rule.base)]} + " " +
           kCombineNames[static_cast<std::size_t>(rule.combine)] + " " +
           kSaltNames[static_cast<std::size_t>(rule.salt)] + ", forks " +
           std::to_string(rule.forks) + "; position " +
           (rule.position == Position::Xor ? "xor" : "add") + ", post-forks " +
           std::to_string(rule.postForks) + "; " + kDrawNames[static_cast<std::size_t>(rule.draw)];
}

constexpr std::string_view kSaltName = "minecraft:aquifer";

std::uint64_t beU64(const hash::Md5Digest& digest, std::size_t at) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value = (value << 8U) | digest.at(at + i);
    }
    return value;
}

std::uint64_t leU64(const hash::Md5Digest& digest, std::size_t at) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(digest.at(at + i)) << (8U * i);
    }
    return value;
}

/// java.lang.String.hashCode, as the JDK documents it.
std::int32_t javaHashCode(std::string_view text) {
    std::uint32_t hash = 0;
    for (const char character : text) {
        hash = (hash * 31U) + static_cast<std::uint32_t>(static_cast<unsigned char>(character));
    }
    return static_cast<std::int32_t>(hash);
}

std::uint64_t saltFor(Salt salt) {
    const std::string_view path = kSaltName.substr(kSaltName.find(':') + 1);
    const hash::Md5Digest digest = hash::md5(kSaltName);
    switch (salt) {
        case Salt::Md5FirstBe:
            return beU64(digest, 0);
        case Salt::Md5FirstLe:
            return leU64(digest, 0);
        case Salt::Md5LastBe:
            return beU64(digest, 8);
        case Salt::Md5LastLe:
            return leU64(digest, 8);
        case Salt::Md5LoXorHi:
            return beU64(digest, 0) ^ beU64(digest, 8);
        case Salt::Md5LoPlusHi:
            return beU64(digest, 0) + beU64(digest, 8);
        case Salt::Md5PathFirstBe:
            return beU64(hash::md5(path), 0);
        case Salt::HashCodeId:
            return static_cast<std::uint64_t>(static_cast<std::int64_t>(javaHashCode(kSaltName)));
        case Salt::HashCodePath:
            return static_cast<std::uint64_t>(static_cast<std::int64_t>(javaHashCode(path)));
        case Salt::None:
            return 0;
    }
    return 0;
}

std::uint64_t baseFor(Base base, std::int64_t worldSeed) {
    switch (base) {
        case Base::WorldSeed:
            return static_cast<std::uint64_t>(worldSeed);
        case Base::LcgLong: {
            rng::JavaRandom random{worldSeed};
            return static_cast<std::uint64_t>(random.nextLong());
        }
        case Base::XoroLo: {
            rng::Xoroshiro128PlusPlus random{worldSeed};
            return static_cast<std::uint64_t>(random.nextLong());
        }
        case Base::Scrambled:
            return (static_cast<std::uint64_t>(worldSeed) ^ rng::JavaRandom::kMultiplier) &
                   rng::JavaRandom::kMask;
        case Base::Zero:
            return 0;
    }
    return 0;
}

std::uint64_t forkLcg(std::uint64_t seed) {
    rng::JavaRandom random{static_cast<std::int64_t>(seed)};
    return static_cast<std::uint64_t>(random.nextLong());
}

/// The per-world stream seed a rule derives from the world seed and the salt.
std::uint64_t streamSeed(const Rule& rule, std::int64_t worldSeed) {
    const std::uint64_t base = baseFor(rule.base, worldSeed);
    const std::uint64_t salt = saltFor(rule.salt);
    std::uint64_t seed = 0;
    switch (rule.combine) {
        case Combine::Xor:
            seed = base ^ salt;
            break;
        case Combine::Add:
            seed = base + salt;
            break;
        case Combine::Sub:
            seed = base - salt;
            break;
    }
    for (int i = 0; i < rule.forks; ++i) {
        seed = forkLcg(seed);
    }
    return seed;
}

/// java.util.Random's 32-bit draws through the Lemire bounded draw this
/// project's Xoroshiro128++ uses.
std::int32_t lemire(rng::JavaRandom& random, std::int32_t bound) {
    const auto limit = static_cast<std::uint32_t>(bound);
    const auto low = [&] {
        return static_cast<std::uint64_t>(static_cast<std::uint32_t>(random.next(32)));
    };
    std::uint64_t scaled = low() * limit;
    if (static_cast<std::uint32_t>(scaled) < limit) {
        const std::uint32_t threshold = (~limit + 1U) % limit;
        while (static_cast<std::uint32_t>(scaled) < threshold) {
            scaled = low() * limit;
        }
    }
    return static_cast<std::int32_t>(scaled >> 32U);
}

/// The speculative one: java.util.Random's power-of-two path for any bound.
std::int32_t mulShift(rng::JavaRandom& random, std::int32_t bound) {
    const std::int64_t scaled = static_cast<std::int64_t>(bound) * random.next(31);
    return static_cast<std::int32_t>(javamath::shr(scaled, 31));
}

/// One derivation, ready to draw: a rule of the space at one world seed, or
/// the shipped modern source.
class Derivation {
public:
    Derivation(std::size_t index, std::int64_t worldSeed)
        : index_(index),
          shipped_(worldSeed, index == kShippedLegacy ? density::RandomSource::Legacy
                                                      : density::RandomSource::Xoroshiro),
          rule_(index < kRules ? ruleAt(index) : Rule{}),
          stream_(index < kRules ? streamSeed(rule_, worldSeed) : 0) {}

    [[nodiscard]] aquifer::Jitter jitterOf(std::int32_t cx, std::int32_t cy,
                                           std::int32_t cz) const {
        if (index_ >= kRules) {
            return shipped_.jitterOf(cx, cy, cz);
        }
        const auto mix = static_cast<std::uint64_t>(rng::positionSeed(cx, cy, cz));
        std::uint64_t seed = rule_.position == Position::Xor ? stream_ ^ mix : stream_ + mix;
        for (int i = 0; i < rule_.postForks; ++i) {
            seed = forkLcg(seed);
        }
        if (rule_.draw == Draw::Xoroshiro) {
            rng::Xoroshiro128PlusPlus random{static_cast<std::int64_t>(seed)};
            const std::int32_t u = random.nextInt(aquifer::kJitterBoundX);
            const std::int32_t v = random.nextInt(aquifer::kJitterBoundY);
            const std::int32_t w = random.nextInt(aquifer::kJitterBoundZ);
            return aquifer::Jitter{.x = u, .y = v, .z = w};
        }
        rng::JavaRandom random{static_cast<std::int64_t>(seed)};
        const auto draw = [&](std::int32_t bound) {
            switch (rule_.draw) {
                case Draw::JdkNextInt:
                    return random.nextInt(bound);
                case Draw::Lemire32:
                    return lemire(random, bound);
                case Draw::MulShift31:
                case Draw::Xoroshiro:
                    break;
            }
            return mulShift(random, bound);
        };
        const std::int32_t u = draw(aquifer::kJitterBoundX);
        const std::int32_t v = draw(aquifer::kJitterBoundY);
        const std::int32_t w = draw(aquifer::kJitterBoundZ);
        return aquifer::Jitter{.x = u, .y = v, .z = w};
    }

    [[nodiscard]] aquifer::CellIndex centreOf(std::int32_t cx, std::int32_t cy,
                                              std::int32_t cz) const {
        const aquifer::Jitter jitter = jitterOf(cx, cy, cz);
        return aquifer::CellIndex{.x = (cx * aquifer::kCellPitchX) + jitter.x,
                                  .y = (cy * aquifer::kCellPitchY) + jitter.y,
                                  .z = (cz * aquifer::kCellPitchZ) + jitter.z};
    }

private:
    std::size_t index_;
    aquifer::CentreSource shipped_;
    Rule rule_;
    std::uint64_t stream_;
};

// --- the readout ----------------------------------------------------------------
//
// THREE BITS PER CELL COLUMN, each a cell's level read at one height. With the
// probe's constants a cell's level depends only on its centre's 40-block band,
// so most layers' levels do not depend on the vertical draw at all and three
// do (`informativeLayers` finds them from the library's level rule, rather than
// this file restating it): at layer -4 the level moves only when v reaches 8,
// at layer 3 when v reaches 4, at layer 6 when v reaches 8. Each is read at
// y = 12L + 4, which is at most 4 blocks from the cell's own centre and at
// least 8 from either vertical neighbour's, in the 6x6 CORE columns of the
// cell, x and z in [16c + 2, 16c + 7], which no horizontal neighbour can be
// nearer to. Which cell owns each core block is still decided per derivation,
// over the 27 neighbouring cells, and a block whose nearest centre is not the
// cell being read is skipped, as is a tie. A cell is read when at least 12 of
// its 36 core blocks are its own, by majority.

constexpr std::int32_t kCellsPerSide = 8;
constexpr std::int32_t kCoreFrom = 2;
constexpr std::int32_t kCoreTo = 7;
constexpr std::int32_t kReadOffset = 4;
constexpr int kMinOwned = 12;

struct World {
    std::string label;
    std::int64_t seed = 0;
    Arm arm;
    /// observed[layer slot][cz][cx][z - core][x - core]: fluid at the read height.
    std::vector<std::vector<std::vector<std::array<std::array<bool, 6>, 6>>>> observed;
};

std::vector<std::int32_t> informativeLayers(const Arm& arm) {
    std::vector<std::int32_t> layers;
    const std::int32_t lowest = javamath::floorDiv(arm.minY, aquifer::kCellPitchY) + 1;
    const std::int32_t highest =
        javamath::floorDiv(arm.minY + arm.height, aquifer::kCellPitchY) - 1;
    for (std::int32_t layer = lowest; layer <= highest; ++layer) {
        const std::int32_t y = (layer * aquifer::kCellPitchY) + kReadOffset;
        if (y < aquifer::lambdaLevel(arm.seaLevel)) {
            continue; // the global lava sea, whatever the cell says
        }
        int wet = 0;
        for (std::int32_t v = 0; v < aquifer::kJitterBoundY; ++v) {
            wet += static_cast<int>(y < levelAt(arm, (layer * aquifer::kCellPitchY) + v));
        }
        if (wet != 0 && wet != aquifer::kJitterBoundY) {
            layers.push_back(layer);
        }
    }
    return layers;
}

bool predictedWet(const Arm& arm, std::int32_t layer, std::int32_t centreY) {
    return (layer * aquifer::kCellPitchY) + kReadOffset < levelAt(arm, centreY);
}

struct Score {
    int checked = 0;
    int agree = 0;
    /// A hash of every draw the score consulted, for equivalence classes.
    std::uint64_t fingerprint = 0xcbf29ce484222325ULL;
};

void mix(std::uint64_t& hash, std::int32_t value) {
    hash = (hash ^ static_cast<std::uint32_t>(value)) * 0x100000001b3ULL;
}

/// Scores one derivation on one world's readouts.
void score(const World& world, const std::vector<std::int32_t>& layers,
           const Derivation& derivation, Score& out) {
    for (std::size_t slot = 0; slot < layers.size(); ++slot) {
        const std::int32_t layer = layers[slot];
        const std::int32_t y = (layer * aquifer::kCellPitchY) + kReadOffset;
        // The 27-neighbour centres for every cell of the window, once.
        // Flat, [cz + 1][cy - layer + 1][cx + 1]: this is the scan's inner loop.
        constexpr std::size_t kSide = kCellsPerSide + 2;
        std::array<aquifer::CellIndex, kSide * 3 * kSide> centres{};
        const auto at = [&](std::int32_t cx, std::int32_t cy,
                            std::int32_t cz) -> aquifer::CellIndex& {
            return centres[(((static_cast<std::size_t>(cz + 1) * 3) +
                             static_cast<std::size_t>(cy - layer + 1)) *
                            kSide) +
                           static_cast<std::size_t>(cx + 1)];
        };
        for (std::int32_t cz = -1; cz <= kCellsPerSide; ++cz) {
            for (std::int32_t cy = layer - 1; cy <= layer + 1; ++cy) {
                for (std::int32_t cx = -1; cx <= kCellsPerSide; ++cx) {
                    at(cx, cy, cz) = derivation.centreOf(cx, cy, cz);
                }
            }
        }
        for (std::int32_t cz = 0; cz < kCellsPerSide; ++cz) {
            for (std::int32_t cx = 0; cx < kCellsPerSide; ++cx) {
                const aquifer::CellIndex own = at(cx, layer, cz);
                mix(out.fingerprint, own.x);
                mix(out.fingerprint, own.y);
                mix(out.fingerprint, own.z);
                int owned = 0;
                int wet = 0;
                for (std::int32_t dz = kCoreFrom; dz <= kCoreTo; ++dz) {
                    for (std::int32_t dx = kCoreFrom; dx <= kCoreTo; ++dx) {
                        const std::int32_t x = (cx * aquifer::kCellPitchX) + dx;
                        const std::int32_t z = (cz * aquifer::kCellPitchZ) + dz;
                        const auto distance = [&](const aquifer::CellIndex& c) {
                            const std::int64_t ex = x - c.x;
                            const std::int64_t ey = y - c.y;
                            const std::int64_t ez = z - c.z;
                            return (ex * ex) + (ey * ey) + (ez * ez);
                        };
                        const std::int64_t mine = distance(own);
                        bool nearest = true;
                        for (std::int32_t oz = -1; oz <= 1 && nearest; ++oz) {
                            for (std::int32_t oy = -1; oy <= 1 && nearest; ++oy) {
                                for (std::int32_t ox = -1; ox <= 1 && nearest; ++ox) {
                                    if (ox == 0 && oy == 0 && oz == 0) {
                                        continue;
                                    }
                                    nearest = distance(at(cx + ox, layer + oy, cz + oz)) > mine;
                                }
                            }
                        }
                        if (!nearest) {
                            continue;
                        }
                        ++owned;
                        wet += static_cast<int>(
                            world.observed[slot][static_cast<std::size_t>(cz)]
                                          [static_cast<std::size_t>(cx)]
                                          [static_cast<std::size_t>(dz - kCoreFrom)]
                                          [static_cast<std::size_t>(dx - kCoreFrom)]);
                    }
                }
                if (owned < kMinOwned || wet * 2 == owned) {
                    continue;
                }
                const bool observed = wet * 2 > owned;
                ++out.checked;
                out.agree += static_cast<int>(observed == predictedWet(world.arm, layer, own.y));
            }
        }
    }
}

std::optional<World> readWorld(const std::string& spec) {
    // <probe-dir>:<arm>:<seed>, split from the right: a path may hold a colon.
    const std::size_t last = spec.rfind(':');
    const std::size_t middle = last == std::string::npos ? last : spec.rfind(':', last - 1);
    if (middle == std::string::npos) {
        std::fprintf(stderr, "expected <probe-dir>:<arm>:<seed>, got %s\n", spec.c_str());
        return std::nullopt;
    }
    const std::filesystem::path probe = spec.substr(0, middle);
    const std::string name = spec.substr(middle + 1, last - middle - 1);
    World world;
    world.label = spec;
    world.seed = std::stoll(spec.substr(last + 1));
    const std::optional<Arm> arm = readArm(probe, name);
    if (!arm) {
        return std::nullopt;
    }
    world.arm = *arm;
    test::GoldenRegion golden(probe / name / "r.0.0.mca");
    for (const std::int32_t layer : informativeLayers(world.arm)) {
        const std::int32_t y = (layer * aquifer::kCellPitchY) + kReadOffset;
        auto& slot = world.observed.emplace_back(
            kCellsPerSide, std::vector<std::array<std::array<bool, 6>, 6>>(kCellsPerSide));
        for (std::int32_t cz = 0; cz < kCellsPerSide; ++cz) {
            for (std::int32_t cx = 0; cx < kCellsPerSide; ++cx) {
                for (std::int32_t dz = kCoreFrom; dz <= kCoreTo; ++dz) {
                    for (std::int32_t dx = kCoreFrom; dx <= kCoreTo; ++dx) {
                        const chunk::BlockState* block = golden.blockAt(
                            (cx * aquifer::kCellPitchX) + dx, y, (cz * aquifer::kCellPitchZ) + dz);
                        if (block == nullptr) {
                            std::fprintf(stderr, "%s: no block at a core column\n", spec.c_str());
                            return std::nullopt;
                        }
                        slot[static_cast<std::size_t>(cz)][static_cast<std::size_t>(cx)]
                            [static_cast<std::size_t>(dz - kCoreFrom)]
                            [static_cast<std::size_t>(dx - kCoreFrom)] =
                                test::isFluid(test::categoryOf(block->name));
                    }
                }
            }
        }
    }
    return world;
}

/// A world whose readouts are what derivation @p planted predicts, with the
/// same ownership rule the scan applies.
World plantedWorld(std::size_t planted, std::int64_t seed) {
    World world;
    world.label = "planted " + std::to_string(planted) + " at " + std::to_string(seed);
    world.seed = seed;
    world.arm = kProbeArm;
    const Derivation derivation(planted, seed);
    for (const std::int32_t layer : informativeLayers(world.arm)) {
        const std::int32_t y = (layer * aquifer::kCellPitchY) + kReadOffset;
        auto& slot = world.observed.emplace_back(
            kCellsPerSide, std::vector<std::array<std::array<bool, 6>, 6>>(kCellsPerSide));
        for (std::int32_t cz = 0; cz < kCellsPerSide; ++cz) {
            for (std::int32_t cx = 0; cx < kCellsPerSide; ++cx) {
                for (std::int32_t dz = kCoreFrom; dz <= kCoreTo; ++dz) {
                    for (std::int32_t dx = kCoreFrom; dx <= kCoreTo; ++dx) {
                        const std::int32_t x = (cx * aquifer::kCellPitchX) + dx;
                        const std::int32_t z = (cz * aquifer::kCellPitchZ) + dz;
                        std::int64_t best = INT64_MAX;
                        std::int32_t ownerY = 0;
                        for (std::int32_t oz = -1; oz <= 1; ++oz) {
                            for (std::int32_t oy = -1; oy <= 1; ++oy) {
                                for (std::int32_t ox = -1; ox <= 1; ++ox) {
                                    const aquifer::CellIndex c =
                                        derivation.centreOf(cx + ox, layer + oy, cz + oz);
                                    const std::int64_t ex = x - c.x;
                                    const std::int64_t ey = y - c.y;
                                    const std::int64_t ez = z - c.z;
                                    const std::int64_t d = (ex * ex) + (ey * ey) + (ez * ez);
                                    if (d < best) {
                                        best = d;
                                        ownerY = c.y;
                                    }
                                }
                            }
                        }
                        slot[static_cast<std::size_t>(cz)][static_cast<std::size_t>(cx)]
                            [static_cast<std::size_t>(dz - kCoreFrom)]
                            [static_cast<std::size_t>(dx - kCoreFrom)] =
                                y < levelAt(world.arm, ownerY);
                    }
                }
            }
        }
    }
    return world;
}

int scan(const std::vector<World>& worlds) {
    const std::vector<std::int32_t> layers = informativeLayers(worlds.front().arm);
    std::printf("informative layers:");
    for (const std::int32_t layer : layers) {
        std::printf(" %d (read at y %d)", layer, (layer * aquifer::kCellPitchY) + kReadOffset);
    }
    std::printf("\n");
    for (const World& world : worlds) {
        if (informativeLayers(world.arm) != layers) {
            std::fprintf(stderr, "%s has other informative layers\n", world.label.c_str());
            return 2;
        }
    }
    std::vector<Score> scores(kDerivations);
    for (std::size_t index = 0; index < kDerivations; ++index) {
        for (const World& world : worlds) {
            score(world, layers, Derivation(index, world.seed), scores[index]);
        }
    }
    // The null: the distribution of agreement over the whole space.
    std::array<int, 11> deciles{};
    for (std::size_t index = 0; index < kRules; ++index) {
        const Score& s = scores[index];
        const int decile = s.checked == 0 ? 0 : (10 * s.agree) / s.checked;
        ++deciles.at(static_cast<std::size_t>(std::clamp(decile, 0, 10)));
    }
    std::printf("rules by agreement decile (0-10%% .. 100%%):");
    for (const int count : deciles) {
        std::printf(" %d", count);
    }
    std::printf("\n");
    std::vector<std::size_t> order(kDerivations);
    for (std::size_t i = 0; i < kDerivations; ++i) {
        order[i] = i;
    }
    std::ranges::sort(order, [&](std::size_t a, std::size_t b) {
        const auto rate = [&](std::size_t i) {
            return scores[i].checked == 0
                       ? 0.0
                       : static_cast<double>(scores[i].agree) / scores[i].checked;
        };
        return rate(a) > rate(b);
    });
    std::map<std::uint64_t, std::vector<std::size_t>> survivors;
    for (std::size_t i = 0; i < kDerivations; ++i) {
        const Score& s = scores[i];
        if (s.checked > 0 && s.agree == s.checked) {
            survivors[s.fingerprint].push_back(i);
        }
    }
    std::printf("shipped Xoroshiro: %d of %d; shipped legacy: %d of %d\n", scores[kModern].agree,
                scores[kModern].checked, scores[kShippedLegacy].agree,
                scores[kShippedLegacy].checked);
    std::printf("top ten:\n");
    for (std::size_t k = 0; k < 10 && k < order.size(); ++k) {
        const std::size_t i = order[k];
        std::printf("  %5zu  %d/%d  %s\n", i, scores[i].agree, scores[i].checked,
                    describe(i).c_str());
    }
    std::printf("%zu surviving equivalence class(es)\n", survivors.size());
    for (const auto& [fingerprint, members] : survivors) {
        std::printf("  class %016llx, %zu member(s):\n",
                    static_cast<unsigned long long>(fingerprint), members.size());
        for (const std::size_t i : members) {
            std::printf("    %5zu  %d/%d  %s\n", i, scores[i].agree, scores[i].checked,
                        describe(i).c_str());
        }
    }
    return 0;
}

// --- the block replay -----------------------------------------------------------

Category expectedCategory(const aquifer::SubstanceAt& substance) {
    switch (substance.substance) {
        case aquifer::Substance::Air:
            return Category::Air;
        case aquifer::Substance::Solid:
            return Category::Solid;
        case aquifer::Substance::Fluid:
            return substance.fluidType == aquifer::FluidType::Lava ? Category::Lava
                                                                   : Category::Water;
    }
    return Category::Solid;
}

Category modelAt(const aquifer::CentreSource& centres, aquifer::StatusCache& statuses,
                 const Arm& arm, std::int32_t x, std::int32_t y, std::int32_t z) {
    const auto constant = [](double value) {
        return [value](std::int32_t, std::int32_t, std::int32_t) { return value; };
    };
    return expectedCategory(aquifer::computeSubstance(
        centres,
        aquifer::AquiferQuery{
            .x = x, .y = y, .z = z, .density = arm.density, .seaLevel = arm.seaLevel},
        statuses, constant(arm.barrier), constant(arm.floodedness), constant(arm.spread),
        constant(arm.lava), constant(arm.psl), aquifer::NoDeepDark{}));
}

struct Tally {
    long long blocks = 0;
    long long agree = 0;
    long long flow = 0;
    long long unexplained = 0;
    std::map<int, long long> byBand;
};

void print(const std::string& label, const Tally& tally) {
    std::printf("%s: blocks %lld agree %lld flow %lld unexplained %lld\n", label.c_str(),
                tally.blocks, tally.agree, tally.flow, tally.unexplained);
    for (const auto& [band, count] : tally.byBand) {
        std::printf("  y %4d..%4d  unexplained %lld\n", band, band + 9, count);
    }
}

constexpr std::int32_t kWindow = kCellsPerSide * aquifer::kCellPitchX;
constexpr std::int32_t kEdgeMargin = 2;

int replay(const std::filesystem::path& probe, const std::string& name, std::int64_t seed,
           density::RandomSource source, std::int32_t stride) {
    const std::optional<Arm> arm = readArm(probe, name);
    if (!arm) {
        return 2;
    }
    const aquifer::CentreSource centres{seed, source};
    test::GoldenRegion golden(probe / name / "r.0.0.mca");
    aquifer::StatusCache statuses;
    Tally tally;
    for (std::int32_t z = kEdgeMargin; z < kWindow - kEdgeMargin; z += stride) {
        for (std::int32_t x = kEdgeMargin; x < kWindow - kEdgeMargin; x += stride) {
            for (std::int32_t y = arm->minY; y < arm->minY + arm->height; ++y) {
                const auto* theirs = golden.blockAt(x, y, z);
                const Category g = test::categoryOf(
                    theirs != nullptr ? theirs->name : std::string("minecraft:air"));
                const Category r = modelAt(centres, statuses, *arm, x, y, z);
                ++tally.blocks;
                if (g == r) {
                    ++tally.agree;
                } else if (test::explainedByFlow(golden, x, y, z, g, r)) {
                    ++tally.flow;
                } else {
                    ++tally.unexplained;
                    ++tally.byBand[javamath::floorDiv(y, 10) * 10];
                }
            }
        }
    }
    print(name + " at seed " + std::to_string(seed) + ", " +
              std::string(source == density::RandomSource::Legacy ? "legacy" : "xoroshiro") +
              " centres",
          tally);
    return 0;
}

/// The replay with a synthetic golden. The synthetic world holds every fluid
/// as a source and has never ticked, so of fluid_flow.hpp's five shapes only
/// one can fire — golden water SOURCE where the model has air, between at
/// least two horizontal water sources — and that one is evaluated here on
/// the synthetic world exactly as the classifier evaluates it on a region.
int nullReplay(std::int64_t seed, std::int32_t stride) {
    const aquifer::CentreSource model{seed, density::RandomSource::Xoroshiro};
    const aquifer::CentreSource truth{seed + 1, density::RandomSource::Xoroshiro};
    aquifer::StatusCache modelStatuses;
    aquifer::StatusCache truthStatuses;
    const Arm& arm = kProbeArm;
    Tally tally;
    for (std::int32_t z = kEdgeMargin; z < kWindow - kEdgeMargin; z += stride) {
        for (std::int32_t x = kEdgeMargin; x < kWindow - kEdgeMargin; x += stride) {
            for (std::int32_t y = arm.minY; y < arm.minY + arm.height; ++y) {
                const Category g = modelAt(truth, truthStatuses, arm, x, y, z);
                const Category r = modelAt(model, modelStatuses, arm, x, y, z);
                ++tally.blocks;
                if (g == r) {
                    ++tally.agree;
                    continue;
                }
                int sources = 0;
                if (g == Category::Water && r == Category::Air) {
                    for (const auto& [dx, dz] :
                         std::array<std::pair<int, int>, 4>{{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}}) {
                        sources += static_cast<int>(modelAt(truth, truthStatuses, arm, x + dx, y,
                                                            z + dz) == Category::Water);
                    }
                }
                if (sources >= 2) {
                    ++tally.flow;
                } else {
                    ++tally.unexplained;
                    ++tally.byBand[javamath::floorDiv(y, 10) * 10];
                }
            }
        }
    }
    print("null: modern at " + std::to_string(seed) + " against a world drawn at " +
              std::to_string(seed + 1),
          tally);
    return 0;
}

/// Highest non-air block of a column, or minY - 1.
std::int32_t surfaceOf(test::GoldenRegion& region, std::int32_t x, std::int32_t z,
                       std::int32_t minY, std::int32_t height) {
    for (std::int32_t y = minY + height - 1; y >= minY; --y) {
        const chunk::BlockState* block = region.blockAt(x, y, z);
        if (block != nullptr && test::categoryOf(block->name) != Category::Air) {
            return y;
        }
    }
    return minY - 1;
}

int control(const std::filesystem::path& probe) {
    test::GoldenRegion legacy(probe / "lc" / "r.0.0.mca");
    test::GoldenRegion modern(probe / "mc" / "r.0.0.mca");
    int columns = 0;
    int differ = 0;
    for (std::int32_t z = 0; z < kWindow; ++z) {
        for (std::int32_t x = 0; x < kWindow; ++x) {
            ++columns;
            differ += static_cast<int>(surfaceOf(legacy, x, z, -64, 384) !=
                                       surfaceOf(modern, x, z, -64, 384));
        }
    }
    std::printf("lc against mc: %d of %d columns differ in height\n", differ, columns);
    test::GoldenRegion lj(probe / "lj" / "r.0.0.mca");
    test::GoldenRegion mj(probe / "mj" / "r.0.0.mca");
    long long blocks = 0;
    long long rawDiffer = 0;
    for (std::int32_t z = 0; z < kWindow; ++z) {
        for (std::int32_t x = 0; x < kWindow; ++x) {
            for (std::int32_t y = -64; y < 320; ++y) {
                const chunk::BlockState* a = lj.blockAt(x, y, z);
                const chunk::BlockState* b = mj.blockAt(x, y, z);
                ++blocks;
                rawDiffer += static_cast<long long>(
                    test::categoryOf(a != nullptr ? a->name : std::string("minecraft:air")) !=
                    test::categoryOf(b != nullptr ? b->name : std::string("minecraft:air")));
            }
        }
    }
    std::printf("lj against mj: %lld of %lld blocks differ in category\n", rawDiffer, blocks);
    return 0;
}

density::RandomSource sourceFrom(int argc, char** argv, int from) {
    for (int i = from; i + 1 < argc; ++i) {
        if (std::string_view{argv[i]} == "--source" && std::string_view{argv[i + 1]} == "legacy") {
            return density::RandomSource::Legacy;
        }
    }
    return density::RandomSource::Xoroshiro;
}

std::int32_t strideFrom(int argc, char** argv, int from) {
    for (int i = from; i + 1 < argc; ++i) {
        if (std::string_view{argv[i]} == "--stride") {
            return static_cast<std::int32_t>(std::stoi(argv[i + 1]));
        }
    }
    return 1;
}

int run(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "replay" && argc >= 5) {
        return replay(argv[2], argv[3], std::stoll(argv[4]), sourceFrom(argc, argv, 5),
                      strideFrom(argc, argv, 5));
    }
    if (mode == "null" && argc >= 3) {
        return nullReplay(std::stoll(argv[2]), strideFrom(argc, argv, 3));
    }
    if (mode == "control" && argc >= 3) {
        return control(argv[2]);
    }
    if (mode == "scan" && argc >= 3) {
        std::vector<World> worlds;
        for (int i = 2; i < argc; ++i) {
            std::optional<World> world = readWorld(argv[i]);
            if (!world) {
                return 2;
            }
            worlds.push_back(std::move(*world));
        }
        return scan(worlds);
    }
    if (mode == "plant" && argc >= 4) {
        const auto planted = static_cast<std::size_t>(std::stoull(argv[2]));
        if (planted >= kDerivations) {
            std::fprintf(stderr, "rule %zu is outside 0..%zu\n", planted, kDerivations - 1);
            return 2;
        }
        std::vector<World> worlds;
        for (int i = 3; i < argc; ++i) {
            worlds.push_back(plantedWorld(planted, std::stoll(argv[i])));
        }
        std::printf("planted: %s\n", describe(planted).c_str());
        return scan(worlds);
    }
    std::fprintf(stderr, "usage: legacy-aquifer-analyze replay <probe> <arm> <seed> [--source "
                         "legacy|xoroshiro] [--stride N]\n"
                         "       legacy-aquifer-analyze null <seed> [--stride N]\n"
                         "       legacy-aquifer-analyze control <probe>\n"
                         "       legacy-aquifer-analyze scan <probe>:<arm>:<seed> ...\n"
                         "       legacy-aquifer-analyze plant <rule> <seed> ...\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    // A missing fixture, a malformed region or a bad number surfaces as an
    // exception; report it and fail rather than terminate.
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
