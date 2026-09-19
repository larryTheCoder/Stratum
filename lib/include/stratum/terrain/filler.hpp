// Stratum — turning a density field into blocks.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Everything before this file answers "what is the density here". This one
// answers "what block is here", which is a different question with a much
// shorter answer — and a longer list of things that are allowed to change it.
//
// WHAT THIS DOES. For every block of a chunk: the dimension's `default_block`
// where `final_density` is positive; below that, where `aquifers_enabled` is
// set, the aquifer's own decision (`aquifer::computeSubstance` — the cell
// lattice, the fluid level rule, source selection and the three-source
// barrier, SPEC §10 milestone MA); otherwise the old shortcut, `default_fluid`
// at or below `sea_level` and air above it. Then, over solid ground only and
// where `ore_veins_enabled` and `aquifers_enabled` are BOTH set, the vein
// system's own replacement (`ore::VeinSource` — SPEC §10 milestone M3).
//
// ORE VEINS, wired (SPEC §11). The RNG derivation is confirmed per block
// against the vanilla server on 79790 of 79790 candidate positions across 11
// CONTRIBUTING seeds, of which 43065 are held-out worlds generated after the
// derivation was fixed. 100.000%, on every seed alone and on copper and iron
// alone. Separately, a PLACEMENT probe whose density crosses zero inside both
// vein ranges agrees on its 11455 solid candidates (its other 25509 are air).
// Those 11455 are seed-100 positions already among the 79790, re-measured at
// a different density — evidence about placement, not additional independent
// confirmation of the derivation, so the two are not summed.
// `ore/vein.hpp` carries the derivation, the brackets around each threshold,
// and the two ambiguities the probes cannot separate.
//
// The guard here is its own measured fact rather than a reading of the
// derivation: veins replace only a block this filler has ALREADY made solid.
// On the sign-varying probe the confirmed chain would have placed 17813 vein
// blocks at positions the server left as air, and the server placed none of
// them — while every position it left solid came back exact, including the
// ones the aquifer's own barrier turned solid against a negative density.
//
// The SECOND pass then has to leave those blocks alone, which is its own
// measured fact: on a probe running an unconditional surface rule, all 12934
// vein blocks survived and all 5548 non-vein candidate positions were
// repainted. Without that guard the overworld's own `deepslate` rule —
// unconditionally true below y = -8 — would erase every iron vein in the
// world, iron's whole range being [-60, -8], and every golden test here would
// have stayed green while it happened, since they all run with veins off.
//
// WHAT THIS DOES NOT DO, and refuses rather than approximating (SPEC §8):
//
//   * **Two narrow pieces of the aquifer**, carried rather than guessed
//     (`aquifer/substance.hpp`'s own header has the numbers): Q6.3's
//     water-over-lava exception, and Pi's mixed-fluid-type branch. Measured
//     against a real, aquifer-on overworld region
//     (`golden_fill_aquifer_test.cpp`): EXACT on 393216 of 393216 blocks'
//     category, before any surface rule runs, on the four chunks that test
//     pins; over a wider 64-chunk sweep the residual — everything the two
//     gaps above together could plausibly explain — is 733 of 6291456.
//   * **Surface rules.** These are not refused, because they only ever
//     REPLACE blocks this filler has already placed — a column filled without
//     them is the same column with stone where grass, dirt, gravel, deepslate
//     and bedrock should be. That is a visible, honest partial result rather
//     than a wrong one.
//
//     `compile` now runs them wherever it can: pass a resolved
//     `surface::RuleGraph` and, for a tree that reads `biome`, the
//     `biome::ParameterList` that resolves one. A tree with one unrunnable
//     construct is refused whole, same as `surface::Executor` itself refuses
//     it — `runsSurfaceRules()` says whether this dimension's did, and
//     `surfaceRulesBlockedBy()` says why not when it didn't. Either way
//     `fill` still produces the honest partial result above; nothing about
//     passing a rule graph in can make a column come out wrong instead of
//     merely incomplete.
#pragma once

#include <stratum/aquifer/lattice.hpp>
#include <stratum/biome/parameter_list.hpp>
#include <stratum/biome/temperature_table.hpp>
#include <stratum/density/graph.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/ore/vein.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/executor.hpp>
#include <stratum/surface/rule_graph.hpp>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace stratum::terrain {

/// Raised when a dimension cannot be filled, naming what stopped it.
class FillError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// One chunk's blocks: 16 by 16 columns over the dimension's own height.
///
/// Palette-backed, because a filled chunk holds three distinct block states
/// and storing 98304 copies of a resource location and its properties would
/// cost more than the terrain did.
class ChunkBuffer {
public:
    explicit ChunkBuffer(const settings::NoiseGeometry& geometry);

