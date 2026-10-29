// Stratum — running a dimension's surface rules.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// `RuleGraph` resolves the tree and says what it cannot run. This runs it.
//
// The split is deliberate and predates this file: loading is a schema
// question and has been settled since M4, while EXECUTING needs semantics
// that vanilla does not document and that had to be measured one at a time.
// So the graph loads whole and the executor refuses whole — a dimension whose
// tree contains one unrunnable construct is refused at compile, by name, with
// the reason, rather than run with that branch quietly skipped. §8 puts a
// world that generates and is silently wrong in the most severe class there
// is, and a surface rule that sometimes does nothing is exactly that.
//
// WHAT IT NEEDS. A `Context` per block, which the caller fills from whatever
// it has. Conditions that want something absent are not approximated: the
// executor refuses the whole tree at compile if it names one, so a Context
// missing a field can only ever be a programming error, never a wrong world.
#pragma once

#include <stratum/data/resource_location.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/javamath.hpp>
#include <stratum/noise/perlin.hpp>
#include <stratum/rng/java_random.hpp>
#include <stratum/rng/xoroshiro128.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/rule_graph.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace stratum::surface {

/// The length of `bandlands`' colour table — a literal in vanilla's own
/// source, not derived from any pack parameter (spec/bandlands-spec.md Q1.1).
inline constexpr std::size_t kClayBandsSize = 192;

/// How far above `sea_level` the `temperature` condition's height
/// adjustment starts (Executor::freezing).
inline constexpr std::int32_t kFreezingOriginAbove = 17;

/// Raised when a rule tree cannot be executed, naming what stopped it.
class ExecutionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// A biome as one compiled Executor knows it (Executor::biomeKey): which of
/// the identifiers its tree's `biome` conditions name it is, or that it is
/// none of them. A caller that resolves one biome for many blocks takes its
/// key once and hands it to every one of them in Context::biomeKey, and each
/// `biome` condition is then answered by one lookup instead of comparing the
/// biome's name against every name the condition lists.
///
/// A default-constructed key is no key, and a key is only ever read by the
/// executor that issued it (the one it was compiled as, or a copy of it,
/// which answers alike): any other ignores it.
struct BiomeKey {
    /// The issuing Executor's own identity; 0 for no key.
    std::uint64_t executor = 0;
    /// The biome's place in that Executor's table.
    std::uint32_t index = 0;
};

/// What the rules may ask about one block.
///
/// Everything here is a fact about the terrain AS THE FILLER LEFT IT, which
/// the caller computes once per column and reuses down it. The executor owns
/// the noise-derived quantities instead, since those are pure functions of the
/// world seed and the position.
struct Context {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;

    /// The column's preliminary surface level, for `above_preliminary_surface`
    /// — the router's `preliminary_surface_level` at (x, 0, z), FLOORED.
    ///
    /// Flooring is measured rather than tidy: a router handing this entry
    /// -0.5 is -1 here, where a `static_cast` would have said 0. It only
    /// separates on a datapack, since vanilla's own `find_top_surface`
    /// returns whole multiples of its `cell_height`.
    ///
    /// The condition does NOT compare y against this directly — see its case
    /// in `Executor::test`, which reaches `8 - surfaceDepth` blocks below it.
    std::int32_t preliminarySurface = 0;

    /// The biome at this position, for `biome`. Absent means the caller does
    /// not carry one — which is only reachable for a tree that names neither,
    /// since a tree that did would have been refused.
    std::optional<data::ResourceLocation> biome;

    /// The same biome as the running executor's key for it, for a caller
    /// that has one: Executor::biomeKey(*biome), or a key it issued for the
    /// biome this block is in. Where the key is the running executor's own,
    /// `biome` conditions answer from it and `biome` above is not read, so a
    /// caller with a key need not fill that in; where it is not (no key, the
    /// default), they compare `biome` by name, as they always did.
    BiomeKey biomeKey;

    /// That biome's declared temperature, for `temperature`. A float32 on
    /// purpose: vanilla holds it as one and the whole comparison is done in
    /// single precision, so widening it here would change the answer.
    float biomeTemperature = 0.0F;

