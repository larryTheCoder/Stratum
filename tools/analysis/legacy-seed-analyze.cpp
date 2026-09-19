// Stratum — scores candidate legacy seedings for a NAMED noise.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Reads tools/analysis/legacy-seed-probe.sh's worlds, inverts every
// cell-corner column into a reading of the noise at (x, 0, z) — see
// density-probe.sh's header for the trick — and asks, for a large enumerated
// space of candidate rules, which of them reproduces those readings.
//
// THE ANSWER SO FAR IS NONE, and the point of this file is that the claim
// comes with a denominator that matches what the code actually does. The
// space it enumerates is exactly:
//
//   base       (5)  the world seed; JavaRandom(worldSeed).nextLong();
//                   the low half of XoroshiroPositionalFactory(worldSeed);
//                   the world seed put through the LCG's own scramble; zero
//   salt      (10)  MD5("ns:path") read as first-eight big-endian, first-eight
//                   little-endian, last-eight big-endian, last-eight
//                   little-endian, lo^hi, lo+hi; MD5("path") first-eight
//                   big-endian; String.hashCode("ns:path");
//                   String.hashCode("path"); no salt at all
//   combine    (3)  base ^ salt, base + salt, base - salt
//   forks      (3)  0, 1 or 2 further rounds of seed = JavaRandom(seed).nextLong()
//   generator  (2)  the Java LCG, or Xoroshiro128++
//   block    (300)  how many complete Perlin blocks are drawn and discarded
//                   before the stack is built
//
// 5 x 10 x 3 x 3 x 2 = 900 seed rules, times 300 block offsets = 270,000
// candidates per dimension. That number is printed with every run, so a claim
// about "N candidates refuted" can be checked against the tool rather than
// against a memory of it.
//
// WHAT IT DOES NOT COVER, said plainly rather than left to be assumed: only
// ONE stack rule, the sequential one — every octave's Perlin block drawn in
// order from the single generator, which is the rule this project has already
// confirmed for `old_blended_noise` under the same flag. The modern
// per-octave MD5 salting scheme adapted to an LCG is NOT in this space. Nor
// is any frequency rule other than the noise's own declared `firstOctave`: an
// earlier write-up described the sweep as covering "frequency
// 2^(firstOctave +/- 1..3)", which no version of this code has done.
//
// THE NULL IS MEASURED, NOT ASSUMED, and that is the other half of the point.
// Every candidate in the space is wrong, so the distribution of their
// agreement counts IS this dimension's null, and `--scan` prints it: mean,
// maximum, and the band and scale that produced it. It is NOT one number
// across dimensions. The band a candidate has to land in is
// 2 / height / (K * scale), so the probe carries the same noise at three
// output scales — `leg_skip`, `leg_skip_q4`, `leg_skip_q16` — under which
// nothing about the seeding changes and the null moves by more than an order
// of magnitude. Quoting one "empirical null" across configurations turns that
// arithmetic into an apparent signal; it is how a score that is merely at its
// own dimension's null reads as an outlier.
//
// DEEPSLATE'S OWN DERIVATION IS A MEMBER OF THIS SPACE, not something outside
// it: base = JavaRandom(worldSeed).nextLong(), salted with the first eight
// bytes of MD5("ns:path") big-endian, XORed, then one further round of
// seed = JavaRandom(seed).nextLong(), driving the LCG, at block offset 0.
// That is rule 182, block 0, and `--candidate 182 0` scores it by name rather
// than leaving it to be inferred from a list of survivors it is absent from.
//
// AND THE SEARCH IS CALIBRATED. `--plant <seedRule> <block>` replaces the
// server's readings with readings synthesised from that candidate, put
// through the same quantisation the server's terrain imposes, and then runs
// the identical scan. If the scan does not return the planted candidate
// first, the scan cannot find a correct answer at this candidate count and no
// refutation drawn from it means anything.
//
// THE PLANT IS NOT ENOUGH, and `--control` is the half it cannot supply. A
// plant synthesises its readings through THIS FILE'S OWN forward model —
// layoutFor, sampleNormal, quantise — so a wrong forward model (a wrong
// persistence schedule, a wrong valueFactor, a missing second stack, a wrong
// cell-corner assumption, a wrong inversion) would be shared by the plant and
// the scan alike and the plant would still come back at rank 1. It shows the
// statistic is SENSITIVE; it cannot show the model is RIGHT.
//
// `--control` scores a rule that is known to be right, against readings that
// come from the server rather than from this file. The probe's mod_* mirror
// dimensions declare no legacy flag, so their noises are seeded by the modern
// derivation lib/src/noise_registry.cpp implements and the conformance suite
// already validates: XoroshiroPositionalFactory(worldSeed).fromHashOf(id),
// then NormalNoise::create. It is scored three ways, through the SAME
// readback, the SAME band and the SAME columns the scan uses:
//
//   library   the library's own NormalNoise::sample — does a correct
//             named-noise rule survive this readback at all?
//   model     this file's layoutFor + sampleNormal driven by modern-seeded
//             Perlin blocks — is the forward model the scan and the plant
//             share the same function the library computes?
//   exact     quantise(model value) compared to the server's reading as an
//             exact double — is the inversion the plant relies on the
//             server's own arithmetic, not merely close to it?
//   negative  THE SAME modern rule at worldSeed + 1, against the same
//             mirror dimensions — does the recovery come from the SEEDING,
//             or would this readback accept anything?
//
// plus layoutFor's hand-rolled valueFactor against the library's
// NormalNoise::valueFactor(), compared as bits.
//
// The negative arm is not the same argument as the legacy-side mirror. The
// legacy dimensions vary the FLAG and the seeding together, so a rule that
// scores at the null there might be failing for either reason; worldSeed + 1
// on the mod_* dimensions varies one thing. It has to FAIL for the control to
// mean anything, and --control treats a negative arm above one tenth of the
// columns as a control failure.
//
// THREE ARMS AND NOT ONE, because they do not see the same faults, which was
// measured rather than assumed. Breaking the model on purpose: dropping the
// second stack or moving its 337/331 ratio collapses the band arm (27-763 of
// 2304). But a ONE-ULP valueFactor — the algebraically equal (5n)/(3(n+1))
// this project already measured off the server and rejected — passes the
// band arm at 2304/2304 AND the exact arm at 2304/2304, and is caught only
// by the bit comparison. And writing the inversion as floor(t - 0.5) rather
// than ceil(t) - 1, the bug that once made the plant score 51%, passes the
// band arm at 2304/2304 and shows up only in the exact arm, at 1107-1166 of
// 2304 (48.0-50.6%). A
// control with one arm would have missed one of the two. See SPEC §11.
//
// If those recover the mod_* dimensions and sit at the null on the leg_*
// ones, then "270,000 candidates, no survivor" is a measurement: a correct
// named-noise rule IS recoverable through this apparatus, and the legacy
// derivation is absent from the space rather than invisible to the tool.
//
// TWO CAVEATS, and neither is discharged by anything above.
//
//   1. It does NOT widen the space: a control on a modern rule cannot say
//      whether the legacy rule uses a stack shape this scan never enumerates.
//
//   2. Everything the control validates, it validates on the MODERN
//      dimensions. The step from "no survivor" to "absent from the space
//      rather than invisible to the tool" therefore ASSUMES the legacy flag
//      changes the SEEDING and nothing else in the terrain pipeline. Nothing
//      here measures that, and it is stated as the assumption it is.
//      `--profile` bounds it without proving it: it reports each dimension's
//      spread and its lag-4 spatial autocorrelation, so a legacy readback
//      whose gradient, output scale, cell spacing or sampling coordinates
//      differed from its modern mirror's would not match it. The measured
//      figures are in SPEC §11 and the comparison is asserted in
//      tests/conformance/vanilla_legacy_seed_control_test.cpp.
//
// AND WHAT THE CONTROL DOES NOT REACH INSIDE THIS FILE: `blocksFor` under
// Generator::Lcg — Java Random driving PerlinNoise::fromRandom, which half
// of the 270,000 candidates use — is exercised by neither `--control` nor the
// conformance case, both of which run Xoroshiro128++ only. That path is
// validated against the server in
// tests/conformance/vanilla_legacy_blended_test.cpp, where
// BlendedNoise::legacyFromWorldSeed is JavaRandom{worldSeed} handed straight
// into the same PerlinNoise::fromRandom.
//
// THIS FILE IS A BUILD TARGET (tools/analysis/CMakeLists.txt) and it is one
// deliberately. Compiled by hand from a header comment, it sat outside every
// gate — format.sh, warnings.sh and tidy.sh all skipped tools/ — so a claim
// that it agrees with the library bit for bit could go stale without a single
// test turning red. It now builds under the project warning set, and
// format.sh and tidy.sh name it, and `ctest --preset conformance`
// runs its --control on both probe worlds (conformance.legacy_seed_control_tool).
//
//   cmake --build --preset dev --target stratum_legacy_seed_analyze
//   A=build/dev/tools/analysis/stratum_legacy_seed_analyze
//   $A .fixtures/1.21.11/probes/legseed_s42 42 --scan
//   $A .fixtures/1.21.11/probes/legseed_s42 42 --candidate 182 0
//   $A .fixtures/1.21.11/probes/legseed_s42 42 --plant 180 7
//   $A .fixtures/1.21.11/probes/legseed_s42 42 --control
//   $A .fixtures/1.21.11/probes/legseed_s42 42 --profile
#include <stratum/chunk/chunk.hpp>
#include <stratum/hash/md5.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/noise/perlin.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/rng/java_random.hpp>
#include <stratum/rng/xoroshiro128.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace stratum;

