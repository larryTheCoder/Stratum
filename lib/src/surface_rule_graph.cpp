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

template<typename Type>
struct TypeInfo {
    Type type;
    std::string_view name;
    std::span<const SchemaField> fields;
};

// The schema itself, generated from mcdoc for the pinned version by
// tools/mcdoc-sync. Hand-editing it would defeat the point: the schema is
// authoritative about which types exist and what fields they take.
#include "surface_schema.inc"

/// Whether row i of @p table is the type whose enumerator is i.
///
/// The generator emits the enumerators and the table rows from one sorted
/// list, so the two agree by construction — and this is what makes "by
/// construction" a thing the compiler checks rather than a thing a comment
/// claims, because every lookup below indexes the table with an enum value.
template<typename Type, std::size_t Count>
[[nodiscard]] constexpr bool rowsMatchEnumerators(const std::array<TypeInfo<Type>, Count>& table) {
    for (std::size_t i = 0; i < Count; ++i) {
        if (static_cast<std::size_t>(table[i].type) != i) {
            return false;
        }
    }
    return true;
}

static_assert(rowsMatchEnumerators(kRuleTable), "kRuleTable is not in RuleType order");
static_assert(rowsMatchEnumerators(kConditionTable),
              "kConditionTable is not in ConditionType order");

/// Whether @p type names a row of @p table at all. A type that came out of a
/// file rather than out of the resolver can be any byte, and indexing the
/// table with one of those aborts in a checked build.
template<typename Type, std::size_t Count>
[[nodiscard]] bool isKnownType(const std::array<TypeInfo<Type>, Count>& table, Type type) noexcept {
    return static_cast<std::size_t>(type) < table.size();
}

template<typename Type, std::size_t Count>
[[nodiscard]] std::optional<Type> typeFromName(const std::array<TypeInfo<Type>, Count>& table,
                                               std::string_view name) noexcept {
    for (const TypeInfo<Type>& info : table) {
        if (info.name == name) {
            return info.type;
        }
    }
    return std::nullopt;
}

/// "floor, ceiling" — for an error that has to say what was allowed.
[[nodiscard]] std::string joinValues(std::span<const std::string_view> values) {
    std::string joined;
    for (const std::string_view value : values) {
        joined += (joined.empty() ? "" : ", ") + std::string(value);
    }
    return joined;
}

[[noreturn]] void fail(const data::ResourceLocation& id, const std::string& what) {
    throw RuleError("surface rule of '" + id.toString() + "': " + what);
}

