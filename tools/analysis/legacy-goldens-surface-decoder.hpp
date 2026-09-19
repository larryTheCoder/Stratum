// Stratum — reading a surface noise's sign straight out of a golden region.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// A dimension's surface rule is a decision tree whose leaves place blocks and
// whose `noise_threshold` conditions are the only thing in it that a world
// seed reaches. So the block a golden region holds at one position is, read
// backwards, a statement about which side of a threshold a noise was on — for
// exactly those conditions the tree actually consulted before placing it, and
// for no others. This header is that inversion, and it is deliberately the
// SAME code for the Nether (where the noises are legacy-seeded and unknown)
// and the overworld (where they are modern-seeded and this build already
// reproduces them exactly), because the overworld run is the only thing that
// says the inversion is right rather than merely self-consistent.
//
// HOW IT DECIDES WHAT IS OBSERVABLE, which is the part a wrong decoder gets
// wrong quietly. It does not hand-read the tree. It walks the RESOLVED
// `surface::RuleGraph` for the position, evaluating every condition it can
// evaluate from the golden region itself, and BRANCHING on every condition it
// cannot:
//
//   noise_threshold          always symbolic — this is what is being read
//   vertical_gradient        symbolic only STRICTLY INSIDE its own band; at
//                            or below `true_at_and_below` it is certainly
//                            true and at or above `false_at_and_above`
//                            certainly false. That is not this file's own
//                            reading of the rule: they are the two early
//                            returns `surface::verticalGradientFires` takes
//                            before it draws at all, so no random source is
//                            consulted there and the answer does not depend
//                            on a seeding this build cannot derive.
//   above_preliminary_surface, temperature
//                            symbolic unless the caller supplies the value
//   stone_depth(add_surface_depth), hole, and any y_above/water carrying a
//                            surface_depth_multiplier
//                            resolved against an ENUMERATED surface depth
//                            when the caller cannot supply one — see kDepthLo
//   biome, steep, water, y_above, stone_depth(plain)
//                            concrete, from the region's own blocks and
//                            biome grid
//
// Every complete assignment of the symbolic conditions is walked; those whose
// leaf places the block the golden actually holds are CONSISTENT; and a
// condition is OBSERVED at that position only when every consistent
// assignment agrees on it. Anything else is masked, by construction rather
// than by judgement. Two conditions on the same noise are not independent, so
// an assignment is pruned the moment the intervals it asserts about one noise
// have no value in common — otherwise the decoder would explore states the
// world cannot be in and report fewer bits than the tree really decides.
//
// WHAT "the block the golden holds" MEANS WHERE THE RULES PLACE NOTHING. The
// leaf that places nothing is consistent exactly when the golden still holds
// what the chunk filler left: the dimension's `default_block` at a solid
// position, its `default_fluid` at a fluid one, air at air. That is why the
// terrain chain has to be right before this means anything, and in the
// Nether it is right to 99.99591% (vanilla_legacy_nether_terrain_test.cpp).
// Positions where NO assignment reproduces the golden are counted and
// excluded, never bent into the nearest bucket (SPEC §8).
//
// THE SURFACE DEPTH ENUMERATION, bounded rather than guessed. `minecraft:surface`
// is itself a NAMED noise, so under a legacy source its own seeding is exactly
// as unknown as the ones being read, and every branch that reads a depth would
// be unreadable if the depth had to be known. It does not: the depth is
//
//     (int)(2.75 * surface(x, 0, z) + 3.0 + 0.25 * u),   u in [0, 1)
//
// and a NormalNoise is bounded by its own parameters. `minecraft:surface` is
// firstOctave -6 with amplitudes [1, 1, 1]; the persistence schedule gives
// 4/7 + 2/7 + 1/7 = 1 per stack, two stacks, and valueFactor
// (1/6) / (0.1 * (1 + 1/3)) = 1.25, so with the textbook |perlin| <= 1 the
// noise lies in [-2.5, 2.5] and the depth in [-3, 10]. The enumeration runs
// [-4, 11], one past that on each side. (Measured, for scale: over the
// 8589934592 columns swept in vanilla_above_preliminary_surface_test.cpp the
// raw value never left [-1.14, 1.14], i.e. depth in [0, 6] — the analytic
// bound is four times wider than anything observed.) A bit is reported only
// when it is determined for EVERY depth in that range, so widening the range
// can only ever lose bits, never invent one.
#pragma once

#include <stratum/biome/temperature_table.hpp>
#include <stratum/chunk/chunk.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/hash/md5.hpp>
#include <stratum/javamath.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/noise/perlin.hpp>
#include <stratum/region/region_file.hpp>
#include <stratum/rng/java_random.hpp>
#include <stratum/rng/xoroshiro128.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/executor.hpp>
#include <stratum/surface/rule_graph.hpp>
#include <stratum/terrain/filler.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace legacy_goldens {

/// The eight seeds tools/fetch-vanilla generates golden regions for. They are
/// EIGHT REGION FILES OVER SIX INDEPENDENT WORLDS: java.util.Random scrambles
/// a seed as `(seed ^ 0x5DEECE66D) & ((1 << 48) - 1)`, so the TOP SIXTEEN bits
/// never reach it — not bit 63 alone, which is what this file used to say —
/// and 0/Long.MIN_VALUE and -1/Long.MAX_VALUE are each one world under any
/// legacy seeding. Measured on the goldens themselves, not assumed: over the
/// 67108864 block positions of the 0 and Long.MIN_VALUE Nether regions the two
/// agree on the air/fluid/solid CATEGORY of every single one, agree on every
/// biome, and differ in 11914 block NAMES (0.0178%, confined to 142 of the
/// 1024 chunks; the -1 / Long.MAX_VALUE pair differs in 160 of 1024) — a
/// feature-sized residue, since features are seeded from the whole 64-bit seed
/// and run after the surface rules. Every denominator drawn from these eight
/// files therefore pools two copies of a world: see `distinctWorlds`.
inline constexpr std::array<std::int64_t, 8> kGoldenSeeds{
    0,
    1,
    -1,
    42,
    2891948927356891LL,
    -4172144997902289642LL,
    9223372036854775807LL,
    -9223372036854775807LL - 1,
};

/// The part of a world seed a legacy (java.util.Random) seeding can see. Two
/// seeds sharing it are ONE world to every legacy-seeded noise, however far
/// apart they look.
[[nodiscard]] inline std::uint64_t legacyWorldKey(std::int64_t seed) {
    return static_cast<std::uint64_t>(seed) & ((UINT64_C(1) << 48U) - 1);
}

/// How many independent worlds a list of seeds actually names, and how many
/// of its entries are a second copy of one already in the list.
struct WorldCount {
    std::size_t regions = 0;
    std::size_t worlds = 0;

    [[nodiscard]] std::size_t duplicates() const noexcept { return regions - worlds; }
};

[[nodiscard]] inline WorldCount distinctWorlds(const std::vector<std::int64_t>& seeds) {
    std::vector<std::uint64_t> keys;
    keys.reserve(seeds.size());
    for (const std::int64_t seed : seeds) {
        keys.push_back(legacyWorldKey(seed));
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return WorldCount{.regions = seeds.size(), .worlds = keys.size()};
}

/// Columns in one region file: 32x32 chunks of 16x16.
inline constexpr std::size_t kColumnsPerRegion = std::size_t{512} * 512;

/// The enumerated surface-depth range, derived in this file's header from
/// `minecraft:surface`'s own declared parameters and |perlin| <= 1.
inline constexpr std::int32_t kDepthLo = -4;
inline constexpr std::int32_t kDepthHi = 11;

// --------------------------------------------------------------- fixtures

/// The extracted `worldgen` tree under @p root, or an empty path when the
/// fixtures are absent — they are Mojang-derived and never committed
/// (SPEC §12), so every caller has to be able to skip.
[[nodiscard]] inline std::filesystem::path findWorldgenTree(const std::filesystem::path& root) {
    if (!std::filesystem::is_directory(root)) {
        return {};
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_directory() && entry.path().filename() == "worldgen" &&
            std::filesystem::is_directory(entry.path() / "density_function")) {
            return entry.path();
        }
    }
    return {};
}