    /// How deep this block sits in its run of solid, counting DOWN from the
    /// world top: 1 at the top of a run, reset by air, and — measured, not
    /// assumed — left UNCHANGED by fluid, which neither breaks a run nor
    /// counts toward it. Fluid is water and lava alike: the aquifer's literal
    /// lava in a water dimension, of the lattice or of the lava sea, holds
    /// the run exactly as water does (SPEC §11). Uncapped.
    std::int32_t stoneDepthAbove = 1;
    /// How deep this block sits in its run of solid counting UP from the
    /// world floor, for `surface_type: ceiling`. NOT the mirror image of
    /// `stoneDepthAbove`: here fluid RESETS the run, exactly as air does —
    /// water as much as lava — so a block one above a pool's roof is at 1.
    /// Measured (SPEC §11); this doc once called it "the same", and the
    /// filler skipped fluid here until the probe that measured lava showed
    /// water resetting too.
    std::int32_t stoneDepthBelow = 1;

    /// The column's water height: one above the FIRST fluid block met
    /// descending — water or lava, measured alike (SPEC §11) — latched and
    /// never updated again. Absent where the column holds no fluid at all,
    /// which makes `water` unconditionally true.
    ///
    /// `sea_level` alone does not create one — only real fluid blocks do.
    std::optional<std::int32_t> waterHeight;

    /// Surface heights of the four neighbours for `steep`, already clamped
    /// into this block's own chunk by the caller. `steep` never reads a
    /// neighbouring chunk, which is what keeps it out of the hot path's way.
    std::int32_t heightWest = 0;
    std::int32_t heightEast = 0;
    std::int32_t heightNorth = 0;
    std::int32_t heightSouth = 0;
};

/// Does @p graph read a column's SURFACE DEPTH anywhere, and so need
/// `minecraft:surface` and `minecraft:surface_secondary` built into the
/// registry it is compiled against?
///
/// Public because a caller has to know BEFORE compiling: `Executor::compile`
/// throws for a registry that cannot answer, and a caller like
/// `terrain::ChunkFiller` would rather report the tree as blocked, by name,
/// than surface that as a crash. It is deliberately not a list of condition
/// types a caller could re-derive: `above_preliminary_surface` reads a depth
/// even though nothing about its name or its schema says so (SPEC §11), and
/// a second copy of that knowledge is exactly what would drift.
[[nodiscard]] bool readsSurfaceDepth(const RuleGraph& graph);

/// Every `random_name` a `vertical_gradient` in this tree salts its
/// dimension's random source with, sorted and deduplicated.
///
/// This list is not a list of noises and never passes through NoiseRegistry,
/// so a dimension whose only draw on its declared random source is a
/// gradient looks, to the registry, like a dimension that needs nothing.
/// Under `legacy_random_source` that once made it the construct this build
/// refused by name; the legacy coin is measured now and the executor runs it
/// (verticalGradientFires, SPEC §11), so this is an inventory, not a gate.
[[nodiscard]] std::vector<std::string> verticalGradientNames(const RuleGraph& graph);

/// Every `worldgen/noise` entry this tree needs built before it can be
/// compiled: the ones its `noise_threshold` conditions name, plus the three
/// that no condition names — `minecraft:surface` and
/// `minecraft:surface_secondary` where a depth is read, and
/// `minecraft:clay_bands_offset` where `bandlands` is placed.
///
/// Sorted and deduplicated. This exists because the list was open-coded at
/// every call site, and one of those sites is now load-bearing in a way it
/// was not: a `legacy_random_source` dimension is refused if it needs a
/// NAMED noise, so "which noises does this dimension need" decides whether
/// it generates at all rather than only what gets built.
[[nodiscard]] std::vector<data::ResourceLocation> requiredNoises(const RuleGraph& graph);

