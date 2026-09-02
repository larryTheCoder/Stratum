// Stratum — worldgen registries and the v1 capability matrix.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum/data/registry.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace stratum::data {

namespace {

struct RegistryInfo {
    Registry registry;
    std::string_view directory;
    bool supported;
};

/// The capability matrix itself. Adding a row here is a change to the public
/// contract (SPEC §8) and belongs in the README table too.
constexpr std::array<RegistryInfo, 16> kRegistries = {{
    {.registry = Registry::DensityFunction,
     .directory = "worldgen/density_function",
     .supported = true},
    {.registry = Registry::Noise, .directory = "worldgen/noise", .supported = true},
    {.registry = Registry::NoiseSettings,
     .directory = "worldgen/noise_settings",
     .supported = true},
    {.registry = Registry::Biome, .directory = "worldgen/biome", .supported = true},
    {.registry = Registry::MultiNoiseBiomeSourceParameterList,
     .directory = "worldgen/multi_noise_biome_source_parameter_list",
     .supported = true},
    {.registry = Registry::WorldPreset, .directory = "worldgen/world_preset", .supported = true},
    {.registry = Registry::Dimension, .directory = "dimension", .supported = true},
    {.registry = Registry::DimensionType, .directory = "dimension_type", .supported = true},
    {.registry = Registry::ConfiguredFeature,
     .directory = "worldgen/configured_feature",
     .supported = false},
    {.registry = Registry::PlacedFeature,
     .directory = "worldgen/placed_feature",
     .supported = false},
    {.registry = Registry::ConfiguredCarver,
     .directory = "worldgen/configured_carver",
     .supported = false},
    {.registry = Registry::Structure, .directory = "worldgen/structure", .supported = false},
    {.registry = Registry::StructureSet, .directory = "worldgen/structure_set", .supported = false},
    {.registry = Registry::TemplatePool, .directory = "worldgen/template_pool", .supported = false},
    {.registry = Registry::ProcessorList,
     .directory = "worldgen/processor_list",
     .supported = false},
    {.registry = Registry::FlatLevelGeneratorPreset,
     .directory = "worldgen/flat_level_generator_preset",
     .supported = false},
}};

[[nodiscard]] const RegistryInfo& infoOf(Registry registry) noexcept {
    return kRegistries[static_cast<std::size_t>(registry)];
}

} // namespace

std::string_view registryDirectory(Registry registry) noexcept {
    return infoOf(registry).directory;
}

std::optional<Registry> registryFromDirectory(std::string_view directory) noexcept {
    for (const RegistryInfo& info : kRegistries) {
        if (info.directory == directory) {
            return info.registry;
        }
    }
    return std::nullopt;
}

bool isSupported(Registry registry) noexcept {
    return infoOf(registry).supported;
}

std::span<const Registry> allRegistries() noexcept {
    // Built once, in declaration order, so a report lists registries the same
    // way every run.
    static const std::array<Registry, kRegistries.size()> kOrder = [] {
        std::array<Registry, kRegistries.size()> order{};
        for (std::size_t i = 0; i < kRegistries.size(); ++i) {
            order[i] = kRegistries[i].registry;
        }
        return order;
    }();
    return kOrder;
}

} // namespace stratum::data
