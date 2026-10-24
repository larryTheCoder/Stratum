// Stratum — resolved density function graphs.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum/data/pack.hpp>
#include <stratum/data/registry.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/graph.hpp>
#include <stratum/density/noise_parameters.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace stratum::density {

namespace {

struct TypeInfo {
    NodeType type;
    std::string_view name;
    std::span<const SchemaField> fields;
};

// The schema itself, generated from mcdoc for the pinned version by
// tools/mcdoc-sync. Hand-editing it would defeat the point: the schema is
// authoritative about which types exist and what fields they take.
#include "density_schema.inc"

/// Whether row i of the table is the type whose enumerator is i. The
/// generator emits both from one sorted list, so they agree by construction;
/// this makes the compiler check it, because every lookup below indexes the
/// table — and the shape table built from it — with an enum value.
[[nodiscard]] constexpr bool rowsMatchEnumerators() {
    for (std::size_t i = 0; i < kTypeTable.size(); ++i) {
        if (static_cast<std::size_t>(kTypeTable[i].type) != i) {
            return false;
        }
    }
    return true;
}

static_assert(rowsMatchEnumerators(), "kTypeTable is not in NodeType order");

[[nodiscard]] const TypeInfo& infoOf(NodeType type) {
    return kTypeTable[static_cast<std::size_t>(type)];
}

/// What a node of one type is made of, counted from that type's schema row:
/// a Function field is one argument and a Number field one parameter, both in
/// declaration order, and a Noise, Spline or Selector field is the node's
/// noise, spline or selector. The interpreter reads exactly that, by position
/// — `arguments[i]` and `parameters[i]` — so a node of any other shape sends
/// it past the end of a vector.
struct Shape {
    std::size_t arguments = 0;
    std::size_t parameters = 0;
    bool noise = false;
    bool spline = false;
    bool selector = false;
    /// The strings a selector may be; empty for a type without one.
    std::span<const std::string_view> selectorValues;
};

[[nodiscard]] constexpr Shape shapeOf(const TypeInfo& info) {
    Shape shape;
    for (const SchemaField& field : info.fields) {
        switch (field.kind) {
            case FieldKind::Function:
                ++shape.arguments;
                break;
            case FieldKind::Number:
                ++shape.parameters;
                break;
            case FieldKind::Noise:
                shape.noise = true;
                break;
            case FieldKind::Spline:
                shape.spline = true;
                break;
            case FieldKind::Selector:
                shape.selector = true;
                shape.selectorValues = field.selectorValues;
                break;
        }
    }
    return shape;
}

/// Every type's shape, one row per type, derived from the schema at compile
/// time: ONE table for the whole check, so that no type can be left out of
/// it the way all but three were when it was written type by type (SPEC §8).
/// An optional field would make a count a range; the pinned schema has none,
/// and an absent one is refused below by the count it leaves short rather
/// than read past.
constexpr std::array<Shape, kTypeTable.size()> kShapes = [] {
    std::array<Shape, kTypeTable.size()> shapes{};
    for (std::size_t i = 0; i < kTypeTable.size(); ++i) {
        shapes[i] = shapeOf(kTypeTable[i]);
    }
    return shapes;
}();

/// Whether the header's kMaxNodeArguments and kMaxNodeParameters are exactly
/// the largest counts in the schema: never below one, which would let a node
/// carry more than the interpreter's operand table holds, and not above
/// every one either, so that the bound stays the schema's and not a guess.
[[nodiscard]] constexpr bool operandBoundsMatchSchema() {
    std::size_t arguments = 0;
    std::size_t parameters = 0;
    for (const Shape& shape : kShapes) {
        arguments = std::max(arguments, shape.arguments);
        parameters = std::max(parameters, shape.parameters);
    }
    return arguments == kMaxNodeArguments && parameters == kMaxNodeParameters;
}

static_assert(operandBoundsMatchSchema(),
              "kMaxNodeArguments or kMaxNodeParameters no longer match the schema");

/// Whether @p type names a row of the table at all. A NodeType that came out
/// of a file rather than out of the resolver can be any byte, and indexing
/// the table with one of those aborts in a checked build — which is how this
/// was found: the freeze reader's own validation was the thing that crashed
/// on the input it was validating.
[[nodiscard]] bool isKnownType(NodeType type) noexcept {
    return static_cast<std::size_t>(type) < kTypeTable.size();
}

/// `value` as a message prints it: every digit a double carries.
[[nodiscard]] std::string spell(double value) {
    std::ostringstream text;
    text << std::setprecision(17) << value;
    return text.str();
}

/// @p node refused by name unless it has exactly its type's shape (kShapes):
/// as many arguments and numeric parameters as the type reads, a noise if
/// and only if it samples one (named or inline, not both), a spline if and
/// only if it is a spline, and a selector the type allows or none. The
/// resolver builds every node to that shape from JSON, so from a pack this
/// cannot fire; it is what stops a resealed frozen blob, whose counts are
/// whatever its bytes say (SPEC §8).
void checkShape(const Node& node) {
    if (!isKnownType(node.type)) {
        throw ResolveError("a node has type " + std::to_string(static_cast<int>(node.type)) +
                           ", which this build does not know");
    }
    const Shape& shape = kShapes[static_cast<std::size_t>(node.type)];
    const std::string name = "'" + std::string(nodeTypeName(node.type)) + "'";
    if (node.parameters.size() != shape.parameters) {
        throw ResolveError(name + " carries " + std::to_string(node.parameters.size()) +
                           " numeric parameter(s) where its type reads " +
                           std::to_string(shape.parameters));
    }
    if (node.arguments.size() != shape.arguments) {
        throw ResolveError(name + " carries " + std::to_string(node.arguments.size()) +
                           " argument(s) where its type reads " + std::to_string(shape.arguments));
    }
    if (node.noise.has_value() && node.inlineNoise.has_value()) {
        throw ResolveError(name + " carries both a named and an inline noise; its noise field "
                                  "is one or the other");
    }
    const bool hasNoise = node.noise.has_value() || node.inlineNoise.has_value();
    if (hasNoise != shape.noise) {
        throw ResolveError(name + (shape.noise ? " carries no noise, where its type samples one"
                                               : " carries a noise its type has no field for"));
    }
    if (node.spline.has_value() != shape.spline) {
        throw ResolveError(name + (shape.spline ? " carries no spline, where its type is one"
                                                : " carries a spline its type has no field for"));
    }
    if (shape.selector) {
        if (std::ranges::find(shape.selectorValues, node.selector) == shape.selectorValues.end()) {
            throw ResolveError(name + " has the selector '" + node.selector +
                               "', which its type does not allow");
        }
    } else if (!node.selector.empty()) {
        throw ResolveError(name + " carries the selector '" + node.selector +
                           "', which its type has no field for");
    }
}

/// The parameters of @p node that would carry the interpreter into undefined
/// behaviour, refused by name (SPEC §8, "Inputs that would reach undefined
/// behaviour") with an OutOfRangeError, which freeze::read reports as a
/// refusal rather than as a malformed blob. Called after checkShape, so every
/// parameter read here exists.
///
/// Each check was prompted by a value that reaches undefined behaviour, but
/// what it refuses is the schema's or the wiki's whole out-of-range set (or,
/// for clamp, an order vanilla's own data keeps), not only the undefined
/// part of it: some of what it stops, such as a `lower_bound` of -5000 or an
/// `xz_scale` of 2000, computed defined output before (SPEC §6, "No version
/// for the undefined-behaviour refusals"). Nothing a vanilla pack writes is
/// refused.
void checkParameters(const Node& node) {
    switch (node.type) {
        case NodeType::Clamp: {
            // std::clamp's precondition is `!(max < min)`; reversed bounds
            // are undefined behaviour, and nothing says what vanilla makes
            // of them. The schema bounds each, not their order.
            const double min = node.parameters[0];
            const double max = node.parameters[1];
            if (std::isnan(min) || std::isnan(max) || min > max) {
                throw OutOfRangeError("'minecraft:clamp' has min " + spell(min) + " above max " +
                                      spell(max) + "; a clamp needs min <= max");
            }
            break;
        }
        case NodeType::FindTopSurface: {
            // The schema's own ranges (vanilla-mcdoc: lower_bound int
            // @ -4064..4062, "(DimensionType.MIN_Y * 2)..(DimensionType.MAX_Y
            // * 2)", and cell_height int @ 1..): outside them the int
            // conversions the scan makes are undefined, and cell_height <= 0
            // would never finish. The upper bound on cell_height is int32's.
            const double lowerBound = node.parameters[0];
            const double cellHeight = node.parameters[1];
            if (std::isnan(lowerBound) || lowerBound <= -4065.0 || lowerBound >= 4063.0) {
                throw OutOfRangeError("'minecraft:find_top_surface' has lower_bound " +
                                      spell(lowerBound) +
                                      ", outside the schema's range [-4064, 4062]");
            }
            if (std::isnan(cellHeight) || cellHeight < 1.0 || cellHeight >= 2147483648.0) {
                throw OutOfRangeError("'minecraft:find_top_surface' has cell_height " +
                                      spell(cellHeight) +
                                      ", outside [1, 2147483647]: the schema requires at least 1, "
                                      "and the scan steps in 32-bit integers");
            }
            break;
        }
        case NodeType::OldBlendedNoise: {
            // minecraft.wiki gives each of these "between 0.001 and 1000.0
            // (both inclusive)". Outside it a factor of 0 makes the blend's
            // coordinate infinite, and a huge scale makes it overflow, and
            // either reaches the lattice as NaN — undefined behaviour in the
            // int conversion. smear_scale_multiplier (1 to 8) cannot get
            // there, so it is not checked.
            static constexpr std::array<std::string_view, 4> kNames = {"xz_scale", "y_scale",
                                                                       "xz_factor", "y_factor"};
            for (std::size_t i = 0; i < kNames.size(); ++i) {
                const double value = node.parameters[i];
                if (std::isnan(value) || value < 0.001 || value > 1000.0) {
                    throw OutOfRangeError("'minecraft:old_blended_noise' has " +
                                          std::string(kNames[i]) + " " + spell(value) +
                                          ", outside [0.001, 1000]");
                }
            }
            break;
        }
        default:
            break;
    }
}

/// Everything the graph holds a node to, from a pack and from a blob alike:
/// its shape first, so that the value checks read only parameters that exist.
void checkNode(const Node& node) {
    checkShape(node);
    checkParameters(node);
}

/// One depth (kMaxDepth) as a message says it.
[[nodiscard]] std::string tooDeep(std::size_t depth) {
    return "sits " + std::to_string(depth) + " nodes and splines deep, past the " +
           std::to_string(kMaxDepth) +
           " this build evaluates; it refuses the graph rather than exhaust its stack walking "
           "it (SPEC §8)";
}

/// Every node and spline of an assembled graph held to kMaxDepth, and to
/// having no cycle, walked with a stack of its own: the walk that checks the
/// depth must not be the recursion it bounds. Indices are already known to
/// name something (Graph::Assembler). Arguments name only earlier nodes and
/// nested splines only earlier splines, so a cycle has to pass from a node
/// to its spline and back through a coordinate — which the resolver never
/// builds and a resealed blob can.
///
/// Nodes and splines share one numbering here: node i is i, spline j is
/// nodes.size() + j.
void checkDepths(std::span<const Node> nodes, std::span<const SplineDefinition> splines) {
    const std::size_t nodeCount = nodes.size();
    const auto describe = [&](std::size_t item) {
        if (item < nodeCount) {
            return "frozen node " + std::to_string(item) + " ('" +
                   std::string(nodeTypeName(nodes[item].type)) + "')";
        }
        return "frozen spline " + std::to_string(item - nodeCount);
    };

    // The child of @p item at or after position @p from, with the position
    // after it: a node's arguments and then its spline, a spline's coordinate
    // and then the nested splines among its points.
    struct Next {
        std::size_t child;
        std::size_t position;
    };

    const auto nextChild = [&](std::size_t item, std::size_t from) -> std::optional<Next> {
        if (item < nodeCount) {
            const Node& node = nodes[item];
            if (from < node.arguments.size()) {
                return Next{.child = node.arguments[from], .position = from + 1};
            }
            if (from == node.arguments.size() && node.spline.has_value()) {
                return Next{.child = nodeCount + *node.spline, .position = from + 1};
            }
            return std::nullopt;
        }
        const SplineDefinition& spline = splines[item - nodeCount];
        if (from == 0) {
            return Next{.child = spline.coordinate, .position = 1};
        }
        for (std::size_t point = from - 1; point < spline.points.size(); ++point) {
            if (spline.points[point].nested.has_value()) {
                return Next{.child = nodeCount + *spline.points[point].nested,
                            .position = point + 2};
            }
        }
        return std::nullopt;
    };

    enum class State : std::uint8_t { Unvisited, OnPath, Done };
    std::vector<State> state(nodeCount + splines.size(), State::Unvisited);
    std::vector<std::size_t> depth(nodeCount + splines.size(), 0);

    struct Frame {
        std::size_t item;
        std::size_t position;
    };

    std::vector<Frame> path;

    for (std::size_t start = 0; start < state.size(); ++start) {
        if (state[start] != State::Unvisited) {
            continue;
        }
        state[start] = State::OnPath;
        path.push_back(Frame{.item = start, .position = 0});
        while (!path.empty()) {
            const Frame top = path.back();
            if (const std::optional<Next> next = nextChild(top.item, top.position)) {
                path.back().position = next->position;
                if (state[next->child] == State::OnPath) {
                    throw ResolveError(describe(top.item) + " leads back to " +
                                       describe(next->child) +
                                       ", so the graph has a cycle, which no build resolves");
                }
                if (state[next->child] == State::Unvisited) {
                    state[next->child] = State::OnPath;
                    path.push_back(Frame{.item = next->child, .position = 0});
                }
                continue;
            }
            // Every child is done: this one is one deeper than the deepest.
            std::size_t deepest = 0;
            for (std::optional<Next> child = nextChild(top.item, 0); child.has_value();
                 child = nextChild(top.item, child->position)) {
                deepest = std::max(deepest, depth[child->child]);
            }
            if (deepest + 1 > kMaxDepth) {
                throw OutOfRangeError(describe(top.item) + " " + tooDeep(deepest + 1));
            }
            depth[top.item] = deepest + 1;
            state[top.item] = State::Done;
            path.pop_back();
        }
    }
}

} // namespace

