// Stratum — a dimension's surface rules, resolved.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum/surface/rule_graph.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace stratum::surface {

namespace {

constexpr std::array<std::string_view, 4> kRuleNames = {"minecraft:sequence", "minecraft:condition",
                                                        "minecraft:block", "minecraft:bandlands"};

constexpr std::array<std::string_view, 11> kConditionNames = {"minecraft:biome",
                                                              "minecraft:noise_threshold",
                                                              "minecraft:not",
                                                              "minecraft:stone_depth",
                                                              "minecraft:vertical_gradient",
                                                              "minecraft:water",
                                                              "minecraft:y_above",
                                                              "minecraft:above_preliminary_surface",
                                                              "minecraft:hole",
                                                              "minecraft:steep",
                                                              "minecraft:temperature"};

/// The position of @p name in @p names, or nothing.
///
/// An index rather than an iterator on purpose: std::array's iterator is a raw
/// pointer in libstdc++ and a class type in MSVC's library, so
/// `const auto found` and `const auto* const found` are each correct on one
/// and a compile error on the other. clang-tidy asks for the pointer spelling;
/// following it here broke both Windows legs.
[[nodiscard]] std::optional<std::size_t> indexOfName(std::span<const std::string_view> names,
                                                     std::string_view name) noexcept {
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i] == name) {
            return i;
        }
    }
    return std::nullopt;
}

[[noreturn]] void fail(const data::ResourceLocation& id, const std::string& what) {
    throw RuleError("surface rule of '" + id.toString() + "': " + what);
}

[[nodiscard]] const nlohmann::json& require(const nlohmann::json& object, const char* field,
                                            const data::ResourceLocation& id,
                                            std::string_view typeName) {
    if (!object.contains(field)) {
        fail(id, "'" + std::string(typeName) + "' has no \"" + field + "\"");
    }
    return object.at(field);
}

/// The same shape settings::loadAll reads for `default_block`: a "Name" and
/// an optional "Properties" of strings. Written again here rather than shared
/// because the settings loader's version reports errors against a settings
/// FIELD, and this one has to report against a rule.
[[nodiscard]] settings::BlockState readBlockState(const nlohmann::json& json,
                                                  const data::ResourceLocation& id,
                                                  std::string_view typeName) {
    if (!json.is_object() || !json.contains("Name")) {
        fail(id, "'" + std::string(typeName) +
                     "' needs \"result_state\" to be an object with a "
                     "\"Name\"");
    }
    const nlohmann::json& name = json.at("Name");
    if (!name.is_string()) {
        fail(id, "'" + std::string(typeName) + "' needs \"Name\" to be an identifier");
    }
    settings::BlockState state;
    state.name = data::ResourceLocation::parse(name.get<std::string>());
    if (json.contains("Properties")) {
        const nlohmann::json& properties = json.at("Properties");
        if (!properties.is_object()) {
            fail(id, "'" + std::string(typeName) + "' needs \"Properties\" to be an object");
        }
        for (const auto& [key, value] : properties.items()) {
            if (!value.is_string()) {
                fail(id, "'" + std::string(typeName) + "' needs the property \"" + key +
                             "\" to be a string, as vanilla writes them");
            }
            state.properties.emplace(key, value.get<std::string>());
        }
    }
    return state;
}

[[nodiscard]] VerticalAnchor readAnchor(const nlohmann::json& json,
                                        const data::ResourceLocation& id, const char* where) {
    if (!json.is_object() || json.size() != 1) {
        fail(id,
             std::string(where) +
                 " must be an object naming exactly one of absolute, above_bottom or below_top");
    }
    // json.begin(), not *json.items().begin(). items() returns a proxy object
    // that dies at the end of the full expression, so binding into it leaves
    // both halves dangling — which debug builds happened to survive and the
    // release legs did not.
    const auto entry = json.begin();
    const std::string& key = entry.key();
    const nlohmann::json& value = entry.value();
    if (!value.is_number_integer()) {
        fail(id, std::string(where) + "'s " + key + " must be a whole number of blocks");
    }
    VerticalAnchor anchor;
    anchor.value = value.get<std::int32_t>();
    if (key == "absolute") {
        anchor.kind = VerticalAnchor::Kind::Absolute;
    } else if (key == "above_bottom") {
        anchor.kind = VerticalAnchor::Kind::AboveBottom;
    } else if (key == "below_top") {
        anchor.kind = VerticalAnchor::Kind::BelowTop;
    } else {
        fail(id, std::string(where) + " names '" + key +
                     "', which is not a vertical anchor; the three are absolute, above_bottom "
                     "and below_top");
    }
    return anchor;
}

} // namespace

std::int32_t VerticalAnchor::resolve(const settings::NoiseGeometry& geometry) const noexcept {
    switch (kind) {
        case Kind::AboveBottom:
            return geometry.minY + value;
        case Kind::BelowTop:
            // The top block is minY + height - 1, so "below_top: 0" is it.
            return (geometry.minY + geometry.height - 1) - value;
        case Kind::Absolute:
        default:
            return value;
    }
}

