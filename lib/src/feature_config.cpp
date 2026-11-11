// Stratum — placed and configured features, from JSON.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Two kinds of "no" (feature/feature.hpp): data the schema does not allow is
// a FeatureError naming the entry; data that is well formed and that this
// build does not run is Unsupported, and the placed feature it belongs to
// goes unrun and listed. An unknown `type` is the second kind: the server
// knows types this build does not.

#include "feature_internal.hpp"

#include <stratum/data/json_integer.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string_view>

namespace stratum::feature::detail {
namespace {

using Json = nlohmann::json;

[[noreturn]] void fail(const std::string& id, const std::string& what) {
    throw FeatureError("placed feature " + id + ": " + what);
}

[[nodiscard]] const Json& field(const Json& object, const char* key, const std::string& id,
                                const std::string& where) {
    if (!object.is_object() || !object.contains(key)) {
        fail(id, where + " needs a '" + key + "' field");
    }
    return object.at(key);
}

[[nodiscard]] std::int32_t integer(const Json& value, const std::string& id,
                                   const std::string& where) {
    const std::optional<std::int32_t> exact =
        value.is_number_integer() ? data::exactInteger<std::int32_t>(value) : std::nullopt;
    if (!exact.has_value()) {
        fail(id, where + " must be a 32-bit whole number");
    }
    return *exact;
}

[[nodiscard]] float number(const Json& value, const std::string& id, const std::string& where) {
    if (!value.is_number()) {
        fail(id, where + " must be a number");
    }
    return value.get<float>();
}

[[nodiscard]] double real(const Json& value, const std::string& id, const std::string& where) {
    if (!value.is_number()) {
        fail(id, where + " must be a number");
    }
    return value.get<double>();
}

/// The `type` of a typed object, without its default namespace.
[[nodiscard]] std::string typeOf(const Json& object, const std::string& id,
                                 const std::string& where, const char* key = "type") {
    const Json& type = field(object, key, id, where);
    if (!type.is_string()) {
        fail(id, where + "'s " + key + " must be a string");
    }
    const std::optional<data::ResourceLocation> parsed =
        data::ResourceLocation::tryParse(type.get<std::string>());
    if (!parsed.has_value()) {
        fail(id, where + " names '" + type.get<std::string>() + "', which is not an identifier");
    }
    return parsed->toString();
}

[[nodiscard]] data::ResourceLocation identifier(const Json& value, const std::string& id,
                                                const std::string& where) {
    if (!value.is_string()) {
        fail(id, where + " must be an identifier");
    }
    const std::optional<data::ResourceLocation> parsed =
        data::ResourceLocation::tryParse(value.get<std::string>());
    if (!parsed.has_value()) {
        fail(id, where + " names '" + value.get<std::string>() + "', which is not an identifier");
    }
    return *parsed;
}

[[nodiscard]] surface::VerticalAnchor anchor(const Json& value, const std::string& id,
                                             const std::string& where) {
    if (!value.is_object() || value.size() != 1) {
        fail(id, where + " must be a vertical anchor: one of {\"absolute\": y}, "
                         "{\"above_bottom\": n}, {\"below_top\": n}");
    }
    // Key and value bound once: GCC 13 at -O3 cannot prove a second
    // `entry.value()` non-null (carver_config.cpp's anchor says the same).
    const auto entry = value.begin();
    const std::string& key = entry.key();
    const Json& inner = entry.value();
    surface::VerticalAnchor result;
    result.value = integer(inner, id, where + "'s " + key);
    if (key == "absolute") {
        result.kind = surface::VerticalAnchor::Kind::Absolute;
    } else if (key == "above_bottom") {
        result.kind = surface::VerticalAnchor::Kind::AboveBottom;
    } else if (key == "below_top") {
        result.kind = surface::VerticalAnchor::Kind::BelowTop;
    } else {
        fail(id, where + " names '" + key + "', which is not a vertical anchor");
    }
    return result;
}

/// A weighted list's entries, each `{"data": ..., "weight": n}`.
template<typename Entry, typename Parse>
void weightedEntries(const Json& list, const std::string& id, const std::string& where,
                     std::vector<Entry>& entries, std::vector<std::int32_t>& weights,
                     const Parse& parse) {
    if (!list.is_array() || list.empty()) {
        fail(id, where + " needs a non-empty list of weighted entries");
    }
    std::int64_t total = 0;
    for (const Json& entry : list) {
        const std::int32_t weight = integer(field(entry, "weight", id, where), id, where);
        if (weight < 0) {
            fail(id, where + " has a negative weight");
        }
        total += weight;
        weights.push_back(weight);
        entries.push_back(parse(field(entry, "data", id, where)));
    }
    if (total <= 0 || total > INT32_MAX) {
        fail(id, where + "'s weights must sum to a positive 32-bit number");
    }
}

[[nodiscard]] IntProvider intProvider(const Json& value, const std::string& id,
                                      const std::string& where) {
    IntProvider provider;
    if (value.is_number_integer()) {
        provider.min = provider.max = integer(value, id, where);
        return provider;
    }
    const std::string type = typeOf(value, id, where);
    const auto bounds = [&] {
        provider.min = integer(field(value, "min_inclusive", id, where), id, where);
        provider.max = integer(field(value, "max_inclusive", id, where), id, where);
        if (provider.min > provider.max) {
            fail(id, where + " needs min_inclusive at most max_inclusive");
        }
    };
    if (type == "minecraft:constant") {
        provider.min = provider.max = integer(field(value, "value", id, where), id, where);
    } else if (type == "minecraft:uniform") {
        provider.kind = IntProvider::Kind::Uniform;
        bounds();
    } else if (type == "minecraft:biased_to_bottom") {
        provider.kind = IntProvider::Kind::BiasedToBottom;
        bounds();
    } else if (type == "minecraft:clamped") {
        provider.kind = IntProvider::Kind::Clamped;
        bounds();
        provider.children.push_back(
            intProvider(field(value, "source", id, where), id, where + "'s source"));
    } else if (type == "minecraft:clamped_normal") {
        provider.kind = IntProvider::Kind::ClampedNormal;
        bounds();
        provider.mean = number(field(value, "mean", id, where), id, where);
        provider.deviation = number(field(value, "deviation", id, where), id, where);
    } else if (type == "minecraft:weighted_list") {
        provider.kind = IntProvider::Kind::WeightedList;
        weightedEntries(field(value, "distribution", id, where), id, where, provider.children,
                        provider.weights,
                        [&](const Json& data) { return intProvider(data, id, where); });
    } else {
        throw Unsupported("int provider " + type);
    }
    // A span the generator's bounded draw cannot hold is nothing vanilla's
    // data writes; refused rather than overflowed.
    if (static_cast<std::int64_t>(provider.max) - provider.min >= INT32_MAX) {
        fail(id, where + " spans more than a 32-bit draw can");
    }
    return provider;
}

[[nodiscard]] HeightProvider heightProvider(const Json& value, const std::string& id,
                                            const std::string& where) {
    HeightProvider provider;
    if (!value.is_object()) {
        fail(id, where + " must be a height provider");
    }
    if (!value.contains("type")) {
        provider.min = provider.max = anchor(value, id, where);
        return provider;
    }
    const std::string type = typeOf(value, id, where);
    const auto bounds = [&] {
        provider.min = anchor(field(value, "min_inclusive", id, where), id, where);
        provider.max = anchor(field(value, "max_inclusive", id, where), id, where);
    };
    const auto optional = [&](const char* key, const std::int32_t fallback) {
        return value.contains(key) ? integer(value.at(key), id, where + "'s " + key) : fallback;
    };
    if (type == "minecraft:constant") {
        provider.min = provider.max = anchor(field(value, "value", id, where), id, where);
    } else if (type == "minecraft:uniform") {
        provider.kind = HeightProvider::Kind::Uniform;
        bounds();
    } else if (type == "minecraft:biased_to_bottom" || type == "minecraft:very_biased_to_bottom") {
        provider.kind = type == "minecraft:biased_to_bottom"
                            ? HeightProvider::Kind::BiasedToBottom
                            : HeightProvider::Kind::VeryBiasedToBottom;
        bounds();
        provider.inner = optional("inner", 1);
        if (provider.inner < 1) {
            fail(id, where + "'s inner must be at least 1");
        }
    } else if (type == "minecraft:trapezoid") {
        provider.kind = HeightProvider::Kind::Trapezoid;
        bounds();
        provider.plateau = optional("plateau", 0);
    } else if (type == "minecraft:weighted_list") {
        provider.kind = HeightProvider::Kind::WeightedList;
        weightedEntries(field(value, "distribution", id, where), id, where, provider.children,
                        provider.weights,
                        [&](const Json& data) { return heightProvider(data, id, where); });
    } else {
        throw Unsupported("height provider " + type);
    }
    return provider;
}

[[nodiscard]] HeightmapKind heightmapKind(const Json& value, const std::string& id,
                                          const std::string& where) {
    if (!value.is_string()) {
        fail(id, where + "'s heightmap must be a string");
    }
    const std::string name = value.get<std::string>();
    if (name == "WORLD_SURFACE_WG") {
        return HeightmapKind::WorldSurfaceWg;
    }
    if (name == "OCEAN_FLOOR_WG") {
        return HeightmapKind::OceanFloorWg;
    }
    if (name == "WORLD_SURFACE") {
        return HeightmapKind::WorldSurface;
    }
    if (name == "OCEAN_FLOOR") {
        return HeightmapKind::OceanFloor;
    }
    if (name == "MOTION_BLOCKING") {
        return HeightmapKind::MotionBlocking;
    }
    if (name == "MOTION_BLOCKING_NO_LEAVES") {
        return HeightmapKind::MotionBlockingNoLeaves;
    }
    fail(id, where + " names heightmap '" + name + "', which is not one");
}

[[nodiscard]] const std::set<data::ResourceLocation>&
tagBlocks(const data::ResourceLocation& tag, const std::string& id, const FeatureSources& sources) {
    const auto found = sources.tags.find(tag);
    if (found == sources.tags.end()) {
        fail(id, "names block tag #" + tag.toString() + ", which the pack does not define");
    }
    return found->second;
}

/// `blocks`-shaped fields: one id, a list of ids, or `#tag`.
[[nodiscard]] std::set<data::ResourceLocation> blockSet(const Json& value, const std::string& id,
                                                        const std::string& where,
                                                        const FeatureSources& sources) {
    std::set<data::ResourceLocation> blocks;
    if (value.is_array()) {
        for (const Json& entry : value) {
            blocks.insert(identifier(entry, id, where));
        }
        return blocks;
    }
    if (value.is_string() && value.get<std::string>().starts_with('#')) {
        const std::optional<data::ResourceLocation> tag =
            data::ResourceLocation::tryParse(value.get<std::string>().substr(1));
        if (!tag.has_value()) {
            fail(id, where + " names '" + value.get<std::string>() + "', which is not a tag");
        }
        return tagBlocks(*tag, id, sources);
    }
    blocks.insert(identifier(value, id, where));
    return blocks;
}

[[nodiscard]] std::array<std::int32_t, 3> offsetOf(const Json& object, const std::string& id,
                                                   const std::string& where) {
    std::array<std::int32_t, 3> offset{};
    if (!object.contains("offset")) {
        return offset;
    }
    const Json& value = object.at("offset");
    if (!value.is_array() || value.size() != 3) {
        fail(id, where + "'s offset must be three whole numbers");
    }
    for (std::size_t axis = 0; axis < 3; ++axis) {
        offset.at(axis) = integer(value.at(axis), id, where + "'s offset");
        // OBSERVED: the server refuses a pack whose offset reaches 16 on any
        // axis ("Position out of range, expected at most 16").
        if (offset.at(axis) <= -16 || offset.at(axis) >= 16) {
            fail(id, where + "'s offset must stay within 15 blocks on each axis");
        }
    }
    return offset;
}

[[nodiscard]] BlockPredicate blockPredicate(const Json& value, const std::string& id,
                                            const std::string& where,
                                            const FeatureSources& sources) {
    BlockPredicate predicate;
    const std::string type = typeOf(value, id, where);
    const auto children = [&] {
        const Json& list = field(value, "predicates", id, where);
        if (!list.is_array()) {
            fail(id, where + "'s predicates must be a list");
        }
        for (const Json& child : list) {
            predicate.children.push_back(blockPredicate(child, id, where, sources));
        }
    };
    if (type == "minecraft:true") {
        return predicate;
    }
    if (type == "minecraft:matching_blocks") {
        predicate.kind = BlockPredicate::Kind::MatchingBlocks;
        predicate.offset = offsetOf(value, id, where);
        predicate.blocks = blockSet(field(value, "blocks", id, where), id, where, sources);
    } else if (type == "minecraft:matching_block_tag") {
        predicate.kind = BlockPredicate::Kind::MatchingBlocks;
        predicate.offset = offsetOf(value, id, where);
        predicate.blocks =
            tagBlocks(identifier(field(value, "tag", id, where), id, where), id, sources);
    } else if (type == "minecraft:matching_fluids") {
        predicate.kind = BlockPredicate::Kind::MatchingFluids;
        predicate.offset = offsetOf(value, id, where);
        const Json& fluids = field(value, "fluids", id, where);
        const Json list = fluids.is_array() ? fluids : Json::array({fluids});
        for (const Json& fluid : list) {
            if (!fluid.is_string()) {
                fail(id, where + "'s fluids must be identifiers");
            }
            const std::string name = fluid.get<std::string>();
            // A source block is the still fluid; the flowing ones are not
            // told apart here, and nothing this build generates flows.
            if (name == "minecraft:water" || name == "water" || name == "#minecraft:water") {
                predicate.water = true;
            } else if (name == "minecraft:lava" || name == "lava" || name == "#minecraft:lava") {
                predicate.lava = true;
            } else {
                throw Unsupported("matching_fluids of " + name);
            }
        }
    } else if (type == "minecraft:inside_world_bounds") {
        predicate.kind = BlockPredicate::Kind::InsideWorldBounds;
        predicate.offset = offsetOf(value, id, where);
    } else if (type == "minecraft:all_of") {
        predicate.kind = BlockPredicate::Kind::AllOf;
        children();
    } else if (type == "minecraft:any_of") {
        predicate.kind = BlockPredicate::Kind::AnyOf;
        children();
    } else if (type == "minecraft:not") {
        predicate.kind = BlockPredicate::Kind::Not;
        predicate.children.push_back(
            blockPredicate(field(value, "predicate", id, where), id, where, sources));
    } else {
        // solid, replaceable, has_sturdy_face, would_survive, unobstructed:
        // each asks what a block IS, which no pack file says.
        throw Unsupported("block predicate " + type);
    }
    return predicate;
}

[[nodiscard]] Modifier modifier(const Json& value, const std::string& id,
                                const FeatureSources& sources) {
    Modifier result;
    const std::string type = typeOf(value, id, "a placement modifier");
    const std::string where = "placement modifier " + type;
    if (type == "minecraft:in_square") {
        result.kind = Modifier::Kind::InSquare;
    } else if (type == "minecraft:count") {
        result.kind = Modifier::Kind::Count;
        result.count = intProvider(field(value, "count", id, where), id, where + "'s count");
    } else if (type == "minecraft:height_range") {
        result.kind = Modifier::Kind::HeightRange;
        result.height = heightProvider(field(value, "height", id, where), id, where + "'s height");
    } else if (type == "minecraft:heightmap") {
        result.kind = Modifier::Kind::Heightmap;
        result.heightmap = heightmapKind(field(value, "heightmap", id, where), id, where);
    } else if (type == "minecraft:biome") {
        result.kind = Modifier::Kind::Biome;
    } else if (type == "minecraft:block_predicate_filter") {
        result.kind = Modifier::Kind::BlockPredicateFilter;
        result.predicate = blockPredicate(field(value, "predicate", id, where), id, where, sources);
    } else if (type == "minecraft:rarity_filter") {
        result.kind = Modifier::Kind::RarityFilter;
        result.chance = integer(field(value, "chance", id, where), id, where + "'s chance");
        if (result.chance < 1) {
            fail(id, where + "'s chance must be positive");
        }
    } else if (type == "minecraft:surface_water_depth_filter") {
        result.kind = Modifier::Kind::SurfaceWaterDepthFilter;
        result.maxWaterDepth =
            integer(field(value, "max_water_depth", id, where), id, where + "'s max_water_depth");
    } else if (type == "minecraft:random_offset") {
        result.kind = Modifier::Kind::RandomOffset;
        result.xzSpread =
            intProvider(field(value, "xz_spread", id, where), id, where + "'s xz_spread");
        result.ySpread =
            intProvider(field(value, "y_spread", id, where), id, where + "'s y_spread");
    } else if (type == "minecraft:environment_scan") {
        result.kind = Modifier::Kind::EnvironmentScan;
        const Json& direction = field(value, "direction_of_search", id, where);
        if (direction != "up" && direction != "down") {
            fail(id, where + R"('s direction_of_search must be "up" or "down")");
        }
        result.scanUp = direction == "up";
        result.maxSteps = integer(field(value, "max_steps", id, where), id, where + "'s max_steps");
        if (result.maxSteps < 1 || result.maxSteps > 32) {
            fail(id, where + "'s max_steps must be 1 to 32");
        }
        result.predicate =
            blockPredicate(field(value, "target_condition", id, where), id, where, sources);
        if (value.contains("allowed_search_condition")) {
            result.hasAllowed = true;
            result.allowed =
                blockPredicate(value.at("allowed_search_condition"), id, where, sources);
        }
    } else if (type == "minecraft:surface_relative_threshold_filter") {
        result.kind = Modifier::Kind::SurfaceRelativeThresholdFilter;
        result.heightmap = heightmapKind(field(value, "heightmap", id, where), id, where);
        if (value.contains("min_inclusive")) {
            result.minInclusive = integer(value.at("min_inclusive"), id, where);
        }
        if (value.contains("max_inclusive")) {
            result.maxInclusive = integer(value.at("max_inclusive"), id, where);
        }
    } else if (type == "minecraft:fixed_placement") {
        result.kind = Modifier::Kind::FixedPlacement;
        const Json& positions = field(value, "positions", id, where);
        if (!positions.is_array()) {
            fail(id, where + "'s positions must be a list");
        }
        for (const Json& position : positions) {
            if (!position.is_array() || position.size() != 3) {
                fail(id, where + "'s positions must each be three whole numbers");
            }
            result.positions.push_back({integer(position.at(0), id, where),
                                        integer(position.at(1), id, where),
                                        integer(position.at(2), id, where)});
        }
    } else if (type == "minecraft:noise_threshold_count") {
        result.kind = Modifier::Kind::NoiseThresholdCount;
        result.noiseLevel = real(field(value, "noise_level", id, where), id, where);
        result.belowNoise = integer(field(value, "below_noise", id, where), id, where);
        result.aboveNoise = integer(field(value, "above_noise", id, where), id, where);
    } else if (type == "minecraft:noise_based_count") {
        result.kind = Modifier::Kind::NoiseBasedCount;
        result.noiseToCountRatio =
            integer(field(value, "noise_to_count_ratio", id, where), id, where);
        result.noiseFactor = real(field(value, "noise_factor", id, where), id, where);
        if (value.contains("noise_offset")) {
            result.noiseOffset = real(value.at("noise_offset"), id, where);
        }
        // The simplex lattice holds |coordinate| below 2^29 (noise/perlin.hpp);
        // a factor of 4 or more keeps every int32 block coordinate inside it.
        if (!(std::abs(result.noiseFactor) >= 4.0)) {
            throw Unsupported("noise_based_count with a noise_factor nearer zero than 4, which "
                              "carries the count noise past its lattice");
        }
    } else {
        // count_on_every_layer: not yet measured (the Nether's features use
        // it alone).
        throw Unsupported("placement modifier " + type);
    }
    return result;
}

[[nodiscard]] settings::BlockState blockState(const Json& value, const std::string& id,
                                              const std::string& where) {
    settings::BlockState state;
    state.name = identifier(field(value, "Name", id, where), id, where + "'s Name");
    if (value.contains("Properties")) {
        const Json& properties = value.at("Properties");
        if (!properties.is_object()) {
            fail(id, where + "'s Properties must be an object");
        }
        for (const auto& [key, property] : properties.items()) {
            if (!property.is_string()) {
                std::string what = where;
                what += "'s property ";
                what += key;
                what += " must be a string";
                fail(id, what);
            }
            state.properties.emplace(key, property.get<std::string>());
        }
    }
    return state;
}

[[nodiscard]] StateProvider stateProvider(const Json& value, const std::string& id,
                                          const std::string& where) {
    StateProvider provider;
    const std::string type = typeOf(value, id, where);
    if (type == "minecraft:simple_state_provider") {
        provider.states.push_back(blockState(field(value, "state", id, where), id, where));
    } else if (type == "minecraft:weighted_state_provider") {
        provider.kind = StateProvider::Kind::Weighted;
        weightedEntries(field(value, "entries", id, where), id, where, provider.states,
                        provider.weights,
                        [&](const Json& data) { return blockState(data, id, where); });
    } else {
        throw Unsupported("block state provider " + type);
    }
    return provider;
}

/// Whether a block stays wherever it is put — the only blocks simple_block
/// writes until a block's own survival rule is measured. The feature probes'
/// marker blocks, every one a full cube with no support rule.
[[nodiscard]] bool staysAnywhere(const data::ResourceLocation& block) {
    static constexpr std::array<std::string_view, 4> kSuffixes = {"_wool", "_concrete",
                                                                  "_terracotta", "_stained_glass"};
    const std::string_view path = block.path();
    return block.namespaceName() == data::ResourceLocation::kDefaultNamespace &&
           std::ranges::any_of(
               kSuffixes, [&](const std::string_view suffix) { return path.ends_with(suffix); });
}

[[nodiscard]] RuleTest ruleTest(const Json& value, const std::string& id, const std::string& where,
                                const FeatureSources& sources) {
    RuleTest test;
    const std::string type = typeOf(value, id, where + "'s target", "predicate_type");
    if (type == "minecraft:always_true") {
        return test;
    }
    test.kind = RuleTest::Kind::Blocks;
    if (type == "minecraft:block_match") {
        test.blocks.insert(identifier(field(value, "block", id, where), id, where));
    } else if (type == "minecraft:tag_match") {
        test.blocks = tagBlocks(identifier(field(value, "tag", id, where), id, where), id, sources);
    } else {
        // blockstate_match, and the two random tests, which draw.
        throw Unsupported("rule test " + type);
    }
    return test;
}

[[nodiscard]] ConfiguredFeature configuredFeature(const Json& value, const std::string& id,
                                                  const FeatureSources& sources) {
    ConfiguredFeature feature;
    const std::string where = "its configured feature";
    const std::string type = typeOf(value, id, where);
    const Json& config = field(value, "config", id, where);
    if (type == "minecraft:simple_block") {
        feature.type = ConfiguredFeature::Type::SimpleBlock;
        feature.toPlace = stateProvider(field(config, "to_place", id, where), id, where);
        for (const settings::BlockState& state : feature.toPlace.states) {
            if (!staysAnywhere(state.name)) {
                throw Unsupported("simple_block of " + state.name.toString() +
                                  ", whose survival where it is placed is not measured");
            }
        }
    } else if (type == "minecraft:ore") {
        feature.type = ConfiguredFeature::Type::Ore;
        feature.size = integer(field(config, "size", id, where), id, where + "'s size");
        if (feature.size < 0 || feature.size > 64) {
            fail(id, where + "'s size must be 0 to 64");
        }
        feature.discardChance =
            number(field(config, "discard_chance_on_air_exposure", id, where), id, where);
        if (!(feature.discardChance >= 0.0F) || !(feature.discardChance <= 1.0F)) {
            fail(id, where + "'s discard_chance_on_air_exposure must be 0 to 1");
        }
        const Json& targets = field(config, "targets", id, where);
        if (!targets.is_array()) {
            fail(id, where + "'s targets must be a list");
        }
        for (const Json& target : targets) {
            feature.targets.push_back(
                {.test = ruleTest(field(target, "target", id, where), id, where, sources),
                 .state = blockState(field(target, "state", id, where), id, where)});
        }
    } else if (type == "minecraft:spring_feature") {
        feature.type = ConfiguredFeature::Type::Spring;
        const Json& state = field(config, "state", id, where);
        const data::ResourceLocation fluid =
            identifier(field(state, "Name", id, where), id, where + "'s state");
        if (fluid.toString() != "minecraft:water" && fluid.toString() != "minecraft:lava") {
            throw Unsupported("spring_feature of the fluid " + fluid.toString());
        }
        // The source block, whatever the fluid state's own properties.
        feature.fluid.name = fluid;
        feature.fluid.properties.emplace("level", "0");
        feature.validBlocks =
            blockSet(field(config, "valid_blocks", id, where), id, where, sources);
        const auto optional = [&](const char* key, const std::int32_t fallback) {
            return config.contains(key) ? integer(config.at(key), id, where + "'s " + key)
                                        : fallback;
        };
        feature.rockCount = optional("rock_count", 4);
        feature.holeCount = optional("hole_count", 1);
        if (config.contains("requires_block_below")) {
            if (!config.at("requires_block_below").is_boolean()) {
                fail(id, where + "'s requires_block_below must be true or false");
            }
            feature.requiresBlockBelow = config.at("requires_block_below").get<bool>();
        }
    } else if (type == "minecraft:disk") {
        feature.type = ConfiguredFeature::Type::Disk;
        feature.radius = intProvider(field(config, "radius", id, where), id, where + "'s radius");
        feature.halfHeight =
            integer(field(config, "half_height", id, where), id, where + "'s half_height");
        if (feature.radius.lowest() < 0 || feature.radius.highest() > 8 || feature.halfHeight < 0 ||
            feature.halfHeight > 4) {
            fail(id, where + " needs a radius of 0 to 8 and a half_height of 0 to 4");
        }
        feature.target = blockPredicate(field(config, "target", id, where), id, where, sources);
        const Json& provider = field(config, "state_provider", id, where);
        feature.toPlace =
            stateProvider(field(provider, "fallback", id, where), id, where + "'s fallback");
        const Json& rules = field(provider, "rules", id, where);
        if (!rules.is_array()) {
            fail(id, where + "'s rules must be a list");
        }
        for (const Json& rule : rules) {
            feature.stateRules.push_back(
                {.ifTrue = blockPredicate(field(rule, "if_true", id, where), id, where, sources),
                 .then = stateProvider(field(rule, "then", id, where), id, where + "'s rule")});
        }
    } else {
        throw Unsupported("feature type " + type);
    }
    return feature;
}

} // namespace

PlacedFeature compilePlaced(const Json& json, const std::string& id,
                            const FeatureSources& sources) {
    PlacedFeature placed;
    const std::string where = "it";
    const Json& feature = field(json, "feature", id, where);
    const Json& placement = field(json, "placement", id, where);
    if (!placement.is_array()) {
        fail(id, "placement must be a list of modifiers");
    }
    // The modifiers first: a feature unrun for its modifiers and its type
    // both names the modifier, which is the framework's and the nearer fix.
    for (const Json& entry : placement) {
        placed.placement.push_back(modifier(entry, id, sources));
    }
    if (feature.is_string()) {
        const data::ResourceLocation reference = identifier(feature, id, "its feature");
        const auto found = sources.configured.find(reference);
        if (found == sources.configured.end()) {
            fail(id, "names configured feature " + reference.toString() +
                         ", which the pack does not define");
        }
        placed.feature = std::make_shared<const ConfiguredFeature>(
            configuredFeature(found->second, id, sources));
    } else {
        placed.feature =
            std::make_shared<const ConfiguredFeature>(configuredFeature(feature, id, sources));
    }
    return placed;
}

BlockClass classOf(const settings::BlockState& state) noexcept {
    if (state.name.namespaceName() != data::ResourceLocation::kDefaultNamespace) {
        return BlockClass::Blocking;
    }
    const std::string_view path = state.name.path();
    if (path == "air" || path == "cave_air" || path == "void_air") {
        return BlockClass::Air;
    }
    if (path == "water") {
        return BlockClass::Water;
    }
    if (path == "lava") {
        return BlockClass::Lava;
    }
    if (path == "powder_snow") {
        return BlockClass::Passable;
    }
    return BlockClass::Blocking;
}

bool isLeaves(const settings::BlockState& state) noexcept {
    return state.name.path().ends_with("_leaves");
}

} // namespace stratum::feature::detail