std::string_view nodeTypeName(NodeType type) noexcept {
    return isKnownType(type) ? infoOf(type).name : "unknown";
}

std::optional<NodeType> nodeTypeFromName(std::string_view name) noexcept {
    for (const TypeInfo& info : kTypeTable) {
        if (info.name == name) {
            return info.type;
        }
    }
    return std::nullopt;
}

std::span<const SchemaField> fieldsOf(NodeType type) noexcept {
    return isKnownType(type) ? infoOf(type).fields : std::span<const SchemaField>{};
}

/// Walks the JSON and builds the graph, memoising by identifier so a
/// function referenced from twenty places is resolved once, and keeping a
/// stack so a cycle can name the path that closed it.
///
/// Held to kMaxDepth twice over (SPEC §8). Its own recursion is counted on
/// the way down, a level per function, spline and reference followed, and
/// refused before the level past the bound is entered: a nesting deep enough
/// to matter runs the stack out before anything below it could be counted on
/// the way back up, and a chain of files each naming the next is deep to the
/// resolver while it is one node to the graph. And each node's depth is
/// counted on the way up, as it is added, because memoising lets the graph
/// grow deeper than the recursion that built it: a function nested a hundred
/// deep in one file and named at the bottom of a second such nesting is two
/// hundred deep, while the resolver never went past a hundred and one.
class Resolver {
public:
    explicit Resolver(const data::Pack& pack) : pack_(pack) {}