/// `.../regions/seed-<seed>/<dimension>/r.0.0.mca`, or an empty path.
[[nodiscard]] inline std::filesystem::path regionOf(const std::filesystem::path& root,
                                                    std::int64_t seed, std::string_view dimension) {
    if (!std::filesystem::is_directory(root)) {
        return {};
    }
    const std::string wanted = "seed-" + std::to_string(seed);
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file() || entry.path().filename() != "r.0.0.mca") {
            continue;
        }
        const std::filesystem::path parent = entry.path().parent_path();
        if (parent.filename() == dimension && parent.parent_path().filename() == wanted) {
            return entry.path();
        }
    }
    return {};
}

// ------------------------------------------------------- one golden chunk

/// Solid, fluid or air — the three the chunk filler's first pass can leave,
/// rederived from the block a position holds. The same three-way split
/// terrain::ChunkFiller uses to build a surface::Context, and it has to be,
/// or the stone-depth runs this decoder reconstructs would not be the ones
/// the server counted.
enum class Category : std::uint8_t { Air, Fluid, Solid };

/// One golden chunk, flattened for random access: a palette lookup per block
/// costs a search, and this is read 32768 times a chunk.
class GoldenChunk {
public:
    /// Takes the decoded chunk BY VALUE and keeps it: every pointer below
    /// points into its palettes, so a GoldenChunk built from a temporary
    /// would dangle the moment it was constructed.
    GoldenChunk(stratum::chunk::Chunk chunk, const stratum::settings::NoiseSettings& settings)
        : chunk_(std::move(chunk)), minY_(settings.geometry.minY),
          height_(settings.geometry.height), chunkX_(chunk_.x()), chunkZ_(chunk_.z()) {
        const std::size_t span = static_cast<std::size_t>(height_) * 16U * 16U;
        blocks_.assign(span, nullptr);
        biomes_.assign(span / 64U, nullptr);
        for (const stratum::chunk::Section& section : chunk_.sections()) {
            const std::int32_t sectionBase = section.y * 16;
            if (sectionBase + 15 < minY_ || sectionBase >= minY_ + height_) {
                continue;
            }
            for (int y = 0; y < 16; ++y) {
                const std::int32_t worldY = sectionBase + y;
                if (worldY < minY_ || worldY >= minY_ + height_) {
                    continue;
                }
                for (int z = 0; z < 16; ++z) {
                    for (int x = 0; x < 16; ++x) {
                        blocks_[indexOf(x, worldY, z)] = &section.blockAt(x, y, z);
                    }
                }
            }
            if (section.biomePalette.empty() || section.biomes.empty()) {
                continue;
            }
            for (int y = 0; y < 4; ++y) {
                const std::int32_t worldY = sectionBase + (y * 4);
                if (worldY < minY_ || worldY >= minY_ + height_) {
                    continue;
                }
                for (int z = 0; z < 4; ++z) {
                    for (int x = 0; x < 4; ++x) {
                        const std::size_t cell = (static_cast<std::size_t>(y) * 16U) +
                                                 (static_cast<std::size_t>(z) * 4U) +
                                                 static_cast<std::size_t>(x);
                        const std::uint16_t entry = section.biomes[cell];
                        if (entry >= section.biomePalette.size()) {
                            continue;
                        }
                        biomes_[biomeIndexOf(x * 4, worldY, z * 4)] = &section.biomePalette[entry];
                    }
                }
            }
        }
        buildColumns(settings);
    }

    [[nodiscard]] std::int32_t chunkX() const noexcept { return chunkX_; }

    [[nodiscard]] std::int32_t chunkZ() const noexcept { return chunkZ_; }

    [[nodiscard]] std::int32_t minY() const noexcept { return minY_; }

    [[nodiscard]] std::int32_t maxY() const noexcept { return minY_ + height_; }

    [[nodiscard]] const stratum::chunk::BlockState* blockAt(int localX, std::int32_t y,
                                                            int localZ) const {
        if (y < minY_ || y >= minY_ + height_) {
            return nullptr;
        }
        return blocks_[indexOf(localX, y, localZ)];
    }

    [[nodiscard]] const std::string* biomeAt(int localX, std::int32_t y, int localZ) const {
        if (y < minY_ || y >= minY_ + height_) {
            return nullptr;
        }
        return biomes_[biomeIndexOf(localX, y, localZ)];
    }

    [[nodiscard]] Category categoryAt(int localX, std::int32_t y, int localZ) const {
        return category_[indexOf(localX, y, localZ)];
    }

    [[nodiscard]] std::int32_t depthAbove(int localX, std::int32_t y, int localZ) const {
        return depthAbove_[indexOf(localX, y, localZ)];
    }

    [[nodiscard]] std::int32_t depthBelow(int localX, std::int32_t y, int localZ) const {
        return depthBelow_[indexOf(localX, y, localZ)];
    }

    /// The topmost non-air y of a column, or minY - 1 for a column of air —
    /// where terrain::ChunkFiller's own surface-rule scan starts, and what
    /// `steep` compares between neighbours.
    [[nodiscard]] std::int32_t scanFrom(int localX, int localZ) const {
        return scanFrom_[(static_cast<std::size_t>(localZ) * 16U) +
                         static_cast<std::size_t>(localX)];
    }

    /// The LOWEST water height consistent with this column: one above the
    /// golden's own topmost fluid block, which is what the chunk filler left
    /// wherever no rule froze it.
    [[nodiscard]] std::optional<std::int32_t> waterHeightLow(int localX, int localZ) const {
        return water_[(static_cast<std::size_t>(localZ) * 16U) + static_cast<std::size_t>(localX)];
    }

    /// The HIGHEST one: a rule that freezes a water surface to ice turns a
    /// fluid position solid, and the golden then shows the fluid one block
    /// lower than the server measured it. Every solid, non-default block
    /// sitting directly above the topmost fluid could be such a position, so
    /// the true height lies in [low, high] and a `water` condition is only
    /// read where both ends answer it the same way.
    [[nodiscard]] std::optional<std::int32_t> waterHeightHigh(int localX, int localZ) const {
        return waterHigh_[(static_cast<std::size_t>(localZ) * 16U) +
                          static_cast<std::size_t>(localX)];
    }

private:
    [[nodiscard]] std::size_t indexOf(int localX, std::int32_t y, int localZ) const {
        return ((static_cast<std::size_t>(y - minY_) * 16U) + static_cast<std::size_t>(localZ)) *
                   16U +
               static_cast<std::size_t>(localX);
    }

    [[nodiscard]] std::size_t biomeIndexOf(int localX, std::int32_t y, int localZ) const {
        return ((static_cast<std::size_t>((y - minY_) / 4) * 4U) +
                static_cast<std::size_t>(localZ / 4)) *
                   4U +
               static_cast<std::size_t>(localX / 4);
    }