/// The same shape settings::loadAll reads for `default_block`: a "Name" and
/// an optional "Properties" of strings. Written again here rather than shared
/// because the settings loader's version reports errors against a settings
/// FIELD, and this one has to report against a rule.
[[nodiscard]] settings::BlockState readBlockState(const nlohmann::json& json,
                                                  const data::ResourceLocation& id,
                                                  std::string_view typeName,
                                                  std::string_view fieldName) {
    if (!json.is_object() || !json.contains("Name")) {
        fail(id, "'" + std::string(typeName) + "' needs \"" + std::string(fieldName) +
                     R"(" to be an object with a "Name")");
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

/// A vertical anchor, against the spellings the schema permits.
///
/// The three are not written out here: mcdoc spells `VerticalAnchor` as a
/// union of one-field structs and the generator reads the set out of it, so
/// the 26.3 addition of `relative_to_sea_level` will arrive as a table entry
/// rather than as a bug report about a rule this build refuses.
[[nodiscard]] VerticalAnchor readAnchor(const nlohmann::json& json,
                                        const data::ResourceLocation& id, std::string_view where,
                                        std::span<const std::string_view> spellings) {
    if (!json.is_object() || json.size() != 1) {
        fail(id, std::string(where) + " must be an object naming exactly one of " +
                     joinValues(spellings));
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
    if (std::ranges::find(spellings, key) == spellings.end()) {
        fail(id, std::string(where) + " names '" + key +
                     "', which is not a vertical anchor; the spellings are " +
                     joinValues(spellings));
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
        // The schema permits a spelling this build has no Kind for. That is
        // 26.3's `relative_to_sea_level`, whose value depends on a sea level
        // VerticalAnchor::resolve() is not given — so it is refused by name
        // rather than resolved as something else (SPEC §8). Reachable only
        // once the version pin moves.
        fail(id, std::string(where) + " names '" + key +
                     "', which the schema permits but this build cannot resolve: "
                     "VerticalAnchor::Kind has no case for it");
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
    return isKnownType(kRuleTable, type) ? kRuleTable[static_cast<std::size_t>(type)].name
                                         : "unknown";
}

std::string_view conditionTypeName(ConditionType type) noexcept {
    return isKnownType(kConditionTable, type) ? kConditionTable[static_cast<std::size_t>(type)].name
                                              : "unknown";
}

std::span<const SchemaField> fieldsOf(RuleType type) noexcept {
    return isKnownType(kRuleTable, type) ? kRuleTable[static_cast<std::size_t>(type)].fields
                                         : std::span<const SchemaField>{};
}

std::span<const SchemaField> fieldsOf(ConditionType type) noexcept {
    return isKnownType(kConditionTable, type)
               ? kConditionTable[static_cast<std::size_t>(type)].fields
               : std::span<const SchemaField>{};
}

std::optional<std::string_view> RuleGraph::unrunnableReason(RuleType /*type*/) noexcept {
    // Every rule type runs: `bandlands` was the last one, closed by the
    // clean-room provision (spec/bandlands-spec.md, SPEC §11).
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

/// One field's value, decoded the way the schema said to decode it. Which of
/// these carry meaning depends on the kind — the same arrangement
/// density::Node uses, and for the same reason: the type each field holds is
/// something the schema already names, and a variant would make every use
/// site name it a second time.
struct FieldValue {
    /// Condition, Rule, Id, and a List of any of those. A scalar of one of
    /// those kinds fills its vector with exactly one entry, so that a list
    /// and its element are read by the same code.
    std::vector<ConditionIndex> conditions;
    std::vector<RuleIndex> rules;
    std::vector<data::ResourceLocation> ids;
    settings::BlockState block;
    VerticalAnchor anchor;
    /// String and Selector.
    std::string text;
    double number = 0.0;
    std::int32_t integer = 0;
    bool boolean = false;
};

/// Resolution is two mutually recursive walks over the JSON, appending as it
/// goes. A surface rule is a tree — vanilla shares nothing between branches —
/// so there is no deduplication here and no need for one.
///
/// Each walk is driven by the generated schema rather than by a switch per
/// type: for every field the table declares, the loop checks it is there
/// unless the schema says it may be absent, reads its value the way the
/// field's KIND says to, and hands it to a binder that knows which member of
/// `Rule` or `Condition` that field's NAME feeds. So a field renamed upstream
/// stops binding and says so by name, and a field ADDED upstream — `is_3d`
/// on `noise_threshold` at 26.2, the whole of `ore_vein` at 26.3 — is a loud
/// refusal rather than a value silently dropped on the floor (SPEC §8).
struct Resolver {
    const data::ResourceLocation& id;
    std::vector<Rule>& rules;
    std::vector<Condition>& conditions;

    [[nodiscard]] RuleIndex readRule(const nlohmann::json& json);
    [[nodiscard]] ConditionIndex readCondition(const nlohmann::json& json);

    [[nodiscard]] FieldValue readField(const SchemaField& field, const nlohmann::json& value,
                                       std::string_view typeName);
    void readScalar(FieldKind kind, const SchemaField& field, const nlohmann::json& value,
                    std::string_view typeName, FieldValue& into);

    void bindRule(Rule& entry, const SchemaField& field, FieldValue&& value,
                  std::string_view typeName) const;
    void bindCondition(Condition& entry, const SchemaField& field, FieldValue&& value,
                       std::string_view typeName) const;

    /// The value of @p field, or nothing when the schema permits its absence.
    [[nodiscard]] const nlohmann::json*
    present(const SchemaField& field, const nlohmann::json& json, std::string_view typeName) const {
        if (json.contains(field.name)) {
            return &json.at(std::string(field.name));
        }
        if (field.optional) {
            return nullptr;
        }
        fail(id, "'" + std::string(typeName) + "' has no \"" + std::string(field.name) + "\"");
    }

    [[noreturn]] void wrongType(const SchemaField& field, const nlohmann::json& value,
                                std::string_view typeName, const char* wanted) const {
        fail(id, "'" + std::string(typeName) + "' needs \"" + std::string(field.name) +
                     "\" to be " + wanted + ", not " + std::string(value.type_name()));
    }

    /// The single value a scalar field read, refusing anything else.
    ///
    /// The binders below are chosen by field NAME and the value was read by
    /// field KIND, so a schema that turned one of the scalar fields into a
    /// list would hand a binder a vector of some other length. Reading off
    /// the front of an empty one is undefined; this is the refusal instead.
    template<typename Value>
    [[nodiscard]] const Value& only(const std::vector<Value>& values, const SchemaField& field,
                                    std::string_view typeName) const {
        if (values.size() != 1) {
            fail(id, "'" + std::string(typeName) + "' needs \"" + std::string(field.name) +
                         "\" to be one value, not " + std::to_string(values.size()));
        }
        return values.front();
    }
};

void Resolver::readScalar(const FieldKind kind, const SchemaField& field,
                          const nlohmann::json& value, const std::string_view typeName,
                          FieldValue& into) {
    switch (kind) {
        case FieldKind::Rule:
            // No field may be written as an identifier at the pinned version;
            // the schema, not this code, is what says so.
            if (value.is_string() && !field.allowsReference) {
                fail(id, "'" + std::string(typeName) + "' does not accept an identifier for \"" +
                             std::string(field.name) +
                             "\" at this version; it must be given inline");
            }
            into.rules.push_back(readRule(value));
            break;
        case FieldKind::Condition:
            if (value.is_string() && !field.allowsReference) {
                fail(id, "'" + std::string(typeName) + "' does not accept an identifier for \"" +
                             std::string(field.name) +
                             "\" at this version; it must be given inline");
            }
            into.conditions.push_back(readCondition(value));
            break;
        case FieldKind::BlockState:
            into.block = readBlockState(value, id, typeName, field.name);
            break;
        case FieldKind::Anchor:
            into.anchor = readAnchor(value, id, field.name, field.values);
            break;
        case FieldKind::Selector:
            if (!value.is_string()) {
                wrongType(field, value, typeName, "a string");
            }
            into.text = value.get<std::string>();
            if (std::ranges::find(field.values, into.text) == field.values.end()) {
                fail(id, "'" + std::string(typeName) + "' has an unknown " +
                             std::string(field.name) + " '" + into.text + "'; the values are " +
                             joinValues(field.values));
            }
            break;
        case FieldKind::Id:
            if (!value.is_string()) {
                wrongType(field, value, typeName, "an identifier");
            }
            into.ids.push_back(data::ResourceLocation::parse(value.get<std::string>()));
            break;
        case FieldKind::String:
            if (!value.is_string()) {
                wrongType(field, value, typeName, "a string");
            }
            into.text = value.get<std::string>();
            break;
        case FieldKind::Int:
            if (!value.is_number_integer()) {
                wrongType(field, value, typeName, "a whole number");
            }
            into.integer = value.get<std::int32_t>();
            break;
        case FieldKind::Number:
            if (!value.is_number()) {
                wrongType(field, value, typeName, "a number");
            }
            into.number = value.get<double>();
            break;
        case FieldKind::Boolean:
            if (!value.is_boolean()) {
                wrongType(field, value, typeName, "true or false");
            }
            into.boolean = value.get<bool>();
            break;
        case FieldKind::List:
            // Caught by readField before it gets here; a list of lists is not
            // a shape the generator emits.
            fail(id, "'" + std::string(typeName) + "' declares \"" + std::string(field.name) +
                         "\" as a list of lists, which this build does not read");
    }
}

FieldValue Resolver::readField(const SchemaField& field, const nlohmann::json& value,
                               const std::string_view typeName) {
    FieldValue read;
    if (field.kind != FieldKind::List) {
        readScalar(field.kind, field, value, typeName, read);
        return read;
    }
    if (!value.is_array()) {
        wrongType(field, value, typeName, "an array");
    }
    for (const nlohmann::json& element : value) {
        readScalar(field.elementKind, field, element, typeName, read);
    }
    return read;
}

void Resolver::bindRule(Rule& entry, const SchemaField& field, FieldValue&& value,
                        const std::string_view typeName) const {
    // By NAME, because a field's name is what says which member it feeds —
    // `water` and `stone_depth` both declare an `int` first and they are
    // different members. The KIND decided how the value above was read; the
    // two together are what the schema contributes.
    if (field.name == "sequence") {
        entry.sequence = std::move(value.rules);
    } else if (field.name == "if_true") {
        entry.condition = only(value.conditions, field, typeName);
    } else if (field.name == "then_run") {
        entry.thenRun = only(value.rules, field, typeName);
    } else if (field.name == "result_state") {
        entry.block = std::move(value.block);
    } else {
        // The schema declares a field this build has nowhere to put. Refused
        // by name rather than ignored: a rule read without one of its fields
        // is a rule that means something else (SPEC §8).
        fail(id, "'" + std::string(typeName) + "' declares \"" + std::string(field.name) +
                     "\", which this build does not read");
    }
}

void Resolver::bindCondition(Condition& entry, const SchemaField& field, FieldValue&& value,
                             const std::string_view typeName) const {
    if (field.name == "biome_is") {
        // The one narrowing here that the schema does not carry: mcdoc
        // declares a plain `[string]`, which permits an empty list, and this
        // has refused one since before the table existed. Kept deliberately
        // rather than widened by accident — changing what the loader accepts
        // is not what deriving the schema was for.
        if (value.ids.empty()) {
            fail(id, "'" + std::string(typeName) + "' needs \"" + std::string(field.name) +
                         "\" to be a non-empty array");
        }
        entry.biomes = std::move(value.ids);
    } else if (field.name == "noise") {
        entry.noise = only(value.ids, field, typeName);
    } else if (field.name == "min_threshold") {
        entry.minThreshold = value.number;
    } else if (field.name == "max_threshold") {
        entry.maxThreshold = value.number;
    } else if (field.name == "invert") {
        entry.invert = only(value.conditions, field, typeName);
    } else if (field.name == "offset") {
        entry.offset = value.integer;
    } else if (field.name == "surface_type") {
        entry.surfaceType = std::move(value.text);
    } else if (field.name == "add_surface_depth") {
        entry.addSurfaceDepth = value.boolean;
    } else if (field.name == "secondary_depth_range") {
        entry.secondaryDepthRange = value.integer;
    } else if (field.name == "random_name") {
        entry.randomName = std::move(value.text);
    } else if (field.name == "true_at_and_below") {
        entry.trueAtAndBelow = value.anchor;
    } else if (field.name == "false_at_and_above") {
        entry.falseAtAndAbove = value.anchor;
    } else if (field.name == "anchor") {
        entry.anchor = value.anchor;
    } else if (field.name == "surface_depth_multiplier") {
        entry.surfaceDepthMultiplier = value.integer;
    } else if (field.name == "add_stone_depth") {
        entry.addStoneDepth = value.boolean;
    } else {
        fail(id, "'" + std::string(typeName) + "' declares \"" + std::string(field.name) +
                     "\", which this build does not read");
    }
}

RuleIndex Resolver::readRule(const nlohmann::json& json) {
    if (!json.is_object() || !json.contains("type") || !json.at("type").is_string()) {
        fail(id, "a rule must be an object with a string \"type\"");
    }
    const auto name = json.at("type").get<std::string>();
    const std::optional<RuleType> found = typeFromName(kRuleTable, name);
    if (!found.has_value()) {
        fail(id, "'" + name +
                     "' is not a surface rule this build knows; the four are sequence, condition, "
                     "block and bandlands");
    }

    Rule entry;
    entry.type = *found;
    // `bandlands` has no fields, so this loop does nothing for it — which is
    // the whole of what being absent from mcdoc costs it.
    for (const SchemaField& field : fieldsOf(entry.type)) {
        const nlohmann::json* const value = present(field, json, name);
        if (value != nullptr) {
            bindRule(entry, field, readField(field, *value, name), name);
        }
    }
    rules.push_back(std::move(entry));
    return static_cast<RuleIndex>(rules.size() - 1);
}

ConditionIndex Resolver::readCondition(const nlohmann::json& json) {
    if (!json.is_object() || !json.contains("type") || !json.at("type").is_string()) {
        fail(id, "a condition must be an object with a string \"type\"");
    }
    const auto name = json.at("type").get<std::string>();
    const std::optional<ConditionType> found = typeFromName(kConditionTable, name);
    if (!found.has_value()) {
        fail(id, "'" + name + "' is not a surface rule condition this build knows");
    }

    Condition entry;
    entry.type = *found;
    // The four conditions absent from mcdoc take no fields, so the same loop
    // covers them by doing nothing.
    for (const SchemaField& field : fieldsOf(entry.type)) {
        const nlohmann::json* const value = present(field, json, name);
        if (value != nullptr) {
            bindCondition(entry, field, readField(field, *value, name), name);
        }
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