    void resolveEverything(Graph& graph) {
        for (const data::PackEntry* entry : pack_.entriesOf(data::Registry::DensityFunction)) {
            const NodeIndex index = resolveReference(graph, entry->id);
            graph.roots_.emplace(entry->id, index);
        }
    }

private:
    /// @p what, after the chain of identifiers that reached it.
    [[nodiscard]] std::string chained(const std::string& what) const {
        std::string chain;
        for (const data::ResourceLocation& id : stack_) {
            chain += (chain.empty() ? "" : " -> ") + id.toString();
        }
        return chain.empty() ? what : chain + ": " + what;
    }

    [[noreturn]] void fail(const std::string& what) const { throw ResolveError(chained(what)); }

    /// The same, for a graph deeper than kMaxDepth: an OutOfRangeError, as
    /// the frozen-blob path raises for one.
    [[noreturn]] void failTooDeep(const std::string& what) const {
        throw OutOfRangeError(chained(what));
    }

    /// One level of the resolver's own recursion, entered on construction
    /// and left on destruction; refused, before it is entered, past
    /// kMaxDepth.
    class Level {
    public:
        explicit Level(Resolver& resolver) : resolver_(resolver) {
            if (resolver_.nesting_ == kMaxDepth) {
                resolver_.failTooDeep(
                    "density functions nest more than " + std::to_string(kMaxDepth) +
                    " deep here, counting each function, spline and reference followed on the "
                    "way down; this build refuses them rather than exhaust its stack (SPEC §8)");
            }
            ++resolver_.nesting_;
        }