    void buildColumns(const stratum::settings::NoiseSettings& settings) {
        const std::size_t span = static_cast<std::size_t>(height_) * 16U * 16U;
        category_.assign(span, Category::Air);
        depthAbove_.assign(span, 0);
        depthBelow_.assign(span, 0);
        scanFrom_.assign(256U, minY_ - 1);
        water_.assign(256U, std::nullopt);
        waterHigh_.assign(256U, std::nullopt);
        for (int localZ = 0; localZ < 16; ++localZ) {
            for (int localX = 0; localX < 16; ++localX) {
                std::int32_t run = 0;
                std::optional<std::int32_t> water;
                std::int32_t top = minY_ - 1;
                for (std::int32_t y = minY_ + height_ - 1; y >= minY_; --y) {
                    const Category category = categorise(blockAt(localX, y, localZ), settings);
                    category_[indexOf(localX, y, localZ)] = category;
                    if (category == Category::Air) {
                        run = 0;
                    } else {
                        if (top < minY_) {
                            top = y;
                        }
                        if (category == Category::Solid) {
                            ++run;
                        } else if (!water.has_value()) {
                            water = y + 1;
                        }
                    }
                    depthAbove_[indexOf(localX, y, localZ)] = run;
                }
                run = 0;
                for (std::int32_t y = minY_; y < minY_ + height_; ++y) {
                    const Category category = category_[indexOf(localX, y, localZ)];
                    if (category == Category::Air) {
                        run = 0;
                    } else if (category == Category::Solid) {
                        ++run;
                    }
                    depthBelow_[indexOf(localX, y, localZ)] = run;
                }
                const std::size_t column =
                    (static_cast<std::size_t>(localZ) * 16U) + static_cast<std::size_t>(localX);
                scanFrom_[column] = top;
                water_[column] = water;
                waterHigh_[column] = water;
                if (water.has_value()) {
                    std::int32_t high = *water;
                    while (high < minY_ + height_) {
                        const stratum::chunk::BlockState* block = blockAt(localX, high, localZ);
                        if (block == nullptr ||
                            category_[indexOf(localX, high, localZ)] != Category::Solid ||
                            block->name == settings.defaultBlock.name.toString()) {
                            break;
                        }
                        ++high;
                    }
                    waterHigh_[column] = high;
                }
            }
        }
    }

    [[nodiscard]] static Category categorise(const stratum::chunk::BlockState* block,
                                             const stratum::settings::NoiseSettings& settings) {
        if (block == nullptr || block->name == "minecraft:air") {
            return Category::Air;
        }
        if (block->name == settings.defaultFluid.name.toString()) {
            return Category::Fluid;
        }
        return Category::Solid;
    }

    stratum::chunk::Chunk chunk_;
    std::int32_t minY_ = 0;
    std::int32_t height_ = 0;
    std::int32_t chunkX_ = 0;
    std::int32_t chunkZ_ = 0;
    std::vector<const stratum::chunk::BlockState*> blocks_;
    std::vector<const std::string*> biomes_;
    std::vector<Category> category_;
    std::vector<std::int32_t> depthAbove_;
    std::vector<std::int32_t> depthBelow_;
    std::vector<std::int32_t> scanFrom_;
    std::vector<std::optional<std::int32_t>> water_;
    std::vector<std::optional<std::int32_t>> waterHigh_;
};

/// Reads every chunk of one region, or throws naming the file.
[[nodiscard]] inline std::vector<GoldenChunk>
readRegion(const std::filesystem::path& path, const stratum::settings::NoiseSettings& settings,
           std::int32_t stride) {
    const stratum::region::RegionFile file = stratum::region::RegionFile::open(path);
    std::vector<GoldenChunk> chunks;
    for (std::int32_t chunkZ = 0; chunkZ < 32; chunkZ += stride) {
        for (std::int32_t chunkX = 0; chunkX < 32; chunkX += stride) {
            if (!file.hasChunk(chunkX, chunkZ)) {
                continue;
            }
            chunks.emplace_back(stratum::chunk::Chunk::decode(
                                    stratum::nbt::read(file.readChunk(chunkX, chunkZ)).root),
                                settings);
        }
    }
    return chunks;
}

// ------------------------------------------------------------- the decoder

/// What the caller can supply for a position; anything absent is branched on
/// or enumerated instead. `surfaceDepth` absent means "enumerate [kDepthLo,
/// kDepthHi]", which is the legacy case; supplying it is the modern one.
struct Known {
    std::optional<std::int32_t> surfaceDepth;
    std::optional<double> surfaceSecondary;
    std::optional<std::int32_t> preliminarySurface;
    /// Whether `steep` fires at this column. Absent means branch on it: the
    /// four heights it compares are the heights the CHUNK FILLER left, and a
    /// golden region only shows the heights AFTER the rules ran. Those are
    /// the same height wherever a rule replaced a block in place, which is
    /// every rule vanilla's overworld and Nether trees contain — but "which
    /// is every rule" is a claim about the tree, not about the decoder, so
    /// the caller states it rather than the decoder assuming it.
    std::optional<bool> steep;
    /// The column's water height, as an interval — see
    /// GoldenChunk::waterHeightHigh. Absent means the column holds no fluid
    /// at all, which makes `water` unconditionally true. When the two ends
    /// disagree about a `water` condition it is branched on instead of
    /// guessed.
    std::optional<std::int32_t> waterLow;
    std::optional<std::int32_t> waterHigh;
};

/// -1 not observed, 0 observed false, 1 observed true — one entry per
/// condition index of the graph.
using Observed = std::vector<std::int8_t>;

/// Raised when the decoder is asked for something it will not approximate.
class DecodeError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Decoder {
public:
    Decoder(const stratum::surface::RuleGraph& graph,
            const stratum::settings::NoiseSettings& settings)
        : graph_(&graph), settings_(&settings) {
        noiseOf_.assign(graph.conditionCount(), -1);
        for (stratum::surface::ConditionIndex i = 0; i < graph.conditionCount(); ++i) {
            const stratum::surface::Condition& condition = graph.condition(i);
            if (condition.type != stratum::surface::ConditionType::NoiseThreshold) {
                continue;
            }
            if (!condition.noise.has_value()) {
                throw DecodeError("a noise_threshold without a noise reached the decoder");
            }
            const std::string id = condition.noise->toString();
            auto found = std::ranges::find(noiseNames_, id);
            if (found == noiseNames_.end()) {
                noiseNames_.push_back(id);
                constraints_.emplace_back();
                found = noiseNames_.end() - 1;
            }
            const auto slot = static_cast<std::size_t>(found - noiseNames_.begin());
            noiseOf_[i] = static_cast<int>(slot);
            constraints_[slot].conditions.push_back(i);
        }
        for (Constraints& constraints : constraints_) {
            std::vector<double> edges;
            for (const stratum::surface::ConditionIndex i : constraints.conditions) {
                const stratum::surface::Condition& condition = graph.condition(i);
                edges.push_back(condition.minThreshold);
                edges.push_back(condition.maxThreshold);
            }
            std::ranges::sort(edges);
            edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
            // Every endpoint, plus one representative strictly between each
            // adjacent pair and one outside each end. A region built from
            // closed intervals over these endpoints is non-empty exactly when
            // one of these points is in it, so this is an exact feasibility
            // test rather than a sampled one.
            constraints.probes.push_back(edges.front() - 1.0);
            for (std::size_t i = 0; i < edges.size(); ++i) {
                constraints.probes.push_back(edges[i]);
                if (i + 1 < edges.size()) {
                    constraints.probes.push_back((edges[i] * 0.5) + (edges[i + 1] * 0.5));
                }
            }
            constraints.probes.push_back(edges.back() + 1.0);
        }
    }

    [[nodiscard]] const std::vector<std::string>& noiseNames() const noexcept {
        return noiseNames_;
    }

    /// Which noise a condition reads, or -1 for a condition that reads none.
    [[nodiscard]] int noiseOf(stratum::surface::ConditionIndex index) const {
        return noiseOf_[index];
    }