namespace {

// Must match tools/analysis/density-probe.sh and legacy-seed-probe.sh.
constexpr double kK = 0.35;
constexpr int kMinY = -64;
constexpr int kHeight = 384;
constexpr int kBlockOffsets = 300;

// --- the probe's own dimensions --------------------------------------------

struct Noise {
    const char* id;
    int firstOctave;
    std::array<double, 3> amplitudes;
    std::size_t amplitudeCount;
};

constexpr Noise kNa{"stratum:na", -3, {1.0, 0.0, 0.0}, 1};
constexpr Noise kNb{"stratum:nb", -3, {1.0, 0.0, 0.0}, 1};
constexpr Noise kNmulti{"stratum:nmulti", -5, {1.0, 1.0, 1.0}, 3};
constexpr Noise kNskip{"stratum:nskip", -5, {1.0, 0.0, 1.0}, 3};

struct Dimension {
    const char* name;
    bool legacy;
    Noise noise;
    /// The output scale the probe wraps the noise in. It changes nothing
    /// about the seeding and everything about the width of the band a wrong
    /// candidate has to land in.
    double scale;
};

const std::array<Dimension, 9> kDimensions{{
    {"leg_single", true, kNa, 2.0},
    {"mod_single", false, kNa, 2.0},
    {"leg_twin", true, kNb, 2.0},
    {"leg_multi", true, kNmulti, 2.0},
    {"mod_multi", false, kNmulti, 2.0},
    {"leg_skip", true, kNskip, 2.0},
    {"mod_skip", false, kNskip, 2.0},
    {"leg_skip_q4", true, kNskip, 0.5},
    {"leg_skip_q16", true, kNskip, 0.125},
}};

// --- the readback -----------------------------------------------------------

struct Column {
    double x = 0.0;
    double z = 0.0;
    double value = 0.0;
};

[[nodiscard]] double quantum(double scale) {
    return 2.0 / static_cast<double>(kHeight) / (kK * scale);
}

/// What the readback could not speak for. `empty` is a column with no block
/// at all — a chunk the server left unbuilt around the forceloaded square.
/// `saturated` is a column whose terrain reached the floor or the ceiling,
/// where the function ran past the gradient's range and the height stopped
/// carrying its value. Saturated columns are the ones that matter: counting
/// them as data lets any candidate that also saturates agree for free, which
/// is one of the two ways an "outlier" score is manufactured.
struct Excluded {
    std::size_t empty = 0;
    std::size_t saturated = 0;
};

[[nodiscard]] std::vector<Column> readColumns(const std::filesystem::path& region, double scale,
                                              Excluded& excluded) {
    std::vector<Column> columns;
    const region::RegionFile file = region::RegionFile::open(region);
    for (std::int32_t chunkZ = 0; chunkZ < 32; ++chunkZ) {
        for (std::int32_t chunkX = 0; chunkX < 32; ++chunkX) {
            if (!file.hasChunk(chunkX, chunkZ)) {
                continue;
            }
            const chunk::Chunk decoded =
                chunk::Chunk::decode(nbt::read(file.readChunk(chunkX, chunkZ)).root);
            for (int localZ = 0; localZ < 16; ++localZ) {
                for (int localX = 0; localX < 16; ++localX) {
                    const std::int32_t x = (chunkX * 16) + localX;
                    const std::int32_t z = (chunkZ * 16) + localZ;
                    if ((x % 4) != 0 || (z % 4) != 0) {
                        continue;
                    }
                    int surface = kMinY - 1;
                    for (int y = kMinY + kHeight - 1; y >= kMinY; --y) {
                        const chunk::BlockState* block = decoded.blockAt(localX, y, localZ);
                        if (block != nullptr && block->name != "minecraft:air") {
                            surface = y;
                            break;
                        }
                    }
                    if (surface < kMinY) {
                        ++excluded.empty;
                        continue;
                    }
                    if (surface == kMinY || surface >= kMinY + kHeight - 1) {
                        ++excluded.saturated;
                        continue;
                    }
                    const double gradient =
                        1.0 - (2.0 * ((surface + 0.5) - kMinY) / static_cast<double>(kHeight));
                    columns.push_back(
                        {static_cast<double>(x), static_cast<double>(z), -gradient / (kK * scale)});
                }
            }
        }
    }
    return columns;
}

/// The server's terrain quantises a value to a block; a planted candidate has
/// to go through the same door or the plant is easier to find than the real
/// answer would be.
[[nodiscard]] double quantise(double value, double scale) {
    // The server places a block wherever K*scale*f + g(y) is positive, and g
    // is the gradient +1 at min_y to -1 at the top, so the crossing sits at
    //     t = min_y + height * (1 + K*scale*f) / 2
    // and the surface is the highest WHOLE y strictly below it: ceil(t) - 1.
    // Writing `floor(t - 0.5)` here instead — the inverse of the readback's
    // own `surface + 0.5`, which is what it looks like it should be — is
    // wrong for every t whose fraction is under a half, i.e. about half of
    // all columns, and it cost this tool an apparent sensitivity of 51%
    // before it was caught by the plant not scoring 100%.
    const double crossing = (((1.0 + (value * kK * scale)) * kHeight / 2.0) + kMinY);
    const auto surface = static_cast<int>(std::ceil(crossing)) - 1;
    const double back = 1.0 - (2.0 * ((surface + 0.5) - kMinY) / static_cast<double>(kHeight));
    return -back / (kK * scale);
}

// --- the candidate space ----------------------------------------------------

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
enum class Generator : std::uint8_t { Lcg, Xoroshiro };

constexpr std::size_t kBases = 5;
constexpr std::size_t kSalts = 10;
constexpr std::size_t kCombines = 3;
constexpr std::size_t kForks = 3;
constexpr std::size_t kGenerators = 2;
constexpr std::size_t kSeedRules = kBases * kSalts * kCombines * kForks * kGenerators;

struct SeedRule {
    Base base = Base::WorldSeed;
    Salt salt = Salt::None;
    Combine combine = Combine::Xor;
    int forks = 0;
    Generator generator = Generator::Lcg;
};

[[nodiscard]] SeedRule ruleAt(std::size_t index) {
    SeedRule rule;
    rule.generator = static_cast<Generator>(index % kGenerators);
    index /= kGenerators;
    rule.forks = static_cast<int>(index % kForks);
    index /= kForks;
    rule.combine = static_cast<Combine>(index % kCombines);
    index /= kCombines;
    rule.salt = static_cast<Salt>(index % kSalts);
    index /= kSalts;
    rule.base = static_cast<Base>(index % kBases);
    return rule;
}

[[nodiscard]] std::string describe(const SeedRule& rule) {
    static constexpr std::array<const char*, kBases> kBaseNames{"worldSeed", "lcgLong", "xoroLo",
                                                                "scrambled", "zero"};
    static constexpr std::array<const char*, kSalts> kSaltNames{
        "md5FirstBE",  "md5FirstLE",     "md5LastBE",  "md5LastLE",    "md5LoXorHi",
        "md5LoPlusHi", "md5PathFirstBE", "hashCodeId", "hashCodePath", "none"};
    static constexpr std::array<const char*, kCombines> kCombineNames{"xor", "add", "sub"};
    std::string text = std::string{kBaseNames[static_cast<std::size_t>(rule.base)]} + " " +
                       kCombineNames[static_cast<std::size_t>(rule.combine)] + " " +
                       kSaltNames[static_cast<std::size_t>(rule.salt)] + ", forks " +
                       std::to_string(rule.forks) + ", " +
                       (rule.generator == Generator::Lcg ? "lcg" : "xoroshiro");
    return text;
}

[[nodiscard]] std::uint64_t beU64(const hash::Md5Digest& digest, std::size_t at) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value = (value << 8U) | digest[at + i];
    }
    return value;
}