        ~Level() { --resolver_.nesting_; }

        Level(const Level&) = delete;
        Level& operator=(const Level&) = delete;
        Level(Level&&) = delete;
        Level& operator=(Level&&) = delete;

    private:
        Resolver& resolver_;
    };

    // THE RECURSION'S OWN FRAMES ARE KEPT THIN. resolveValue, resolveObject,
    // resolveSpline and resolveReference call one another once per level of
    // nesting, so whatever each holds on the stack is paid once per level.
    // Everything that does not recurse — reading a leaf field, checking
    // a shape, and building every message — is in a function of its own,
    // whose frame is gone again before the recursion goes a level deeper.
    // Unoptimised, where a compiler gives each temporary string a slot of its
    // own, that took a level of resolveValue and resolveObject from 4.8 KiB
    // to 1.1 in the sanitize preset's build (SPEC §8).

    /// Resolves a density function named by identifier, or returns the node
    /// it already resolved to.
    NodeIndex resolveReference(Graph& graph, const data::ResourceLocation& id) {
        if (const auto found = resolved_.find(id); found != resolved_.end()) {
            return found->second;
        }
        const data::PackEntry& entry = definitionOf(id);
        stack_.push_back(id);
        const NodeIndex index = resolveValue(graph, entry.json);
        stack_.pop_back();

        resolved_.emplace(id, index);
        return index;
    }

    /// A density function is a number, an identifier, or an inline object.
    /// Public because the builder resolves the density functions written
    /// inline in a noise settings router through it.
public:
    NodeIndex resolveValue(Graph& graph, const nlohmann::json& value) {
        const Level level(*this);
        if (value.is_number()) {
            return addConstant(graph, value);
        }
        if (value.is_string()) {
            return resolveReference(graph, idOf(value));
        }
        if (!value.is_object()) {
            failNotAFunction(value);
        }
        return resolveObject(graph, value);
    }

private:
    NodeIndex resolveObject(Graph& graph, const nlohmann::json& object) {
        Node node;
        node.type = typeOf(object);
        for (const SchemaField& field : infoOf(node.type).fields) {
            const nlohmann::json* const value = fieldOf(object, node.type, field);
            if (value == nullptr) {
                continue;
            }
            // Only these two recurse.
            if (field.kind == FieldKind::Function) {
                checkReference(node.type, field, *value);
                node.arguments.push_back(resolveValue(graph, *value));
            } else if (field.kind == FieldKind::Spline) {
                node.spline = resolveSpline(graph, *value);
            } else {
                readLeaf(node, field, *value);
            }
        }
        checkResolved(node);
        return addNode(graph, std::move(node));
    }