    /// How many complete assignments the last decode() walked, and how many
    /// of them reproduced the golden block. `consistent == 0` means the tree
    /// cannot explain the golden at all — a terrain-chain error, or a block
    /// the rules never place — and NOTHING is observed there.
    struct Counts {
        std::size_t explored = 0;
        std::size_t consistent = 0;
        bool overflowed = false;
    };

    /// Decodes one position. @p into is resized and filled with -1/0/1 per
    /// condition index. @p touched, when given, is filled with 1 for every
    /// condition SOME consistent assignment had to decide — so `touched` and
    /// not `observed` is exactly "the tree consulted this noise here and the
    /// block still does not say which way", which is what masking looks like.
    Counts decode(const stratum::surface::Context& at, const Known& known,
                  const stratum::chunk::BlockState& golden, Category category, Observed& into,
                  std::vector<std::uint8_t>* touched = nullptr) const {
        into.assign(graph_->conditionCount(), -1);
        if (touched != nullptr) {
            touched->assign(graph_->conditionCount(), 0);
        }
        State state;
        state.at = &at;
        state.known = &known;
        state.golden = &golden;
        state.category = category;
        state.assign.assign(graph_->conditionCount(), -1);
        state.first = true;
        state.into = &into;
        state.touched = touched;
        if (known.surfaceDepth.has_value()) {
            explore(state, *known.surfaceDepth);
        } else {
            for (std::int32_t depth = kDepthLo; depth <= kDepthHi; ++depth) {
                explore(state, depth);
            }
        }
        if (state.counts.consistent == 0 || state.counts.overflowed) {
            into.assign(graph_->conditionCount(), -1);
            if (touched != nullptr) {
                touched->assign(graph_->conditionCount(), 0);
            }
        }
        return state.counts;
    }

private:
    /// A cap on the assignment walk. Reached means "this position is not
    /// decoded", never "these are the assignments": a truncated walk could
    /// report a bit the untruncated one would not.
    static constexpr std::size_t kLeafCap = 4096;

    struct Constraints {
        std::vector<stratum::surface::ConditionIndex> conditions;
        std::vector<double> probes;
    };

    /// `Wildcard` is `bandlands`: a per-column terracotta table this decoder
    /// does not build. Treating that leaf as consistent with ANY golden block
    /// is the conservative direction — it adds consistent assignments, so it
    /// can only ever turn an observed bit into an unobserved one.
    enum class Status : std::uint8_t { Placed, None, Hit, Wildcard };

    struct State {
        const stratum::surface::Context* at = nullptr;
        const Known* known = nullptr;
        const stratum::chunk::BlockState* golden = nullptr;
        Category category = Category::Solid;
        std::int32_t depth = 0;
        std::vector<std::int8_t> assign;
        stratum::surface::ConditionIndex hit = 0;
        bool first = true;
        Observed* into = nullptr;
        std::vector<std::uint8_t>* touched = nullptr;
        Counts counts;
    };

    void explore(State& state, std::int32_t depth) const {
        state.depth = depth;
        walk(state);
    }

    void walk(State& state) const {
        if (state.counts.overflowed) {
            return;
        }
        const stratum::settings::BlockState* placed = nullptr;
        const Status status = run(graph_->root(), state, placed);
        if (status == Status::Hit) {
            const stratum::surface::ConditionIndex hit = state.hit;
            for (const std::int8_t value : {std::int8_t{1}, std::int8_t{0}}) {
                if (!feasible(state, hit, value)) {
                    continue;
                }
                state.assign[hit] = value;
                walk(state);
                state.assign[hit] = -1;
            }
            return;
        }
        ++state.counts.explored;
        if (state.counts.explored > kLeafCap) {
            state.counts.overflowed = true;
            return;
        }
        if (status != Status::Wildcard &&
            !matches(status == Status::Placed ? placed : nullptr, state)) {
            return;
        }
        ++state.counts.consistent;
        if (state.touched != nullptr) {
            for (std::size_t i = 0; i < state.assign.size(); ++i) {
                if (state.assign[i] >= 0) {
                    (*state.touched)[i] = 1;
                }
            }
        }
        Observed& into = *state.into;
        if (state.first) {
            into = state.assign;
            state.first = false;
            return;
        }
        for (std::size_t i = 0; i < into.size(); ++i) {
            if (into[i] != state.assign[i]) {
                into[i] = -1;
            }
        }
    }

    /// Does a leaf placing @p placed (or nothing) reproduce the golden block?
    [[nodiscard]] bool matches(const stratum::settings::BlockState* placed,
                               const State& state) const {
        if (placed == nullptr) {
            // The filler's own block stands.
            switch (state.category) {
                case Category::Air:
                    return state.golden->name == "minecraft:air";
                case Category::Fluid:
                    return sameState(settings_->defaultFluid, *state.golden);
                case Category::Solid:
                    return sameState(settings_->defaultBlock, *state.golden);
            }
            return false;
        }
        return sameState(*placed, *state.golden);
    }

    [[nodiscard]] static bool sameState(const stratum::settings::BlockState& rule,
                                        const stratum::chunk::BlockState& golden) {
        if (rule.name.toString() != golden.name) {
            return false;
        }
        if (rule.properties.size() != golden.properties.size()) {
            return false;
        }
        for (const auto& [key, value] : golden.properties) {
            const auto found = rule.properties.find(key);
            if (found == rule.properties.end() || found->second != value) {
                return false;
            }
        }
        return true;
    }

