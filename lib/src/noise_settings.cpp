// Stratum — noise settings: a dimension's geometry, flags and noise router.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum/data/pack.hpp>
#include <stratum/data/registry.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/graph.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/version.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace stratum::settings {

namespace {

/// What the loader does with a field, as the schema describes it.
enum class SettingsKind : std::uint8_t {
    BlockState,
    Boolean,
    Int,
    Geometry,
    Router,
    MaterialRule,
    SpawnTarget,
};

struct SettingsField {
    std::string_view name;
    SettingsKind kind = SettingsKind::Int;
    bool optional = false;
};

struct GeometryField {
    std::string_view name;
    std::int64_t minimum = 0;
    std::int64_t maximum = 0;
};

// Generated from mcdoc for the pinned version by tools/mcdoc-sync. Editing
// it by hand would defeat the point: the schema is what says which fields
// exist at 1.21.11, and which of them moved since 1.21.8.
#include "settings_schema.inc"

static_assert(kRouterFields.size() == kRouterEntryCount,
              "the RouterEntry enum and the generated field list are out of step; both come "
              "from tools/mcdoc-sync, so one of them was edited by hand");

class Loader {
public:
    Loader(data::ResourceLocation id, density::Graph::Builder& builder)
        : id_(std::move(id)), builder_(&builder) {}

    [[nodiscard]] NoiseSettings load(const nlohmann::json& json) {
        if (!json.is_object()) {
            fail({}, "must be an object, not " + std::string(json.type_name()));
        }

        NoiseSettings settings;
        settings.id = id_;

        // Every key in the file must be one the schema declares. A pack
        // carrying a field this build has never heard of is a pack expecting
        // something of us, and honouring the rest of it silently is how a
        // world generates and is quietly not the one that was asked for
        // (SPEC §8).
        for (const auto& [key, value] : json.items()) {
            const bool known = std::ranges::any_of(
                kSettingsFields, [&key](const SettingsField& field) { return field.name == key; });
            if (!known) {
                fail(key, "is not a field of noise settings at " + std::string(kMinecraftVersion));
            }
        }

        for (const SettingsField& field : kSettingsFields) {
            if (!json.contains(field.name)) {
                if (field.optional) {
                    continue;
                }
                fail(field.name, "is missing");
            }
            read(settings, field, json.at(field.name));
        }
        return settings;
    }

private:
    [[noreturn]] void fail(std::string_view field, const std::string& what) const {
        std::string message = "noise settings '" + id_.toString() + "'";
        if (!field.empty()) {
            message += ": \"" + std::string(field) + "\"";
        }
        throw SettingsError(message + " " + what);
    }

    void read(NoiseSettings& settings, const SettingsField& field, const nlohmann::json& value) {
        switch (field.kind) {
            case SettingsKind::BlockState: {
                BlockState state = readBlockState(field.name, value);
                if (field.name == "default_block") {
                    settings.defaultBlock = std::move(state);
                } else {
                    settings.defaultFluid = std::move(state);
                }
                break;
            }
            case SettingsKind::Boolean: {
                if (!value.is_boolean()) {
                    fail(field.name, "must be a boolean, not " + std::string(value.type_name()));
                }
                const bool flag = value.get<bool>();
                if (field.name == "disable_mob_generation") {
                    settings.disableMobGeneration = flag;
                } else if (field.name == "aquifers_enabled") {
                    settings.aquifersEnabled = flag;
                } else if (field.name == "ore_veins_enabled") {
                    settings.oreVeinsEnabled = flag;
                } else if (field.name == "legacy_random_source") {
                    settings.legacyRandomSource = flag;
                } else {
                    fail(field.name, "is a boolean the loader does not know what to do with");
                }
                break;
            }
            case SettingsKind::Int:
                // sea_level is the only one, and the schema gives it no
                // bounds at this version — a dimension may put its sea
                // outside its own height, and vanilla's caves preset does.
                settings.seaLevel = static_cast<std::int32_t>(readInt(field.name, value));
                break;
            case SettingsKind::Geometry:
                settings.geometry = readGeometry(value);
                break;
            case SettingsKind::Router:
                settings.router = readRouter(value);
                break;
            case SettingsKind::MaterialRule:
                settings.surfaceRule = value;
                break;
            case SettingsKind::SpawnTarget:
                settings.spawnTarget = value;
                break;
        }
    }

    [[nodiscard]] std::int64_t readInt(std::string_view field, const nlohmann::json& value) const {
        if (!value.is_number_integer()) {
            fail(field, "must be an integer, not " + std::string(value.type_name()));
        }
        return value.get<std::int64_t>();
    }

    [[nodiscard]] BlockState readBlockState(std::string_view field,
                                            const nlohmann::json& value) const {
        if (!value.is_object() || !value.contains("Name")) {
            fail(field, R"(must be an object with a "Name")");
        }
        const nlohmann::json& name = value.at("Name");
        if (!name.is_string()) {
            fail(field, R"("Name" must be a string)");
        }

        BlockState state;
        try {
            state.name = data::ResourceLocation::parse(name.get<std::string>());
        } catch (const data::ResourceLocationError& error) {
            fail(field, error.what());
        }

        if (value.contains("Properties")) {
            const nlohmann::json& properties = value.at("Properties");
            if (!properties.is_object()) {
                fail(field, R"("Properties" must be an object)");
            }
            for (const auto& [key, property] : properties.items()) {
                if (!property.is_string()) {
                    // Vanilla writes them as strings even when they read as
                    // numbers — "level": "0" — and accepting a bare number
                    // here would make two spellings of one state.
                    fail(field, "property \"" + key + "\" must be a string");
                }
                state.properties.emplace(key, property.get<std::string>());
            }
        }
        return state;
    }