    SplineIndex resolveSpline(Graph& graph, const nlohmann::json& value) {
        const Level level(*this);
        checkSpline(value);
        SplineDefinition definition;
        definition.coordinate = resolveValue(graph, value.at("coordinate"));
        for (const nlohmann::json& point : pointsOf(value)) {
            SplinePoint resolved = pointOf(point, definition);
            const nlohmann::json& pointValue = point.at("value");
            if (pointValue.is_number()) {
                resolved.value = pointValue.get<double>();
            } else {
                resolved.nested = resolveSpline(graph, pointValue);
            }
            definition.points.push_back(resolved);
        }
        return addSpline(graph, std::move(definition));
    }

    // --- what the recursion calls, and does not recurse from ---------------

    /// The pack entry @p id names, refusing a cycle back to it and an
    /// identifier the pack does not define.
    [[nodiscard]] const data::PackEntry& definitionOf(const data::ResourceLocation& id) const {
        const auto cycleAt = std::ranges::find(stack_, id);
        if (cycleAt != stack_.end()) {
            std::string cycle;
            for (auto it = cycleAt; it != stack_.end(); ++it) {
                cycle += it->toString() + " -> ";
            }
            cycle += id.toString();
            throw ResolveError("density function cycle: " + cycle);
        }

        const data::PackEntry* entry = pack_.find(data::Registry::DensityFunction, id);
        if (entry == nullptr) {
            fail("references density function '" + id.toString() +
                 "', which the pack does not define");
        }
        return *entry;
    }

    /// The documented shorthand: a bare number is a constant.
    [[nodiscard]] NodeIndex addConstant(Graph& graph, const nlohmann::json& value) {
        return addNode(graph, Node{.type = NodeType::Constant,
                                   .arguments = {},
                                   .parameters = {value.get<double>()},
                                   .noise = std::nullopt,
                                   .inlineNoise = std::nullopt,
                                   .spline = std::nullopt,
                                   .selector = {}});
    }

    [[nodiscard]] data::ResourceLocation idOf(const nlohmann::json& value) const {
        return parseId(value.get<std::string>());
    }

    [[noreturn]] void failNotAFunction(const nlohmann::json& value) const {
        fail("a density function must be a number, an identifier or an object, not " +
             std::string(value.type_name()));
    }

    /// The type an inline function's "type" names.
    [[nodiscard]] NodeType typeOf(const nlohmann::json& object) const {
        if (!object.contains("type")) {
            fail(R"(an inline density function has no "type")");
        }
        const nlohmann::json& typeValue = object.at("type");
        if (!typeValue.is_string()) {
            fail(R"("type" must be a string, not )" + std::string(typeValue.type_name()));
        }

        const std::string typeName = typeValue.get<std::string>();
        const std::optional<NodeType> type = nodeTypeFromName(typeName);
        if (!type.has_value()) {
            // Named rather than skipped: a node we cannot represent would
            // otherwise become a silently different world (SPEC §8).
            fail("density function type '" + typeName +
                 "' is not implemented by this engine. The types it implements are those "
                 "vanilla's own data uses.");
        }
        return *type;
    }

    /// @p field's value in @p object, or nothing for an optional field that
    /// is absent. The schema marks some fields optional, with a documented
    /// default; the rest are required.
    [[nodiscard]] const nlohmann::json* fieldOf(const nlohmann::json& object, NodeType type,
                                                const SchemaField& field) const {
        if (object.contains(field.name)) {
            return &object.at(field.name);
        }
        if (field.optional) {
            return nullptr;
        }
        fail("'" + std::string(nodeTypeName(type)) + "' is missing its \"" +
             std::string(field.name) + "\"");
    }

    /// Some fields take a nested function but not a reference to one —
    /// clamp's input, at this version. Only the schema records that
    /// distinction.
    void checkReference(NodeType type, const SchemaField& field,
                        const nlohmann::json& value) const {
        if (value.is_string() && !field.allowsReference) {
            fail("'" + std::string(nodeTypeName(type)) + "' does not accept an identifier for \"" +
                 std::string(field.name) + "\" at this version; it must be given inline");
        }
    }