/// The four clamped neighbour heights `steep` compares, given a way to read a
/// column's surface height. Chunk-local and clamped, so no neighbouring chunk
/// is ever touched.
///
/// @p heightAt receives ABSOLUTE coordinates and must return the highest y
/// holding a non-air block, with FLUID COUNTING as non-air — vanilla's
/// WORLD_SURFACE heightmap, not OCEAN_FLOOR. Any consistent offset cancels,
/// since only differences are used.
template<typename HeightAt>
void fillSteepNeighbours(Context& at, HeightAt&& heightAt) {
    const std::int32_t lx = javamath::floorMod(at.x, 16);
    const std::int32_t lz = javamath::floorMod(at.z, 16);
    const std::int32_t bx = at.x - lx;
    const std::int32_t bz = at.z - lz;
    at.heightWest = heightAt(bx + std::max(lx - 1, 0), bz + lz);
    at.heightEast = heightAt(bx + std::min(lx + 1, 15), bz + lz);
    at.heightNorth = heightAt(bx + lx, bz + std::max(lz - 1, 0));
    at.heightSouth = heightAt(bx + lx, bz + std::min(lz + 1, 15));
}

/// Per-call scratch for Executor::apply: what the rules read that is the
/// same at every y of a column, kept from the first block of the column that
/// reads it.
///
/// WHY IT EXISTS. Every one of these reads is a pure function of the world
/// seed and (x, z) alone: the surface depth (a `minecraft:surface` sample at
/// (x, 0, z) and one positional draw at (x, 0, z)), `minecraft:surface_secondary`
/// at (x, 0, z), each `noise_threshold`'s noise at (x, 0, z), `bandlands`'
/// offset and the freezing field `temperature` reads above its origin. Yet a
/// rule tree asks for them per block — vanilla's overworld asks for the
/// surface depth on every default block it walks, through
/// `above_preliminary_surface`, 38 000 noise samples a chunk for 256 columns.
///
/// EXACT, refusals included. A read that throws (a sample the noise refuses,
/// a depth no int holds) throws at the first block that asks for it, exactly
/// as before, and the exception leaves the call, so nothing is kept from it;
/// a read that returns is kept, and every later block of the column would
/// have computed the same value and returned. What depends on more than the
/// column — the y, a condition's own range or multiplier, and every
/// refusal those can draw — is still computed per block.
///
/// Keyed by its column: a call for another (x, z) forgets everything first,
/// so one memo can be handed every block of a chunk in any order. It belongs
/// to one caller at a time and never to the Executor, which is shared
/// between threads (SPEC §4.1).
class ColumnMemo {
public:
    ColumnMemo() = default;

private:
    friend class Executor;

    /// Binds the memo to the column (@p x, @p z), forgetting what it held if
    /// that is another column.
    void bind(const std::int32_t x, const std::int32_t z) noexcept {
        if (bound_ && x == x_ && z == z_) {
            return;
        }
        x_ = x;
        z_ = z;
        bound_ = true;
        hasDepth_ = false;
        hasSecondary_ = false;
        hasBandOffset_ = false;
        hasFreezingField_ = false;
        std::fill(hasNoise_.begin(), hasNoise_.end(), char{0});
    }

    std::int32_t x_ = 0;
    std::int32_t z_ = 0;
    bool bound_ = false;
    bool hasDepth_ = false;
    bool hasSecondary_ = false;
    bool hasBandOffset_ = false;
    bool hasFreezingField_ = false;
    std::int32_t depth_ = 0;
    double secondary_ = 0.0;
    std::int32_t bandOffset_ = 0;
    float freezingField_ = 0.0F;
    /// One per noise a `noise_threshold` names, by the slot compile() gave
    /// it; sized on the first such read.
    std::vector<double> noise_;
    std::vector<char> hasNoise_;
};

