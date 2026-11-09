// Stratum — configured carvers, parsed (carver/config.hpp).
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum/carver/config.hpp>
#include <stratum/data/json_integer.hpp>
#include <stratum/data/resource_location.hpp>

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace stratum::carver {

namespace {

[[noreturn]] void fail(const std::string& id, const std::string& what) {
    throw CarverError("configured carver " + id + ": " + what);
}

void requireOnly(const nlohmann::json& object, std::initializer_list<std::string_view> fields,
                 const std::string& id, const std::string& where) {
    for (const auto& [key, unused] : object.items()) {
        bool known = false;
        for (const std::string_view field : fields) {
            known = known || key == field;
        }
        if (!known) {
            std::string message = where;
            message += " has field '";
            message += key;
            message += "', which this carver type does not have";
            fail(id, message);
        }
    }
}

[[nodiscard]] const nlohmann::json& field(const nlohmann::json& object, const char* name,
                                          const std::string& id, const std::string& where) {
    if (!object.contains(name)) {
        fail(id, where + " has no \"" + name + "\"");
    }
    return object.at(name);
}

/// A pack float: a JSON number read as the float nearest to it, as the pack
/// stores it (spec A-Q1.3: shipped 0.15 is its float rounding).
[[nodiscard]] float packFloat(const nlohmann::json& value, const std::string& id,
                              const std::string& where) {
    if (!value.is_number()) {
        fail(id, where + " must be a number, not " + value.dump());
    }
    const double wide = value.get<double>();
    if (!std::isfinite(wide) || std::fabs(wide) > 3.4028234663852886e38) {
        fail(id, where + " is " + value.dump() + ", which no finite float holds");
    }
    return static_cast<float>(wide);
}

[[nodiscard]] std::string typeOf(const nlohmann::json& object, const std::string& id,
                                 const std::string& where) {
    const nlohmann::json& type = field(object, "type", id, where);
    if (!type.is_string()) {
        fail(id, where + "'s \"type\" must be a string");
    }
    const std::optional<data::ResourceLocation> parsed =
        data::ResourceLocation::tryParse(type.get<std::string>());
    if (!parsed.has_value()) {
        fail(id,
             where + "'s \"type\" '" + type.get<std::string>() + "' is not a resource location");
    }
    return parsed->toString();
}

[[nodiscard]] FloatProvider floatProvider(const nlohmann::json& value, const std::string& id,
                                          const std::string& where) {
    if (value.is_number()) {
        return FloatProvider{.kind = FloatProvider::Kind::Constant,
                             .a = packFloat(value, id, where)};
    }
    if (!value.is_object()) {
        fail(id, where + " must be a number or a float provider, not " + value.dump());
    }
    const std::string type = typeOf(value, id, where);
    if (type == "minecraft:constant") {
        requireOnly(value, {"type", "value"}, id, where);
        return FloatProvider{.kind = FloatProvider::Kind::Constant,
                             .a = packFloat(field(value, "value", id, where), id, where)};
    }
    if (type == "minecraft:uniform") {
        requireOnly(value, {"type", "min_inclusive", "max_exclusive"}, id, where);
        const float min = packFloat(field(value, "min_inclusive", id, where), id, where);
        const float max = packFloat(field(value, "max_exclusive", id, where), id, where);
        if (!(max > min)) {
            fail(id, where + "'s uniform needs max_exclusive above min_inclusive");
        }
        return FloatProvider{.kind = FloatProvider::Kind::Uniform, .a = min, .b = max};
    }
    if (type == "minecraft:trapezoid") {
        requireOnly(value, {"type", "min", "max", "plateau"}, id, where);
        const float min = packFloat(field(value, "min", id, where), id, where);
        const float max = packFloat(field(value, "max", id, where), id, where);
        const float plateau = packFloat(field(value, "plateau", id, where), id, where);
        if (!(max >= min) || !(plateau >= 0.0F) || !(plateau <= max - min)) {
            fail(id, where + "'s trapezoid needs min <= max and 0 <= plateau <= max - min");
        }
        return FloatProvider{
            .kind = FloatProvider::Kind::Trapezoid, .a = min, .b = max, .c = plateau};
    }
    fail(id, where + " is a float provider of type " + type +
                 ", which this build does not implement (it implements constant, uniform and "
                 "trapezoid, the ones spec/carvers-*-spec.md specify)");
}

[[nodiscard]] surface::VerticalAnchor anchor(const nlohmann::json& value, const std::string& id,
                                             const std::string& where) {
    if (!value.is_object() || value.size() != 1) {
        fail(id, where + " must be a vertical anchor: one of {\"absolute\": y}, "
                         "{\"above_bottom\": n}, {\"below_top\": n}");
    }
    // Key and value bound once, as surface_rule_graph.cpp's readAnchor does:
    // GCC 13 at -O3 cannot prove a second `entry.value()` non-null and raises
    // -Wnull-dereference inside nlohmann/json (an error in CI's release legs).
    const auto entry = value.begin();
    const std::string& key = entry.key();
    const nlohmann::json& inner = entry.value();
    if (!inner.is_number_integer()) {
        fail(id, where + "'s " + key + " must be a 32-bit whole number");
    }
    const std::optional<std::int32_t> y = data::exactInteger<std::int32_t>(inner);
    if (!y.has_value()) {
        fail(id, where + "'s " + key + " must be a 32-bit whole number");
    }
    surface::VerticalAnchor result;
    result.value = *y;
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

[[nodiscard]] HeightProvider heightProvider(const nlohmann::json& value, const std::string& id,
                                            const std::string& where) {
    if (!value.is_object()) {
        fail(id, where + " must be a height provider");
    }
    if (!value.contains("type")) {
        // A bare anchor is a constant height.
        const surface::VerticalAnchor y = anchor(value, id, where);
        return HeightProvider{
            .kind = HeightProvider::Kind::Constant, .minInclusive = y, .maxInclusive = y};
    }
    const std::string type = typeOf(value, id, where);
    if (type == "minecraft:constant") {
        requireOnly(value, {"type", "value"}, id, where);
        const surface::VerticalAnchor y = anchor(field(value, "value", id, where), id, where);
        return HeightProvider{
            .kind = HeightProvider::Kind::Constant, .minInclusive = y, .maxInclusive = y};
    }
    if (type == "minecraft:uniform") {
        requireOnly(value, {"type", "min_inclusive", "max_inclusive"}, id, where);
        return HeightProvider{
            .kind = HeightProvider::Kind::Uniform,
            .minInclusive = anchor(field(value, "min_inclusive", id, where), id, where),
            .maxInclusive = anchor(field(value, "max_inclusive", id, where), id, where)};
    }
    fail(id, where + " is a height provider of type " + type +
                 ", which this build does not implement (it implements uniform and constant, "
                 "the ones spec/carvers-a-spec.md Q1.5 specifies)");
}

[[nodiscard]] Replaceable parseReplaceable(const nlohmann::json& value, const std::string& id) {
    Replaceable result;
    const auto blockId = [&id](const nlohmann::json& entry) {
        if (!entry.is_string()) {
            fail(id, "\"replaceable\" holds " + entry.dump() + ", which is not a block id");
        }
        const std::optional<data::ResourceLocation> parsed =
            data::ResourceLocation::tryParse(entry.get<std::string>());
        if (!parsed.has_value()) {
            fail(id, "\"replaceable\" names '" + entry.get<std::string>() +
                         "', which is not a block id");
        }
        return *parsed;
    };
    if (value.is_string() && value.get<std::string>().starts_with('#')) {
        const std::optional<data::ResourceLocation> tag =
            data::ResourceLocation::tryParse(value.get<std::string>().substr(1));
        if (!tag.has_value()) {
            fail(id, "\"replaceable\" names tag '" + value.get<std::string>() +
                         "', which is not a resource location");
        }
        result.isTag = true;
        result.tag = *tag;
    } else if (value.is_string()) {
        result.blocks.push_back(blockId(value));
    } else if (value.is_array()) {
        for (const nlohmann::json& entry : value) {
            result.blocks.push_back(blockId(entry));
        }
    } else {
        fail(id, "\"replaceable\" must be a #tag, a block id or a list of block ids");
    }
    return result;
}

void debugSettings(const nlohmann::json& config, const std::string& id) {
    if (!config.contains("debug_settings")) {
        return;
    }
    const nlohmann::json& debug = config.at("debug_settings");
    if (!debug.is_object()) {
        fail(id, "\"debug_settings\" must be an object");
    }
    requireOnly(debug, {"debug_mode", "air_state", "water_state", "lava_state", "barrier_state"},
                id, "debug_settings");
    if (debug.contains("debug_mode")) {
        if (!debug.at("debug_mode").is_boolean()) {
            fail(id, "\"debug_settings.debug_mode\" must be true or false");
        }
        if (debug.at("debug_mode").get<bool>()) {
            fail(id, "\"debug_settings.debug_mode\" is true, and carver debug output is not "
                     "implemented by this build: it is refused rather than carved without "
                     "it (IMPLEMENTER-RULING-01; spec/carvers-a-spec.md Q13.4)");
        }
    }
}

} // namespace

ConfiguredCarver ConfiguredCarver::fromJson(const nlohmann::json& json, const std::string& id) {
    // Named, so that no reference field() returns is bound beside a temporary.
    const std::string definition = "the definition";
    const std::string inConfig = "config";
    const std::string inShape = "shape";
    if (!json.is_object()) {
        fail(id, "is not a JSON object");
    }
    requireOnly(json, {"type", "config"}, id, definition);
    const std::string type = typeOf(json, id, definition);
    ConfiguredCarver carver;
    if (type == "minecraft:cave") {
        carver.type = CarverType::Cave;
    } else if (type == "minecraft:nether_cave") {
        carver.type = CarverType::NetherCave;
    } else if (type == "minecraft:canyon") {
        carver.type = CarverType::Canyon;
    } else {
        fail(id, "is of type " + type +
                     ", which this build does not implement (it implements minecraft:cave, "
                     "minecraft:nether_cave and minecraft:canyon)");
    }
    const nlohmann::json& config = field(json, "config", id, definition);
    if (!config.is_object()) {
        fail(id, "\"config\" must be an object");
    }

    if (carver.type == CarverType::Canyon) {
        requireOnly(config,
                    {"probability", "y", "yScale", "lava_level", "replaceable", "debug_settings",
                     "vertical_rotation", "shape"},
                    id, inConfig);
    } else {
        requireOnly(config,
                    {"probability", "y", "yScale", "lava_level", "replaceable", "debug_settings",
                     "horizontal_radius_multiplier", "vertical_radius_multiplier", "floor_level"},
                    id, inConfig);
    }

    carver.probability = packFloat(field(config, "probability", id, inConfig), id, "probability");
    if (!(carver.probability >= 0.0F) || !(carver.probability <= 1.0F)) {
        fail(id, "\"probability\" must lie in [0, 1]");
    }
    carver.y = heightProvider(field(config, "y", id, inConfig), id, "\"y\"");
    carver.yScale = floatProvider(field(config, "yScale", id, inConfig), id, "\"yScale\"");
    carver.lavaLevel = anchor(field(config, "lava_level", id, inConfig), id, "\"lava_level\"");
    carver.replaceable = parseReplaceable(field(config, "replaceable", id, inConfig), id);
    debugSettings(config, id);

    if (carver.type == CarverType::Canyon) {
        carver.verticalRotation = floatProvider(field(config, "vertical_rotation", id, inConfig),
                                                id, "\"vertical_rotation\"");
        const nlohmann::json& shape = field(config, "shape", id, inConfig);
        if (!shape.is_object()) {
            fail(id, "\"shape\" must be an object");
        }
        requireOnly(shape,
                    {"distance_factor", "thickness", "width_smoothness", "horizontal_radius_factor",
                     "vertical_radius_default_factor", "vertical_radius_center_factor"},
                    id, inShape);
        carver.shape.distanceFactor = floatProvider(field(shape, "distance_factor", id, inShape),
                                                    id, "\"shape.distance_factor\"");
        carver.shape.thickness =
            floatProvider(field(shape, "thickness", id, inShape), id, "\"shape.thickness\"");
        const nlohmann::json& smoothness = field(shape, "width_smoothness", id, inShape);
        const std::optional<std::int32_t> width = data::exactInteger<std::int32_t>(smoothness);
        if (!smoothness.is_number_integer() || !width.has_value() || *width < 1) {
            fail(id, "\"shape.width_smoothness\" must be a positive whole number");
        }
        carver.shape.widthSmoothness = *width;
        carver.shape.horizontalRadiusFactor =
            floatProvider(field(shape, "horizontal_radius_factor", id, inShape), id,
                          "\"shape.horizontal_radius_factor\"");
        carver.shape.verticalRadiusDefaultFactor =
            packFloat(field(shape, "vertical_radius_default_factor", id, inShape), id,
                      "\"shape.vertical_radius_default_factor\"");
        carver.shape.verticalRadiusCenterFactor =
            packFloat(field(shape, "vertical_radius_center_factor", id, inShape), id,
                      "\"shape.vertical_radius_center_factor\"");
    } else {
        carver.horizontalRadiusMultiplier =
            floatProvider(field(config, "horizontal_radius_multiplier", id, inConfig), id,
                          "\"horizontal_radius_multiplier\"");
        carver.verticalRadiusMultiplier =
            floatProvider(field(config, "vertical_radius_multiplier", id, inConfig), id,
                          "\"vertical_radius_multiplier\"");
        carver.floorLevel =
            floatProvider(field(config, "floor_level", id, inConfig), id, "\"floor_level\"");
    }
    return carver;
}

std::vector<CarverEntry> parseBiomeCarvers(const nlohmann::json& field, const std::string& biome) {
    const auto entryOf = [&biome](const nlohmann::json& entry, const std::size_t k) {
        if (entry.is_string()) {
            const std::string& text = entry.get<std::string>();
            if (text.starts_with('#')) {
                throw CarverError("biome " + biome + "'s carvers names tag '" + text +
                                  "', and configured-carver tags are not implemented by this "
                                  "build");
            }
            const std::optional<data::ResourceLocation> id = data::ResourceLocation::tryParse(text);
            if (!id.has_value()) {
                throw CarverError("biome " + biome + "'s carvers names '" + text +
                                  "', which is not a resource location");
            }
            return CarverEntry{*id};
        }
        if (entry.is_object()) {
            return CarverEntry{ConfiguredCarver::fromJson(
                entry, "inline at " + biome + " carvers[" + std::to_string(k) + "]")};
        }
        throw CarverError("biome " + biome + "'s carvers holds " + entry.dump() +
                          ", which is neither a configured carver id nor a definition");
    };

    std::vector<CarverEntry> entries;
    if (field.is_string()) {
        entries.push_back(entryOf(field, 0));
    } else if (field.is_array()) {
        entries.reserve(field.size());
        for (std::size_t k = 0; k < field.size(); ++k) {
            entries.push_back(entryOf(field.at(k), k));
        }
    } else {
        throw CarverError("biome " + biome + "'s carvers is " + field.dump() +
                          ", which is neither an id nor a list (spec/carvers-b-spec.md Q11.1)");
    }
    return entries;
}

} // namespace stratum::carver
