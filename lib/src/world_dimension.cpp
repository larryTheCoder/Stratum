// Stratum — one dimension of one world, compiled from its frozen pipeline.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum/biome/parameter_list.hpp>
#include <stratum/biome/temperature_table.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/javamath.hpp>
#include <stratum/surface/rule_graph.hpp>
#include <stratum/world/dimension.hpp>

#include <string>
#include <vector>

namespace stratum::world {

namespace {

constexpr std::int32_t kQuartWidth = 4;
constexpr std::int32_t kQuartsPerChunkEdge = 4;

[[nodiscard]] const settings::NoiseSettings& settingsNamed(const freeze::Pipeline& pipeline,
                                                           const data::ResourceLocation& id) {
    const auto found = pipeline.settings.find(id);
    if (found == pipeline.settings.end()) {
        throw DimensionError("the frozen pipeline has no noise settings '" + id.toString() + "'");
    }
    return found->second;
}

[[nodiscard]] const biome::ParameterList& listNamed(const freeze::Pipeline& pipeline,
                                                    const data::ResourceLocation& id) {
    const auto found = pipeline.biomeParameters.find(id);
    if (found == pipeline.biomeParameters.end()) {
        throw DimensionError("the frozen pipeline has no biome parameter list '" + id.toString() +
                             "'");
    }
    return found->second;
}

[[nodiscard]] std::vector<data::ResourceLocation> everyNoise(const freeze::Pipeline& pipeline) {
    std::vector<data::ResourceLocation> ids;
    ids.reserve(pipeline.noises.size());
    for (const auto& [id, parameters] : pipeline.noises) {
        ids.push_back(id);
    }
    return ids;
}

} // namespace

/// Declaration order IS construction order, and it is load-bearing: see
/// dimension.hpp's header. `filler` and `biomeInterpreter` are last because
/// they hold references and pointers into everything above them.
struct CompiledDimension::Impl {
    freeze::Pipeline pipeline;
    settings::NoiseSettings settings;
    const biome::ParameterList& biomeParameters;
    surface::RuleGraph surfaceRules;
    density::NoiseRegistry noises;
    terrain::ChunkFiller filler;
    density::Interpreter biomeInterpreter;

    Impl(freeze::Pipeline frozen, const data::ResourceLocation& noiseSettings,
         const data::ResourceLocation& biomeParameterList, const std::int64_t worldSeed)
        : pipeline(std::move(frozen)), settings(settingsNamed(pipeline, noiseSettings)),
          biomeParameters(listNamed(pipeline, biomeParameterList)),
          surfaceRules(surface::RuleGraph::resolve(settings.surfaceRule, noiseSettings)),
          noises(density::NoiseRegistry::create(pipeline.noises, everyNoise(pipeline), worldSeed,
                                                settings.legacyRandomSource
                                                    ? density::RandomSource::Legacy
                                                    : density::RandomSource::Xoroshiro)),
          filler(terrain::ChunkFiller::compile(pipeline.graph, noises, settings, &surfaceRules,
                                               &biomeParameters, &pipeline.biomeTemperatures)),
          biomeInterpreter(pipeline.graph, noises,
                           density::CellGeometry{.width = settings.geometry.cellWidth(),
                                                 .height = settings.geometry.cellHeight()}) {}
};

CompiledDimension::CompiledDimension() = default;
CompiledDimension::~CompiledDimension() = default;

std::unique_ptr<CompiledDimension>
CompiledDimension::compile(freeze::Pipeline pipeline, const data::ResourceLocation& noiseSettings,
                           const data::ResourceLocation& biomeParameterList,
                           const std::int64_t worldSeed) {
    // Named-lookup failures surface before anything expensive is built.
    static_cast<void>(settingsNamed(pipeline, noiseSettings));
    static_cast<void>(listNamed(pipeline, biomeParameterList));

    auto dimension = std::unique_ptr<CompiledDimension>(new CompiledDimension());
    dimension->impl_ =
        std::make_unique<Impl>(std::move(pipeline), noiseSettings, biomeParameterList, worldSeed);
    return dimension;
}

const settings::NoiseGeometry& CompiledDimension::geometry() const noexcept {
    return impl_->settings.geometry;
}

void CompiledDimension::fillBlocks(const std::int32_t chunkX, const std::int32_t chunkZ,
                                   terrain::ChunkBuffer& into) const {
    impl_->filler.fill(chunkX, chunkZ, into);
}

void CompiledDimension::fillBiomes(const std::int32_t chunkX, const std::int32_t chunkZ,
                                   std::span<const data::ResourceLocation*> into) const {
    const settings::NoiseSettings& settings = impl_->settings;
    const std::int32_t quartsHigh = javamath::floorDiv(settings.geometry.height, kQuartWidth);
    const auto expected = static_cast<std::size_t>(quartsHigh) * kQuartsPerChunkEdge *
                          static_cast<std::size_t>(kQuartsPerChunkEdge);
    if (into.size() != expected) {
        throw DimensionError("a biome output of " + std::to_string(into.size()) +
                             " entries for a dimension that needs " + std::to_string(expected));
    }

    const std::int32_t baseQuartX = chunkX * kQuartsPerChunkEdge;
    const std::int32_t baseQuartZ = chunkZ * kQuartsPerChunkEdge;
    const std::int32_t baseQuartY = javamath::floorDiv(settings.geometry.minY, kQuartWidth);
    const auto& router = settings.router;

    // One cache for the whole chunk: it is keyed by cell, so reusing it
    // across positions changes no value, only how often corners are redone.
    density::Interpreter::CornerCache cache(impl_->biomeInterpreter.cacheSize());
    const density::Interpreter& interpreter = impl_->biomeInterpreter;
    std::size_t index = 0;
    for (std::int32_t qy = 0; qy < quartsHigh; ++qy) {
        for (std::int32_t qz = 0; qz < kQuartsPerChunkEdge; ++qz) {
            for (std::int32_t qx = 0; qx < kQuartsPerChunkEdge; ++qx) {
                const density::Point at{.x = (baseQuartX + qx) * kQuartWidth,
                                        .y = (baseQuartY + qy) * kQuartWidth,
                                        .z = (baseQuartZ + qz) * kQuartWidth};
                const biome::ClimateSample sample{
                    .temperature = interpreter.evaluate(
                        router.at(settings::RouterEntry::Temperature), at, cache),
                    .humidity = interpreter.evaluate(router.at(settings::RouterEntry::Vegetation),
                                                     at, cache),
                    .continentalness = interpreter.evaluate(
                        router.at(settings::RouterEntry::Continents), at, cache),
                    .erosion =
                        interpreter.evaluate(router.at(settings::RouterEntry::Erosion), at, cache),
                    .depth =
                        interpreter.evaluate(router.at(settings::RouterEntry::Depth), at, cache),
                    .weirdness =
                        interpreter.evaluate(router.at(settings::RouterEntry::Ridges), at, cache)};
                into[index++] = &impl_->biomeParameters.find(sample);
            }
        }
    }
}

} // namespace stratum::world