    [[nodiscard]] NoiseGeometry readGeometry(const nlohmann::json& value) const {
        if (!value.is_object()) {
            fail("noise", "must be an object, not " + std::string(value.type_name()));
        }
        for (const auto& [key, ignored] : value.items()) {
            const bool known = std::ranges::any_of(
                kGeometryFields, [&key](const GeometryField& field) { return field.name == key; });
            if (!known) {
                fail("noise", "carries \"" + key + "\", which is not part of it at " +
                                  std::string(kMinecraftVersion));
            }
        }

        NoiseGeometry geometry;
        for (const GeometryField& field : kGeometryFields) {
            if (!value.contains(field.name)) {
                fail("noise", "is missing \"" + std::string(field.name) + "\"");
            }
            const std::int64_t number = readInt(field.name, value.at(field.name));
            // The bounds come from the schema, not from here.
            if (number < field.minimum || number > field.maximum) {
                fail("noise", "\"" + std::string(field.name) + "\" is " + std::to_string(number) +
                                  ", outside the schema's range [" + std::to_string(field.minimum) +
                                  ", " + std::to_string(field.maximum) + "]");
            }
            const auto narrowed = static_cast<std::int32_t>(number);
            if (field.name == "min_y") {
                geometry.minY = narrowed;
            } else if (field.name == "height") {
                geometry.height = narrowed;
            } else if (field.name == "size_horizontal") {
                geometry.sizeHorizontal = narrowed;
            } else {
                geometry.sizeVertical = narrowed;
            }
        }

        // Constraints the schema's per-field ranges cannot express, and that
        // the cell sampler will depend on. Checked here rather than assumed
        // there, where a remainder would silently become a half-height cell
        // at the top of the world.
        if (geometry.minY % 16 != 0) {
            fail("noise", "\"min_y\" is " + std::to_string(geometry.minY) +
                              ", which is not a whole number of sections");
        }
        if (geometry.height % 16 != 0) {
            fail("noise", "\"height\" is " + std::to_string(geometry.height) +
                              ", which is not a whole number of sections");
        }
        if (geometry.height % geometry.cellHeight() != 0) {
            fail("noise", "\"height\" is " + std::to_string(geometry.height) +
                              ", which is not a whole number of " +
                              std::to_string(geometry.cellHeight()) + "-block cells");
        }
        return geometry;
    }

    [[nodiscard]] NoiseRouter readRouter(const nlohmann::json& value) const {
        if (!value.is_object()) {
            fail("noise_router", "must be an object, not " + std::string(value.type_name()));
        }
        for (const auto& [key, ignored] : value.items()) {
            if (std::ranges::find(kRouterFields, key) == kRouterFields.end()) {
                fail("noise_router", "carries \"" + key + "\", which is not a router entry at " +
                                         std::string(kMinecraftVersion));
            }
        }

        NoiseRouter router;
        for (std::size_t i = 0; i < kRouterFields.size(); ++i) {
            const std::string_view name = kRouterFields[i];
            if (!value.contains(name)) {
                fail("noise_router", "is missing \"" + std::string(name) + "\"");
            }
            try {
                router.entries[i] = builder_->add(value.at(name));
            } catch (const density::ResolveError& error) {
                // The resolver names the chain it walked; what it cannot
                // know is which router entry the chain started from, because
                // an inline function has no identifier of its own.
                throw SettingsError("noise settings '" + id_.toString() + "': \"" +
                                    std::string(name) + "\": " + error.what());
            }
        }
        return router;
    }

    data::ResourceLocation id_;
    density::Graph::Builder* builder_;
};

} // namespace

std::string_view routerEntryName(RouterEntry entry) noexcept {
    const auto index = static_cast<std::size_t>(entry);
    return index < kRouterFields.size() ? kRouterFields[index] : "unknown";
}

NoiseSettings NoiseSettings::load(const data::PackEntry& entry, density::Graph::Builder& builder) {
    Loader loader(entry.id, builder);
    return loader.load(entry.json);
}

LoadedSettings loadAll(const data::Pack& pack) {
    density::Graph::Builder builder(pack);
    // The pack's own density functions first, so that the roots keep the
    // node indices they would have had without any settings — a router
    // referring to `overworld/continents` then resolves to the very same
    // node rather than a copy of it.
    builder.addNamed();

    std::map<data::ResourceLocation, NoiseSettings> settings;
    for (const data::PackEntry* entry : pack.entriesOf(data::Registry::NoiseSettings)) {
        settings.emplace(entry->id, NoiseSettings::load(*entry, builder));
    }

    return LoadedSettings{.graph = builder.release(), .settings = std::move(settings)};
}

} // namespace stratum::settings