    /// Is there a value of this condition's noise satisfying every constraint
    /// the assignment (with @p value added for @p index) asserts about it?
    [[nodiscard]] bool feasible(const State& state, stratum::surface::ConditionIndex index,
                                std::int8_t value) const {
        const int slot = noiseOf_[index];
        if (slot < 0) {
            return true;
        }
        const Constraints& constraints = constraints_[static_cast<std::size_t>(slot)];
        for (const double probe : constraints.probes) {
            bool ok = true;
            for (const stratum::surface::ConditionIndex other : constraints.conditions) {
                const std::int8_t want = other == index ? value : state.assign[other];
                if (want < 0) {
                    continue;
                }
                const stratum::surface::Condition& condition = graph_->condition(other);
                const bool holds =
                    condition.minThreshold <= probe && probe <= condition.maxThreshold;
                if (holds != (want == 1)) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] Status run(stratum::surface::RuleIndex index, State& state,
                             const stratum::settings::BlockState*& out) const {
        const stratum::surface::Rule& rule = graph_->rule(index);
        switch (rule.type) {
            case stratum::surface::RuleType::Sequence:
                for (const stratum::surface::RuleIndex child : rule.sequence) {
                    const Status status = run(child, state, out);
                    if (status != Status::None) {
                        return status;
                    }
                }
                return Status::None;
            case stratum::surface::RuleType::Condition: {
                const int held = test(rule.condition, state);
                if (held == kHit) {
                    return Status::Hit;
                }
                if (held == 0) {
                    return Status::None;
                }
                return run(rule.thenRun, state, out);
            }
            case stratum::surface::RuleType::Block:
                out = &rule.block;
                return Status::Placed;
            case stratum::surface::RuleType::Bandlands:
                return Status::Wildcard;
        }
        throw DecodeError("an unknown rule type reached the decoder");
    }

    static constexpr int kHit = 2;

    /// 0, 1, or kHit — kHit meaning "this condition is symbolic and not yet
    /// assigned", with state.hit naming it.
    [[nodiscard]] int test(stratum::surface::ConditionIndex index, State& state) const {
        const stratum::surface::Condition& condition = graph_->condition(index);
        const stratum::surface::Context& at = *state.at;
        switch (condition.type) {
            case stratum::surface::ConditionType::Not: {
                const int inner = test(condition.invert, state);
                return inner == kHit ? kHit : 1 - inner;
            }
            case stratum::surface::ConditionType::NoiseThreshold:
            case stratum::surface::ConditionType::Temperature:
                return symbolic(index, state);
            case stratum::surface::ConditionType::VerticalGradient: {
                const std::int32_t trueAt = condition.trueAtAndBelow.resolve(settings_->geometry);
                const std::int32_t falseAt = condition.falseAtAndAbove.resolve(settings_->geometry);
                if (at.y <= trueAt) {
                    return 1;
                }
                if (at.y >= falseAt) {
                    return 0;
                }
                return symbolic(index, state);
            }
            case stratum::surface::ConditionType::AbovePreliminarySurface: {
                if (!state.known->preliminarySurface.has_value()) {
                    return symbolic(index, state);
                }
                return at.y >= *state.known->preliminarySurface + state.depth - 8 ? 1 : 0;
            }
            case stratum::surface::ConditionType::Biome:
                if (!at.biome.has_value()) {
                    throw DecodeError("a biome condition reached the decoder without a biome");
                }
                return std::ranges::find(condition.biomes, *at.biome) != condition.biomes.end() ? 1
                                                                                                : 0;
            case stratum::surface::ConditionType::Steep:
                if (!state.known->steep.has_value()) {
                    return symbolic(index, state);
                }
                return *state.known->steep ? 1 : 0;
            case stratum::surface::ConditionType::Hole:
                return state.depth <= 0 ? 1 : 0;
            case stratum::surface::ConditionType::YAbove: {
                const std::int32_t left = at.y + (condition.addStoneDepth ? at.stoneDepthAbove : 0);
                const std::int32_t right = condition.anchor.resolve(settings_->geometry) +
                                           (condition.surfaceDepthMultiplier * state.depth);
                return left >= right ? 1 : 0;
            }
            case stratum::surface::ConditionType::Water: {
                if (!state.known->waterLow.has_value()) {
                    return 1;
                }
                const std::int32_t left = at.y + (condition.addStoneDepth ? at.stoneDepthAbove : 0);
                const std::int32_t offset =
                    condition.offset + (condition.surfaceDepthMultiplier * state.depth);
                const bool low = left >= *state.known->waterLow + offset;
                const bool high =
                    left >= state.known->waterHigh.value_or(*state.known->waterLow) + offset;
                if (low != high) {
                    return symbolic(index, state);
                }
                return low ? 1 : 0;
            }
            case stratum::surface::ConditionType::StoneDepth: {
                const bool ceiling = condition.surfaceType == "ceiling";
                const std::int32_t run = ceiling ? at.stoneDepthBelow : at.stoneDepthAbove;
                if (run <= 0) {
                    return 0;
                }
                std::int32_t threshold = condition.offset;
                if (condition.addSurfaceDepth) {
                    threshold += state.depth;
                }
                if (condition.secondaryDepthRange != 0) {
                    if (!state.known->surfaceSecondary.has_value()) {
                        throw DecodeError(
                            "a stone_depth with a secondary_depth_range reached the decoder "
                            "without minecraft:surface_secondary");
                    }
                    threshold += static_cast<std::int32_t>(
                        (*state.known->surfaceSecondary + 1.0) * 0.5 *
                        static_cast<double>(condition.secondaryDepthRange));
                }
                return run - 1 <= threshold ? 1 : 0;
            }
        }
        throw DecodeError("an unknown condition type reached the decoder");
    }

    [[nodiscard]] int symbolic(stratum::surface::ConditionIndex index, State& state) const {
        if (state.assign[index] >= 0) {
            return state.assign[index];
        }
        state.hit = index;
        return kHit;
    }

    const stratum::surface::RuleGraph* graph_;
    const stratum::settings::NoiseSettings* settings_;
    std::vector<std::string> noiseNames_;
    std::vector<Constraints> constraints_;
    std::vector<int> noiseOf_;
};

// ------------------------------------------------- driving it over a region
//
// Everything below is shared with
// tests/conformance/vanilla_legacy_goldens_surface_test.cpp. A conformance
// case that re-derived the walk — which positions the surface system visits,
// how a stone-depth run is counted, what a water height is — would be
// asserting its own second copy of the decoder rather than the one the
// analyzer's control validated, which is the single thing this readback
// cannot afford.

using stratum::data::ResourceLocation;
using stratum::settings::NoiseSettings;
using stratum::surface::Context;
using stratum::surface::RuleGraph;

/// The six blocks the ore-vein system places. A vein block is solid but is
/// not the default block, and terrain::ChunkFiller SKIPS it: the surface
/// system never repaints one. Listed here rather than reached through the
/// filler because the list is private to it — a position it skips is a
/// position whose golden block says nothing at all about any noise.
[[nodiscard]] bool isVeinBlock(const stratum::chunk::BlockState& block) {
    static constexpr std::array<std::string_view, 6> kVeinBlocks{
        "minecraft:granite", "minecraft:copper_ore",         "minecraft:raw_copper_block",
        "minecraft:tuff",    "minecraft:deepslate_iron_ore", "minecraft:raw_iron_block"};
    return std::ranges::find(kVeinBlocks, block.name) != kVeinBlocks.end();
}

/// Every position terrain::ChunkFiller's second pass would hand the surface
/// rules, in its order: from the column's topmost NON-AIR block down, and
/// once solid has been crossed, non-solid positions are skipped for good.
/// Reproduced rather than approximated — a decoder that scored positions
/// vanilla never visited would be reading blocks the rules never decided.
template<typename Fn>
void forEachSurfacePosition(const GoldenChunk& chunk, int localX, int localZ, bool oreVeins,
                            Fn&& body) {
    const std::int32_t from = chunk.scanFrom(localX, localZ);
    if (from < chunk.minY()) {
        return;
    }
    bool crossedSolid = false;
    for (std::int32_t y = from; y >= chunk.minY(); --y) {
        const Category category = chunk.categoryAt(localX, y, localZ);
        if (category != Category::Solid) {
            if (crossedSolid) {
                continue;
            }
        } else {
            crossedSolid = true;
            const stratum::chunk::BlockState* block = chunk.blockAt(localX, y, localZ);
            if (oreVeins && block != nullptr && isVeinBlock(*block)) {
                continue;
            }
        }
        body(y, category);
    }
}

/// Builds the surface::Context for one position from the golden region alone.
/// `steep`'s four neighbour heights are clamped into the chunk exactly as
/// surface::fillSteepNeighbours does, so no neighbouring chunk is read.
[[nodiscard]] Context contextFor(const GoldenChunk& chunk, int localX, int localZ, std::int32_t y) {
    Context at;
    at.x = (chunk.chunkX() * 16) + localX;
    at.z = (chunk.chunkZ() * 16) + localZ;
    at.y = y;
    at.stoneDepthAbove = chunk.depthAbove(localX, y, localZ);
    at.stoneDepthBelow = chunk.depthBelow(localX, y, localZ);
    at.waterHeight = chunk.waterHeightLow(localX, localZ);
    at.heightWest = chunk.scanFrom(std::max(localX - 1, 0), localZ);
    at.heightEast = chunk.scanFrom(std::min(localX + 1, 15), localZ);
    at.heightNorth = chunk.scanFrom(localX, std::max(localZ - 1, 0));
    at.heightSouth = chunk.scanFrom(localX, std::min(localZ + 1, 15));
    if (const std::string* biome = chunk.biomeAt(localX, y, localZ); biome != nullptr) {
        at.biome = ResourceLocation::parse(*biome);
    }
    return at;
}

/// A condition's human name: the noise it reads and the interval it tests.
[[nodiscard]] std::string describeCondition(const RuleGraph& graph,
                                            stratum::surface::ConditionIndex index) {
    const stratum::surface::Condition& condition = graph.condition(index);
    std::string text = condition.noise.has_value() ? condition.noise->toString() : std::string{"?"};
    char buffer[64];
    if (condition.maxThreshold > 1e300) {
        std::snprintf(buffer, sizeof(buffer), " >= %.6g", condition.minThreshold);
    } else {
        std::snprintf(buffer, sizeof(buffer), " in [%.6g, %.6g]", condition.minThreshold,
                      condition.maxThreshold);
    }
    return text + buffer;
}

/// Does a rule's block state name the same thing the golden holds?
[[nodiscard]] bool sameState(const stratum::settings::BlockState& rule,
                             const stratum::chunk::BlockState& golden) {
    if (rule.name.toString() != golden.name || rule.properties.size() != golden.properties.size()) {
        return false;
    }
    for (const auto& [key, value] : golden.properties) {
        const auto found = rule.properties.find(key);
        if (found == rule.properties.end() || found->second != value) {
            return false;
        }
    }
    return true;
}

// ------------------------------------------------------------------ tallies

/// One condition's readback over one dimension.
struct ConditionTally {
    std::size_t observed = 0;    ///< positions where the block decides this bit
    std::size_t touchedOnly = 0; ///< the tree consulted it and the block does not say
    std::size_t trueBits = 0;
    std::size_t columns = 0;        ///< distinct (x, z) it is observed at
    std::size_t contradictions = 0; ///< two y in one column decoding it BOTH ways
};

struct WalkStats {
    std::size_t positions = 0;
    std::size_t unexplained = 0; ///< no assignment reproduces the golden block
    std::size_t overflowed = 0;
    std::size_t noBiome = 0;
    /// THE REPLAY ARM, and the reason the control is worth anything. The
    /// decoder reconstructs a surface::Context out of a POST-rule region:
    /// stone-depth runs, a water height, four neighbour heights. Where a rule
    /// changed a position's CATEGORY — the overworld tree places
    /// `minecraft:water` and `minecraft:air`, and the Nether's places lava
    /// under `hole` — that reconstruction cannot be exact, and a wrong
    /// Context is a wrong decode that no amount of internal consistency would
    /// catch. So the control also RUNS the library's own surface::Executor on
    /// the reconstructed Context with the true modern noises and asks whether
    /// it reproduces the golden block. It is an independent check on the
    /// reconstruction, and the decoded bits are scored on the positions where
    /// it passes as well as on all of them, with both numbers reported.
    std::size_t replayed = 0;
    std::size_t replayAgreed = 0;
};

/// A per-column field of decoded bits for one condition: 512x512 of them, one
/// region's worth. -1 where the column never decided it, and kPoisoned where
/// two positions in the SAME column decided it BOTH ways.
///
/// A contradiction is not noise to be averaged away. A `noise_threshold`
/// samples at (x, 0, z), so its value is constant down a column and two
/// positions cannot honestly disagree about it: a contradicting column is a
/// column where the decoder is demonstrably wrong, and the rate of them is
/// this readback's own measured error bound. They are dropped whole rather
/// than resolved by first-wins, which would keep exactly the wrong half.
struct Field {
    static constexpr std::int8_t kPoisoned = -2;

    std::vector<std::int8_t> bits;
    std::size_t contradictions = 0;

    Field() : bits(kColumnsPerRegion, -1) {}

    [[nodiscard]] static std::size_t indexOf(std::int32_t x, std::int32_t z) {
        return (static_cast<std::size_t>(z) * 512U) + static_cast<std::size_t>(x);
    }

    void set(std::int32_t x, std::int32_t z, std::int8_t value) {
        std::int8_t& slot = bits[indexOf(x, z)];
        if (slot == kPoisoned) {
            return;
        }
        if (slot >= 0 && slot != value) {
            ++contradictions;
            slot = kPoisoned;
            return;
        }
        slot = value;
    }
};

/// What the control has beyond the golden region: the library's own compiled
/// rules and the biome temperatures they read. Null for the Nether, whose
/// tree cannot be compiled under a legacy source at all.
struct Replay {
    const stratum::surface::Executor* executor = nullptr;
    const stratum::biome::TemperatureTable* temperatures = nullptr;
    /// Score only the positions the replay reproduces.
    bool gate = false;
};

struct Walk {
    WalkStats stats;
    std::vector<ConditionTally> tallies;
    /// Per condition, the decoded per-column field. Only filled when asked
    /// for: the overworld's 33 conditions over eight regions would be 70 MB
    /// of it for nothing.
    std::vector<Field> fields;
    bool keepFields = false;
};

/// Walks one golden region and decodes every surface position in it.
/// @p knownFor supplies what the caller can compute for a column; returning a
/// Known with no surfaceDepth makes the decoder enumerate it.
template<typename KnownFor>
void walkRegion(const std::filesystem::path& regionPath, const NoiseSettings& settings,
                const Decoder& decoder, std::int32_t stride, bool trustSteep, const Replay& replay,
                KnownFor&& knownFor, Walk& into) {
    const std::vector<GoldenChunk> chunks =
        legacy_goldens::readRegion(regionPath, settings, stride);
    Observed observed;
    std::vector<std::uint8_t> touched;
    const stratum::settings::BlockState air{.name = ResourceLocation::parse("minecraft:air"),
                                            .properties = {}};
    for (const GoldenChunk& chunk : chunks) {
        for (int localZ = 0; localZ < 16; ++localZ) {
            for (int localX = 0; localX < 16; ++localX) {
                const std::int32_t worldX = (chunk.chunkX() * 16) + localX;
                const std::int32_t worldZ = (chunk.chunkZ() * 16) + localZ;
                Known known = knownFor(worldX, worldZ);
                known.waterLow = chunk.waterHeightLow(localX, localZ);
                known.waterHigh = chunk.waterHeightHigh(localX, localZ);
                if (trustSteep) {
                    const Context column = contextFor(chunk, localX, localZ, chunk.minY());
                    known.steep = (column.heightWest - column.heightEast >= 4) ||
                                  (column.heightSouth - column.heightNorth >= 4);
                }
                forEachSurfacePosition(
                    chunk, localX, localZ, settings.oreVeinsEnabled,
                    [&](std::int32_t y, Category category) {
                        const stratum::chunk::BlockState* golden = chunk.blockAt(localX, y, localZ);
                        if (golden == nullptr) {
                            return;
                        }
                        Context at = contextFor(chunk, localX, localZ, y);
                        if (!at.biome.has_value()) {
                            ++into.stats.noBiome;
                            return;
                        }
                        ++into.stats.positions;
                        if (replay.executor != nullptr) {
                            Context played = at;
                            played.preliminarySurface = known.preliminarySurface.value_or(0);
                            if (replay.temperatures != nullptr) {
                                played.biomeTemperature = replay.temperatures->at(*at.biome);
                            }
                            const stratum::settings::BlockState* placed =
                                replay.executor->apply(played);
                            const stratum::settings::BlockState& expected =
                                placed != nullptr
                                    ? *placed
                                    : (category == Category::Air
                                           ? air
                                           : (category == Category::Fluid ? settings.defaultFluid
                                                                          : settings.defaultBlock));
                            ++into.stats.replayed;
                            if (sameState(expected, *golden)) {
                                ++into.stats.replayAgreed;
                            } else if (replay.gate) {
                                return;
                            }
                        }
                        const Decoder::Counts counts =
                            decoder.decode(at, known, *golden, category, observed, &touched);
                        if (counts.overflowed) {
                            ++into.stats.overflowed;
                            return;
                        }
                        if (counts.consistent == 0) {
                            ++into.stats.unexplained;
                            return;
                        }
                        for (std::size_t i = 0; i < observed.size(); ++i) {
                            if (decoder.noiseOf(static_cast<stratum::surface::ConditionIndex>(i)) <
                                0) {
                                continue;
                            }
                            ConditionTally& tally = into.tallies[i];
                            if (observed[i] < 0) {
                                if (touched[i] != 0) {
                                    ++tally.touchedOnly;
                                }
                                continue;
                            }
                            ++tally.observed;
                            if (observed[i] == 1) {
                                ++tally.trueBits;
                            }
                            if (into.keepFields) {
                                into.fields[i].set(worldX, worldZ, observed[i]);
                            }
                        }
                    });
            }
        }
    }
    if (into.keepFields) {
        for (std::size_t i = 0; i < into.fields.size(); ++i) {
            into.tallies[i].contradictions += into.fields[i].contradictions;
            for (const std::int8_t bit : into.fields[i].bits) {
                if (bit >= 0) {
                    ++into.tallies[i].columns;
                }
            }
        }
    }
}

[[nodiscard]] double percent(std::size_t part, std::size_t whole) {
    return whole == 0 ? 0.0 : 100.0 * static_cast<double>(part) / static_cast<double>(whole);
}

// ------------------------------------------------------------- the control

struct ControlArm {
    std::size_t bits = 0;
    std::size_t agreed = 0;
    std::size_t majority = 0; ///< the trivial predictor: always the commoner answer
    /// A handful of disagreements, spelled out. A control that merely reports
    /// a percentage cannot be debugged; one that names the columns can.
    std::vector<std::string> samples;
};

/// Scores the decoded bits of one region against a noise registry. @p noises
/// may be built at the world seed (the recovery arm) or at worldSeed + 1 (the
/// negative arm); everything else about the run is identical.
void scoreAgainstRegistry(const RuleGraph& graph, const Decoder& decoder,
                          const stratum::density::NoiseRegistry& noises, const Walk& walk,
                          std::map<std::string, ControlArm>& into) {
    for (std::size_t i = 0; i < walk.fields.size(); ++i) {
        const auto index = static_cast<stratum::surface::ConditionIndex>(i);
        if (decoder.noiseOf(index) < 0) {
            continue;
        }
        const stratum::surface::Condition& condition = graph.condition(index);
        const stratum::noise::NormalNoise* noise = noises.find(*condition.noise);
        if (noise == nullptr) {
            continue;
        }
        ControlArm& arm = into[condition.noise->toString()];
        const Field& field = walk.fields[i];
        std::size_t trueBits = 0;
        std::size_t bits = 0;
        std::size_t agreed = 0;
        for (std::int32_t z = 0; z < 512; ++z) {
            for (std::int32_t x = 0; x < 512; ++x) {
                const std::int8_t bit = field.bits[Field::indexOf(x, z)];
                if (bit < 0) {
                    continue;
                }
                ++bits;
                if (bit == 1) {
                    ++trueBits;
                }
                const double value =
                    noise->sample(static_cast<double>(x), 0.0, static_cast<double>(z));
                const bool holds =
                    condition.minThreshold <= value && value <= condition.maxThreshold;
                if (holds == (bit == 1)) {
                    ++agreed;
                } else if (arm.samples.size() < 8) {
                    arm.samples.push_back(condition.noise->toString() + " at (" +
                                          std::to_string(x) + ", " + std::to_string(z) +
                                          ") decoded " + std::to_string(int{bit}) + " value " +
                                          std::to_string(value));
                }
            }
        }
        arm.bits += bits;
        arm.agreed += agreed;
        arm.majority += std::max(trueBits, bits - trueBits);
    }
}

/// The router's `preliminary_surface_level` as it reaches
/// `above_preliminary_surface`: sampled on a 16-block lattice anchored at the
/// world origin, each sample floored where it is taken, blended, floored
/// again. That is ChunkFiller's own `preliminarySurfaceIn`, and it is called
/// rather than copied — tests/conformance/vanilla_psl_lattice_test.cpp is
/// what pins the reading against the server (36864 of 36864 columns), and a
/// second copy of it here is exactly what would drift.
///
/// Sampling on the lattice is also what makes the control affordable: one
/// region needs 1089 router evaluations instead of 262144.
class PreliminarySurface {
public:
    PreliminarySurface(const stratum::density::Graph& graph,
                       const stratum::density::NoiseRegistry& noises, const NoiseSettings& settings)
        : interpreter_(graph, noises,
                       stratum::density::CellGeometry{.width = settings.geometry.cellWidth(),
                                                      .height = settings.geometry.cellHeight()}),
          root_(settings.router.at(stratum::settings::RouterEntry::PreliminarySurfaceLevel)) {}

    [[nodiscard]] std::int32_t at(std::int32_t x, std::int32_t z) {
        constexpr std::int32_t kPitch = stratum::terrain::ChunkFiller::kPreliminarySurfacePitch;
        const std::int32_t x0 = stratum::javamath::floorDiv(x, kPitch) * kPitch;
        const std::int32_t z0 = stratum::javamath::floorDiv(z, kPitch) * kPitch;
        const std::array<double, 4> corners{raw(x0, z0), raw(x0 + kPitch, z0), raw(x0, z0 + kPitch),
                                            raw(x0 + kPitch, z0 + kPitch)};
        return stratum::terrain::ChunkFiller::preliminarySurfaceIn(corners, x - x0, z - z0);
    }

private:
    [[nodiscard]] double raw(std::int32_t x, std::int32_t z) {
        const auto key = (static_cast<std::int64_t>(x) << 32) ^ static_cast<std::uint32_t>(z);
        const auto found = cache_.find(key);
        if (found != cache_.end()) {
            return found->second;
        }
        const double value =
            interpreter_.evaluate(root_, stratum::density::Point{.x = x, .y = 0, .z = z});
        cache_.emplace(key, value);
        return value;
    }

    stratum::density::Interpreter interpreter_;
    stratum::density::NodeIndex root_;
    std::map<std::int64_t, double> cache_;
};

// -------------------------------------------------------- the candidate space
//
// THE SAME 270,000 CANDIDATES tools/analysis/legacy-seed-analyze.cpp
// enumerates, in the SAME index order, so that a rule number printed by one
// tool names the same rule in the other — 5 bases x 10 salts x 3 combines x
// 3 fork counts x 2 generators = 900 seed rules, times 300 block offsets.
// That file owns the scan over synthetic probe worlds and is not edited here;
// this is a second SCORER for the same space, against a different oracle (the
// golden Nether regions rather than a probe dimension's terrain height), and
// it re-states the enumeration rather than sharing it because the two tools
// have to be able to disagree for the comparison between them to mean
// anything. If they ever diverge, the candidate named by `--candidate 182 0`
// — deepslate's own derivation — is the fixed point that says so.
//
// The same two gaps apply, unchanged and restated so they are not assumed
// away: ONE stack rule (every octave's Perlin block drawn in order from a
// single generator), no per-octave salting, no frequency rule other than the
// noise's declared firstOctave, and no discarded-LCG-STEP offset of the kind
// `minecraft:end_islands` turned out to use.

inline constexpr std::size_t kBases = 5;
inline constexpr std::size_t kSalts = 10;
inline constexpr std::size_t kCombines = 3;
inline constexpr std::size_t kForks = 3;
inline constexpr std::size_t kGenerators = 2;
inline constexpr std::size_t kSeedRules = kBases * kSalts * kCombines * kForks * kGenerators;
inline constexpr std::size_t kBlockOffsets = 300;

/// deepslate's own derivation, as legacy-seed-analyze.cpp names it:
/// JavaRandom(worldSeed).nextLong(), XORed with the first eight bytes of
/// MD5("ns:path") big-endian, one further fork, driving the LCG, at offset 0.
inline constexpr std::size_t kDeepslateRule = 182;

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

struct SeedRule {
    Base base = Base::WorldSeed;
    Salt salt = Salt::None;
    Combine combine = Combine::Xor;
    int forks = 0;
    Generator generator = Generator::Lcg;
};

[[nodiscard]] inline SeedRule ruleAt(std::size_t index) {
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

[[nodiscard]] inline std::string describe(const SeedRule& rule) {
    static constexpr std::array<const char*, kBases> kBaseNames{"worldSeed", "lcgLong", "xoroLo",
                                                                "scrambled", "zero"};
    static constexpr std::array<const char*, kSalts> kSaltNames{
        "md5FirstBE",  "md5FirstLE",     "md5LastBE",  "md5LastLE",    "md5LoXorHi",
        "md5LoPlusHi", "md5PathFirstBE", "hashCodeId", "hashCodePath", "none"};
    static constexpr std::array<const char*, kCombines> kCombineNames{"xor", "add", "sub"};
    return std::string{kBaseNames[static_cast<std::size_t>(rule.base)]} + " " +
           kCombineNames[static_cast<std::size_t>(rule.combine)] + " " +
           kSaltNames[static_cast<std::size_t>(rule.salt)] + ", forks " +
           std::to_string(rule.forks) + ", " +
           (rule.generator == Generator::Lcg ? "lcg" : "xoroshiro");
}

[[nodiscard]] inline std::uint64_t beU64(const stratum::hash::Md5Digest& digest, std::size_t at) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value = (value << 8U) | digest[at + i];
    }
    return value;
}

[[nodiscard]] inline std::uint64_t leU64(const stratum::hash::Md5Digest& digest, std::size_t at) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(digest[at + i]) << (8U * i);
    }
    return value;
}

