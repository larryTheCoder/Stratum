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
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace stratum::surface {

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

    /// The column's preliminary surface level, for `above_preliminary_surface`.
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
    /// with the reason for each. Vanilla's overworld is refused today: nine of
    /// its fifteen condition types are still unsettled (SPEC §11).
    /// @p noises must hold every entry `graph.referencedNoises()` names, plus
    /// `minecraft:surface` and `minecraft:surface_secondary` if any rule reads
    /// a surface depth. A missing one is an error at compile, naming the
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
    /// The cast TRUNCATES toward zero and there is no clamp, so the value
    /// reaches -1 on about one column in 22000 with vanilla's parameters —
    /// which is not a curiosity, because `hole` is exactly `depth <= 0`.
    ///
    /// `u` is one `nextDouble` from the world seed's UNSALTED positional
    /// source at (x, 0, z) — fork once, no name, no MD5. That is a different
    /// derivation from `vertical_gradient`'s, which forks, salts with the
    /// MD5 of the name, and forks AGAIN. Mixing the two up is the single
    /// easiest mistake here and it cost this project two sessions.
    [[nodiscard]] std::int32_t surfaceDepth(std::int32_t x, std::int32_t z) const;

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
};

} // namespace stratum::surface