[[nodiscard]] std::uint64_t leU64(const hash::Md5Digest& digest, std::size_t at) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(digest[at + i]) << (8U * i);
    }
    return value;
}

/// Java's String.hashCode, which is one of the spellings a derivation could
/// plausibly use and is cheap enough to include rather than argue about.
[[nodiscard]] std::int32_t javaHashCode(std::string_view text) {
    std::uint32_t hash = 0;
    for (const char character : text) {
        hash = (hash * 31U) + static_cast<std::uint32_t>(static_cast<unsigned char>(character));
    }
    return static_cast<std::int32_t>(hash);
}

[[nodiscard]] std::uint64_t saltFor(Salt salt, std::string_view id) {
    const std::size_t colon = id.find(':');
    const std::string_view path = colon == std::string_view::npos ? id : id.substr(colon + 1);
    const hash::Md5Digest digest = hash::md5(id);
    const hash::Md5Digest pathDigest = hash::md5(path);
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
            return beU64(pathDigest, 0);
        case Salt::HashCodeId:
            return static_cast<std::uint64_t>(static_cast<std::int64_t>(javaHashCode(id)));
        case Salt::HashCodePath:
            return static_cast<std::uint64_t>(static_cast<std::int64_t>(javaHashCode(path)));
        case Salt::None:
            return 0;
    }
    return 0;
}