/// A compiled rule tree, immutable and safe to share across threads.
class Executor {
public:
    /// Compiles @p graph for one world seed.
    ///
    /// Throws ExecutionError naming every construct this build cannot run,
    /// with the reason for each — which, per spec/bandlands-spec.md closing
    /// `bandlands`, is nothing left in the schema itself: every rule type and
    /// every condition type runs. What can still make a REAL dimension's
    /// compile fail is a missing input this construct needs at read time
    /// (a `Context` field the caller cannot supply, or a noise `noises`
    /// does not hold) rather than an unsupported construct.
    /// @p noises must hold every entry `graph.referencedNoises()` names, plus
    /// `minecraft:surface` and `minecraft:surface_secondary` if any rule reads
    /// a surface depth, and `minecraft:clay_bands_offset` if any rule uses
    /// `bandlands`. A missing one is an error at compile, naming the
    /// identifier, rather than on the chunk that first reached it.
    ///
    /// It may be null for a tree that needs neither — which is not a special
    /// case worth avoiding: most trees small enough to test by hand are one.
    ///
    /// @p noises also carries the dimension's declared random source, and a
    /// `vertical_gradient` draws its coin from that source: Xoroshiro128++
    /// salted with the MD5 of its `random_name`, or under
    /// `legacy_random_source` java.util.Random salted with its
    /// `String.hashCode` (verticalGradientFires; SPEC §11). With no registry
    /// there is no declared source, and the modern one is used — which is
    /// why ChunkFiller::compile, the path a real dimension takes, always
    /// supplies one.
    [[nodiscard]] static Executor compile(const RuleGraph& graph, std::int64_t worldSeed,
                                          const settings::NoiseGeometry& geometry,
                                          const density::NoiseRegistry* noises = nullptr,
                                          std::int32_t seaLevel = 0);

    /// An Executor keeps POINTERS to the graph, the geometry and the noises,
    /// so all three have to outlive it. Compiling from a temporary graph is
    /// therefore a dangling read, and it is an easy one to write:
    ///
    ///     Executor::compile(RuleGraph::resolve(json, id), seed, geometry)
    ///
    /// reads like it should work. This overload makes it a COMPILE error
    /// instead of a crash at the first `apply`, which is where it was found.
    static Executor compile(RuleGraph&&, std::int64_t, const settings::NoiseGeometry&,
                            const density::NoiseRegistry* = nullptr, std::int32_t = 0) = delete;

    /// The surface depth at a column: an integer, constant down the column,
    /// and a pure function of the world seed and (x, z).
    ///
    ///     (int)(2.75 * surfaceNoise(x, 0, z) + 3.0 + 0.25 * u)
    ///
    /// The cast TRUNCATES toward zero and there is NO BOTTOM CLAMP AT 0 —
    /// measured, not assumed. `max(0, surfaceDepth)` is REFUTED. A clamp at 0
    /// shows up only where the returned value is negative, which the golden
    /// regions never reach (0 of 2097152 columns, every column of all eight),
    /// so for two milestones this was open on a sample ~40x too small to say
    /// anything. Widening the same eight seeds to x, z in [-16384, 16384) —
    /// 8589934592 columns — finds 98 at depth -1, on six of the eight seeds,
    /// 1 in 87652393. Probing three of those clusters at three seeds with a
    /// bare `{ above_preliminary_surface -> diamond_block }` rule over a
    /// solid column puts the band's lower edge at `psl + surfaceDepth - 8` on
    /// all 49 separating columns and never at
    /// `psl + max(0, surfaceDepth) - 8`.
    ///
    /// And exactly that far, because of where the field's tail stops. Every
    /// one of those 49 columns has depth EXACTLY -1, and the lowest raw value
    /// anywhere in the 8589934592 swept columns is -1.134416806, so no column
    /// at depth -2 or below has ever been observed. A clamp at -1 or lower —
    /// `max(-1, surfaceDepth)`, say — predicts the same edge as no clamp at
    /// all on every column this reading contains, and is NOT separated by it.
    /// Nothing here is evidence against one; the engine simply does not apply
    /// one, and if vanilla did, only a column at depth <= -2 could say so.
    /// The numbers are in
    /// `tests/conformance/vanilla_above_preliminary_surface_test.cpp` and the
    /// apparatus is `tools/analysis/aps-clamp-probe.sh`. What IS common is 0
    /// (6745 of the golden columns, about one in 311), and that is not a
    /// curiosity, because `hole` is exactly `depth <= 0`.
    ///
    /// `u` is one `nextDouble` from the world seed's UNSALTED positional
    /// source at (x, 0, z) — fork once, no name, no MD5. That is a different
    /// derivation from `vertical_gradient`'s, which forks, salts with the
    /// MD5 of the name, and forks AGAIN. Mixing the two up is the single
    /// easiest mistake here and it cost this project two sessions.
    [[nodiscard]] std::int32_t surfaceDepth(std::int32_t x, std::int32_t z) const;