    [[nodiscard]] std::int32_t minY() const noexcept { return minY_; }

    [[nodiscard]] std::int32_t height() const noexcept { return height_; }

    /// Throws FillError for a position outside the chunk or the dimension,
    /// rather than reading whatever is next to it in memory.
    [[nodiscard]] const settings::BlockState& at(int localX, std::int32_t y, int localZ) const;

    void set(int localX, std::int32_t y, int localZ, const settings::BlockState& block);

    /// How many distinct block states this chunk holds.
    [[nodiscard]] std::size_t paletteSize() const noexcept { return palette_.size(); }

    /// The distinct block states, for a caller translating each once rather
    /// than once per block. Entry 0 is always air: every buffer starts there.
    [[nodiscard]] const std::vector<settings::BlockState>& palette() const noexcept {
        return palette_;
    }

    /// Which `palette()` entry a position holds; `palette()[paletteIndexAt(
    /// ...)]` is `at(...)`. Throws FillError like `at`.
    [[nodiscard]] std::uint16_t paletteIndexAt(int localX, std::int32_t y, int localZ) const;

private:
    [[nodiscard]] std::size_t indexOf(int localX, std::int32_t y, int localZ) const;
    [[nodiscard]] std::uint16_t intern(const settings::BlockState& block);

    std::int32_t minY_ = 0;
    std::int32_t height_ = 0;
    std::vector<settings::BlockState> palette_;
    std::vector<std::uint16_t> blocks_;
};

/// A dimension's terrain, ready to fill chunks from.
///
/// Immutable once compiled and safe to share between threads; the mutable
/// part of generating is the CornerCache and the ChunkBuffer, both of which
/// belong to the calling task (SPEC §4.1).
class ChunkFiller {
public:
    /// Throws FillError, naming what is missing, for a dimension this build
    /// cannot fill — including a router entry the wiring needs but that this
    /// dimension's graph cannot evaluate.
    ///
    /// @p surfaceRules, @p biomeParameters and @p biomeTemperatures are all
    /// optional and all external: like @p graph and @p noises, whatever they
    /// point to must outlive this ChunkFiller. Passing none of them
    /// reproduces exactly the behaviour before surface rules existed here —
    /// bare stone, fluid and air. Passing @p surfaceRules without
    /// @p biomeParameters is fine for a tree that never reads `biome` or
    /// `temperature`; passing @p biomeParameters without @p biomeTemperatures
    /// is fine for one that reads `biome` but never `temperature`, since
    /// `temperature` is the only construct that needs a biome's own
    /// DECLARED value rather than just its identity. Any of these missing
    /// for a tree that names the construct is treated the same as an
    /// unrunnable construct rather than run with a missing Context field
    /// (see `runsSurfaceRules()`).
    [[nodiscard]] static ChunkFiller
    compile(const density::Graph& graph, const density::NoiseRegistry& noises,
            const settings::NoiseSettings& settings,
            const surface::RuleGraph* surfaceRules = nullptr,
            const biome::ParameterList* biomeParameters = nullptr,
            const biome::TemperatureTable* biomeTemperatures = nullptr);

    /// Fills @p into with the chunk at chunk coordinates @p chunkX, @p chunkZ.
    ///
    /// Blocks are visited cell by cell rather than column by column, so the
    /// eight cell corners `interpolated` needs are computed once and reused
    /// across the 128 blocks of a cell instead of once per block. Measured,
    /// that is the difference between 39.7 and 0.46 seconds a chunk.
    ///
    /// When `runsSurfaceRules()` is true, a second pass then walks every
    /// column top to bottom and bottom to top — for the stone-depth runs and
    /// the latched water height `surface::Context` needs — and asks the
    /// compiled `surface::Executor` what replaces what it just placed.
    void fill(std::int32_t chunkX, std::int32_t chunkZ, ChunkBuffer& into) const;

    [[nodiscard]] const settings::NoiseSettings& settings() const noexcept { return *settings_; }

    /// Whether this dimension's surface rules actually run. False is not an
    /// error and not silent: a column filled without them is bare stone
    /// where grass, dirt, gravel, deepslate and bedrock belong — visibly
    /// incomplete rather than the wrong-but-plausible world SPEC §8's
    /// severe class is about. surfaceRulesBlockedBy() says why, when this is
    /// false and a surfaceRules graph was given at all.
    [[nodiscard]] bool runsSurfaceRules() const noexcept { return surfaceExecutor_.has_value(); }