[[nodiscard]] std::uint64_t baseFor(Base base, std::int64_t worldSeed) {
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
            // The LCG's own initial scramble, applied without then drawing.
            return (static_cast<std::uint64_t>(worldSeed) ^ UINT64_C(0x5DEECE66D)) &
                   ((UINT64_C(1) << 48U) - 1);
        case Base::Zero:
            return 0;
    }
    return 0;
}

[[nodiscard]] std::int64_t seedFor(const SeedRule& rule, std::int64_t worldSeed,
                                   std::string_view id) {
    const std::uint64_t base = baseFor(rule.base, worldSeed);
    const std::uint64_t salt = saltFor(rule.salt, id);
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
        rng::JavaRandom random{static_cast<std::int64_t>(seed)};
        seed = static_cast<std::uint64_t>(random.nextLong());
    }
    return static_cast<std::int64_t>(seed);
}

/// `amplitude != 0.0`, spelled so the project's -Wfloat-equal does not fire.
/// Exactly equivalent for any non-NaN value, which is all a declared
/// amplitude can be, and the amplitudes here are literal 0.0 or 1.0.
[[nodiscard]] bool nonZero(double amplitude) {
    return std::abs(amplitude) > 0.0;
}

// --- the stack rule ---------------------------------------------------------
//
// Sequential, and only sequential: 2 * octaveCount Perlin blocks drawn in
// order — the first stack's octaves, then the second's — which is the shape
// `BlendedNoise::legacy` uses and the only stack rule this project has ever
// confirmed under the flag.

struct Octave {
    std::size_t block = 0;
    double amplitude = 0.0;
    double persistence = 0.0;
    double frequency = 0.0;
};

struct Layout {
    std::vector<Octave> first;
    std::vector<Octave> second;
    double valueFactor = 1.0;
    std::size_t blocksPerNoise = 0;
};

[[nodiscard]] Layout layoutFor(const Noise& noise) {
    Layout layout;
    const auto count = static_cast<int>(noise.amplitudeCount);
    std::size_t block = 0;
    for (std::vector<Octave>* stack : {&layout.first, &layout.second}) {
        double frequency = std::ldexp(1.0, noise.firstOctave);
        double persistence = std::ldexp(1.0, count - 1) / (std::ldexp(1.0, count) - 1.0);
        for (std::size_t i = 0; i < noise.amplitudeCount; ++i) {
            if (nonZero(noise.amplitudes[i])) {
                stack->push_back({block, noise.amplitudes[i], persistence, frequency});
                ++block;
            }
            frequency *= 2.0;
            persistence *= 0.5;
        }
    }
    layout.blocksPerNoise = block;

    std::size_t first = 0;
    std::size_t last = noise.amplitudeCount;
    while (last > first && !nonZero(noise.amplitudes[last - 1])) {
        --last;
    }
    while (first < last && !nonZero(noise.amplitudes[first])) {
        ++first;
    }
    const auto effective = static_cast<double>(last - first);
    layout.valueFactor = (1.0 / 6.0) / (0.1 * (1.0 + (1.0 / effective)));
    return layout;
}

[[nodiscard]] double sampleStack(const std::vector<Octave>& stack,
                                 const std::vector<noise::PerlinNoise>& blocks, std::size_t offset,
                                 double x, double y, double z) {
    double total = 0.0;
    for (const Octave& octave : stack) {
        const noise::PerlinNoise& perlin = blocks[offset + octave.block];
        total += (perlin.sample(x * octave.frequency, y * octave.frequency, z * octave.frequency) *
                  octave.amplitude) *
                 octave.persistence;
    }
    return total;
}

[[nodiscard]] double sampleNormal(const Layout& layout,
                                  const std::vector<noise::PerlinNoise>& blocks, std::size_t offset,
                                  double x, double y, double z) {
    constexpr double kSecondFrequency = 337.0 / 331.0;
    const double combined = sampleStack(layout.first, blocks, offset, x, y, z) +
                            sampleStack(layout.second, blocks, offset, x * kSecondFrequency,
                                        y * kSecondFrequency, z * kSecondFrequency);
    return combined * layout.valueFactor;
}