    /// A field that holds no function: a number, a noise or a selector.
    void readLeaf(Node& node, const SchemaField& field, const nlohmann::json& value) const {
        const std::string typeName(nodeTypeName(node.type));
        switch (field.kind) {
            case FieldKind::Number:
                if (!value.is_number()) {
                    fail("'" + typeName + "' expects \"" + std::string(field.name) +
                         "\" to be a number, not " + std::string(value.type_name()));
                }
                node.parameters.push_back(value.get<double>());
                break;
            case FieldKind::Noise:
                // A union in the schema, so a union here: the field either
                // names a `worldgen/noise` entry or carries that entry's
                // parameters inline. Both are legal to vanilla, and the
                // generated table used to record only the first — which made
                // a datapack that inlines its noise parameters something this
                // engine refused for no reason (SPEC §8, §11).
                if (value.is_string()) {
                    // No noise field at the pinned version forbids the
                    // identifier, so this refusal cannot fire today. It is
                    // here because the table decides and not this code: a
                    // bare `NoiseParameters` field is a shape the generator
                    // emits, and it means inline only.
                    if (!field.allowsReference) {
                        fail("'" + typeName + "' does not accept an identifier for \"" +
                             std::string(field.name) +
                             "\" at this version; it must be given inline");
                    }
                    node.noise = parseId(value.get<std::string>());
                } else if (value.is_object()) {
                    try {
                        node.inlineNoise = NoiseParameters::fromInline(
                            value, "the noise written inline in '" + typeName + "' as \"" +
                                       std::string(field.name) + "\"");
                    } catch (const NoiseError& error) {
                        // Rethrown as a ResolveError so that the message
                        // carries the chain of identifiers that reached this
                        // node, which is the whole point of fail().
                        fail(error.what());
                    }
                } else {
                    fail("'" + typeName + "' expects \"" + std::string(field.name) +
                         "\" to name a noise or to be one, not " + std::string(value.type_name()));
                }
                break;
            case FieldKind::Selector:
                if (!value.is_string()) {
                    fail("'" + typeName + "' expects \"" + std::string(field.name) +
                         "\" to be a string");
                }
                node.selector = value.get<std::string>();
                if (std::ranges::find(field.selectorValues, node.selector) ==
                    field.selectorValues.end()) {
                    fail("'" + typeName + "' has an unknown " + std::string(field.name) + " '" +
                         node.selector + "'");
                }
                break;
            case FieldKind::Function:
            case FieldKind::Spline:
                // resolveObject's own, since they recurse.
                break;
        }
    }

    /// checkNode, rethrown through fail() for the chain of identifiers.
    void checkResolved(const Node& node) const {
        try {
            checkNode(node);
        } catch (const ResolveError& error) {
            fail(error.what());
        }
    }

    void checkSpline(const nlohmann::json& value) const {
        if (!value.is_object()) {
            fail("a spline must be an object, not " + std::string(value.type_name()));
        }
        if (!value.contains("coordinate") || !value.contains("points")) {
            fail(R"(a spline needs both "coordinate" and "points")");
        }
    }

    [[nodiscard]] const nlohmann::json& pointsOf(const nlohmann::json& spline) const {
        const nlohmann::json& points = spline.at("points");
        if (!points.is_array()) {
            fail(R"(a spline's "points" must be an array)");
        }
        return points;
    }

    /// @p point's location and derivative, after the points of @p spline
    /// read so far: its value is the caller's, since it may recurse.
    [[nodiscard]] SplinePoint pointOf(const nlohmann::json& point,
                                      const SplineDefinition& spline) const {
        if (!point.is_object() || !point.contains("location") || !point.contains("value") ||
            !point.contains("derivative")) {
            fail(R"(a spline point needs "location", "value" and "derivative")");
        }
        SplinePoint resolved;
        resolved.location = point.at("location").get<double>();
        resolved.derivative = point.at("derivative").get<double>();
        // Interpolation walks the points in order; out-of-order points would
        // silently sample the wrong segment.
        if (!spline.points.empty() && resolved.location < spline.points.back().location) {
            fail("a spline's points are not in ascending order of location");
        }
        return resolved;
    }

    /// Appends @p spline, whose coordinate and nested splines are in the graph
    /// already, refusing one with no points and one past kMaxDepth.
    [[nodiscard]] SplineIndex addSpline(Graph& graph, SplineDefinition spline) {
        if (spline.points.empty()) {
            fail("a spline has no points");
        }
        std::size_t depth = nodeDepths_[spline.coordinate] + 1;
        for (const SplinePoint& point : spline.points) {
            if (point.nested.has_value()) {
                depth = std::max(depth, splineDepths_[*point.nested] + 1);
            }
        }
        if (depth > kMaxDepth) {
            failTooDeep("a spline " + tooDeep(depth));
        }

        splineDepths_.push_back(depth);
        graph.splines_.push_back(std::move(spline));
        return static_cast<SplineIndex>(graph.splines_.size() - 1);
    }

    [[nodiscard]] data::ResourceLocation parseId(const std::string& text) const {
        try {
            return data::ResourceLocation::parse(text);
        } catch (const data::ResourceLocationError& error) {
            fail(error.what());
        }
    }