std::string_view ruleTypeName(RuleType type) noexcept {
    return kRuleNames[static_cast<std::size_t>(type)];
}

std::string_view conditionTypeName(ConditionType type) noexcept {
    return kConditionNames[static_cast<std::size_t>(type)];
}

std::optional<std::string_view> RuleGraph::unrunnableReason(RuleType type) noexcept {
    if (type == RuleType::Bandlands) {
        return "it paints the mesa banding, and while a probe shows which terracotta it places "
               "(SPEC §11) the order the bands run in is not settled and nothing documents it";
    }
    return std::nullopt;
}

bool verticalGradientFires(const rng::PositionalSource& source, const std::int32_t x,
                           const std::int32_t y, const std::int32_t z,
                           const std::int32_t trueAtAndBelow,
                           const std::int32_t falseAtAndAbove) noexcept {
    if (y <= trueAtAndBelow) {
        return true;
    }
    if (y >= falseAtAndAbove) {
        return false;
    }
    const double span = static_cast<double>(falseAtAndAbove) - static_cast<double>(trueAtAndBelow);
    const double probability =
        (static_cast<double>(falseAtAndAbove) - static_cast<double>(y)) / span;
    rng::Xoroshiro128PlusPlus draw = source.at(x, y, z);
    return static_cast<double>(draw.nextFloat()) < probability;
}

std::optional<std::string_view> RuleGraph::unrunnableReason(ConditionType type) noexcept {
    switch (type) {
        case ConditionType::Temperature:
            // The height sweep that was missing has now been run, and it
            // REFUTES the flat-threshold reading rather than confirming it.
            // The condition fires above a height that moves with the biome's
            // temperature at about eight blocks per 0.01 — y = -46, 58, 66, 74
            // and 186 for temperatures 0.15, 0.29, 0.30, 0.31 and 0.45. So it
            // is a height-adjusted temperature compared against a fixed
            // threshold, and the adjustment carries a per-column term the
            // sweep can see but has not yet separated.
            return "it compares a HEIGHT-ADJUSTED temperature, not the biome's own: the firing "
                   "height moves about eight blocks per 0.01 of biome temperature, and the "
                   "per-column term in that adjustment is not yet derived (SPEC §11)";
        case ConditionType::Hole:
        case ConditionType::NoiseThreshold:
        case ConditionType::Steep:
        case ConditionType::StoneDepth:
        case ConditionType::Water:
        case ConditionType::YAbove:
        case ConditionType::AbovePreliminarySurface:
        case ConditionType::Not:
        case ConditionType::VerticalGradient:
        default:
            return std::nullopt;
    }
}

const Rule& RuleGraph::rule(RuleIndex index) const {
    if (index >= rules_.size()) {
        throw RuleError("rule " + std::to_string(index) + " is outside this graph");
    }
    return rules_[index];
}

const Condition& RuleGraph::condition(ConditionIndex index) const {
    if (index >= conditions_.size()) {
        throw RuleError("condition " + std::to_string(index) + " is outside this graph");
    }
    return conditions_[index];
}

std::vector<data::ResourceLocation> RuleGraph::referencedNoises() const {
    std::vector<data::ResourceLocation> named;
    for (const Condition& entry : conditions_) {
        if (entry.noise.has_value()) {
            named.push_back(*entry.noise);
        }
    }
    std::ranges::sort(named);
    named.erase(std::ranges::unique(named).begin(), named.end());
    return named;
}

std::vector<std::string> RuleGraph::unrunnable() const {
    std::vector<std::string> names;
    for (const Rule& entry : rules_) {
        if (unrunnableReason(entry.type).has_value()) {
            names.emplace_back(ruleTypeName(entry.type));
        }
    }
    for (const Condition& entry : conditions_) {
        if (unrunnableReason(entry.type).has_value()) {
            names.emplace_back(conditionTypeName(entry.type));
        }
    }
    std::ranges::sort(names);
    names.erase(std::ranges::unique(names).begin(), names.end());
    return names;
}

namespace {

/// Resolution is two mutually recursive walks over the JSON, appending as it
/// goes. A surface rule is a tree — vanilla shares nothing between branches —
/// so there is no deduplication here and no need for one.
struct Resolver {
    const data::ResourceLocation& id;
    std::vector<Rule>& rules;
    std::vector<Condition>& conditions;