/// Every Perlin block a seed rule can reach, drawn once in order. Building
/// them incrementally is what keeps the block sweep linear instead of
/// quadratic.
[[nodiscard]] std::vector<noise::PerlinNoise> blocksFor(const SeedRule& rule, std::int64_t seed,
                                                        std::size_t needed) {
    std::vector<noise::PerlinNoise> blocks;
    blocks.reserve(needed);
    if (rule.generator == Generator::Lcg) {
        rng::JavaRandom random{seed};
        for (std::size_t i = 0; i < needed; ++i) {
            blocks.push_back(noise::PerlinNoise::fromRandom(random));
        }
    } else {
        rng::Xoroshiro128PlusPlus random{seed};
        for (std::size_t i = 0; i < needed; ++i) {
            blocks.push_back(noise::PerlinNoise::fromRandom(random));
        }
    }
    return blocks;
}

// --- the control arm --------------------------------------------------------
//
// The modern derivation, which this repository already knows is right, run
// through the readback the scan uses. Nothing here is a candidate: it is the
// answer, and the question is whether the apparatus can see it.

/// The Perlin blocks the MODERN rule draws, in the order `layoutFor` indexes
/// them: the first stack's non-zero octaves, then the second's. The seeding is
/// lib/src/noise_registry.cpp's — one positional fork of the world seed, the
/// noise's identifier hashed into it — and lib/src/perlin.cpp's per-octave
/// "octave_<n>" salt on top.
///
/// It is spelled out here rather than reached through OctaveNoise because the
/// point of the `model` arm is to drive THIS FILE'S sampleNormal, and
/// OctaveNoise does not hand out its octaves. If this seeding were wrong the
/// `library` arm below would disagree with the `model` arm, which is exactly
/// what the "identical" count reports.
[[nodiscard]] std::vector<noise::PerlinNoise> modernBlocksFor(const Noise& noise,
                                                              std::int64_t worldSeed) {
    const rng::XoroshiroPositionalFactory factory{worldSeed};
    rng::Xoroshiro128PlusPlus random = factory.fromHashOf(noise.id);
    std::vector<noise::PerlinNoise> blocks;
    for (int stack = 0; stack < 2; ++stack) {
        const auto baseLo = static_cast<std::uint64_t>(random.nextLong());
        const auto baseHi = static_cast<std::uint64_t>(random.nextLong());
        for (std::size_t i = 0; i < noise.amplitudeCount; ++i) {
            if (!nonZero(noise.amplitudes[i])) {
                continue;
            }
            const std::string name =
                "octave_" + std::to_string(noise.firstOctave + static_cast<int>(i));
            const rng::Seed128 salt = rng::seedFromHashOf(name);
            rng::Xoroshiro128PlusPlus octaveRandom{
                rng::Seed128{.lo = baseLo ^ salt.lo, .hi = baseHi ^ salt.hi}};
            blocks.push_back(noise::PerlinNoise::fromRandom(octaveRandom));
        }
    }
    return blocks;
}

[[nodiscard]] noise::NormalNoise libraryNoiseFor(const Noise& noise, std::int64_t worldSeed) {
    const rng::XoroshiroPositionalFactory factory{worldSeed};
    rng::Xoroshiro128PlusPlus random = factory.fromHashOf(noise.id);
    return noise::NormalNoise::create(
        random, noise.firstOctave,
        std::span<const double>{noise.amplitudes.data(), noise.amplitudeCount});
}

struct Control {
    std::size_t columns = 0;
    /// Within the band, by the library's NormalNoise and by this file's model.
    std::size_t library = 0;
    std::size_t model = 0;
    /// Columns where the two arms are the same double, bit for bit.
    std::size_t identical = 0;
    /// Columns where quantise(model) reproduces the server's reading exactly.
    std::size_t exact = 0;
    /// THE NEGATIVE CONTROL: the same modern derivation, same readback, same
    /// band, at worldSeed + 1. A rule that is recovered because the readback
    /// accepts anything would be recovered here too; a rule that is recovered
    /// because the SEEDING is right must fall to this dimension's null. The
    /// legacy-side mirror argues the same thing, but it varies the flag as
    /// well as the seed, so it cannot separate the two on its own.
    std::size_t negative = 0;
    /// The largest |ours - server| over every column, in noise units — kept
    /// per arm, because a single accumulator fed from one of them reports
    /// nothing about the other. The `model` arm is the one the plant and the
    /// scan actually share.
    double worstLibrary = 0.0;
    double worstModel = 0.0;
    /// The library's valueFactor against the one layoutFor computes.
    double libraryValueFactor = 0.0;
    double modelValueFactor = 0.0;
    bool valueFactorIdentical = false;
};

[[nodiscard]] Control controlFor(const Dimension& dimension, std::int64_t worldSeed,
                                 const std::vector<Column>& columns) {
    const Layout layout = layoutFor(dimension.noise);
    const double band = 0.5 * quantum(dimension.scale);
    const noise::NormalNoise reference = libraryNoiseFor(dimension.noise, worldSeed);
    const std::vector<noise::PerlinNoise> blocks = modernBlocksFor(dimension.noise, worldSeed);
    const noise::NormalNoise wrongSeed = libraryNoiseFor(dimension.noise, worldSeed + 1);

    Control control;
    control.columns = columns.size();
    control.libraryValueFactor = reference.valueFactor();
    control.modelValueFactor = layout.valueFactor;
    control.valueFactorIdentical = std::bit_cast<std::uint64_t>(control.libraryValueFactor) ==
                                   std::bit_cast<std::uint64_t>(control.modelValueFactor);

    for (const Column& column : columns) {
        const double byLibrary = reference.sample(column.x, 0.0, column.z);
        const double byModel = sampleNormal(layout, blocks, 0, column.x, 0.0, column.z);
        if (std::abs(byLibrary - column.value) <= band + 1e-12) {
            ++control.library;
        }
        if (std::abs(byModel - column.value) <= band + 1e-12) {
            ++control.model;
        }
        if (std::bit_cast<std::uint64_t>(byLibrary) == std::bit_cast<std::uint64_t>(byModel)) {
            ++control.identical;
        }
        // Exact, not near: the readback maps a whole half-open interval of
        // values onto one block, so a correct model put back through
        // `quantise` must land on the server's own double rather than near it.
        if (std::bit_cast<std::uint64_t>(quantise(byModel, dimension.scale)) ==
            std::bit_cast<std::uint64_t>(column.value)) {
            ++control.exact;
        }
        if (std::abs(wrongSeed.sample(column.x, 0.0, column.z) - column.value) <= band + 1e-12) {
            ++control.negative;
        }
        control.worstLibrary = std::max(control.worstLibrary, std::abs(byLibrary - column.value));
        control.worstModel = std::max(control.worstModel, std::abs(byModel - column.value));
    }
    return control;
}

