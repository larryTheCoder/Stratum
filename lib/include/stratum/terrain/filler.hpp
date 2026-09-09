// Stratum — turning a density field into blocks.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Everything before this file answers "what is the density here". This one
// answers "what block is here", which is a different question with a much
// shorter answer — and a longer list of things that are allowed to change it.
//
// WHAT THIS DOES. For every block of a chunk: the dimension's `default_block`
// where `final_density` is positive, its `default_fluid` at or below
// `sea_level` where it is not, and air above that. That is the whole rule.
//
// WHAT THIS DOES NOT DO, and refuses rather than approximating (SPEC §8):
//
//   * **Aquifers.** Where they run, the substance is not a function of the
//     density at all: an aquifer decides a local fluid level, drains what is
//     above it, and places stone BARRIERS between bodies of water at
//     different levels. Measured on one golden seed, that is 1.12% of blocks
//     — 81% of them water becoming air. A filler that ignored the flag would
//     flood every cave in the world and call it terrain, so `compile` refuses
//     a dimension with `aquifers_enabled` by name.
//   * **Ore veins.** Same reasoning, smaller effect.
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

#include <stratum/biome/parameter_list.hpp>
#include <stratum/density/graph.hpp>
#include <stratum/density/interpreter.hpp>
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
    /// Throws FillError, naming the flag, for a dimension whose blocks are
    /// not a function of the density alone.
    ///
    /// @p surfaceRules and @p biomeParameters are both optional and both
    /// external: like @p graph and @p noises, whatever they point to must
    /// outlive this ChunkFiller. Passing neither reproduces exactly the
    /// behaviour before surface rules existed here — bare stone, fluid and
    /// air. Passing @p surfaceRules without @p biomeParameters is fine for a
    /// tree that never reads `biome`; for one that does, it is treated the
    /// same as an unrunnable construct rather than run with a missing
    /// Context field (see `runsSurfaceRules()`).
    [[nodiscard]] static ChunkFiller compile(const density::Graph& graph,
                                             const density::NoiseRegistry& noises,
                                             const settings::NoiseSettings& settings,
                                             const surface::RuleGraph* surfaceRules = nullptr,
                                             const biome::ParameterList* biomeParameters = nullptr);

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

private:
    ChunkFiller(const density::Graph& graph, const density::NoiseRegistry& noises,
                const settings::NoiseSettings& settings);

    void applySurfaceRules(std::int32_t chunkX, std::int32_t chunkZ, ChunkBuffer& into) const;

    const settings::NoiseSettings* settings_;
    density::Interpreter interpreter_;
    density::NodeIndex finalDensity_{};

    const biome::ParameterList* biomeParameters_ = nullptr;
    bool surfaceNeedsBiome_ = false;
    bool surfaceNeedsPreliminarySurface_ = false;
    bool surfaceNeedsSteep_ = false;
    std::optional<surface::Executor> surfaceExecutor_;
    std::vector<std::string> surfaceRulesBlockedBy_;
};

} // namespace stratum::terrain
