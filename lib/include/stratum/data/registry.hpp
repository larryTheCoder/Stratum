// Stratum — worldgen registries and the v1 capability matrix.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// SPEC §8 makes the capability matrix part of the public contract: the
// engine executes a named set of registries, and anything outside it is
// reported by name rather than quietly ignored. "A pack that half-loads
// silently is a bug of the highest severity class."
//
// AMBIGUITY, flagged rather than guessed (CLAUDE.md): §8 says unsupported
// registries are "a hard error at load". Read literally that makes vanilla's
// own data unloadable, since it ships 258 placed_features and 224
// configured_features that v1 never executes — and the Tier-A goldens are
// generated with features stripped precisely because they are out of scope.
// This layer therefore classifies and reports; whether a given rejected
// entry is fatal is the caller's policy, so no reading is baked in here.
// See PackLoadOptions::rejectUnsupported.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace stratum::data {

enum class Registry : std::uint8_t {
    // Executed by v1 (SPEC §8).
    DensityFunction,
    Noise,
    NoiseSettings,
    Biome,
    MultiNoiseBiomeSourceParameterList,
    WorldPreset,
    Dimension,
    DimensionType,

    // Named as unsupported by SPEC §8.
    ConfiguredFeature,
    PlacedFeature,
    ConfiguredCarver,
    Structure,
    StructureSet,
    TemplatePool,
    ProcessorList,
    FlatLevelGeneratorPreset,
};

/// The directory a registry's entries live in, relative to `data/<namespace>/`
/// — for example "worldgen/density_function".
[[nodiscard]] std::string_view registryDirectory(Registry registry) noexcept;

/// The registry a directory names, or nothing if it is not one we know.
[[nodiscard]] std::optional<Registry> registryFromDirectory(std::string_view directory) noexcept;

/// Whether v1 executes this registry (SPEC §8).
[[nodiscard]] bool isSupported(Registry registry) noexcept;

/// Every registry this build knows about, supported or not, so that a
/// rejection can name what it found.
[[nodiscard]] std::span<const Registry> allRegistries() noexcept;

} // namespace stratum::data