    /// The same quantity BEFORE the cast, which is what makes the sentence
    /// above measurable rather than asserted: the distance between the field
    /// and a negative depth is a property of `minecraft:surface`'s own
    /// amplitudes, and only the raw double shows it. Nothing in generation
    /// calls this — it exists for the census that pins it.
    [[nodiscard]] double surfaceDepthRaw(std::int32_t x, std::int32_t z) const;

    /// Whether `minecraft:temperature` fires — that is, whether it is cold
    /// enough to freeze at this block.
    ///
    /// It compares a HEIGHT-ADJUSTED temperature, which is what took the long
    /// time to establish: the flat threshold of 0.30 this project recorded for
    /// two milestones was an artefact of never sweeping height. Above
    /// `sea_level + 17` the biome's temperature is reduced with height, so the
    /// firing boundary moves about eight blocks per 0.01 of temperature.
    ///
    /// Every step after the noise is FLOAT32 and left to right, and neither
    /// half of that is decoration: widening to double, or folding
    /// `* 0.05f / 40.0f` into the algebraically equal `* 0.00125f`, each
    /// disagrees with the server (SPEC §11).
    ///
    /// The noise is a 2D simplex over a permutation from a Java LCG seeded
    /// with the CONSTANT 1234 — no world seed, no salt. So this field is the
    /// same in every world, which is worth knowing before looking for a seed
    /// in it.
    ///
    /// Defined for every sea level: `sea_level + 17` is formed in 64 bits, so
    /// above INT32_MAX - 17 no y is above it. compile() refuses that sea level
    /// for a tree with a temperature condition, so generation never asks.
    [[nodiscard]] bool freezing(std::int32_t x, std::int32_t y, std::int32_t z,
                                float biomeTemperature) const noexcept;

    /// The block the rules place at @p at, or nullptr where they place none —
    /// which is the ordinary case, and means the filler's block stands.
    [[nodiscard]] const settings::BlockState* apply(const Context& at) const;

    /// The same, keeping the column's constant reads in @p memo (ColumnMemo)
    /// for the next block of the column instead of recomputing them. The
    /// same block, and the same refusal at the same block, as apply(at).
    [[nodiscard]] const settings::BlockState* apply(const Context& at, ColumnMemo& memo) const;

    /// @p biome's key for Context::biomeKey (BiomeKey). Every `biome`
    /// condition answers a block carrying it exactly as it answers one whose
    /// Context::biome is @p biome: the answers are worked out at compile, by
    /// the same comparison, for each identifier the tree names and once for
    /// all the identifiers it does not. Never throws for any identifier.
    [[nodiscard]] BiomeKey biomeKey(const data::ResourceLocation& biome) const;

    /// The block `bandlands` places at @p x, @p y, @p z: one entry of this
    /// dimension's clay-bands table, indexed by a per-column noise-derived
    /// shift (spec/bandlands-spec.md Q4). Throws ExecutionError if @p x/@p z's
    /// index falls outside the table — vanilla's own unguarded index
    /// arithmetic can do this for extreme y (Q5.1), and this reproduces that
    /// exactly rather than clamping it into something vanilla never places.
    [[nodiscard]] const settings::BlockState& bandlandsAt(std::int32_t x, std::int32_t y,
                                                          std::int32_t z) const;

    /// One entry of this dimension's clay-bands table, built once at compile
    /// from the world seed (spec/bandlands-spec.md Q2-Q3). Exposed directly
    /// so the table's construction can be checked against the server's own
    /// array without going through bandlandsAt()'s noise-driven read path.
    /// Throws ExecutionError if this tree never names `bandlands` — the
    /// table is only ever built when something will read it.
    [[nodiscard]] const settings::BlockState& clayBandAt(std::size_t index) const;

private:
    Executor(const RuleGraph& graph, const settings::NoiseGeometry& geometry,
             const density::NoiseRegistry* noises, std::int64_t worldSeed, std::int32_t seaLevel,
             noise::PerlinNoise temperature)
        : graph_(&graph), geometry_(&geometry), noises_(noises), seaLevel_(seaLevel),
          temperature_(temperature), jitter_(rng::XoroshiroPositionalFactory{worldSeed}.base()) {}

