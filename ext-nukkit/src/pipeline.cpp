// Stratum — the Nukkit binding's own generation pipeline.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#include <stratum/biome/parameter_list.hpp>
#include <stratum/biome/temperature_table.hpp>
#include <stratum/density/graph.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/mapping/biome.hpp>
#include <stratum/surface/rule_graph.hpp>
#include <stratum/terrain/filler.hpp>

#include <nlohmann/json.hpp>
#include <stratum_nukkit/pipeline.hpp>

#include <fstream>
#include <sstream>

namespace stratum::nukkit {

namespace {

constexpr int kQuartWidth = 4;

/// The biome at one quart-resolution point — the exact same construction
/// `tools/analysis/generate-world.cpp`'s own `biomeAt()` uses, copied
/// rather than shared because that file is a one-off analysis tool
/// (SPEC §12) this binding does not depend on, not because the logic
/// differs.
[[nodiscard]] data::ResourceLocation
javaBiomeAt(density::Interpreter& interpreter, density::Interpreter::CornerCache& cache,
            const settings::NoiseSettings& settings, const biome::ParameterList& parameters,
            std::int32_t qx, std::int32_t qy, std::int32_t qz) {
    const density::Point at{.x = qx * kQuartWidth, .y = qy * kQuartWidth, .z = qz * kQuartWidth};
    const biome::ClimateSample sample{
        .temperature =
            interpreter.evaluate(settings.router.at(settings::RouterEntry::Temperature), at, cache),
        .humidity =
            interpreter.evaluate(settings.router.at(settings::RouterEntry::Vegetation), at, cache),
        .continentalness =
            interpreter.evaluate(settings.router.at(settings::RouterEntry::Continents), at, cache),
        .erosion =
            interpreter.evaluate(settings.router.at(settings::RouterEntry::Erosion), at, cache),
        .depth = interpreter.evaluate(settings.router.at(settings::RouterEntry::Depth), at, cache),
        .weirdness =
            interpreter.evaluate(settings.router.at(settings::RouterEntry::Ridges), at, cache)};
    return parameters.find(sample);
}

/// `biome_parameters/minecraft/overworld.json`, parsed for @p dimension.
/// The one piece of `Pipeline::Impl`'s construction that is file I/O rather
/// than built from an already-stable sibling member.
[[nodiscard]] biome::ParameterList loadBiomeParameters(const std::filesystem::path& packDir,
                                                       const data::ResourceLocation& dimension) {
    const std::ifstream parametersFile(packDir / "biome_parameters" / "minecraft" /
                                       "overworld.json");
    if (!parametersFile) {
        throw NukkitError("stratum-nukkit: no biome_parameters/minecraft/overworld.json under " +
                          packDir.string());
    }
    std::stringstream parametersJson;
    parametersJson << parametersFile.rdbuf();
    return biome::ParameterList::fromJson(nlohmann::json::parse(parametersJson.str()), dimension);
}

/// MA is landed, so aquifers run; ore veins stay off because their RNG
/// derivation is still open (SPEC's M3 section) and `ChunkFiller` refuses
/// the flag outright — matches `tools/analysis/generate-world.cpp`. Applied
/// through a free function, not a statement in `Impl`'s constructor body,
/// because `filler`'s own initializer already needs `&overworld` with
/// these flags already set — a member initializer list runs before the
/// constructor body, so setting them there would be one step too late.
[[nodiscard]] settings::NoiseSettings withGenerationFlags(settings::NoiseSettings settings) {
    settings.aquifersEnabled = true;
    settings.oreVeinsEnabled = false;
    return settings;
}

[[nodiscard]] std::vector<data::ResourceLocation>
wantedNoises(const settings::LoadedSettings& loaded, const surface::RuleGraph& surfaceRules) {
    auto wanted = loaded.graph.referencedNoises();
    const auto surfaceNoises = surfaceRules.referencedNoises();
    wanted.insert(wanted.end(), surfaceNoises.begin(), surfaceNoises.end());
    // Not picked up by referencedNoises() above —
    // tools/analysis/generate-world.cpp names the same three explicitly,
    // for the same reason.
    wanted.push_back(data::ResourceLocation::parse("minecraft:surface"));
    wanted.push_back(data::ResourceLocation::parse("minecraft:surface_secondary"));
    wanted.push_back(data::ResourceLocation::parse("minecraft:clay_bands_offset"));
    return wanted;
}

} // namespace

/// Everything a compiled pipeline needs, built in place rather than
/// assembled from locals and moved in afterward.
///
/// This distinction is load-bearing, not stylistic: `terrain::ChunkFiller
/// ::compile` stores POINTERS to the `surfaceRules`, `biomeParameters` and
/// `biomeTemperatures` it is given (its own doc: "whatever they point to
/// must outlive this ChunkFiller"). A first version of this file built
/// those three as locals in the (free) `compile()` function, called
/// `ChunkFiller::compile` with their addresses, and then `std::move()`d the
/// locals into an `Impl` it constructed afterward — which moves each
/// object's DATA into a new address while leaving `filler` still holding
/// pointers to the OLD one, a stack frame that no longer exists once
/// `compile()` returns. It ran, sometimes; it also produced an
/// out-of-bounds read inside `Interpreter::Scope::has` reached through
/// `ChunkFiller::fill`, from a chunk position no different from ones that
/// had just worked — exactly what a dangling pointer into reused stack
/// memory looks like, caught by `_GLIBCXX_ASSERTIONS` rather than always
/// crashing the same way. Every member below that `ChunkFiller::compile`
/// or `Interpreter`'s own constructor takes a pointer/reference into is
/// declared BEFORE `filler`/`biomeInterpreter`, so their addresses are
/// already final — this object's own, not a local's — by the time those
/// two are built from them.
struct Pipeline::Impl {
    data::Pack pack;
    settings::LoadedSettings loaded;
    settings::NoiseSettings overworld;
    biome::ParameterList biomeParameters;
    biome::TemperatureTable biomeTemperatures;
    surface::RuleGraph surfaceRules;
    density::NoiseRegistry noises;
    terrain::ChunkFiller filler;
    density::Interpreter biomeInterpreter;