// --- the readback's own shape, per dimension --------------------------------
//
// The control validates the readback, the coordinate convention, the band and
// the inversion on the MODERN dimensions only, so the step from "no survivor"
// to "absent from the space rather than invisible to the tool" rests on the
// legacy flag changing the SEEDING and nothing else in the terrain pipeline.
// That is an assumption, and `--profile` is the cheap measurement that bounds
// it: if the flag also changed the gradient, the output scale, the cell
// spacing or the sampling coordinates, the legacy readback would not have the
// same spread or the same neighbour-to-neighbour structure as its modern
// mirror. It cannot prove the pipelines identical — nothing short of the
// pipeline itself can — but it is refutable, and it was not being measured.
//
// lag 4 because the probe reads one column every 4 blocks: the shortest
// spacing the fixture offers, where a change in cell size or frequency shows
// up most strongly.

struct Profile {
    std::size_t columns = 0;
    double mean = 0.0;
    double sd = 0.0;
    /// Pearson correlation between a column and its neighbour 4 blocks away,
    /// pooled over both axes, about the dimension's own mean.
    double lag4 = 0.0;
    std::size_t lagPairs = 0;
};

/// Flat index into the lattice below. Each operand is widened before the
/// multiply rather than after it: `size_t(gz * side + gx)` computes the whole
/// address in `int` and only then widens, which is what
/// bugprone-misplaced-widening-cast objects to and what would overflow if the
/// lattice ever grew.
[[nodiscard]] std::size_t latticeIndex(int gx, int gz, int side) {
    return (static_cast<std::size_t>(gz) * static_cast<std::size_t>(side)) +
           static_cast<std::size_t>(gx);
}

[[nodiscard]] Profile profileFor(const std::vector<Column>& columns) {
    Profile profile;
    profile.columns = columns.size();
    if (columns.empty()) {
        return profile;
    }
    double sum = 0.0;
    for (const Column& column : columns) {
        sum += column.value;
    }
    profile.mean = sum / static_cast<double>(columns.size());
    double variance = 0.0;
    for (const Column& column : columns) {
        const double d = column.value - profile.mean;
        variance += d * d;
    }
    variance /= static_cast<double>(columns.size());
    profile.sd = std::sqrt(variance);

    // A dense lattice rather than a map: the probe reads every 4th block of a
    // 512-block region, so the grid is 128x128 and fits in a flat vector.
    constexpr int kStep = 4;
    constexpr int kSide = 512 / kStep;
    std::vector<double> grid(static_cast<std::size_t>(kSide) * kSide, 0.0);
    std::vector<char> present(static_cast<std::size_t>(kSide) * kSide, 0);
    for (const Column& column : columns) {
        const int gx = static_cast<int>(column.x) / kStep;
        const int gz = static_cast<int>(column.z) / kStep;
        if (gx < 0 || gx >= kSide || gz < 0 || gz >= kSide) {
            continue;
        }
        const std::size_t index = latticeIndex(gx, gz, kSide);
        grid[index] = column.value;
        present[index] = 1;
    }

    double covariance = 0.0;
    std::size_t pairs = 0;
    for (int gz = 0; gz < kSide; ++gz) {
        for (int gx = 0; gx < kSide; ++gx) {
            const std::size_t here = latticeIndex(gx, gz, kSide);
            if (present[here] == 0) {
                continue;
            }
            const double a = grid[here] - profile.mean;
            if (gx + 1 < kSide) {
                const std::size_t east = latticeIndex(gx + 1, gz, kSide);
                if (present[east] != 0) {
                    covariance += a * (grid[east] - profile.mean);
                    ++pairs;
                }
            }
            if (gz + 1 < kSide) {
                const std::size_t south = latticeIndex(gx, gz + 1, kSide);
                if (present[south] != 0) {
                    covariance += a * (grid[south] - profile.mean);
                    ++pairs;
                }
            }
        }
    }
    profile.lagPairs = pairs;
    if (pairs != 0 && variance > 0.0) {
        profile.lag4 = covariance / (static_cast<double>(pairs) * variance);
    }
    return profile;
}

struct Result {
    std::size_t seedRule = 0;
    std::size_t block = 0;
    std::size_t agreed = 0;
};

