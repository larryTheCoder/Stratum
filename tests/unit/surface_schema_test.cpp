// Stratum — the generated surface-rule schema, and the loader that reads it.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The generated tables are checked for REPRODUCIBILITY by CI, which catches a
// hand-edit to them but not a table that is faithfully derived and wrong for
// the engine — and not a loader that has stopped keeping up with one. This is
// the other half, and it asks two questions the Python generator tests cannot.
//
//   * Does the table that reached C++ say what the loader needs? A field name
//     or a field kind that drifted would be read out of the JSON as the wrong
//     thing, or looked for under a name no vanilla file writes.
//   * Does the loader bind everything the table declares? A field mcdoc grows
//     later — `is_3d` on `noise_threshold` at 26.2 — must be a loud refusal
//     rather than a value dropped on the floor (SPEC §8). The coverage test
//     below builds its input FROM the schema, so it fails the day that
//     happens rather than the day somebody notices the world is wrong.
#include <stratum/surface/rule_graph.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <map>
#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using stratum::data::ResourceLocation;
using stratum::surface::ConditionType;
using stratum::surface::FieldKind;
using stratum::surface::RuleError;
using stratum::surface::RuleGraph;
using stratum::surface::RuleType;
using stratum::surface::SchemaField;

namespace {

/// Every type of one family, by name, read off the generated table.
template<typename Type, typename Naming>
[[nodiscard]] std::vector<std::string> allTypeNames(Naming naming) {
    std::vector<std::string> names;
    for (std::size_t i = 0;; ++i) {
        const std::string_view name = naming(static_cast<Type>(i));
        if (name == "unknown") {
            return names;
        }
        names.emplace_back(name);
    }
}

/// A value the schema would accept for @p kind, built without knowing which
/// field it is for. This is what makes the coverage test below schema-driven
/// rather than a second hand-written list of vanilla's field names.
[[nodiscard]] nlohmann::json sampleFor(FieldKind kind, const SchemaField& field) {
    switch (kind) {
        case FieldKind::Rule:
            // The two simplest leaves there are: neither takes a field, so
            // neither can drag this sample into a type it is not testing.
            return nlohmann::json{{"type", "minecraft:bandlands"}};
        case FieldKind::Condition:
            return nlohmann::json{{"type", "minecraft:hole"}};
        case FieldKind::BlockState:
            return nlohmann::json{{"Name", "minecraft:stone"}};
        case FieldKind::Anchor: {
            REQUIRE_FALSE(field.values.empty());
            return nlohmann::json{{std::string(field.values.front()), 0}};
        }
        case FieldKind::Selector:
            REQUIRE_FALSE(field.values.empty());
            return nlohmann::json(std::string(field.values.front()));
        case FieldKind::Id:
        case FieldKind::String:
            return nlohmann::json("minecraft:test");
        case FieldKind::Int:
            return nlohmann::json(0);
        case FieldKind::Number:
            return nlohmann::json(0.5);
        case FieldKind::Boolean:
            return nlohmann::json(false);
        case FieldKind::List:
            // One element, not none: `biome_is` is refused empty.
            return nlohmann::json::array({sampleFor(field.elementKind, field)});
    }
    FAIL("no sample for this field kind");
}

/// An object of @p typeName carrying every field @p fields declares.
[[nodiscard]] nlohmann::json wholeOf(std::string_view typeName,
                                     std::span<const SchemaField> fields) {
    nlohmann::json object{{"type", std::string(typeName)}};
    for (const SchemaField& field : fields) {
        object[std::string(field.name)] = sampleFor(field.kind, field);
    }
    return object;
}

[[nodiscard]] RuleGraph resolve(const nlohmann::json& json) {
    return RuleGraph::resolve(json, ResourceLocation::parse("minecraft:test"));
}

/// A condition cannot be resolved on its own, so it is wrapped in the rule
/// that takes one.
[[nodiscard]] nlohmann::json wrap(const nlohmann::json& condition) {
    return nlohmann::json{{"type", "minecraft:condition"},
                          {"if_true", condition},
                          {"then_run", {{"type", "minecraft:bandlands"}}}};
}

[[nodiscard]] std::map<std::string, FieldKind> shapeOf(std::span<const SchemaField> fields) {
    std::map<std::string, FieldKind> shape;
    for (const SchemaField& field : fields) {
        shape.emplace(std::string(field.name), field.kind);
    }
    return shape;
}

} // namespace