    [[nodiscard]] const settings::BlockState* runRule(RuleIndex index, const Context& at,
                                                      ColumnMemo& memo) const;
    [[nodiscard]] bool test(ConditionIndex index, const Context& at, ColumnMemo& memo) const;
    [[nodiscard]] std::int32_t depthFor(const Condition& condition, const Context& at,
                                        ColumnMemo& memo) const;

    /// The column reads ColumnMemo keeps, each computed exactly as its
    /// uncached form and kept only once it has returned. @p memo is bound
    /// to (at.x, at.z).
    [[nodiscard]] std::int32_t columnSurfaceDepth(const Context& at, ColumnMemo& memo) const;
    [[nodiscard]] double columnSecondary(const Context& at, ColumnMemo& memo) const;
    [[nodiscard]] double columnNoise(const data::ResourceLocation& name, std::size_t slot,
                                     const Context& at, ColumnMemo& memo) const;
    [[nodiscard]] const settings::BlockState& bandlandsAt(const Context& at,
                                                          ColumnMemo& memo) const;
    /// `bandlands`' table entry at @p y for a column shifted by @p offset.
    [[nodiscard]] const settings::BlockState& bandAt(std::int32_t y, std::int32_t offset) const;
    [[nodiscard]] float freezingField(std::int32_t x, std::int32_t z) const noexcept;
    [[nodiscard]] bool freezing(const Context& at, ColumnMemo& memo) const noexcept;
    /// The `biome` condition @p condition, whose answers are column
    /// @p biomeSlot of biomeAnswers_, at @p at: from at.biomeKey where it is
    /// this executor's, by name otherwise.
    [[nodiscard]] bool biomeMatches(const Condition& condition, std::size_t biomeSlot,
                                    const Context& at) const;

    /// This executor's identity, which its BiomeKeys carry: one per compile()
    /// and never 0, so no key another compile issued is ever taken for one of
    /// this one's. A copy keeps it, and answers every key alike.
    std::uint64_t id_ = 0;

    const RuleGraph* graph_;
    const settings::NoiseGeometry* geometry_;
    const density::NoiseRegistry* noises_;
    std::int32_t seaLevel_ = 0;
    /// Seeded from the constant 1234, so it is built once and never varies.
    noise::PerlinNoise temperature_;
    /// The unsalted positional source the surface depth's jitter comes from.
    /// Xoroshiro whatever the dimension declares, and unreachable under
    /// `legacy_random_source`: a depth needs `minecraft:surface`, which a
    /// legacy registry refuses to build (NoiseRegistry::create), so a legacy
    /// tree that reads one never compiles.
    rng::PositionalSource jitter_;
    /// Resolved once at compile, so no lookup happens per block.
    const noise::NormalNoise* surface_ = nullptr;
    const noise::NormalNoise* surfaceSecondary_ = nullptr;
    /// One positional source per `random_name`, of the kind the registry's
    /// declared random source calls for, built at compile so that running a
    /// rule costs no hashing and no forking.
    using GradientSource = std::variant<rng::PositionalSource, rng::LegacyPositionalSource>;
    std::vector<GradientSource> gradientSources_;

    /// What a condition's test reads that is fixed once the tree is
    /// compiled, worked out at compile instead of at every block: its
    /// anchors resolved against the geometry (compile() has refused any that
    /// do not resolve), its gradient's source found by name, its noise's
    /// ColumnMemo slot and its `surface_type` read. By condition index; each
    /// field is meaningful only for the type that reads it.
    struct CompiledCondition {
        /// `y_above`'s anchor.
        std::int32_t anchor = 0;
        /// `vertical_gradient`'s two anchors.
        std::int32_t trueAtAndBelow = 0;
        std::int32_t falseAtAndAbove = 0;
        /// `vertical_gradient`'s source in gradientSources_, or kNoSource
        /// for a name with none (which test() refuses, as it always did).
        std::size_t gradient = kNoSource;
        /// `noise_threshold`'s slot in a ColumnMemo: one per distinct noise
        /// name, so two conditions over one noise read it once a column.
        std::size_t noiseSlot = 0;
        /// `stone_depth`'s `surface_type` is "ceiling".
        bool ceiling = false;
        /// `biome`'s column in biomeAnswers_: one per biome condition.
        std::size_t biomeSlot = 0;
    };