/// One pass over the whole space. `probe` is the subset of columns each
/// candidate is scored on; `full` is every column, used only on the few that
/// survive, so the reported count is always over the whole dimension.
[[nodiscard]] std::vector<Result> scan(const Dimension& dimension, std::int64_t worldSeed,
                                       const std::vector<Column>& columns, std::size_t probeColumns,
                                       std::size_t keep, std::size_t& candidatesScanned,
                                       std::vector<std::size_t>& histogram) {
    const Layout layout = layoutFor(dimension.noise);
    const double band = 0.5 * quantum(dimension.scale);
    const std::size_t needed = kBlockOffsets + layout.blocksPerNoise;
    const std::size_t sampled = std::min(probeColumns, columns.size());

    std::vector<Result> best;
    candidatesScanned = 0;
    histogram.assign(sampled + 1, 0);

    for (std::size_t ruleIndex = 0; ruleIndex < kSeedRules; ++ruleIndex) {
        const SeedRule rule = ruleAt(ruleIndex);
        const std::int64_t seed = seedFor(rule, worldSeed, dimension.noise.id);
        const std::vector<noise::PerlinNoise> blocks = blocksFor(rule, seed, needed);
        for (std::size_t offset = 0; offset < kBlockOffsets; ++offset) {
            ++candidatesScanned;
            std::size_t agreed = 0;
            for (std::size_t i = 0; i < sampled; ++i) {
                const Column& column = columns[i];
                const double ours = sampleNormal(layout, blocks, offset, column.x, 0.0, column.z);
                if (std::abs(ours - column.value) <= band + 1e-12) {
                    ++agreed;
                }
            }
            ++histogram[agreed];
            if (agreed * 2 >= sampled) {
                std::size_t full = 0;
                for (const Column& column : columns) {
                    const double ours =
                        sampleNormal(layout, blocks, offset, column.x, 0.0, column.z);
                    if (std::abs(ours - column.value) <= band + 1e-12) {
                        ++full;
                    }
                }
                best.push_back({ruleIndex, offset, full});
            }
        }
    }
    std::sort(best.begin(), best.end(),
              [](const Result& a, const Result& b) { return a.agreed > b.agreed; });
    if (best.size() > keep) {
        best.resize(keep);
    }
    return best;
}

