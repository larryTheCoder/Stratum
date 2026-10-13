// Stratum — one dimension of one world, compiled from its frozen pipeline.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum/biome/parameter_list.hpp>
#include <stratum/biome/source.hpp>
#include <stratum/biome/temperature_table.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/javamath.hpp>
#include <stratum/surface/executor.hpp>
#include <stratum/surface/rule_graph.hpp>
#include <stratum/world/dimension.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
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

/// @p source bound to what it reads in @p pipeline, which must outlive it.
[[nodiscard]] biome::Source bind(const freeze::Pipeline& pipeline, const BiomeSource& source) {
    if (source.type() == BiomeSource::Type::TheEnd) {
        return biome::Source::theEnd();
    }
    const std::optional<data::ResourceLocation>& list = source.parameterList();
    if (!list.has_value()) {
        // multiNoise() always names one; nothing else builds this type.
        throw DimensionError("a minecraft:multi_noise biome source with no parameter list");
    }
    return biome::Source::multiNoise(listNamed(pipeline, *list));
}

[[nodiscard]] const data::ResourceLocation& theEndType() {
    static const data::ResourceLocation kId{"minecraft", "the_end"};
    return kId;
}

/// The noises THIS dimension needs — not every noise the pipeline carries.
///
/// The distinction used to be invisible, because a pipeline holds one graph
/// for every dimension in the pack and building the extras cost only time.
/// It is not invisible any more: `NoiseRegistry::create` refuses a
/// `legacy_random_source` dimension exactly when it is asked for a NAMED
/// noise, so handing it the pack's whole list would refuse vanilla's End —
/// which names none of them — for noises the overworld wants.
///
/// Two sources, because the two graphs are separate objects and neither
/// knows about the other: the density functions reachable from this
/// dimension's own fifteen router entries, and whatever its surface rule
/// tree needs (`surface::requiredNoises`, which knows about the three that
/// no condition names).
[[nodiscard]] std::vector<data::ResourceLocation>
noisesFor(const freeze::Pipeline& pipeline, const settings::NoiseSettings& settings,
          const surface::RuleGraph& surfaceRules) {
    std::vector<density::NodeIndex> roots;
    roots.reserve(settings::kRouterEntryCount);
    for (const density::NodeIndex entry : settings.router.entries) {
        roots.push_back(entry);
    }
    std::vector<data::ResourceLocation> ids = pipeline.graph.noisesReachableFrom(roots);
    for (const data::ResourceLocation& id : surface::requiredNoises(surfaceRules)) {
        if (std::ranges::find(ids, id) == ids.end()) {
            ids.push_back(id);
        }
    }
    return ids;
}

} // namespace

BiomeSource BiomeSource::multiNoise(data::ResourceLocation parameterList) {
    return {Type::MultiNoise, std::move(parameterList)};
}

BiomeSource BiomeSource::theEnd() {
    return {Type::TheEnd, std::nullopt};
}

BiomeSource BiomeSource::parse(const std::string_view spelling) {
    const data::ResourceLocation id = data::ResourceLocation::parse(spelling);
    if (id == theEndType()) {
        return theEnd();
    }
    // Vanilla's other biome source types: not lists, and not implemented.
    // Refused by name here, so that `minecraft:fixed` is never reported as a
    // parameter list the pipeline happens not to carry.
    static constexpr std::array<std::string_view, 2> kUnimplemented = {"fixed", "checkerboard"};
    if (id.namespaceName() == data::ResourceLocation::kDefaultNamespace) {
        if (id.path() == "multi_noise") {
            throw DimensionError("biome source 'minecraft:multi_noise' names no parameter list: "
                                 "name the list it searches instead (minecraft:overworld, "
                                 "minecraft:nether)");
        }
        if (std::ranges::find(kUnimplemented, std::string_view(id.path())) !=
            kUnimplemented.end()) {
            throw DimensionError("biome source '" + id.toString() +
                                 "' is not implemented by this build (SPEC §8): only "
                                 "minecraft:multi_noise, named by its parameter list, and "
                                 "minecraft:the_end are");
        }
    }
    return multiNoise(id);
}