[[nodiscard]] inline std::int32_t javaHashCode(std::string_view text) {
    std::uint32_t hash = 0;
    for (const char character : text) {
        hash = (hash * 31U) + static_cast<std::uint32_t>(static_cast<unsigned char>(character));
    }
    return static_cast<std::int32_t>(hash);
}

[[nodiscard]] inline std::uint64_t saltFor(Salt salt, std::string_view id) {
    const std::size_t colon = id.find(':');
    const std::string_view path = colon == std::string_view::npos ? id : id.substr(colon + 1);
    const stratum::hash::Md5Digest digest = stratum::hash::md5(id);
    const stratum::hash::Md5Digest pathDigest = stratum::hash::md5(path);
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

[[nodiscard]] inline std::uint64_t baseFor(Base base, std::int64_t worldSeed) {
    switch (base) {
        case Base::WorldSeed:
            return static_cast<std::uint64_t>(worldSeed);
        case Base::LcgLong: {
            stratum::rng::JavaRandom random{worldSeed};
            return static_cast<std::uint64_t>(random.nextLong());
        }
        case Base::XoroLo: {
            stratum::rng::Xoroshiro128PlusPlus random{worldSeed};
            return static_cast<std::uint64_t>(random.nextLong());
        }
        case Base::Scrambled:
            return (static_cast<std::uint64_t>(worldSeed) ^ UINT64_C(0x5DEECE66D)) &
                   ((UINT64_C(1) << 48U) - 1);
        case Base::Zero:
            return 0;
    }
    return 0;
}

[[nodiscard]] inline std::int64_t seedFor(const SeedRule& rule, std::int64_t worldSeed,
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
        stratum::rng::JavaRandom random{static_cast<std::int64_t>(seed)};
        seed = static_cast<std::uint64_t>(random.nextLong());
    }
    return static_cast<std::int64_t>(seed);
}