    /// Appends @p node, whose arguments and spline are in the graph already,
    /// refusing it past kMaxDepth.
    [[nodiscard]] NodeIndex addNode(Graph& graph, Node node) {
        std::size_t depth = 1;
        for (const NodeIndex argument : node.arguments) {
            depth = std::max(depth, nodeDepths_[argument] + 1);
        }
        if (node.spline.has_value()) {
            depth = std::max(depth, splineDepths_[*node.spline] + 1);
        }
        if (depth > kMaxDepth) {
            failTooDeep("'" + std::string(nodeTypeName(node.type)) + "' " + tooDeep(depth));
        }

        nodeDepths_.push_back(depth);
        graph.nodes_.push_back(std::move(node));
        return static_cast<NodeIndex>(graph.nodes_.size() - 1);
    }

    const data::Pack& pack_;
    std::map<data::ResourceLocation, NodeIndex> resolved_;
    std::vector<data::ResourceLocation> stack_;
    /// How many levels of resolveValue and resolveSpline are under way.
    std::size_t nesting_ = 0;
    /// The depth (kMaxDepth) of each node and spline added so far, parallel
    /// to the graph's own tables: one graph per resolver (Graph::Builder).
    std::vector<std::size_t> nodeDepths_;
    std::vector<std::size_t> splineDepths_;
};

/// The builder's innards, kept out of the header so that Resolver stays a
/// detail of this file.
class Graph::Builder::State {
public:
    explicit State(const data::Pack& pack) : resolver_(pack) {}

    Resolver resolver_;
    Graph graph_;
};

Graph::Builder::Builder(const data::Pack& pack) : state_(std::make_unique<State>(pack)) {}

Graph::Builder::~Builder() = default;

void Graph::Builder::addNamed() {
    state_->resolver_.resolveEverything(state_->graph_);
}

NodeIndex Graph::Builder::add(const nlohmann::json& value) {
    return state_->resolver_.resolveValue(state_->graph_, value);
}

Graph Graph::Builder::release() {
    return std::move(state_->graph_);
}

NodeIndex Graph::Assembler::addNode(Node node) {
    const auto next = static_cast<NodeIndex>(graph_.nodes_.size());
    for (const NodeIndex argument : node.arguments) {
        if (argument >= next) {
            throw ResolveError("a frozen node's argument names node " + std::to_string(argument) +
                               ", which is not before it");
        }
    }
    // What the resolver refuses of a pack, refused of a blob too — and the
    // shape, which the resolver cannot get wrong and a blob can.
    checkNode(node);
    graph_.nodes_.push_back(std::move(node));
    return next;
}

SplineIndex Graph::Assembler::addSpline(SplineDefinition spline) {
    const auto next = static_cast<SplineIndex>(graph_.splines_.size());
    // The resolver refuses a spline with no points; the interpreter reads
    // its first one wherever the coordinate falls (SPEC §8).
    if (spline.points.empty()) {
        throw ResolveError("frozen spline " + std::to_string(next) + " has no points");
    }
    for (const SplinePoint& point : spline.points) {
        if (point.value.has_value() == point.nested.has_value()) {
            throw ResolveError("a frozen spline point is either both a value and a nested "
                               "spline or neither");
        }
        if (point.nested.has_value() && *point.nested >= next) {
            throw ResolveError("a frozen spline nests spline " + std::to_string(*point.nested) +
                               ", which is not before it");
        }
    }
    graph_.splines_.push_back(std::move(spline));
    return next;
}

void Graph::Assembler::addRoot(const data::ResourceLocation& id, NodeIndex root) {
    if (root >= graph_.nodes_.size()) {
        throw ResolveError("frozen root '" + id.toString() + "' names node " +
                           std::to_string(root) + ", which does not exist");
    }
    graph_.roots_.emplace(id, root);
}

Graph Graph::Assembler::release() {
    // Nodes name splines and splines name nodes, so neither table can be
    // finished before the other. The references that cross between them are
    // therefore checked here, once both are whole, rather than as each entry
    // arrives.
    for (std::size_t i = 0; i < graph_.nodes_.size(); ++i) {
        const std::optional<SplineIndex>& spline = graph_.nodes_[i].spline;
        if (spline.has_value() && *spline >= graph_.splines_.size()) {
            throw ResolveError("frozen node " + std::to_string(i) + " names spline " +
                               std::to_string(*spline) + ", which does not exist");
        }
    }
    for (std::size_t i = 0; i < graph_.splines_.size(); ++i) {
        if (graph_.splines_[i].coordinate >= graph_.nodes_.size()) {
            throw ResolveError("frozen spline " + std::to_string(i) + " has coordinate " +
                               std::to_string(graph_.splines_[i].coordinate) +
                               ", which is not a node");
        }
    }
    // Last, once every index is known to name something: the cycle and the
    // depth the evaluator would otherwise find by recursing into them.
    checkDepths(graph_.nodes_, graph_.splines_);
    return std::move(graph_);
}

Graph Graph::resolveAll(const data::Pack& pack) {
    Builder builder(pack);
    builder.addNamed();
    return builder.release();
}

const Node& Graph::node(NodeIndex index) const {
    if (index >= nodes_.size()) {
        throw ResolveError("node index " + std::to_string(index) + " is out of range");
    }
    return nodes_[index];
}