std::string BiomeSource::toString() const {
    if (type_ == Type::TheEnd || !parameterList_.has_value()) {
        return theEndType().toString();
    }
    return parameterList_->toString();
}

/// Declaration order IS construction order, and it is load-bearing: see
/// dimension.hpp's header. `filler` and `biomeInterpreter` are last because
/// they hold references and pointers into everything above them.
struct CompiledDimension::Impl {
    freeze::Pipeline pipeline;
    settings::NoiseSettings settings;
    biome::Source biomeSource;
    surface::RuleGraph surfaceRules;
    density::NoiseRegistry noises;
    terrain::ChunkFiller filler;
    density::Interpreter biomeInterpreter;

    Impl(freeze::Pipeline frozen, const data::ResourceLocation& noiseSettings,
         const BiomeSource& source, const std::int64_t worldSeed)
        : pipeline(std::move(frozen)), settings(settingsNamed(pipeline, noiseSettings)),
          biomeSource(bind(pipeline, source)),
          surfaceRules(surface::RuleGraph::resolve(settings.surfaceRule, noiseSettings)),
          noises(density::NoiseRegistry::create(
              pipeline.noises, noisesFor(pipeline, settings, surfaceRules), worldSeed,
              settings.legacyRandomSource ? density::RandomSource::Legacy
                                          : density::RandomSource::Xoroshiro)),
          filler(terrain::ChunkFiller::compile(pipeline.graph, noises, settings, &surfaceRules,
                                               biomeSource, &pipeline.biomeTemperatures)),
          biomeInterpreter(pipeline.graph, noises,
                           density::CellGeometry{.width = settings.geometry.cellWidth(),
                                                 .height = settings.geometry.cellHeight()}) {
        // What the biome source reads, refused now rather than at the first
        // chunk: the End's source reads erosion alone, a multi-noise one all
        // six climate entries.
        for (const settings::RouterEntry entry : biomeSource.routerEntries()) {
            biomeInterpreter.requireEvaluable(settings.router.at(entry));
        }
    }
};

CompiledDimension::CompiledDimension() = default;
CompiledDimension::~CompiledDimension() = default;

std::unique_ptr<CompiledDimension>
CompiledDimension::compile(freeze::Pipeline pipeline, const data::ResourceLocation& noiseSettings,
                           const BiomeSource& biomeSource, const std::int64_t worldSeed) {
    // Named-lookup failures surface before anything expensive is built.
    static_cast<void>(settingsNamed(pipeline, noiseSettings));
    static_cast<void>(bind(pipeline, biomeSource));

    auto dimension = std::unique_ptr<CompiledDimension>(new CompiledDimension());
    dimension->impl_ =
        std::make_unique<Impl>(std::move(pipeline), noiseSettings, biomeSource, worldSeed);
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
    fillBiomes(chunkX, chunkZ, javamath::floorDiv(settings.geometry.minY, kQuartWidth), into);
}

void CompiledDimension::fillBiomes(const std::int32_t chunkX, const std::int32_t chunkZ,
                                   const std::int32_t minQuartY,
                                   std::span<const data::ResourceLocation*> into) const {
    constexpr std::size_t kQuartsPerLayer = std::size_t{kQuartsPerChunkEdge} * kQuartsPerChunkEdge;
    if (into.size() % kQuartsPerLayer != 0) {
        throw DimensionError("a biome output of " + std::to_string(into.size()) +
                             " entries, which is not a whole number of 16-quart layers");
    }

    // One cache for the whole chunk: it is keyed by cell, so reusing it
    // across positions changes no value, only how often corners are redone.
    density::Interpreter::CornerCache cache(impl_->biomeInterpreter.cacheSize());
    impl_->biomeSource.fillChunk(impl_->biomeInterpreter, impl_->settings.router, chunkX, chunkZ,
                                 minQuartY, into, cache);
}

} // namespace stratum::world