/// `amplitude != 0.0`, spelled so -Wfloat-equal does not fire.
[[nodiscard]] inline bool nonZero(double amplitude) {
    return std::abs(amplitude) > 0.0;
}

struct Octave {
    std::size_t block = 0;
    double amplitude = 0.0;
    double persistence = 0.0;
    double frequency = 0.0;
};

/// The sequential stack rule, and only it: the first stack's non-zero octaves
/// then the second's, drawn in order from one generator.
struct Layout {
    std::vector<Octave> first;
    std::vector<Octave> second;
    double valueFactor = 1.0;
    std::size_t blocksPerNoise = 0;
};

[[nodiscard]] inline Layout layoutFor(int firstOctave, const std::vector<double>& amplitudes) {
    Layout layout;
    const auto count = static_cast<int>(amplitudes.size());
    std::size_t block = 0;
    for (std::vector<Octave>* stack : {&layout.first, &layout.second}) {
        double frequency = std::ldexp(1.0, firstOctave);
        double persistence = std::ldexp(1.0, count - 1) / (std::ldexp(1.0, count) - 1.0);
        for (const double amplitude : amplitudes) {
            if (nonZero(amplitude)) {
                stack->push_back({.block = block,
                                  .amplitude = amplitude,
                                  .persistence = persistence,
                                  .frequency = frequency});
                ++block;
            }
            frequency *= 2.0;
            persistence *= 0.5;
        }
    }
    layout.blocksPerNoise = block;

    std::size_t first = 0;
    std::size_t last = amplitudes.size();
    while (last > first && !nonZero(amplitudes[last - 1])) {
        --last;
    }
    while (first < last && !nonZero(amplitudes[first])) {
        ++first;
    }
    const auto effective = static_cast<double>(last - first);
    layout.valueFactor = (1.0 / 6.0) / (0.1 * (1.0 + (1.0 / effective)));
    return layout;
}

