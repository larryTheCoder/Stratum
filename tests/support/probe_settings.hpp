// Stratum — a probe spec entry as the noise settings the server was given.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// tools/analysis/density-probe.sh turns each entry of a spec into a
// dimension and its `noise_settings`, filling every field the entry leaves
// out with the script's own default. A case that runs the shipped filler
// over a probe world has to rebuild exactly those settings, and the only
// copy it may trust is the corpus's own spec.json: so this mirrors the
// script field for field, and refuses any key it does not know rather than
// dropping it — an entry with a field the script reads and this does not
// would otherwise run Stratum on a different world than the server's, and
// score that as a parity failure.
//
// Only the RAW shape is built (`raw_final_density` verbatim). The density
// shape — `K * flat_cache(function) + gradient` — is refused by name: no
// case needs it yet, and a copy of K kept here could drift from the script's.
#pragma once

#include <stratum/settings/noise_settings.hpp>

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>

namespace stratum::test {

/// density-probe.sh's `noise_settings` for one spec @p entry, written the way
/// the script writes it. @p probeOnly names keys the generating script added
/// for its own test to read (an expected layout, a label), which the server
/// never saw; every other key must be one density-probe.sh reads. Throws
/// std::runtime_error naming the key otherwise.
[[nodiscard]] inline nlohmann::json
probeNoiseSettings(const nlohmann::json& entry,
                   std::initializer_list<std::string_view> probeOnly = {}) {
    // Every key density-probe.sh reads from an entry. `biome` goes to the
    // dimension, not to noise_settings, so it is accepted and not carried: a
    // surface rule that reads it is refused by ChunkFiller without a
    // biome::ParameterList, which no caller here supplies. min_y and height
    // go to the dimension type AND to noise_settings.
    constexpr std::array<std::string_view, 16> kKnown{"name",
                                                      "min_y",
                                                      "height",
                                                      "raw_final_density",
                                                      "function",
                                                      "surface_rule",
                                                      "biome",
                                                      "size_vertical",
                                                      "size_horizontal",
                                                      "aquifers_enabled",
                                                      "ore_veins_enabled",
                                                      "legacy_random_source",
                                                      "sea_level",
                                                      "default_block",
                                                      "default_fluid",
                                                      "router"};
    for (const auto& [key, value] : entry.items()) {
        static_cast<void>(value);
        bool known = false;
        for (const std::string_view candidate : kKnown) {
            known = known || key == candidate;
        }
        for (const std::string_view candidate : probeOnly) {
            known = known || key == candidate;
        }
        if (!known) {
            throw std::runtime_error("probe spec entry '" + entry.value("name", std::string("?")) +
                                     "' has a key density-probe.sh does not read: " + key);
        }
    }
    if (!entry.contains("raw_final_density")) {
        throw std::runtime_error("probe spec entry '" + entry.value("name", std::string("?")) +
                                 "' is a density probe (`function`); only the raw shape is built");
    }
    constexpr std::int32_t kMinY = -64;
    constexpr std::int32_t kHeight = 384;
    const auto minY = entry.value("min_y", kMinY);
    nlohmann::json router = nlohmann::json::object();
    for (std::size_t i = 0; i < settings::kRouterEntryCount; ++i) {
        router[std::string(settings::routerEntryName(static_cast<settings::RouterEntry>(i)))] = 0;
    }
    if (entry.contains("router")) {
        for (const auto& [key, value] : entry.at("router").items()) {
            if (!router.contains(key) || key == "final_density") {
                throw std::runtime_error("probe spec entry '" +
                                         entry.at("name").get<std::string>() +
                                         "' sets a router entry density-probe.sh does not: " + key);
            }
            router[key] = value;
        }
    }
    router["final_density"] = entry.at("raw_final_density");
    // The script's default surface rule paints the entry's own default_block
    // over itself, so it changes nothing whatever that block is.
    const nlohmann::json defaultBlock =
        entry.value("default_block", nlohmann::json{{"Name", "minecraft:stone"}});
    return nlohmann::json{
        {"sea_level", entry.value("sea_level", minY)},
        {"disable_mob_generation", true},
        {"aquifers_enabled", entry.value("aquifers_enabled", false)},
        {"ore_veins_enabled", entry.value("ore_veins_enabled", false)},
        {"legacy_random_source", entry.value("legacy_random_source", false)},
        {"default_block", defaultBlock},
        {"default_fluid", entry.value("default_fluid", nlohmann::json{{"Name", "minecraft:air"}})},
        {"noise",
         {{"min_y", minY},
          {"height", entry.value("height", kHeight)},
          {"size_horizontal", entry.value("size_horizontal", 1)},
          {"size_vertical", entry.value("size_vertical", 1)}}},
        {"spawn_target", nlohmann::json::array()},
        {"surface_rule",
         entry.value("surface_rule",
                     nlohmann::json{{"type", "minecraft:block"}, {"result_state", defaultBlock}})},
        {"noise_router", router},
    };
}

} // namespace stratum::test