    Impl(const std::filesystem::path& packDir, const data::ResourceLocation& dimension,
         const std::int64_t worldSeed)
        : pack(data::Pack::open(packDir / "worldgen")), loaded(settings::loadAll(pack)),
          overworld(withGenerationFlags(loaded.settings.at(dimension))),
          biomeParameters(loadBiomeParameters(packDir, dimension)),
          biomeTemperatures(biome::TemperatureTable::fromPack(pack)),
          surfaceRules(surface::RuleGraph::resolve(overworld.surfaceRule, dimension)),
          noises(density::NoiseRegistry::create(pack, wantedNoises(loaded, surfaceRules), worldSeed,
                                                density::RandomSource::Xoroshiro)),
          filler(terrain::ChunkFiller::compile(loaded.graph, noises, overworld, &surfaceRules,
                                               &biomeParameters, &biomeTemperatures)),
          biomeInterpreter(loaded.graph, noises,
                           density::CellGeometry{.width = overworld.geometry.cellWidth(),
                                                 .height = overworld.geometry.cellHeight()}) {}
};

Pipeline::Pipeline() = default;
Pipeline::~Pipeline() = default;

std::unique_ptr<Pipeline> Pipeline::compile(const std::filesystem::path& packDir,
                                            const data::ResourceLocation& dimension,
                                            const std::int64_t worldSeed) {
    static const auto kOverworld = data::ResourceLocation::parse("minecraft:overworld");
    if (dimension != kOverworld) {
        throw NukkitError("stratum-nukkit: '" + dimension.toString() +
                          "' is not a dimension this pipeline can compile yet — only "
                          "minecraft:overworld, blocked elsewhere on legacy_random_source (M4)");
    }
    auto pipeline = std::unique_ptr<Pipeline>(new Pipeline());
    pipeline->impl_ = std::make_unique<Impl>(packDir, dimension, worldSeed);
    return pipeline;
}

std::int32_t Pipeline::minY() const noexcept {
    return impl_->overworld.geometry.minY;
}

std::int32_t Pipeline::height() const noexcept {
    return impl_->overworld.geometry.height;
}

void Pipeline::fill(const std::int32_t chunkX, const std::int32_t chunkZ,
                    const std::span<std::int32_t> fullBlockIds,
                    const std::span<std::int32_t> biomeIds) const {
    const settings::NoiseGeometry& geometry = impl_->overworld.geometry;
    constexpr int kChunkWidth = 16;

    // Checked before anything expensive runs, not after: a caller that
    // sized its arrays wrong should not first pay for a whole chunk's
    // worth of terrain it was never going to be able to hand back.
    if (fullBlockIds.size() != static_cast<std::size_t>(kChunkWidth) * kChunkWidth *
                                   static_cast<std::size_t>(geometry.height)) {
        throw NukkitError("stratum-nukkit: fullBlockIds is the wrong size for this dimension's "
                          "height — caller and pipeline disagree on geometry");
    }
    const int quartsPerColumn = geometry.height / kQuartWidth;
    if (biomeIds.size() != static_cast<std::size_t>(quartsPerColumn) * 4 * 4) {
        throw NukkitError("stratum-nukkit: biomeIds is the wrong size for this dimension's "
                          "height — caller and pipeline disagree on geometry");
    }

    terrain::ChunkBuffer buffer(geometry);
    impl_->filler.fill(chunkX, chunkZ, buffer);

    for (std::int32_t ly = 0; ly < geometry.height; ++ly) {
        const std::int32_t y = geometry.minY + ly;
        for (int lz = 0; lz < kChunkWidth; ++lz) {
            for (int lx = 0; lx < kChunkWidth; ++lx) {
                const settings::BlockState& block = buffer.at(lx, y, lz);
                const std::size_t index =
                    (static_cast<std::size_t>(ly) * kChunkWidth + static_cast<std::size_t>(lz)) *
                        kChunkWidth +
                    static_cast<std::size_t>(lx);
                fullBlockIds[index] = resolveNukkitFullId(block);
            }
        }
    }

    const std::int32_t baseQuartX = chunkX * (kChunkWidth / kQuartWidth);
    const std::int32_t baseQuartZ = chunkZ * (kChunkWidth / kQuartWidth);
    const std::int32_t baseQuartY = geometry.minY / kQuartWidth;
    density::Interpreter::CornerCache biomeCache(impl_->biomeInterpreter.cacheSize());
    for (std::int32_t qy = 0; qy < quartsPerColumn; ++qy) {
        for (std::int32_t qz = 0; qz < 4; ++qz) {
            for (std::int32_t qx = 0; qx < 4; ++qx) {
                const data::ResourceLocation javaBiome = javaBiomeAt(
                    impl_->biomeInterpreter, biomeCache, impl_->overworld, impl_->biomeParameters,
                    baseQuartX + qx, baseQuartY + qy, baseQuartZ + qz);
                const std::optional<std::int32_t> bedrockId = mapping::bedrockBiomeId(javaBiome);
                if (!bedrockId.has_value()) {
                    throw NukkitError("stratum-nukkit: biome '" + javaBiome.toString() +
                                      "' has no Bedrock id in this build's generated table (a "
                                      "custom/datapack biome — the nearest-biome fallback is not "
                                      "implemented yet, see lib/mapping/README.md)");
                }
                const std::size_t index =
                    (static_cast<std::size_t>(qy) * 4 + static_cast<std::size_t>(qz)) * 4 +
                    static_cast<std::size_t>(qx);
                biomeIds[index] = *bedrockId;
            }
        }
    }
}

std::int32_t resolveNukkitFullId(const settings::BlockState& block) {
    throw NukkitError("stratum-nukkit: no Nukkit block id mapping for '" + block.name.toString() +
                      "' — the Java-state-to-Nukkit-legacy-id table has not been sourced yet "
                      "(PROGRESS.md's M5 section); refusing rather than guessing (SPEC §8)");
}

} // namespace stratum::nukkit