const SplineDefinition& Graph::spline(SplineIndex index) const {
    if (index >= splines_.size()) {
        throw ResolveError("spline index " + std::to_string(index) + " is out of range");
    }
    return splines_[index];
}

NodeIndex Graph::rootOf(const data::ResourceLocation& id) const {
    const auto found = roots_.find(id);
    if (found == roots_.end()) {
        throw ResolveError("no density function named '" + id.toString() + "'");
    }
    return found->second;
}

bool Graph::contains(const data::ResourceLocation& id) const {
    return roots_.contains(id);
}

std::vector<data::ResourceLocation> Graph::referencedNoises() const {
    std::set<data::ResourceLocation> unique;
    for (const Node& node : nodes_) {
        if (node.noise.has_value()) {
            unique.insert(*node.noise);
        }
    }
    return {unique.begin(), unique.end()};
}

std::vector<NodeIndex> Graph::reachableFrom(const NodeIndex root) const {
    if (static_cast<std::size_t>(root) >= nodes_.size()) {
        throw ResolveError("no node " + std::to_string(root) + " in a graph of " +
                           std::to_string(nodes_.size()));
    }

    // SPLINES ARE PART OF THE REACH, and forgetting them is how a walk
    // undercounts: a spline's `coordinate` is a density function, and in the
    // overworld those coordinates are continents, erosion and ridges — three
    // named noises that appear NOWHERE in the node arguments above them.
    //
    // An explicit stack, not recursion: a spline tree is deep enough that a
    // hostile pack could reach the C stack's limit, and a refusal has to be
    // an error rather than a crash (SPEC §8). Interior indices are bounds
    // checked too, because a graph can also arrive from Assembler — a frozen
    // pipeline blob is input, and a corrupt one must be refused rather than
    // read out of bounds.
    std::vector<char> seenNode(nodes_.size(), 0);
    std::vector<char> seenSpline(splines_.size(), 0);
    std::vector<NodeIndex> pending{root};
    std::vector<SplineIndex> pendingSplines;
    std::vector<NodeIndex> out;
    seenNode[static_cast<std::size_t>(root)] = 1;

    const auto pushNode = [&](const NodeIndex index) {
        if (static_cast<std::size_t>(index) >= nodes_.size()) {
            throw ResolveError("a node index of " + std::to_string(index) +
                               " is outside this graph, which has " +
                               std::to_string(nodes_.size()) + " node(s)");
        }
        if (std::exchange(seenNode[static_cast<std::size_t>(index)], static_cast<char>(1)) == 0) {
            pending.push_back(index);
        }
    };
    const auto pushSpline = [&](const SplineIndex index) {
        if (static_cast<std::size_t>(index) >= splines_.size()) {
            throw ResolveError("a spline index of " + std::to_string(index) +
                               " is outside this graph, which has " +
                               std::to_string(splines_.size()) + " spline(s)");
        }
        if (std::exchange(seenSpline[static_cast<std::size_t>(index)], static_cast<char>(1)) == 0) {
            pendingSplines.push_back(index);
        }
    };

    while (!pending.empty() || !pendingSplines.empty()) {
        while (!pending.empty()) {
            const NodeIndex index = pending.back();
            pending.pop_back();
            out.push_back(index);
            const Node& node = nodes_[static_cast<std::size_t>(index)];
            for (const NodeIndex argument : node.arguments) {
                pushNode(argument);
            }
            if (node.spline.has_value()) {
                pushSpline(*node.spline);
            }
        }
        while (!pendingSplines.empty()) {
            const SplineIndex index = pendingSplines.back();
            pendingSplines.pop_back();
            const SplineDefinition& spline = splines_[static_cast<std::size_t>(index)];
            pushNode(spline.coordinate);
            for (const SplinePoint& point : spline.points) {
                if (point.nested.has_value()) {
                    pushSpline(*point.nested);
                }
            }
        }
    }

    std::sort(out.begin(), out.end());
    return out;
}

std::vector<data::ResourceLocation> Graph::noisesReachableFrom(const NodeIndex root) const {
    std::set<data::ResourceLocation> unique;
    for (const NodeIndex index : reachableFrom(root)) {
        const Node& node = nodes_[static_cast<std::size_t>(index)];
        if (node.noise.has_value()) {
            unique.insert(*node.noise);
        }
    }
    return {unique.begin(), unique.end()};
}

std::vector<data::ResourceLocation>
Graph::noisesReachableFrom(std::span<const NodeIndex> roots) const {
    // The union of the per-root answer, rather than a second walk with a
    // shared visited set. Which roots a dimension has is a list of fifteen,
    // so the repeated visits cost nothing measurable, and the alternative is
    // two definitions of "reaches" in one library that could drift apart —
    // which is exactly the bug this whole area is about.
    std::set<data::ResourceLocation> unique;
    for (const NodeIndex root : roots) {
        for (const data::ResourceLocation& id : noisesReachableFrom(root)) {
            unique.insert(id);
        }
    }
    return {unique.begin(), unique.end()};
}

} // namespace stratum::density