    /// Every construct blocking this dimension's surface rules, by name.
    /// Mostly `surface::RuleGraph::unrunnable()`, verbatim; a tree that reads
    /// `biome` or the biome's own declared `temperature` without the data
    /// this build needs for either is appended the same way, since from a
    /// caller's chair "the construct runs, but nothing here supplies it" is
    /// the same outcome as "the construct does not run yet". Empty when
    /// runsSurfaceRules() is true, or when compile() was given no
    /// surfaceRules at all.
    [[nodiscard]] const std::vector<std::string>& surfaceRulesBlockedBy() const noexcept {
        return surfaceRulesBlockedBy_;
    }

    /// The horizontal pitch the router's `preliminary_surface_level` is
    /// SAMPLED on before it reaches `above_preliminary_surface` — measured,
    /// and not a column (SPEC §11). 16 blocks, anchored at the world origin
    /// through floorDiv (so the lattice does not fold at x = 0), which is the
    /// same lattice as every chunk's own four corners.
    static constexpr std::int32_t kPreliminarySurfacePitch = 16;

    /// The value that reaches the condition at a column @p offsetX, @p offsetZ
    /// blocks into a lattice cell whose four RAW samples are @p corners,
    /// ordered (x0,z0), (x1,z0), (x0,z1), (x1,z1).
    ///
    /// The floor falls TWICE, and both places are measured rather than
    /// chosen. Each lattice sample is floored where it is taken; the four
    /// integers are then blended linearly in x and z; the blend is floored
    /// again. `tools/analysis/psl-lattice-probe.sh`'s `f_*` family is the
    /// only probe of this entry whose arms are not integers, and so the only
    /// one that could ever have separated these. With arms -0.5 and +0.5, at
    /// the measured pitch and anchor, this reading scores 36864 of 36864
    /// columns and 65536 of 65536 at negative coordinates, while flooring
    /// only after the blend scores 20423 and 34387, truncating at the sample
    /// 3119, truncating after the blend 4764, rounding after it 22010,
    /// quantising to the cell's LOWER corner 21039 and to the NEAREST of the
    /// four corners 21103. On vanilla's own router the difference is
    /// invisible — `find_top_surface` returns whole multiples of its cell
    /// height — so this is a data-pack-only distinction, and it is measured.
    [[nodiscard]] static std::int32_t preliminarySurfaceIn(const std::array<double, 4>& corners,
                                                           std::int32_t offsetX,
                                                           std::int32_t offsetZ) noexcept {
        const double u =
            static_cast<double>(offsetX) / static_cast<double>(kPreliminarySurfacePitch);
        const double v =
            static_cast<double>(offsetZ) / static_cast<double>(kPreliminarySurfacePitch);
        const double low =
            std::floor(corners[0]) + ((std::floor(corners[1]) - std::floor(corners[0])) * u);
        const double high =
            std::floor(corners[2]) + ((std::floor(corners[3]) - std::floor(corners[2])) * u);
        return static_cast<std::int32_t>(std::floor(low + ((high - low) * v)));
    }

private:
    ChunkFiller(const density::Graph& graph, const density::NoiseRegistry& noises,
                const settings::NoiseSettings& settings);

    void applySurfaceRules(std::int32_t chunkX, std::int32_t chunkZ, ChunkBuffer& into) const;

    const settings::NoiseSettings* settings_;
    density::Interpreter interpreter_;
    density::NodeIndex finalDensity_{};

    const biome::ParameterList* biomeParameters_ = nullptr;
    const biome::TemperatureTable* biomeTemperatures_ = nullptr;
    bool surfaceNeedsBiome_ = false;
    bool surfaceNeedsTemperature_ = false;
    bool surfaceNeedsPreliminarySurface_ = false;
    bool surfaceNeedsSteep_ = false;
    std::optional<surface::Executor> surfaceExecutor_;
    std::vector<std::string> surfaceRulesBlockedBy_;

    // Present only when BOTH ore_veins_enabled and aquifers_enabled are
    // set (ore::veinsPlaceBlocks): the coupling is measured, not assumed.
    // Built at compile() from the registry's own worldSeed, same as the
    // aquifer's centres below and for the same reason.
    std::optional<ore::VeinSource> oreVeins_;

    // Present only when settings_->aquifersEnabled — its own RNG derivation
    // is per-world (the salted positional source, SPEC §4), so it is built
    // once at compile() from the registry's own worldSeed() rather than
    // re-derived per block.
    std::optional<aquifer::CentreSource> aquiferCentres_;
};

} // namespace stratum::terrain
