// Stratum — resolved density function graphs.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum/data/pack.hpp>
#include <stratum/data/registry.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/graph.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
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

[[nodiscard]] const TypeInfo& infoOf(NodeType type) {
    return kTypeTable[static_cast<std::size_t>(type)];
}

/// Whether @p type names a row of the table at all. A NodeType that came out
/// of a file rather than out of the resolver can be any byte, and indexing
/// the table with one of those aborts in a checked build — which is how this
/// was found: the freeze reader's own validation was the thing that crashed
/// on the input it was validating.
[[nodiscard]] bool isKnownType(NodeType type) noexcept {
    return static_cast<std::size_t>(type) < kTypeTable.size();
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
    [[noreturn]] void fail(const std::string& what) const {
        std::string chain;
        for (const data::ResourceLocation& id : stack_) {
            chain += (chain.empty() ? "" : " -> ") + id.toString();
        }
        throw ResolveError(chain.empty() ? what : chain + ": " + what);
    }

    /// Resolves a density function named by identifier, or returns the node
    /// it already resolved to.
    NodeIndex resolveReference(Graph& graph, const data::ResourceLocation& id) {
        if (const auto found = resolved_.find(id); found != resolved_.end()) {
            return found->second;
        }

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

        stack_.push_back(id);
        const NodeIndex index = resolveValue(graph, entry->json);
        stack_.pop_back();

        resolved_.emplace(id, index);
        return index;
    }

    /// A density function is a number, an identifier, or an inline object.
    /// Public because the builder resolves the density functions written
    /// inline in a noise settings router through it.
public:
    NodeIndex resolveValue(Graph& graph, const nlohmann::json& value) {
        if (value.is_number()) {
            // The documented shorthand: a bare number is a constant.
            return addNode(graph, Node{.type = NodeType::Constant,
                                       .arguments = {},
                                       .parameters = {value.get<double>()},
                                       .noise = std::nullopt,
                                       .spline = std::nullopt,
                                       .selector = {}});
        }
        if (value.is_string()) {
            return resolveReference(graph, parseId(value.get<std::string>()));
        }
        if (!value.is_object()) {
            fail("a density function must be a number, an identifier or an object, not " +
                 std::string(value.type_name()));
        }
        return resolveObject(graph, value);
    }

private:
    NodeIndex resolveObject(Graph& graph, const nlohmann::json& object) {
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

        Node node;
        node.type = *type;
        for (const SchemaField& field : infoOf(*type).fields) {
            if (!object.contains(field.name)) {
                // The schema marks some fields optional, with a documented
                // default; the rest are required.
                if (field.optional) {
                    continue;
                }
                fail("'" + typeName + "' is missing its \"" + std::string(field.name) + "\"");
            }
            const nlohmann::json& fieldValue = object.at(field.name);

            switch (field.kind) {
                case FieldKind::Function:
                    // Some fields take a nested function but not a reference
                    // to one — clamp's input, at this version. Only the
                    // schema records that distinction.
                    if (fieldValue.is_string() && !field.allowsReference) {
                        fail("'" + typeName + "' does not accept an identifier for \"" +
                             std::string(field.name) +
                             "\" at this version; it must be given inline");
                    }
                    node.arguments.push_back(resolveValue(graph, fieldValue));
                    break;
                case FieldKind::Number:
                    if (!fieldValue.is_number()) {
                        fail("'" + typeName + "' expects \"" + std::string(field.name) +
                             "\" to be a number, not " + std::string(fieldValue.type_name()));
                    }
                    node.parameters.push_back(fieldValue.get<double>());
                    break;
                case FieldKind::NoiseRef:
                    if (!fieldValue.is_string()) {
                        fail("'" + typeName + "' expects \"" + std::string(field.name) +
                             "\" to name a noise, not " + std::string(fieldValue.type_name()));
                    }
                    node.noise = parseId(fieldValue.get<std::string>());
                    break;
                case FieldKind::Selector: {
                    if (!fieldValue.is_string()) {
                        fail("'" + typeName + "' expects \"" + std::string(field.name) +
                             "\" to be a string");
                    }
                    node.selector = fieldValue.get<std::string>();
                    if (std::ranges::find(field.selectorValues, node.selector) ==
                        field.selectorValues.end()) {
                        fail("'" + typeName + "' has an unknown " + std::string(field.name) + " '" +
                             node.selector + "'");
                    }
                    break;
                }
                case FieldKind::Spline:
                    node.spline = resolveSpline(graph, fieldValue);
                    break;
            }
        }
        return addNode(graph, std::move(node));
    }

    SplineIndex resolveSpline(Graph& graph, const nlohmann::json& value) {
        if (!value.is_object()) {
            fail("a spline must be an object, not " + std::string(value.type_name()));
        }
        if (!value.contains("coordinate") || !value.contains("points")) {
            fail(R"(a spline needs both "coordinate" and "points")");
        }

        SplineDefinition definition;
        definition.coordinate = resolveValue(graph, value.at("coordinate"));

        const nlohmann::json& points = value.at("points");
        if (!points.is_array()) {
            fail(R"(a spline's "points" must be an array)");
        }

        double previousLocation = 0.0;
        bool first = true;
        for (const nlohmann::json& point : points) {
            if (!point.is_object() || !point.contains("location") || !point.contains("value") ||
                !point.contains("derivative")) {
                fail(R"(a spline point needs "location", "value" and "derivative")");
            }

            SplinePoint resolved;
            resolved.location = point.at("location").get<double>();
            resolved.derivative = point.at("derivative").get<double>();

            // Interpolation walks the points in order; out-of-order points
            // would silently sample the wrong segment.
            if (!first && resolved.location < previousLocation) {
                fail("a spline's points are not in ascending order of location");
            }
            previousLocation = resolved.location;
            first = false;

            const nlohmann::json& pointValue = point.at("value");
            if (pointValue.is_number()) {
                resolved.value = pointValue.get<double>();
            } else {
                resolved.nested = resolveSpline(graph, pointValue);
            }
            definition.points.push_back(resolved);
        }

        if (definition.points.empty()) {
            fail("a spline has no points");
        }

        graph.splines_.push_back(std::move(definition));
        return static_cast<SplineIndex>(graph.splines_.size() - 1);
    }

    [[nodiscard]] data::ResourceLocation parseId(const std::string& text) const {
        try {
            return data::ResourceLocation::parse(text);
        } catch (const data::ResourceLocationError& error) {
            fail(error.what());
        }
    }

    [[nodiscard]] static NodeIndex addNode(Graph& graph, Node node) {
        graph.nodes_.push_back(std::move(node));
        return static_cast<NodeIndex>(graph.nodes_.size() - 1);
    }

    const data::Pack& pack_;
    std::map<data::ResourceLocation, NodeIndex> resolved_;
    std::vector<data::ResourceLocation> stack_;
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
    graph_.nodes_.push_back(std::move(node));
    return next;
}

SplineIndex Graph::Assembler::addSpline(SplineDefinition spline) {
    const auto next = static_cast<SplineIndex>(graph_.splines_.size());
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

} // namespace stratum::density
