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
#include <vector>

namespace stratum::surface {

/// The length of `bandlands`' colour table — a literal in vanilla's own
/// source, not derived from any pack parameter (spec/bandlands-spec.md Q1.1).
inline constexpr std::size_t kClayBandsSize = 192;

/// Raised when a rule tree cannot be executed, naming what stopped it.
class ExecutionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
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

    /// That biome's declared temperature, for `temperature`. A float32 on
    /// purpose: vanilla holds it as one and the whole comparison is done in
    /// single precision, so widening it here would change the answer.
    float biomeTemperature = 0.0F;

    /// How deep this block sits in its run of solid, counting DOWN from the
    /// world top: 1 at the top of a run, reset by air, and — measured, not
    /// assumed — left UNCHANGED by fluid, which neither breaks a run nor
    /// counts toward it. Uncapped.
    std::int32_t stoneDepthAbove = 1;
    /// The same counting up from the world floor, for `surface_type: ceiling`.
    std::int32_t stoneDepthBelow = 1;

    /// The column's water height: one above the FIRST fluid block met
    /// descending, latched and never updated again. Absent where the column
    /// holds no fluid at all, which makes `water` unconditionally true.
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
/// Public for the same reason requiredNoises() is: a caller has to know
/// BEFORE compiling. This list is not a list of noises and never passes
/// through NoiseRegistry, so a dimension whose only draw on its declared
/// random source is a gradient looks, to the registry, like a dimension that
/// needs nothing. Under `legacy_random_source` that is exactly the case this
/// build cannot derive, so the filler refuses it by name (SPEC §11).
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
    [[nodiscard]] bool freezing(std::int32_t x, std::int32_t y, std::int32_t z,
                                float biomeTemperature) const noexcept;

    /// The block the rules place at @p at, or nullptr where they place none —
    /// which is the ordinary case, and means the filler's block stands.
    [[nodiscard]] const settings::BlockState* apply(const Context& at) const;

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

    [[nodiscard]] const settings::BlockState* runRule(RuleIndex index, const Context& at) const;
    [[nodiscard]] bool test(ConditionIndex index, const Context& at) const;
    [[nodiscard]] std::int32_t depthFor(const Condition& condition, const Context& at) const;

    const RuleGraph* graph_;
    const settings::NoiseGeometry* geometry_;
    const density::NoiseRegistry* noises_;
    std::int32_t seaLevel_ = 0;
    /// Seeded from the constant 1234, so it is built once and never varies.
    noise::PerlinNoise temperature_;
    /// The unsalted positional source the surface depth's jitter comes from.
    rng::PositionalSource jitter_;
    /// Resolved once at compile, so no lookup happens per block.
    const noise::NormalNoise* surface_ = nullptr;
    const noise::NormalNoise* surfaceSecondary_ = nullptr;
    /// One positional source per `random_name`, built at compile so that
    /// running a rule costs no MD5 and no forking.
    std::map<std::string, rng::PositionalSource> gradients_;

    /// `bandlands`' own state: the table, built once (empty/default-filled
    /// until clayBandsBuilt_ says otherwise), and the noise its per-column
    /// shift reads. Built only when the tree names `bandlands` at all.
    bool clayBandsBuilt_ = false;
    std::array<settings::BlockState, kClayBandsSize> clayBands_{};
    const noise::NormalNoise* clayBandsOffset_ = nullptr;
};

} // namespace stratum::surface