TEST_CASE("the generated tables carry every type the engine knows", "[surface][schema]") {
    // Four and eleven, ten of them from mcdoc and five hand-written because
    // mcdoc does not declare them at all.
    CHECK(allTypeNames<RuleType>(stratum::surface::ruleTypeName) ==
          std::vector<std::string>{"minecraft:bandlands", "minecraft:block", "minecraft:condition",
                                   "minecraft:sequence"});
    CHECK(allTypeNames<ConditionType>(stratum::surface::conditionTypeName) ==
          std::vector<std::string>{"minecraft:above_preliminary_surface", "minecraft:biome",
                                   "minecraft:hole", "minecraft:noise_threshold", "minecraft:not",
                                   "minecraft:steep", "minecraft:stone_depth",
                                   "minecraft:temperature", "minecraft:vertical_gradient",
                                   "minecraft:water", "minecraft:y_above"});
}

TEST_CASE("every field name and kind the loader depends on", "[surface][schema]") {
    // Pinned rather than counted. A field renamed upstream, or one whose kind
    // moved between `int` and `float`, is read out of the JSON as the wrong
    // thing — and this is the assertion that says so by name.
    CHECK(shapeOf(fieldsOf(RuleType::Block)) ==
          std::map<std::string, FieldKind>{{"result_state", FieldKind::BlockState}});
    CHECK(shapeOf(fieldsOf(RuleType::Condition)) ==
          std::map<std::string, FieldKind>{{"if_true", FieldKind::Condition},
                                           {"then_run", FieldKind::Rule}});
    CHECK(shapeOf(fieldsOf(RuleType::Sequence)) ==
          std::map<std::string, FieldKind>{{"sequence", FieldKind::List}});
    CHECK(fieldsOf(RuleType::Sequence).front().elementKind == FieldKind::Rule);

    CHECK(shapeOf(fieldsOf(ConditionType::Biome)) ==
          std::map<std::string, FieldKind>{{"biome_is", FieldKind::List}});
    CHECK(fieldsOf(ConditionType::Biome).front().elementKind == FieldKind::Id);
    CHECK(shapeOf(fieldsOf(ConditionType::NoiseThreshold)) ==
          std::map<std::string, FieldKind>{{"noise", FieldKind::Id},
                                           {"min_threshold", FieldKind::Number},
                                           {"max_threshold", FieldKind::Number}});
    CHECK(shapeOf(fieldsOf(ConditionType::Not)) ==
          std::map<std::string, FieldKind>{{"invert", FieldKind::Condition}});
    CHECK(shapeOf(fieldsOf(ConditionType::StoneDepth)) ==
          std::map<std::string, FieldKind>{{"offset", FieldKind::Int},
                                           {"surface_type", FieldKind::Selector},
                                           {"add_surface_depth", FieldKind::Boolean},
                                           {"secondary_depth_range", FieldKind::Int}});
    CHECK(shapeOf(fieldsOf(ConditionType::VerticalGradient)) ==
          std::map<std::string, FieldKind>{{"random_name", FieldKind::String},
                                           {"true_at_and_below", FieldKind::Anchor},
                                           {"false_at_and_above", FieldKind::Anchor}});
    CHECK(shapeOf(fieldsOf(ConditionType::Water)) ==
          std::map<std::string, FieldKind>{{"offset", FieldKind::Int},
                                           {"surface_depth_multiplier", FieldKind::Int},
                                           {"add_stone_depth", FieldKind::Boolean}});
    CHECK(shapeOf(fieldsOf(ConditionType::YAbove)) ==
          std::map<std::string, FieldKind>{{"anchor", FieldKind::Anchor},
                                           {"surface_depth_multiplier", FieldKind::Int},
                                           {"add_stone_depth", FieldKind::Boolean}});
}

TEST_CASE("the five types absent from mcdoc take no fields", "[surface][schema]") {
    // What is hand-written about each of them is its name, and this is the
    // assertion that it stays that little.
    CHECK(fieldsOf(RuleType::Bandlands).empty());
    for (const ConditionType type : {ConditionType::AbovePreliminarySurface, ConditionType::Hole,
                                     ConditionType::Steep, ConditionType::Temperature}) {
        CHECK(fieldsOf(type).empty());
    }
}

TEST_CASE("the closed sets of strings come from the schema", "[surface][schema]") {
    const auto valuesOf = [](std::span<const SchemaField> fields, std::string_view name) {
        std::vector<std::string> found;
        for (const SchemaField& field : fields) {
            if (field.name == name) {
                for (const std::string_view value : field.values) {
                    found.emplace_back(value);
                }
            }
        }
        return found;
    };

    CHECK(valuesOf(fieldsOf(ConditionType::StoneDepth), "surface_type") ==
          std::vector<std::string>{"floor", "ceiling"});
    // Read out of mcdoc's union of one-field structs, not written out here.
    // 26.3 adds `relative_to_sea_level` and it will arrive on its own.
    CHECK(valuesOf(fieldsOf(ConditionType::YAbove), "anchor") ==
          std::vector<std::string>{"absolute", "above_bottom", "below_top"});
}

