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
//   g++ -std=c++20 -O2 -I lib/include -I build/dev/lib/generated
//       tools/analysis/legacy-seed-analyze.cpp -L build/dev/lib -lstratum_core -lz
//       -o build/legacy-seed-analyze
//   build/legacy-seed-analyze .fixtures/1.21.11/probes/legseed_s42 42 --scan
//   build/legacy-seed-analyze .fixtures/1.21.11/probes/legseed_s42 42 --candidate 182 0
//   build/legacy-seed-analyze .fixtures/1.21.11/probes/legseed_s42 42 --plant 180 7
#include <stratum/chunk/chunk.hpp>
#include <stratum/hash/md5.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/noise/perlin.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/rng/java_random.hpp>
#include <stratum/rng/xoroshiro128.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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
                    columns.push_back({static_cast<double>(x), static_cast<double>(z),
                                       -gradient / (kK * scale)});
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
    const double crossing =
        (((1.0 + (value * kK * scale)) * kHeight / 2.0) + kMinY);
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
    static constexpr std::array<const char*, kBases> bases{"worldSeed", "lcgLong", "xoroLo",
                                                           "scrambled", "zero"};
    static constexpr std::array<const char*, kSalts> salts{
        "md5FirstBE", "md5FirstLE",     "md5LastBE",  "md5LastLE",   "md5LoXorHi",
        "md5LoPlusHi", "md5PathFirstBE", "hashCodeId", "hashCodePath", "none"};
    static constexpr std::array<const char*, kCombines> combines{"xor", "add", "sub"};
    std::string text = std::string{bases[static_cast<std::size_t>(rule.base)]} + " " +
                       combines[static_cast<std::size_t>(rule.combine)] + " " +
                       salts[static_cast<std::size_t>(rule.salt)] + ", forks " +
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
            if (noise.amplitudes[i] != 0.0) {
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
    while (last > first && noise.amplitudes[last - 1] == 0.0) {
        --last;
    }
    while (first < last && noise.amplitudes[first] == 0.0) {
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
        total += (perlin.sample(x * octave.frequency, y * octave.frequency,
                                z * octave.frequency) *
                  octave.amplitude) *
                 octave.persistence;
    }
    return total;
}

[[nodiscard]] double sampleNormal(const Layout& layout,
                                  const std::vector<noise::PerlinNoise>& blocks,
                                  std::size_t offset, double x, double y, double z) {
    constexpr double kSecondFrequency = 337.0 / 331.0;
    const double combined =
        sampleStack(layout.first, blocks, offset, x, y, z) +
        sampleStack(layout.second, blocks, offset, x * kSecondFrequency, y * kSecondFrequency,
                    z * kSecondFrequency);
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

struct Result {
    std::size_t seedRule = 0;
    std::size_t block = 0;
    std::size_t agreed = 0;
};

/// One pass over the whole space. `probe` is the subset of columns each
/// candidate is scored on; `full` is every column, used only on the few that
/// survive, so the reported count is always over the whole dimension.
[[nodiscard]] std::vector<Result> scan(const Dimension& dimension, std::int64_t worldSeed,
                                       const std::vector<Column>& columns,
                                       std::size_t probeColumns, std::size_t keep,
                                       std::size_t& candidatesScanned, std::vector<std::size_t>& histogram) {
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
                const double ours =
                    sampleNormal(layout, blocks, offset, column.x, 0.0, column.z);
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
        column.value =
            quantise(sampleNormal(layout, blocks, offset, column.x, 0.0, column.z),
                     dimension.scale);
    }
    return planted;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
                     "usage: %s <probe dir> <world seed> --scan|--candidate <r> <b>|--plant <r> <b>\n",
                     argv[0]);
        return 2;
    }
    const std::filesystem::path root{argv[1]};
    const std::int64_t seed = std::strtoll(argv[2], nullptr, 10);
    const std::string mode{argv[3]};

    std::printf("candidate space: %zu seed rules x %d block offsets = %zu per dimension\n",
                kSeedRules, kBlockOffsets, kSeedRules * kBlockOffsets);

    for (const Dimension& dimension : kDimensions) {
        const std::filesystem::path region = root / dimension.name / "r.0.0.mca";
        if (!std::filesystem::is_regular_file(region)) {
            std::fprintf(stderr, "missing %s\n", region.c_str());
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
            const std::vector<noise::PerlinNoise> blocks =
                blocksFor(rule, seedFor(rule, seed, dimension.noise.id),
                          offset + layout.blocksPerNoise);
            const double band = 0.5 * quantum(dimension.scale);
            std::size_t agreed = 0;
            for (const Column& column : columns) {
                const double ours =
                    sampleNormal(layout, blocks, offset, column.x, 0.0, column.z);
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
            const std::vector<Column> planted =
                plant(dimension, seed, columns, ruleIndex, offset);
            std::size_t scanned = 0;
            std::vector<std::size_t> histogram;
            const std::vector<Result> best =
                scan(dimension, seed, planted, 128, 8, scanned, histogram);
            std::printf("%-11s planted rule %zu (%s) block %zu into %zu columns; %zu scanned\n",
                        dimension.name, ruleIndex, describe(ruleAt(ruleIndex)).c_str(), offset,
                        planted.size(), scanned);
            std::size_t rank = 0;
            for (std::size_t i = 0; i < best.size(); ++i) {
                std::printf("    #%zu rule %zu block %zu: %zu/%zu  %s\n", i + 1,
                            best[i].seedRule, best[i].block, best[i].agreed, planted.size(),
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
        const double mean = total == 0 ? 0.0 : static_cast<double>(sum) / static_cast<double>(total);
        std::printf("%-13s %-6s scale %5.3f  band %.5f  columns %4zu (sat %4zu, empty %4zu)  "
                    "null mean %6.2f/128 (%5.2f%%)  null max %3zu/128  survivors %zu\n",
                    dimension.name, dimension.legacy ? "legacy" : "modern", dimension.scale,
                    0.5 * quantum(dimension.scale), columns.size(), excluded.saturated,
                    excluded.empty, mean, 100.0 * mean / 128.0, worst, best.size());
        for (const Result& result : best) {
            std::printf("    best rule %zu block %zu: %zu/%zu  %s\n", result.seedRule,
                        result.block, result.agreed, columns.size(),
                        describe(ruleAt(result.seedRule)).c_str());
        }
    }
    return 0;
}