    static constexpr std::size_t kNoSource = static_cast<std::size_t>(-1);

    std::vector<CompiledCondition> compiled_;
    std::size_t noiseSlots_ = 0;

    /// What biomeKey() issues and the `biome` conditions read back. Every
    /// identifier any `biome` condition lists has its own index, in the
    /// order of the map, and every other identifier shares the one past
    /// them (biomeKeys_ - 1), since no condition matches any of those.
    /// biomeAnswers_[index * biomeSlots_ + slot] is condition `slot`'s answer
    /// for the biome at `index`, found at compile by the very comparison a
    /// keyless block makes.
    std::map<data::ResourceLocation, std::uint32_t> biomeIndex_;
    std::vector<char> biomeAnswers_;
    std::size_t biomeSlots_ = 0;
    std::uint32_t biomeKeys_ = 0;

    /// The graph's rules and conditions, by index, read without the graph's
    /// range check: compile() takes every index the tree holds from the
    /// graph itself. An index past them goes through the graph's own checked
    /// accessor, which refuses it as it always did.
    std::vector<const Rule*> rules_;
    std::vector<const Condition*> conditions_;

    [[nodiscard]] const Rule& ruleAt(const RuleIndex index) const {
        return index < rules_.size() ? *rules_[index] : graph_->rule(index);
    }

    [[nodiscard]] const Condition& conditionAt(const ConditionIndex index) const {
        return index < conditions_.size() ? *conditions_[index] : graph_->condition(index);
    }

    /// The rule tree as a straight list, run by apply() instead of runRule's
    /// recursion: the same tests in the same order, the same block placed.
    ///
    /// The walk runs a sequence's children in order and returns the first
    /// block one places; a condition returns nothing where its test fails,
    /// and its rule's answer otherwise. So a placed block always leaves the
    /// whole walk, and "nothing" always continues with what follows: a
    /// sequence's code is its children's code in order, and a condition's is
    /// a test that jumps past its rule's code when it fails. A `not` chain
    /// is the condition it ends in, tested and negated once per `not` —
    /// exactly what the walk computes, which evaluates nothing besides the
    /// condition at its end. Built only from a tree whose every index is in
    /// range and every rule type runnable, and under kMaxProgram ops; any
    /// other tree is walked, as it always was.
    struct ProgramOp {
        enum class Code : std::uint8_t {
            /// test(condition), negated when `negate`; on false, go to `jump`.
            Test,
            /// Return `block`.
            Place,
            /// Return bandlandsAt's block.
            Bandlands,
            /// Return nothing: the root placed nothing.
            Nothing,
        };
        Code code = Code::Nothing;
        bool negate = false;
        ConditionIndex condition = 0;
        std::uint32_t jump = 0;
        const settings::BlockState* block = nullptr;
    };

    static constexpr std::size_t kMaxProgram = std::size_t{1} << 16;

    /// Empty where the tree is walked instead (see ProgramOp).
    std::vector<ProgramOp> program_;

    /// Appends @p index's code to program_; false where it cannot be built.
    [[nodiscard]] bool lowerRule(const RuleGraph& graph, RuleIndex index, std::size_t depth);

    [[nodiscard]] const settings::BlockState* runProgram(const Context& at, ColumnMemo& memo) const;

    /// `bandlands`' own state: the table, built once (empty/default-filled
    /// until clayBandsBuilt_ says otherwise), and the noise its per-column
    /// shift reads. Built only when the tree names `bandlands` at all.
    bool clayBandsBuilt_ = false;
    std::array<settings::BlockState, kClayBandsSize> clayBands_{};
    const noise::NormalNoise* clayBandsOffset_ = nullptr;
};

} // namespace stratum::surface