TEST_CASE("the loader binds every field the schema declares", "[surface][schema]") {
    // Built FROM the schema, so a field added upstream reaches the loader in
    // this test on the day it is generated. The loader refuses a field it
    // cannot place, so an unbound one fails here by name rather than silently
    // going unread.
    const auto ruleNames = allTypeNames<RuleType>(stratum::surface::ruleTypeName);
    for (std::size_t i = 0; i < ruleNames.size(); ++i) {
        CAPTURE(ruleNames[i]);
        REQUIRE_NOTHROW(resolve(wholeOf(ruleNames[i], fieldsOf(static_cast<RuleType>(i)))));
    }

    const auto conditionNames = allTypeNames<ConditionType>(stratum::surface::conditionTypeName);
    for (std::size_t i = 0; i < conditionNames.size(); ++i) {
        CAPTURE(conditionNames[i]);
        REQUIRE_NOTHROW(
            resolve(wrap(wholeOf(conditionNames[i], fieldsOf(static_cast<ConditionType>(i))))));
    }
}

TEST_CASE("a field given the wrong kind of value is refused by name", "[surface][schema]") {
    // The kinds are not decoration: each one decides how the value is read
    // out of the JSON, and a value of the wrong shape is refused rather than
    // coerced into whatever nlohmann would have made of it.
    const auto stoneDepth = [](nlohmann::json offset) {
        return wrap(nlohmann::json{{"type", "minecraft:stone_depth"},
                                   {"offset", std::move(offset)},
                                   {"surface_type", "floor"},
                                   {"add_surface_depth", false},
                                   {"secondary_depth_range", 0}});
    };
    CHECK_NOTHROW(resolve(stoneDepth(3)));
    // Int, so not a string and not a fraction.
    CHECK_THROWS_WITH(resolve(stoneDepth("3")),
                      ContainsSubstring("offset") && ContainsSubstring("whole number"));
    CHECK_THROWS_WITH(resolve(stoneDepth(3.5)), ContainsSubstring("offset"));

    // Number, so a fraction is exactly what it wants.
    const auto threshold = [](nlohmann::json value) {
        return wrap(nlohmann::json{{"type", "minecraft:noise_threshold"},
                                   {"noise", "minecraft:surface"},
                                   {"min_threshold", std::move(value)},
                                   {"max_threshold", 0.5}});
    };
    CHECK_NOTHROW(resolve(threshold(-0.25)));
    CHECK_THROWS_WITH(resolve(threshold("low")),
                      ContainsSubstring("min_threshold") && ContainsSubstring("number"));

    // Id, so an identifier and not a number.
    CHECK_THROWS_WITH(resolve(wrap(nlohmann::json{{"type", "minecraft:noise_threshold"},
                                                  {"noise", 7},
                                                  {"min_threshold", 0.0},
                                                  {"max_threshold", 0.5}})),
                      ContainsSubstring("noise") && ContainsSubstring("identifier"));

    // List, so an array and not one of its elements on its own.
    CHECK_THROWS_WITH(resolve(wrap(nlohmann::json{{"type", "minecraft:biome"},
                                                  {"biome_is", "minecraft:plains"}})),
                      ContainsSubstring("biome_is") && ContainsSubstring("array"));

    // Boolean, so not the 0 and 1 a laxer reader would have taken.
    CHECK_THROWS_WITH(resolve(wrap(nlohmann::json{{"type", "minecraft:water"},
                                                  {"offset", 0},
                                                  {"surface_depth_multiplier", 0},
                                                  {"add_stone_depth", 1}})),
                      ContainsSubstring("add_stone_depth") && ContainsSubstring("true or false"));
}

TEST_CASE("an anchor spelling outside the schema's set is refused", "[surface][schema]") {
    CHECK_THROWS_WITH(resolve(wrap(nlohmann::json{{"type", "minecraft:y_above"},
                                                  {"anchor", {{"relative_to_sea_level", 4}}},
                                                  {"surface_depth_multiplier", 0},
                                                  {"add_stone_depth", false}})),
                      ContainsSubstring("relative_to_sea_level") &&
                          ContainsSubstring("above_bottom"));
}