[[nodiscard]] inline double sampleStack(const std::vector<Octave>& stack,
                                        const std::vector<stratum::noise::PerlinNoise>& blocks,
                                        std::size_t offset, double x, double y, double z) {
    double total = 0.0;
    for (const Octave& octave : stack) {
        const stratum::noise::PerlinNoise& perlin = blocks[offset + octave.block];
        total += (perlin.sample(x * octave.frequency, y * octave.frequency, z * octave.frequency) *
                  octave.amplitude) *
                 octave.persistence;
    }
    return total;
}

[[nodiscard]] inline double sampleNormal(const Layout& layout,
                                         const std::vector<stratum::noise::PerlinNoise>& blocks,
                                         std::size_t offset, double x, double y, double z) {
    constexpr double kSecondFrequency = 337.0 / 331.0;
    const double combined = sampleStack(layout.first, blocks, offset, x, y, z) +
                            sampleStack(layout.second, blocks, offset, x * kSecondFrequency,
                                        y * kSecondFrequency, z * kSecondFrequency);
    return combined * layout.valueFactor;
}

[[nodiscard]] inline std::vector<stratum::noise::PerlinNoise>
blocksFor(const SeedRule& rule, std::int64_t seed, std::size_t needed) {
    std::vector<stratum::noise::PerlinNoise> blocks;
    blocks.reserve(needed);
    if (rule.generator == Generator::Lcg) {
        stratum::rng::JavaRandom random{seed};
        for (std::size_t i = 0; i < needed; ++i) {
            blocks.push_back(stratum::noise::PerlinNoise::fromRandom(random));
        }
    } else {
        stratum::rng::Xoroshiro128PlusPlus random{seed};
        for (std::size_t i = 0; i < needed; ++i) {
            blocks.push_back(stratum::noise::PerlinNoise::fromRandom(random));
        }
    }
    return blocks;
}

} // namespace legacy_goldens
