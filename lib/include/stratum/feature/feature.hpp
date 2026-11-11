// Stratum — the decoration stage: placed features, run over carved terrain.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// SPEC §10 M6. A chunk's decoration runs every placed feature its biomes
// list, generation step by generation step, each from its own seed, reading
// and writing the 3x3 chunks around it.
//
// THE PARITY BAR (the owner, 2026-11-11; SPEC §7): the FRAMEWORK is exact —
// the seeds, the order features run in, which features a chunk runs, the
// placement modifiers and the providers they draw through. None of it is
// documented to the bit, so all of it is measured against the server
// (tools/analysis/feature-probe.sh; SPEC §11, "The decoration framework,
// measured") and each claim below says what measured it. A feature TYPE is
// exact where a clean-room spec under spec/ specifies it and statistical
// where none does; each says which in its own comment.
//
// WHAT IS NOT IMPLEMENTED IS NAMED, NOT REFUSED (the owner, the same day):
// a placed feature this build cannot run — its feature type, a modifier, a
// provider or a predicate it uses — is left out of the world, and
// FeatureSet::unsupported() lists it with the reason. It keeps its place in
// the order, so every other feature's seed is the server's. Malformed JSON is
// still refused (FeatureError), naming the entry.
#pragma once

#include <stratum/biome/voronoi.hpp>
#include <stratum/data/block_tags.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/rng/worldgen_random.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/rule_graph.hpp>
#include <stratum/terrain/filler.hpp>

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace stratum::feature {

/// Feature data that is not what the schema allows: refused, by name.
class FeatureError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Feature data that is well formed and that this build does not run. Caught
/// where a placed feature is compiled, which then goes unrun and listed.
class Unsupported : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// An int provider. Every draw MEASURED (feature-probe.sh --variant draws):
///
///   uniform           min + nextInt(max - min + 1)
///   biased_to_bottom  min + nextInt(nextInt(max - min + 1) + 1)
///   clamped           the source's sample, clamped
///   clamped_normal    mean + gaussian * deviation, truncated toward zero,
///                     clamped (the terrain variant, where it goes negative)
///   weighted_list     nextInt(total weight), walked down the entries
struct IntProvider {
    enum class Kind : std::uint8_t {
        Constant,
        Uniform,
        BiasedToBottom,
        Clamped,
        ClampedNormal,
        WeightedList,
    };
    Kind kind = Kind::Constant;
    /// Constant: the value in `min`. The others: the inclusive bounds.
    std::int32_t min = 0;
    std::int32_t max = 0;
    float mean = 0.0F;
    float deviation = 0.0F;
    /// Clamped: the source. WeightedList: the entries, with `weights`.
    std::vector<IntProvider> children;
    std::vector<std::int32_t> weights;

    [[nodiscard]] std::int32_t sample(rng::WorldgenRandom& random) const;
    /// The least and greatest value sample() can return.
    [[nodiscard]] std::int32_t lowest() const;
    [[nodiscard]] std::int32_t highest() const;
};

/// A height provider, over vertical anchors. MEASURED (--variant draws), with
/// range = max - min:
///
///   uniform                min + nextInt(range + 1)
///   biased_to_bottom       min + nextInt(nextInt(range - inner + 1) + inner)
///   very_biased_to_bottom  min + nextInt(nextInt(nextInt(range - inner + 1)
///                          + inner) + inner)
///   trapezoid              plateau >= range: the uniform; else, with
///                          m = (range - plateau) / 2 and n = range - m,
///                          min + nextInt(n + 1) + nextInt(m + 1)
///   weighted_list          as the int provider's
///
/// NOT MEASURED: a range that is empty or inverted (max below min, or `inner`
/// past the range). The server logs it and this build returns min, drawing
/// nothing.
struct HeightProvider {
    enum class Kind : std::uint8_t {
        Constant,
        Uniform,
        BiasedToBottom,
        VeryBiasedToBottom,
        Trapezoid,
        WeightedList,
    };
    Kind kind = Kind::Constant;
    surface::VerticalAnchor min;
    surface::VerticalAnchor max;
    std::int32_t inner = 1;
    std::int32_t plateau = 0;
    std::vector<HeightProvider> children;
    std::vector<std::int32_t> weights;

    [[nodiscard]] std::int32_t sample(rng::WorldgenRandom& random,
                                      const settings::NoiseGeometry& geometry) const;
};

/// The heightmaps a modifier may name. The `_WG` pair are the terrain's,
/// fixed before any feature runs; the other four are live, and see what
/// features have written (MEASURED, --variant terrain: a marker on a column
/// raised the live maps for the markers after it and neither `_WG` map).
enum class HeightmapKind : std::uint8_t {
    WorldSurfaceWg,
    OceanFloorWg,
    WorldSurface,
    OceanFloor,
    MotionBlocking,
    MotionBlockingNoLeaves,
};

/// A block predicate, at an offset from the position asked.
struct BlockPredicate {
    enum class Kind : std::uint8_t {
        True,
        /// `matching_blocks` and `matching_block_tag`, the tag resolved.
        MatchingBlocks,
        /// `matching_fluids`: water, lava, or both.
        MatchingFluids,
        /// The offset position's y is inside the dimension.
        InsideWorldBounds,
        AllOf,
        AnyOf,
        Not,
    };
    Kind kind = Kind::True;
    std::array<std::int32_t, 3> offset{};
    std::set<data::ResourceLocation> blocks;
    bool water = false;
    bool lava = false;
    std::vector<BlockPredicate> children;
};

/// One placement modifier. Each turns a position into none, one or several.
struct Modifier {
    enum class Kind : std::uint8_t {
        /// x += nextInt(16), then z += nextInt(16).
        InSquare,
        /// The position, `count` times.
        Count,
        /// y from a height provider.
        HeightRange,
        /// y from a heightmap; nothing where the column is empty.
        Heightmap,
        /// Kept where the position's block-level biome lists the feature.
        Biome,
        /// Kept where the predicate holds.
        BlockPredicateFilter,
        /// Kept where nextFloat() < 1 / chance.
        RarityFilter,
        /// Kept where the live WORLD_SURFACE less the live OCEAN_FLOOR is at
        /// most `maxWaterDepth`.
        SurfaceWaterDepthFilter,
        /// Offset by draws of x, y, z in that order — one provider for x and
        /// z, each drawn — a constant drawing nothing.
        RandomOffset,
        /// A walk up or down to the first block the target holds at.
        EnvironmentScan,
        /// Kept where y is within the heightmap's value plus the two bounds.
        SurfaceRelativeThresholdFilter,
        /// The listed positions that fall in the chunk being decorated.
        FixedPlacement,
        /// The position `aboveNoise` times where the count noise at (x / 200,
        /// z / 200) is above `noiseLevel`, `belowNoise` times elsewhere.
        NoiseThresholdCount,
        /// The position ceil((noise(x / factor, z / factor) + offset) *
        /// ratio) times, or not at all where that is not positive.
        NoiseBasedCount,
    };
    Kind kind = Kind::InSquare;
    IntProvider count;
    IntProvider xzSpread;
    IntProvider ySpread;
    HeightProvider height;
    HeightmapKind heightmap = HeightmapKind::WorldSurfaceWg;
    BlockPredicate predicate;
    /// EnvironmentScan: where the walk may continue; `hasAllowed` unset when
    /// the modifier names none.
    BlockPredicate allowed;
    bool hasAllowed = false;
    bool scanUp = false;
    std::int32_t maxSteps = 0;
    std::int32_t chance = 1;
    std::int32_t maxWaterDepth = 0;
    /// SurfaceRelativeThresholdFilter's bounds, an absent one unbounded.
    std::int32_t minInclusive = INT32_MIN;
    std::int32_t maxInclusive = INT32_MAX;
    std::vector<std::array<std::int32_t, 3>> positions;
    /// The two noise counts. THE COUNT NOISE, MEASURED (--variant misc): a 2D
    /// simplex over the permutation a java.util.Random seeded with the
    /// constant 2345 builds — the one seed of 200 000 searched that fits 288
    /// chunks' counts, and 200 the one scale of 2000 — so it is the same field
    /// in every world, as the surface rules' temperature noise is (seeded
    /// 1234, surface/executor.hpp).
    double noiseLevel = 0.0;
    std::int32_t belowNoise = 0;
    std::int32_t aboveNoise = 0;
    std::int32_t noiseToCountRatio = 0;
    double noiseFactor = 1.0;
    double noiseOffset = 0.0;
};

/// A block state provider.
struct StateProvider {
    enum class Kind : std::uint8_t {
        Simple,
        /// nextInt(total weight), walked down the entries (MEASURED, --variant
        /// draws, plane 8).
        Weighted,
    };
    Kind kind = Kind::Simple;
    std::vector<settings::BlockState> states;
    std::vector<std::int32_t> weights;

    [[nodiscard]] const settings::BlockState& sample(rng::WorldgenRandom& random) const;
};

/// A rule test: which blocks an ore target replaces.
struct RuleTest {
    enum class Kind : std::uint8_t {
        AlwaysTrue,
        /// `block_match` and `tag_match`, the tag resolved.
        Blocks,
    };
    Kind kind = Kind::AlwaysTrue;
    std::set<data::ResourceLocation> blocks;
};

/// One of an ore's `targets`: the blocks it replaces, and with what.
struct OreTarget {
    RuleTest test;
    settings::BlockState state;
};

/// A configured feature this build runs.
struct ConfiguredFeature {
    enum class Type : std::uint8_t {
        /// Writes one state at the position, whatever is there (MEASURED: a
        /// marker replaced air, water and stone alike). Run only for blocks
        /// known to stay wherever they are put; a block that needs support
        /// (a plant) is Unsupported until its survival rule is measured.
        SimpleBlock,
        /// `minecraft:ore`: a blob about a segment through the position.
        ///
        /// NO SPEC; MEASURED, block for block (feature-probe.sh, the `ore*`
        /// variants; SPEC §11, "The ore feature, measured"): 354 574 blocks
        /// of twelve sizes replacing whatever was there, every one and no
        /// other. With `s` the size and (x, y, z) the position:
        ///
        ///   a = nextFloat() * pi, in single precision
        ///   the segment runs from (x + sin(a) * s/8, y + nextInt(3) - 2,
        ///   z + cos(a) * s/8) to (x - sin(a) * s/8, y + nextInt(3) - 2,
        ///   z - cos(a) * s/8): the true sine and cosine, rounded to single,
        ///   and the products single
        ///
        /// Then the attempt is GIVEN UP, three draws in, unless some column
        /// within H + R blocks of (x, z) on both axes has the terrain's
        /// ocean-floor height at or above y - 2 - R, where H = ceil(s/8) and
        /// R = ceil((s/8 + 1) / 2) (the `oresky` and `oresurface` variants).
        /// Otherwise, for i in [0, s), with t = i/s in single precision:
        ///
        ///   centre_i = the segment at t
        ///   radius_i = ((S(pi * t) + 1) * nextDouble() * s/16 + 1) / 2
        ///
        /// S the carvers' table sine (carver/trig.hpp) — the true sine
        /// leaves three blocks of the 354 574 wrong, the table none. A block
        /// is in the blob when its centre is strictly inside some node's
        /// sphere. So an attempt that runs draws 3 + 2s longs. A node whose
        /// sphere lies wholly inside another's is dropped first — of each
        /// pair i < j still standing, the smaller, when the square of the
        /// radii's difference exceeds the squared distance of the centres —
        /// which changes no block of the blob, and decides the order below
        /// (the `oreexposed` variant: 272 blocks wrong without it of
        /// 168 694, none with).
        ///
        /// A block of the blob is offered to each target in turn and takes
        /// the first whose test it passes — unless it is discarded:
        /// `discard_chance_on_air_exposure` of 0 discards nothing and draws
        /// nothing; of 1 discards every block with air on one of its six
        /// sides, and draws nothing; between them, each block that passes a
        /// test draws nextFloat(), and is discarded when that is below the
        /// chance and it has air beside it (the `orediscard` variant). The
        /// blob's blocks take their draws node by node, and within a node x
        /// outermost, then y, then z, each block once.
        ///
        /// Where two chunks' ores meet, which came first decides what the
        /// second may replace, and so how many draws it takes; the server's
        /// chunk order is not fixed, so there the two can differ, and with
        /// them the rest of that feature's ores in that chunk.
        Ore,
        /// `minecraft:spring_feature`: one source block of a fluid, in a wall.
        ///
        /// minecraft.wiki's "Spring" gives the rule, and the server keeps to
        /// it (feature-probe.sh --variant springs: of 12 960 attempts of the
        /// shipped water and lava springs the rule places 49, and the server
        /// placed those 49 and one more, beside a chunk the comparison did
        /// not hold): the block above must be one of `valid_blocks`; so must
        /// the block below, where `requires_block_below`; and of the five
        /// blocks beside and below, exactly `rock_count` must be valid and
        /// exactly `hole_count` air. It draws nothing. The block written is
        /// the fluid's source (`level` 0, MEASURED, whatever `falling` the
        /// config's fluid state names), and it is marked for a fluid update;
        /// the server stores a scheduled fluid tick there instead, which
        /// comes to the same flow once the chunk ticks.
        ///
        /// NOT MEASURED: what the spring's own block may be. Here, air or a
        /// valid block — every spring of the probe replaced one or the other.
        Spring,
        /// `minecraft:disk`: a disc of columns about the position.
        ///
        /// MEASURED (feature-probe.sh --variant disks, the shipped sand, clay
        /// and gravel disks: 12 325 blocks written, 12 170 the server's and
        /// the other 155 another disk's, where two met in another order):
        /// one draw, the radius; every column within it of the position,
        /// dx^2 + dz^2 <= radius^2; in each, every block from `half_height`
        /// above the position to `half_height` below that the target holds
        /// at is rewritten — with the first rule's state whose predicate
        /// holds at that block, else the fallback's. The first hypothesis
        /// tried, and no block of it wrong.
        Disk,
    };
    Type type = Type::SimpleBlock;
    StateProvider toPlace;
    std::int32_t size = 0;
    float discardChance = 0.0F;
    std::vector<OreTarget> targets;
    /// Spring: the source block, and the rule's four fields.
    settings::BlockState fluid;
    std::set<data::ResourceLocation> validBlocks;
    bool requiresBlockBelow = true;
    std::int32_t rockCount = 4;
    std::int32_t holeCount = 1;
    /// Disk: its radius and half height, the blocks it rewrites, and what
    /// with — the first rule whose predicate holds at the block, else the
    /// fallback.
    IntProvider radius;
    std::int32_t halfHeight = 0;
    BlockPredicate target;

    struct StateRule {
        BlockPredicate ifTrue;
        StateProvider then;
    };

    std::vector<StateRule> stateRules;
};

struct PlacedFeature {
    std::shared_ptr<const ConfiguredFeature> feature;
    std::vector<Modifier> placement;
};

/// What a world freezes of its features (SPEC §6), detached from the pack:
/// every placed and configured feature and every biome's `features` field as
/// JSON, as written, and every block tag they name, resolved to its blocks.
struct FeatureSources {
    std::map<data::ResourceLocation, nlohmann::json> placed;
    std::map<data::ResourceLocation, nlohmann::json> configured;
    std::map<data::ResourceLocation, nlohmann::json> biomeFeatures;
    std::map<data::ResourceLocation, std::set<data::ResourceLocation>> tags;

    [[nodiscard]] bool empty() const noexcept { return biomeFeatures.empty(); }

    [[nodiscard]] bool operator==(const FeatureSources&) const = default;
};

/// Gathers @p pack's features: its placed and configured features, each
/// biome's `features` field, and every block tag of @p tags (a feature may
/// name any, and which it names is only known once it is compiled). Throws
/// FeatureError for a biome with no `features` field, and reads nothing
/// else: which features a dimension runs, and whether their data holds, is
/// FeatureSet::build's to say, for that dimension's biomes — so a pack whose
/// features are broken still generates its terrain, undecorated.
[[nodiscard]] FeatureSources gatherFeatureSources(const data::Pack& pack,
                                                  const data::BlockTags& tags);

/// One dimension's features, in the order they run. Immutable once built.
class FeatureSet {
public:
    struct Entry {
        data::ResourceLocation id;
        /// Null for a feature this build does not run.
        std::shared_ptr<const PlacedFeature> feature;
    };

    struct UnsupportedFeature {
        data::ResourceLocation id;
        std::string reason;
    };

    /// Compiles the features of @p biomes — the dimension's possible biomes,
    /// in its biome source's own order, each once — from @p sources.
    ///
    /// THE ORDER, MEASURED (--variant order: 77 shipped features' indices
    /// read off the server, every one reproduced; --variant biomefilter: a
    /// feature per biome and no shared ones, reproduced too). Per step, a
    /// graph: an edge from each feature of a biome's list to the next. Its
    /// nodes in first-seen order, biomes taken in @p biomes' order; a
    /// depth-first walk from each node in that order, a node's successors
    /// in that order; the order features run in is the walk's post-order,
    /// reversed. A feature's index is its position in its step.
    ///
    /// Throws FeatureError for a biome @p sources does not hold, a `features`
    /// field that is not lists of feature ids, a feature id the pack does not
    /// define, a feature listed in a cycle, or malformed feature JSON.
    [[nodiscard]] static FeatureSet build(const FeatureSources& sources,
                                          std::span<const data::ResourceLocation> biomes);

    /// Each step's features, in order.
    [[nodiscard]] const std::vector<std::vector<Entry>>& steps() const noexcept { return steps_; }

    /// Whether @p biome lists the feature at @p index of @p step.
    [[nodiscard]] bool lists(const data::ResourceLocation& biome, std::size_t step,
                             std::size_t index) const noexcept;

    /// Every feature of this dimension's biomes that goes unrun, and why.
    [[nodiscard]] const std::vector<UnsupportedFeature>& unsupported() const noexcept {
        return unsupported_;
    }

    /// How many of the dimension's listed features this build runs.
    [[nodiscard]] std::size_t supportedCount() const noexcept { return supported_; }

    /// Every state a feature of this set may write, each once, in a stable
    /// order: for a binding that has to know a dimension's block states
    /// before it fills a chunk.
    [[nodiscard]] std::vector<settings::BlockState> writtenStates() const;

private:
    std::vector<std::vector<Entry>> steps_;
    /// Per biome, per step: whether it lists each index.
    std::map<data::ResourceLocation, std::vector<std::vector<bool>>> listed_;
    std::vector<UnsupportedFeature> unsupported_;
    std::size_t supported_ = 0;
};

/// The terrain's two heightmaps, taken once a chunk is filled and carved and
/// before any feature writes into it: per column (`z * 16 + x`), the y above
/// the highest block that is not air (`worldSurface`) and above the highest
/// that is none of air, fluid and powder snow (`oceanFloor`); the dimension's
/// minimum y for a column with none.
struct TerrainHeightmaps {
    std::array<std::int32_t, 256> worldSurface{};
    std::array<std::int32_t, 256> oceanFloor{};
};

[[nodiscard]] TerrainHeightmaps terrainHeightmaps(const terrain::ChunkBuffer& blocks);

/// One chunk of a Region.
struct RegionChunk {
    /// Filled and carved; features write into it.
    terrain::ChunkBuffer* blocks = nullptr;
    const TerrainHeightmaps* heightmaps = nullptr;
    /// The chunk's quart biomes, `(qy * 4 + qz) * 4 + qx` with qy counted up
    /// from quart layer `minQuartY` (world::CompiledDimension::fillBiomes).
    std::span<const data::ResourceLocation* const> biomes;
    std::int32_t minQuartY = 0;
};

/// The 3x3 chunks a chunk's decoration reads and writes: `chunks[(dz + 1) * 3
/// + (dx + 1)]` is the chunk at (centreX + dx, centreZ + dz). All nine are
/// needed — which features a chunk runs is decided by the biomes of all nine
/// (MEASURED, --variant order: 288 chunks, no feature seen that the nine do
/// not list, 616 that the centre alone does not).
struct Region {
    std::int32_t centreX = 0;
    std::int32_t centreZ = 0;
    std::array<RegionChunk, 9> chunks{};
    settings::NoiseGeometry geometry;
    std::int64_t worldSeed = 0;
    /// The quart biome at a position outside the nine chunks.
    std::function<const data::ResourceLocation&(biome::QuartPos)> biomeOutside;
};

/// Decorates the centre chunk of @p region with @p features. Deterministic in
/// its inputs; the order chunks are decorated in is the caller's, and where
/// two chunks' features meet, the later sees the earlier's blocks, as on the
/// server (whose own order is not fixed).
void decorate(const FeatureSet& features, Region& region);

} // namespace stratum::feature