/// The readings a chosen candidate would produce, put through the same
/// quantisation the server's terrain imposes.
[[nodiscard]] std::vector<Column> plant(const Dimension& dimension, std::int64_t worldSeed,
                                        const std::vector<Column>& columns, std::size_t ruleIndex,
                                        std::size_t offset) {
    const Layout layout = layoutFor(dimension.noise);
    const SeedRule rule = ruleAt(ruleIndex);
    const std::int64_t seed = seedFor(rule, worldSeed, dimension.noise.id);
    const std::vector<noise::PerlinNoise> blocks =
        blocksFor(rule, seed, offset + layout.blocksPerNoise);
    std::vector<Column> planted = columns;
    for (Column& column : planted) {
        column.value = quantise(sampleNormal(layout, blocks, offset, column.x, 0.0, column.z),
                                dimension.scale);
    }
    return planted;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
                     "usage: %s <probe dir> <world seed> --scan|--control|--profile|--candidate "
                     "<r> <b>|--plant <r> <b>\n",
                     argv[0]);
        return 2;
    }
    const std::filesystem::path root{argv[1]};
    const std::int64_t seed = std::strtoll(argv[2], nullptr, 10);
    const std::string mode{argv[3]};

    std::printf("candidate space: %zu seed rules x %d block offsets = %zu per dimension\n",
                kSeedRules, kBlockOffsets, kSeedRules * kBlockOffsets);

    // --control's verdict, accumulated across dimensions so it can be stated
    // once as a count rather than eyeballed off nine lines.
    std::size_t modernDimensions = 0;
    std::size_t modernColumns = 0;
    std::size_t modernRecovered = 0;
    std::size_t modernExact = 0;
    std::size_t legacyColumns = 0;
    std::size_t legacyAgreed = 0;
    std::size_t negativeColumns = 0;
    std::size_t negativeAgreed = 0;
    bool controlFailed = false;

    for (const Dimension& dimension : kDimensions) {
        const std::filesystem::path region = root / dimension.name / "r.0.0.mca";
        if (!std::filesystem::is_regular_file(region)) {
            std::fprintf(stderr, "missing %s\n", region.string().c_str());
            return 1;
        }
        Excluded excluded;
        const std::vector<Column> columns = readColumns(region, dimension.scale, excluded);

        if (mode == "--candidate") {
            // One named candidate, full-scored on every dimension. A
            // refutation is worth more as a number than as an absence from a
            // list of survivors, and the derivation deepslate uses is a
            // member of this space (rule 182, block 0) rather than something
            // outside it.
            if (argc < 6) {
                std::fprintf(stderr, "--candidate needs a rule index and a block offset\n");
                return 2;
            }
            const auto ruleIndex = static_cast<std::size_t>(std::strtoull(argv[4], nullptr, 10));
            const auto offset = static_cast<std::size_t>(std::strtoull(argv[5], nullptr, 10));
            const Layout layout = layoutFor(dimension.noise);
            const SeedRule rule = ruleAt(ruleIndex);
            const std::vector<noise::PerlinNoise> blocks = blocksFor(
                rule, seedFor(rule, seed, dimension.noise.id), offset + layout.blocksPerNoise);
            const double band = 0.5 * quantum(dimension.scale);
            std::size_t agreed = 0;
            for (const Column& column : columns) {
                const double ours = sampleNormal(layout, blocks, offset, column.x, 0.0, column.z);
                if (std::abs(ours - column.value) <= band + 1e-12) {
                    ++agreed;
                }
            }
            std::printf("%-13s %-6s band %.5f  rule %zu block %zu: %zu/%zu (%.2f%%)  %s\n",
                        dimension.name, dimension.legacy ? "legacy" : "modern", band, ruleIndex,
                        offset, agreed, columns.size(),
                        100.0 * static_cast<double>(agreed) / static_cast<double>(columns.size()),
                        describe(rule).c_str());
            continue;
        }

        if (mode == "--profile") {
            const Profile profile = profileFor(columns);
            std::printf("%-13s %-6s scale %5.3f  columns %4zu  mean %+.6f  sd %.6f  "
                        "lag4 %.6f (%zu pairs)\n",
                        dimension.name, dimension.legacy ? "legacy" : "modern", dimension.scale,
                        profile.columns, profile.mean, profile.sd, profile.lag4, profile.lagPairs);
            continue;
        }

        if (mode == "--control") {
            const Control control = controlFor(dimension, seed, columns);
            std::printf("%-13s %-6s band %.6f  columns %4zu  library %4zu/%4zu  model %4zu/%4zu  "
                        "identical %4zu  exact %4zu  negative %4zu  worst(library) %.9f  "
                        "worst(model) %.9f  valueFactor %.17g%s\n",
                        dimension.name, dimension.legacy ? "legacy" : "modern",
                        0.5 * quantum(dimension.scale), control.columns, control.library,
                        control.columns, control.model, control.columns, control.identical,
                        control.exact, control.negative, control.worstLibrary, control.worstModel,
                        control.libraryValueFactor,
                        control.valueFactorIdentical ? "" : " (DIFFERS FROM layoutFor)");
            if (!control.valueFactorIdentical) {
                std::printf("    layoutFor valueFactor %.17g\n", control.modelValueFactor);
                controlFailed = true;
            }
            if (dimension.legacy) {
                legacyColumns += control.columns;
                legacyAgreed += control.library;
            } else {
                ++modernDimensions;
                modernColumns += control.columns;
                modernRecovered += control.library;
                modernExact += control.exact;
                negativeColumns += control.columns;
                negativeAgreed += control.negative;
                if (control.library != control.columns || control.model != control.columns ||
                    control.identical != control.columns || control.exact != control.columns) {
                    controlFailed = true;
                }
                // The negative control has to FAIL to mean anything: a wrong
                // world seed that still recovered the dimension would say the
                // readback, not the seeding, is doing the work. One tenth is
                // far above every null this probe measures (the widest is
                // 11.4% at scale 0.125, and these three are all at scale 2)
                // and far below the 100% a real recovery reaches.
                if (control.negative * 10 > control.columns) {
                    controlFailed = true;
                }
            }
            continue;
        }

        if (mode == "--plant") {
            if (!dimension.legacy) {
                continue;
            }
            if (argc < 6) {
                std::fprintf(stderr, "--plant needs a rule index and a block offset\n");
                return 2;
            }
            const auto ruleIndex = static_cast<std::size_t>(std::strtoull(argv[4], nullptr, 10));
            const auto offset = static_cast<std::size_t>(std::strtoull(argv[5], nullptr, 10));
            const std::vector<Column> planted = plant(dimension, seed, columns, ruleIndex, offset);
            std::size_t scanned = 0;
            std::vector<std::size_t> histogram;
            const std::vector<Result> best =
                scan(dimension, seed, planted, 128, 8, scanned, histogram);
            std::printf("%-11s planted rule %zu (%s) block %zu into %zu columns; %zu scanned\n",
                        dimension.name, ruleIndex, describe(ruleAt(ruleIndex)).c_str(), offset,
                        planted.size(), scanned);
            std::size_t rank = 0;
            for (std::size_t i = 0; i < best.size(); ++i) {
                std::printf("    #%zu rule %zu block %zu: %zu/%zu  %s\n", i + 1, best[i].seedRule,
                            best[i].block, best[i].agreed, planted.size(),
                            describe(ruleAt(best[i].seedRule)).c_str());
                if (best[i].seedRule == ruleIndex && best[i].block == offset) {
                    rank = i + 1;
                }
            }
            std::printf("    planted candidate found at rank %zu\n", rank);
            continue;
        }

        std::size_t scanned = 0;
        std::vector<std::size_t> histogram;
        const std::vector<Result> best = scan(dimension, seed, columns, 128, 5, scanned, histogram);

        // The null, straight off the scanned population: the whole 270,000
        // candidates are wrong (none reaches the survival threshold), so their
        // agreement counts ARE this dimension's null distribution.
        std::size_t total = 0;
        std::size_t sum = 0;
        std::size_t worst = 0;
        for (std::size_t i = 0; i < histogram.size(); ++i) {
            total += histogram[i];
            sum += histogram[i] * i;
            if (histogram[i] != 0) {
                worst = i;
            }
        }
        const double mean =
            total == 0 ? 0.0 : static_cast<double>(sum) / static_cast<double>(total);
        std::printf("%-13s %-6s scale %5.3f  band %.5f  columns %4zu (sat %4zu, empty %4zu)  "
                    "null mean %6.2f/128 (%5.2f%%)  null max %3zu/128  survivors %zu\n",
                    dimension.name, dimension.legacy ? "legacy" : "modern", dimension.scale,
                    0.5 * quantum(dimension.scale), columns.size(), excluded.saturated,
                    excluded.empty, mean, 100.0 * mean / 128.0, worst, best.size());
        for (const Result& result : best) {
            std::printf("    best rule %zu block %zu: %zu/%zu  %s\n", result.seedRule, result.block,
                        result.agreed, columns.size(), describe(ruleAt(result.seedRule)).c_str());
        }
    }

    if (mode == "--control") {
        if (modernDimensions == 0) {
            std::fprintf(stderr, "no modern mirror dimensions in this probe\n");
            return 1;
        }
        std::printf("control: the modern rule recovers %zu/%zu columns over %zu mirror "
                    "dimensions (%zu exact through quantise), reaches %zu/%zu on the "
                    "legacy dimensions, and at worldSeed + 1 reaches %zu/%zu on the mirror "
                    "dimensions themselves\n",
                    modernRecovered, modernColumns, modernDimensions, modernExact, legacyAgreed,
                    legacyColumns, negativeAgreed, negativeColumns);
        if (controlFailed) {
            std::fprintf(stderr,
                         "control FAILED: a rule this repository already validates is not "
                         "recovered through this readback, so no refutation drawn from the scan "
                         "means anything\n");
            return 1;
        }
        std::printf("control passed: a correct named-noise rule is recoverable through this "
                    "apparatus\n");
    }
    return 0;
}
