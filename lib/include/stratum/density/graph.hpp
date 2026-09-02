// Stratum — resolved density function graphs.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Turns the JSON of a pack's density functions into one immutable graph:
// references resolved, inline definitions flattened, splines built, cycles
// refused. Nothing here evaluates anything — that is the next step (SPEC
// §4.1 allows an interpreter as the M2 stepping stone before the compiled
// program in M3).
//
// The node set and every field name below were derived from vanilla's own
// 35 density functions rather than from memory, which is how `shift_a`'s
// `argument` turned out to be a *noise* identifier while `abs`'s is a nested
// function. A type this build does not implement is refused by name (SPEC
// §8): the alternative is a graph that silently means something else.

#pragma once

#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace stratum::density {

/// Raised when a pack's density functions cannot be resolved. The message
/// carries the chain of identifiers that led to the problem, because "cycle
/// detected" without the path is not actionable in a graph this size.
class ResolveError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class NodeType : std::uint8_t {
    Constant,
    Add,
    Mul,
    Min,
    Max,
    Abs,
    Cube,
    HalfNegative,
    QuarterNegative,
    Clamp,
    RangeChoice,
    Noise,
    ShiftedNoise,
    ShiftA,
    ShiftB,
    BlendAlpha,
    BlendOffset,
    OldBlendedNoise,
    FlatCache,
    Cache2d,
    CacheOnce,
    Interpolated,
    Spline,
    YClampedGradient,
    WeirdScaledSampler,
    EndIslands,
};

/// "minecraft:shifted_noise".
[[nodiscard]] std::string_view nodeTypeName(NodeType type) noexcept;
[[nodiscard]] std::optional<NodeType> nodeTypeFromName(std::string_view name) noexcept;

using NodeIndex = std::uint32_t;
using SplineIndex = std::uint32_t;

/// One point of a cubic spline. Its value is either a constant or another
/// spline; exactly one of the two is set.
struct SplinePoint {
    double location = 0.0;
    double derivative = 0.0;
    std::optional<double> value;
    std::optional<SplineIndex> nested;
};

struct SplineDefinition {
    /// The density function the spline is sampled along.
    NodeIndex coordinate = 0;
    std::vector<SplinePoint> points;
};

/// A resolved node. Which of these fields carry meaning depends on the type,
/// and the order of `arguments` and `parameters` is the order the schema
/// declares them in — see fieldsOf().
struct Node {
    NodeType type = NodeType::Constant;
    std::vector<NodeIndex> arguments;
    std::vector<double> parameters;
    std::optional<data::ResourceLocation> noise;
    std::optional<SplineIndex> spline;
    std::string selector;
};

/// What a node type is made of, in declaration order.
enum class FieldKind : std::uint8_t {
    Function, ///< a nested density function: number, reference or inline
    Number,   ///< a plain number
    NoiseRef, ///< an identifier naming a worldgen/noise entry
    Selector, ///< a fixed string, such as rarity_value_mapper
    Spline,   ///< a spline definition
};

struct Field {
    std::string_view name;
    FieldKind kind;
};

/// The fields a node type takes, in the order vanilla declares them.
[[nodiscard]] std::vector<Field> fieldsOf(NodeType type);

class Graph {
public:
    /// Resolves every density function in @p pack, so that a cycle or a
    /// dangling reference anywhere is found at load rather than on the first
    /// chunk that happens to reach it.
    [[nodiscard]] static Graph resolveAll(const data::Pack& pack);

    [[nodiscard]] const Node& node(NodeIndex index) const;
    [[nodiscard]] const SplineDefinition& spline(SplineIndex index) const;

    /// The node a named density function resolves to. Throws if the pack has
    /// no such entry.
    [[nodiscard]] NodeIndex rootOf(const data::ResourceLocation& id) const;
    [[nodiscard]] bool contains(const data::ResourceLocation& id) const;

    [[nodiscard]] const std::map<data::ResourceLocation, NodeIndex>& roots() const noexcept {
        return roots_;
    }

    [[nodiscard]] std::size_t nodeCount() const noexcept { return nodes_.size(); }

    [[nodiscard]] std::size_t splineCount() const noexcept { return splines_.size(); }

    /// Every noise identifier the graph references, so a caller can check
    /// they all exist before generation begins.
    [[nodiscard]] std::vector<data::ResourceLocation> referencedNoises() const;

private:
    friend class Resolver;

    std::vector<Node> nodes_;
    std::vector<SplineDefinition> splines_;
    std::map<data::ResourceLocation, NodeIndex> roots_;
};

} // namespace stratum::density
