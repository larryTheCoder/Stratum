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
#include <stratum/density/noise_parameters.hpp>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
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

/// A ResolveError for a graph whose numbers lie outside what this build
/// accepts (SPEC §8) — a node's parameter, or a depth past kMaxDepth — as
/// against one that is malformed. A frozen blob tells the two apart: builds
/// from before those refusals froze such graphs at the same pipeline engine
/// version, so what to do about one differs (SPEC §6).
class OutOfRangeError : public ResolveError {
public:
    using ResolveError::ResolveError;
};

/// The deepest graph this build resolves or evaluates, counted in nodes and
/// splines (SPEC §8, "Inputs that would reach undefined behaviour").
///
/// A node's DEPTH is one more than the deepest of its arguments and its
/// spline; a spline's is one more than the deeper of its coordinate and its
/// nested splines. So a `constant` is 1, `abs(constant)` is 2, and a named
/// function is as deep where it is referenced as where it is defined. The
/// interpreter recurses once per level of that, and the resolver once per
/// level of JSON and per reference it follows, so a graph deep enough runs
/// the C++ stack out — undefined behaviour, not an error. Twenty thousand
/// levels of `abs` did, in the resolver and, through a resealed blob, in the
/// interpreter.
///
/// Sized by the stack, which is what it protects. Those recursions keep thin
/// frames (density_graph.cpp, density_interpreter.cpp), and at 256 levels
/// resolving and then evaluating a graph ran in 320 KiB of stack in the
/// sanitize preset's build (Clang, unoptimised, UBSan, where every local and
/// temporary has a slot of its own) and in 224 KiB in GCC's unoptimised
/// build; a chain of 256 `interpolated`, the deepest frame per level, in 640
/// and 320 KiB. That is inside the 1 MiB a Windows main thread or a JVM
/// thread gets by default; before the frames were thinned a level took about
/// 6 KiB there, and 512 levels did not fit in 2 MiB. Vanilla's deepest at
/// 1.21.11 is 46, the `final_density` of the overworld, amplified and
/// large_biomes settings, so this is five and a half times that. What the
/// server does with a graph past it is unmeasured.
///
/// Refused by name from a pack (Graph::Builder, on the way down for the
/// resolver's own nesting and on the way up for the depth, which a function
/// named in one file and nested again in another can carry past either alone)
/// and from a blob (Graph::Assembler::release, which also refuses the cycle
/// only a blob can carry). Every Graph comes from one or the other, so
/// nothing walks a deeper one.
inline constexpr std::size_t kMaxDepth = 256;

/// The density function types the pinned version defines. Generated from
/// mcdoc — see tools/mcdoc-sync — so the set follows the schema rather than
/// whatever vanilla's own data happens to use, which is eight types fewer.
enum class NodeType : std::uint8_t {
#include <stratum/density/node_types.inc>
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
    /// The `worldgen/noise` entry this node's noise field names.
    std::optional<data::ResourceLocation> noise;
    /// The parameters its noise field carried inline instead. A node with a
    /// noise field has exactly one of these two set; every other node has
    /// neither.
    ///
    /// Both spellings are legal — mcdoc declares the field as
    /// `#[id="worldgen/noise"] string | NoiseParameters` — so both are
    /// resolved. What differs is downstream: a named noise can be seeded,
    /// and an inline one cannot yet be (see Interpreter).
    std::optional<NoiseParameters> inlineNoise;
    std::optional<SplineIndex> spline;
    std::string selector;
};

/// What a node type is made of, in declaration order.
enum class FieldKind : std::uint8_t {
    Function, ///< a nested density function: number, reference or inline
    Number,   ///< a plain number
    Noise,    ///< a worldgen/noise entry: an identifier, or its parameters inline
    Selector, ///< a fixed string, such as rarity_value_mapper
    Spline,   ///< a spline definition
};

/// One field of a node type, as the schema declares it.
struct SchemaField {
    std::string_view name;
    FieldKind kind = FieldKind::Number;
    /// Absent fields are permitted, with a documented default.
    bool optional = false;
    /// Whether the value may be given as an identifier rather than written
    /// out. `clamp`'s input may not be, at this version — a distinction only
    /// the schema records. Every `noise` field may, and may equally be
    /// written inline, which is what makes that kind a union.
    bool allowsReference = false;
    /// The permitted strings, for a Selector.
    std::span<const std::string_view> selectorValues;
};

/// The fields a node type takes, in the order the schema declares them.
[[nodiscard]] std::span<const SchemaField> fieldsOf(NodeType type) noexcept;

/// The most arguments (Function fields) and numeric parameters (Number
/// fields) any node type takes at the pinned schema: `range_choice` and
/// `shifted_noise` take three functions, `old_blended_noise` five numbers.
/// A node carries exactly its type's counts (the graph refuses any other
/// shape), so these bound every node. density_graph.cpp checks them against
/// the schema at compile time, so a schema that grew past them stops the
/// build rather than a node.
inline constexpr std::size_t kMaxNodeArguments = 3;
inline constexpr std::size_t kMaxNodeParameters = 5;

class Graph {
public:
    /// Resolves every density function in @p pack, so that a cycle or a
    /// dangling reference anywhere is found at load rather than on the first
    /// chunk that happens to reach it.
    [[nodiscard]] static Graph resolveAll(const data::Pack& pack);

