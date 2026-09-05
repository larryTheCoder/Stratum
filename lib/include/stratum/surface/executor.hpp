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
#include <stratum/rng/xoroshiro128.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/rule_graph.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>

namespace stratum::surface {

/// Raised when a rule tree cannot be executed, naming what stopped it.
class ExecutionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// What the rules may ask about one block.
struct Context {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;

    /// The column's preliminary surface level, for `above_preliminary_surface`.
    std::int32_t preliminarySurface = 0;

    /// The biome at this position, for `biome` and `temperature`. Absent means
    /// the caller does not carry one — which is only reachable for a tree that
    /// names neither, since a tree that did would have been refused.
    std::optional<data::ResourceLocation> biome;
};

/// A compiled rule tree, immutable and safe to share across threads.
class Executor {
public:
    /// Compiles @p graph for one world seed.
    ///
    /// Throws ExecutionError naming every construct this build cannot run,
    /// with the reason for each. Vanilla's overworld is refused today: nine of
    /// its fifteen condition types are still unsettled (SPEC §11).
    [[nodiscard]] static Executor compile(const RuleGraph& graph, std::int64_t worldSeed,
                                          const settings::NoiseGeometry& geometry);

    /// The block the rules place at @p at, or nullptr where they place none —
    /// which is the ordinary case, and means the filler's block stands.
    [[nodiscard]] const settings::BlockState* apply(const Context& at) const;

private:
    Executor(const RuleGraph& graph, const settings::NoiseGeometry& geometry)
        : graph_(&graph), geometry_(&geometry) {}

    [[nodiscard]] const settings::BlockState* runRule(RuleIndex index, const Context& at) const;
    [[nodiscard]] bool test(ConditionIndex index, const Context& at) const;

    const RuleGraph* graph_;
    const settings::NoiseGeometry* geometry_;
    /// One positional source per `random_name`, built at compile so that
    /// running a rule costs no MD5 and no forking.
    std::map<std::string, rng::PositionalSource> gradients_;
};

} // namespace stratum::surface