    [[nodiscard]] RuleIndex readRule(const nlohmann::json& json);
    [[nodiscard]] ConditionIndex readCondition(const nlohmann::json& json);
};

RuleIndex Resolver::readRule(const nlohmann::json& json) {
    if (!json.is_object() || !json.contains("type") || !json.at("type").is_string()) {
        fail(id, "a rule must be an object with a string \"type\"");
    }
    const auto name = json.at("type").get<std::string>();
    const std::optional<std::size_t> found = indexOfName(kRuleNames, name);
    if (!found.has_value()) {
        fail(id, "'" + name +
                     "' is not a surface rule this build knows; the four are sequence, condition, "
                     "block and bandlands");
    }

    Rule entry;
    entry.type = static_cast<RuleType>(*found);
    switch (entry.type) {
        case RuleType::Sequence: {
            const nlohmann::json& list = require(json, "sequence", id, name);
            if (!list.is_array()) {
                fail(id, "'" + name + "' needs \"sequence\" to be an array");
            }
            for (const nlohmann::json& child : list) {
                entry.sequence.push_back(readRule(child));
            }
            break;
        }
        case RuleType::Condition:
            entry.condition = readCondition(require(json, "if_true", id, name));
            entry.thenRun = readRule(require(json, "then_run", id, name));
            break;
        case RuleType::Block:
            entry.block = readBlockState(require(json, "result_state", id, name), id, name);
            break;
        case RuleType::Bandlands:
            break;
    }
    rules.push_back(std::move(entry));
    return static_cast<RuleIndex>(rules.size() - 1);
}

ConditionIndex Resolver::readCondition(const nlohmann::json& json) {
    if (!json.is_object() || !json.contains("type") || !json.at("type").is_string()) {
        fail(id, "a condition must be an object with a string \"type\"");
    }
    const auto name = json.at("type").get<std::string>();
    const std::optional<std::size_t> found = indexOfName(kConditionNames, name);
    if (!found.has_value()) {
        fail(id, "'" + name + "' is not a surface rule condition this build knows");
    }

    Condition entry;
    entry.type = static_cast<ConditionType>(*found);
    switch (entry.type) {
        case ConditionType::Biome: {
            const nlohmann::json& list = require(json, "biome_is", id, name);
            if (!list.is_array() || list.empty()) {
                fail(id, "'" + name + "' needs \"biome_is\" to be a non-empty array");
            }
            for (const nlohmann::json& biome : list) {
                if (!biome.is_string()) {
                    fail(id, "'" + name + "' needs every \"biome_is\" entry to be an identifier");
                }
                entry.biomes.push_back(data::ResourceLocation::parse(biome.get<std::string>()));
            }
            break;
        }
        case ConditionType::NoiseThreshold:
            entry.noise =
                data::ResourceLocation::parse(require(json, "noise", id, name).get<std::string>());
            entry.minThreshold = require(json, "min_threshold", id, name).get<double>();
            entry.maxThreshold = require(json, "max_threshold", id, name).get<double>();
            break;
        case ConditionType::Not:
            entry.invert = readCondition(require(json, "invert", id, name));
            break;
        case ConditionType::StoneDepth:
            entry.offset = require(json, "offset", id, name).get<std::int32_t>();
            entry.addSurfaceDepth = require(json, "add_surface_depth", id, name).get<bool>();
            entry.secondaryDepthRange =
                require(json, "secondary_depth_range", id, name).get<std::int32_t>();
            entry.surfaceType = require(json, "surface_type", id, name).get<std::string>();
            if (entry.surfaceType != "floor" && entry.surfaceType != "ceiling") {
                fail(id, "'" + name + "' has surface_type '" + entry.surfaceType +
                             "'; the two are floor and ceiling");
            }
            break;
        case ConditionType::VerticalGradient:
            entry.randomName = require(json, "random_name", id, name).get<std::string>();
            entry.trueAtAndBelow =
                readAnchor(require(json, "true_at_and_below", id, name), id, "true_at_and_below");
            entry.falseAtAndAbove =
                readAnchor(require(json, "false_at_and_above", id, name), id, "false_at_and_above");
            break;
        case ConditionType::Water:
            entry.offset = require(json, "offset", id, name).get<std::int32_t>();
            entry.surfaceDepthMultiplier =
                require(json, "surface_depth_multiplier", id, name).get<std::int32_t>();
            entry.addStoneDepth = require(json, "add_stone_depth", id, name).get<bool>();
            break;
        case ConditionType::YAbove:
            entry.anchor = readAnchor(require(json, "anchor", id, name), id, "anchor");
            entry.surfaceDepthMultiplier =
                require(json, "surface_depth_multiplier", id, name).get<std::int32_t>();
            entry.addStoneDepth = require(json, "add_stone_depth", id, name).get<bool>();
            break;
        case ConditionType::AbovePreliminarySurface:
        case ConditionType::Hole:
        case ConditionType::Steep:
        case ConditionType::Temperature:
            // No fields. All four are absent from mcdoc entirely, which is
            // why the schema here is written out (see the header).
            break;
    }
    conditions.push_back(std::move(entry));
    return static_cast<ConditionIndex>(conditions.size() - 1);
}

} // namespace

RuleGraph RuleGraph::resolve(const nlohmann::json& json, const data::ResourceLocation& id) {
    RuleGraph graph;
    Resolver resolver{.id = id, .rules = graph.rules_, .conditions = graph.conditions_};
    graph.root_ = resolver.readRule(json);
    return graph;
}

} // namespace stratum::surface
