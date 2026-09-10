// Stratum — a biome's own declared temperature.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum/biome/temperature_table.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/registry.hpp>
#include <stratum/data/resource_location.hpp>

#include <nlohmann/json.hpp>

#include <string>

namespace stratum::biome {

TemperatureTable TemperatureTable::fromPack(const data::Pack& pack) {
    TemperatureTable table;
    for (const data::PackEntry* entry : pack.entriesOf(data::Registry::Biome)) {
        const nlohmann::json& json = entry->json;
        if (!json.contains("temperature") || !json.at("temperature").is_number()) {
            throw TemperatureError("biome '" + entry->id.toString() +
                                   "' has no numeric \"temperature\"");
        }
        table.temperatures_.emplace(entry->id, json.at("temperature").get<float>());
    }
    return table;
}

float TemperatureTable::at(const data::ResourceLocation& id) const {
    const auto found = temperatures_.find(id);
    if (found == temperatures_.end()) {
        throw TemperatureError("biome '" + id.toString() +
                               "' has no worldgen/biome entry in this pack");
    }
    return found->second;
}

} // namespace stratum::biome