    /// Builds one graph from more than the pack's own `density_function`
    /// entries. A noise settings router carries fifteen density functions
    /// written inline, and they have to land in the *same* graph as the
    /// named ones: they share subtrees with them — every router in vanilla
    /// refers to `overworld/continents` — and a second graph would resolve
    /// those a second time, sample the same noise twice, and give one
    /// pipeline two node numbering schemes.
    class Builder {
    public:
        explicit Builder(const data::Pack& pack);
        Builder(const Builder&) = delete;
        Builder& operator=(const Builder&) = delete;
        Builder(Builder&&) = delete;
        Builder& operator=(Builder&&) = delete;
        ~Builder();

        /// Resolves every density function the pack names, as roots.
        void addNamed();

        /// Resolves one function given inline, by reference, or as a bare
        /// number. The node is reachable from whatever the caller does with
        /// the index; it is not made a root.
        [[nodiscard]] NodeIndex add(const nlohmann::json& value);

        [[nodiscard]] Graph release();

    private:
        class State;
        std::unique_ptr<State> state_;
    };

    /// Rebuilds a graph from parts resolved once already — see below.
    class Assembler;

    [[nodiscard]] const Node& node(NodeIndex index) const;
    [[nodiscard]] const SplineDefinition& spline(SplineIndex index) const;

    /// Every node, by index, and every spline. Unchecked, unlike node() and
    /// spline() above, and inline: for the hot path of a caller indexing with
    /// what the graph itself holds. Every index a graph holds — a node's
    /// arguments and spline, a spline's coordinate and nested splines, a
    /// root — names something in it, because Builder and Assembler refuse a
    /// graph in which one does not; an index from anywhere else goes through
    /// the checked accessors.
    [[nodiscard]] std::span<const Node> nodes() const noexcept { return nodes_; }

    [[nodiscard]] std::span<const SplineDefinition> splines() const noexcept { return splines_; }

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
    /// they all exist before generation begins. Noises written inline name
    /// nothing and so appear in no list; they travel on the node itself.
    [[nodiscard]] std::vector<data::ResourceLocation> referencedNoises() const;

    /// Every node reachable from @p root, @p root included, sorted. Follows
    /// arguments AND splines: a spline's `coordinate` is a density function,
    /// and in the overworld those coordinates are continents, erosion and
    /// ridges — three named noises that appear in no argument list above
    /// them, so a walk that skips splines silently undercounts.
    ///
    /// This is what lets a caller ask what ONE router entry needs, rather
    /// than what the whole graph needs. The two are very different in a
    /// legacy dimension: the nether's fifteen router entries reference three
    /// named noises between them, and thirteen of the fifteen reference none
    /// (SPEC §11).
    ///
    /// This is the ONLY reachability walk in the library. Everything that
    /// asks "what does this dimension need" — the registry's `wanted`, the
    /// validator's, and the conformance tables — goes through it or through
    /// the two filters below, so there is one definition of "reaches".
    [[nodiscard]] std::vector<NodeIndex> reachableFrom(NodeIndex root) const;

    /// The named noises reachable from @p root, sorted and deduplicated.
    /// Same reach as reachableFrom(); inline noises name nothing and so
    /// appear here no more than they do in referencedNoises().
    [[nodiscard]] std::vector<data::ResourceLocation> noisesReachableFrom(NodeIndex root) const;

    /// The same over several roots at once, unioned — which is the question
    /// a whole dimension asks: one graph holds every dimension's functions,
    /// so referencedNoises() above answers "what does this pack name", never
    /// "what does this dimension need". The difference decides whether a
    /// `legacy_random_source` dimension can be built at all
    /// (NoiseRegistry::create): vanilla's End needs no named noise, but the
    /// graph it shares with the overworld names a dozen.
    [[nodiscard]] std::vector<data::ResourceLocation>
    noisesReachableFrom(std::span<const NodeIndex> roots) const;

private:
    friend class Resolver;

    std::vector<Node> nodes_;
    std::vector<SplineDefinition> splines_;
    std::map<data::ResourceLocation, NodeIndex> roots_;
};

/// Rebuilds a graph from parts that were resolved once already, which is what
/// reading a frozen pipeline needs (SPEC §6). It validates rather than
/// trusts: a stored index that points nowhere is a corrupt blob, and a graph
/// assembled from one would generate a world quietly unlike the one that was
/// frozen.
class Graph::Assembler {
public:
    /// Nodes must arrive in the order they were resolved in, because every
    /// argument index has to name a node already added — the same property
    /// that makes the arguments acyclic. Nested splines likewise name only
    /// earlier splines.
    NodeIndex addNode(Node node);
    SplineIndex addSpline(SplineDefinition spline);
    void addRoot(const data::ResourceLocation& id, NodeIndex root);

    /// The graph, once the references between nodes and splines check out.
    /// Nothing orders a node against its spline's coordinate, so a blob can
    /// close a cycle through one — a ResolveError — and a blob can be deeper
    /// than kMaxDepth — an OutOfRangeError, naming the node. Both are found
    /// here with a stack of its own, before anything recurses over the graph.
    [[nodiscard]] Graph release();

private:
    Graph graph_;
};

} // namespace stratum::density
