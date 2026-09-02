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
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace stratum::density {

namespace {

struct TypeInfo {
    NodeType type;
    std::string_view name;
    std::vector<Field> fields;
};

/// The node set, and each type's fields in declaration order. Derived from
/// vanilla's own density functions: every entry here is exercised by data in
/// the pinned version, and nothing is listed that was not observed.
[[nodiscard]] const std::vector<TypeInfo>& typeTable() {
    static const std::vector<TypeInfo> kTable = {
        {NodeType::Constant, "minecraft:constant", {{"argument", FieldKind::Number}}},
        {NodeType::Add,
         "minecraft:add",
         {{"argument1", FieldKind::Function}, {"argument2", FieldKind::Function}}},
        {NodeType::Mul,
         "minecraft:mul",
         {{"argument1", FieldKind::Function}, {"argument2", FieldKind::Function}}},
        {NodeType::Min,
         "minecraft:min",
         {{"argument1", FieldKind::Function}, {"argument2", FieldKind::Function}}},
        {NodeType::Max,
         "minecraft:max",
         {{"argument1", FieldKind::Function}, {"argument2", FieldKind::Function}}},
        {NodeType::Abs, "minecraft:abs", {{"argument", FieldKind::Function}}},
        {NodeType::Cube, "minecraft:cube", {{"argument", FieldKind::Function}}},
        {NodeType::HalfNegative, "minecraft:half_negative", {{"argument", FieldKind::Function}}},
        {NodeType::QuarterNegative,
         "minecraft:quarter_negative",
         {{"argument", FieldKind::Function}}},
        {NodeType::Clamp,
         "minecraft:clamp",
         {{"input", FieldKind::Function}, {"min", FieldKind::Number}, {"max", FieldKind::Number}}},
        {NodeType::RangeChoice,
         "minecraft:range_choice",
         {{"input", FieldKind::Function},
          {"min_inclusive", FieldKind::Number},
          {"max_exclusive", FieldKind::Number},
          {"when_in_range", FieldKind::Function},
          {"when_out_of_range", FieldKind::Function}}},
        {NodeType::Noise,
         "minecraft:noise",
         {{"noise", FieldKind::NoiseRef},
          {"xz_scale", FieldKind::Number},
          {"y_scale", FieldKind::Number}}},
        {NodeType::ShiftedNoise,
         "minecraft:shifted_noise",
         {{"noise", FieldKind::NoiseRef},
          {"shift_x", FieldKind::Function},
          {"shift_y", FieldKind::Function},
          {"shift_z", FieldKind::Function},
          {"xz_scale", FieldKind::Number},
          {"y_scale", FieldKind::Number}}},
        // shift_a and shift_b take a noise, not a nested function — the one
        // place where `argument` does not mean what it does elsewhere.
        {NodeType::ShiftA, "minecraft:shift_a", {{"argument", FieldKind::NoiseRef}}},
        {NodeType::ShiftB, "minecraft:shift_b", {{"argument", FieldKind::NoiseRef}}},
        {NodeType::BlendAlpha, "minecraft:blend_alpha", {}},
        {NodeType::BlendOffset, "minecraft:blend_offset", {}},
        {NodeType::OldBlendedNoise,
         "minecraft:old_blended_noise",
         {{"xz_scale", FieldKind::Number},
          {"y_scale", FieldKind::Number},
          {"xz_factor", FieldKind::Number},
          {"y_factor", FieldKind::Number},
          {"smear_scale_multiplier", FieldKind::Number}}},
        {NodeType::FlatCache, "minecraft:flat_cache", {{"argument", FieldKind::Function}}},
        {NodeType::Cache2d, "minecraft:cache_2d", {{"argument", FieldKind::Function}}},
        {NodeType::CacheOnce, "minecraft:cache_once", {{"argument", FieldKind::Function}}},
        {NodeType::Interpolated, "minecraft:interpolated", {{"argument", FieldKind::Function}}},
        {NodeType::Spline, "minecraft:spline", {{"spline", FieldKind::Spline}}},
        {NodeType::YClampedGradient,
         "minecraft:y_clamped_gradient",
         {{"from_y", FieldKind::Number},
          {"to_y", FieldKind::Number},
          {"from_value", FieldKind::Number},
          {"to_value", FieldKind::Number}}},
        {NodeType::WeirdScaledSampler,
         "minecraft:weird_scaled_sampler",
         {{"input", FieldKind::Function},
          {"noise", FieldKind::NoiseRef},
          {"rarity_value_mapper", FieldKind::Selector}}},
        {NodeType::EndIslands, "minecraft:end_islands", {}},
    };
    return kTable;
}

[[nodiscard]] const TypeInfo& infoOf(NodeType type) {
    return typeTable()[static_cast<std::size_t>(type)];
}

/// rarity_value_mapper's permitted values, as vanilla's data uses them.
constexpr std::array<std::string_view, 2> kRarityValueMappers = {"type_1", "type_2"};

} // namespace

std::string_view nodeTypeName(NodeType type) noexcept {
    return infoOf(type).name;
}

std::optional<NodeType> nodeTypeFromName(std::string_view name) noexcept {
    for (const TypeInfo& info : typeTable()) {
        if (info.name == name) {
            return info.type;
        }
    }
    return std::nullopt;
}

std::vector<Field> fieldsOf(NodeType type) {
    return infoOf(type).fields;
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
        for (const Field& field : infoOf(*type).fields) {
            if (!object.contains(field.name)) {
                fail("'" + typeName + "' is missing its \"" + std::string(field.name) + "\"");
            }
            const nlohmann::json& fieldValue = object.at(field.name);

            switch (field.kind) {
                case FieldKind::Function:
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
                    if (std::ranges::find(kRarityValueMappers, node.selector) ==
                        kRarityValueMappers.end()) {
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

Graph Graph::resolveAll(const data::Pack& pack) {
    Graph graph;
    Resolver resolver(pack);
    resolver.resolveEverything(graph);
    return graph;
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
