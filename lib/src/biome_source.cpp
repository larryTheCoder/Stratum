// Stratum — a dimension's biome source: which biome sits at a quart.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// See biome/source.hpp for the rule and where each part of it comes from.
// The End's rule is adapted from cubiomes (MIT, Cubitect/cubiomes,
// biomenoise.c: mapEnd, mapEndBiome and getEndBiome) and read off the
// server's own router where the two part; the order of its last two tests
// is this build's own. Recorded in NOTICE.

#include <stratum/biome/source.hpp>
#include <stratum/javamath.hpp>

#include <algorithm>
#include <array>
#include <cstddef>

namespace stratum::biome {

namespace {

constexpr std::int32_t kQuartWidth = 4;
constexpr std::int32_t kQuartsPerChunkEdge = 4;
constexpr std::size_t kQuartsPerLayer = std::size_t{kQuartsPerChunkEdge} * kQuartsPerChunkEdge;

constexpr std::array<settings::RouterEntry, 6> kClimateEntries = {
    settings::RouterEntry::Temperature, settings::RouterEntry::Vegetation,
    settings::RouterEntry::Continents,  settings::RouterEntry::Erosion,
    settings::RouterEntry::Depth,       settings::RouterEntry::Ridges};

constexpr std::array<settings::RouterEntry, 1> kEndEntries = {settings::RouterEntry::Erosion};

[[nodiscard]] const data::ResourceLocation& theEnd() {
    static const data::ResourceLocation kId{"minecraft", "the_end"};
    return kId;
}

[[nodiscard]] const data::ResourceLocation& endHighlands() {
    static const data::ResourceLocation kId{"minecraft", "end_highlands"};
    return kId;
}

[[nodiscard]] const data::ResourceLocation& endMidlands() {
    static const data::ResourceLocation kId{"minecraft", "end_midlands"};
    return kId;
}

[[nodiscard]] const data::ResourceLocation& endBarrens() {
    static const data::ResourceLocation kId{"minecraft", "end_barrens"};
    return kId;
}

[[nodiscard]] const data::ResourceLocation& smallEndIslands() {
    static const data::ResourceLocation kId{"minecraft", "small_end_islands"};
    return kId;
}

} // namespace

const data::ResourceLocation& EndRule::central() {
    return theEnd();
}

const data::ResourceLocation& EndRule::outer(const double erosion) {
    // The ORDER is this build's own choice, not cubiomes' (whose order
    // would send a NaN to small islands) and not measured: every
    // comparison is false for a NaN, so one falls through all three to
    // barrens — the biome cubiomes gives its overflow rings, where the same
    // ring test makes its End density column NaN (biomenoise.c,
    // mapEndBiome and sampleNoiseColumnEnd). The edges themselves are the
    // server's, held to within 1e-10 of each (end-biome-router-probe.sh):
    // 0.25 is midlands, -0.0625 midlands, -0.21875 barrens.
    if (erosion > kHighlandsAbove) {
        return endHighlands();
    }
    if (erosion >= kMidlandsFrom) {
        return endMidlands();
    }
    if (erosion < kSmallIslandsBelow) {
        return smallEndIslands();
    }
    return endBarrens();
}

std::span<const settings::RouterEntry> Source::routerEntries() const noexcept {
    if (type_ == Type::TheEnd) {
        return kEndEntries;
    }
    return kClimateEntries;
}

const data::ResourceLocation& Source::at(const density::Interpreter& interpreter,
                                         const settings::NoiseRouter& router,
                                         const density::Point quartCorner,
                                         density::Interpreter::CornerCache& cache) const {
    if (type_ == Type::TheEnd) {
        // The chunk by floorDiv of the block, which is floorDiv of the quart
        // by four: quartCorner is a multiple of 4 on every axis.
        const std::int32_t chunkX = javamath::floorDiv(quartCorner.x, std::int32_t{16});
        const std::int32_t chunkZ = javamath::floorDiv(quartCorner.z, std::int32_t{16});
        if (EndRule::isCentral(chunkX, chunkZ)) {
            return EndRule::central();
        }
        return EndRule::outer(
            interpreter.evaluate(router.at(settings::RouterEntry::Erosion),
                                 EndRule::samplePoint(chunkX, quartCorner.y, chunkZ), cache));
    }
    const ClimateSample sample{
        .temperature =
            interpreter.evaluate(router.at(settings::RouterEntry::Temperature), quartCorner, cache),
        .humidity =
            interpreter.evaluate(router.at(settings::RouterEntry::Vegetation), quartCorner, cache),
        .continentalness =
            interpreter.evaluate(router.at(settings::RouterEntry::Continents), quartCorner, cache),
        .erosion =
            interpreter.evaluate(router.at(settings::RouterEntry::Erosion), quartCorner, cache),
        .depth = interpreter.evaluate(router.at(settings::RouterEntry::Depth), quartCorner, cache),
        .weirdness =
            interpreter.evaluate(router.at(settings::RouterEntry::Ridges), quartCorner, cache)};
    return parameters_->find(sample);
}

void Source::fillChunk(const density::Interpreter& interpreter, const settings::NoiseRouter& router,
                       const std::int32_t chunkX, const std::int32_t chunkZ,
                       const std::int32_t minQuartY, std::span<const data::ResourceLocation*> into,
                       density::Interpreter::CornerCache& cache) const {
    const std::size_t layers = into.size() / kQuartsPerLayer;
    if (type_ == Type::TheEnd) {
        fillEndChunk(interpreter, router, chunkX, chunkZ, minQuartY,
                     into.first(layers * kQuartsPerLayer), cache);
        return;
    }
    const std::int32_t baseQuartX = chunkX * kQuartsPerChunkEdge;
    const std::int32_t baseQuartZ = chunkZ * kQuartsPerChunkEdge;
    std::size_t index = 0;
    for (std::size_t layer = 0; layer < layers; ++layer) {
        const std::int32_t blockY = (minQuartY + static_cast<std::int32_t>(layer)) * kQuartWidth;
        for (std::int32_t qz = 0; qz < kQuartsPerChunkEdge; ++qz) {
            for (std::int32_t qx = 0; qx < kQuartsPerChunkEdge; ++qx) {
                into[index++] = &at(interpreter, router,
                                    density::Point{.x = (baseQuartX + qx) * kQuartWidth,
                                                   .y = blockY,
                                                   .z = (baseQuartZ + qz) * kQuartWidth},
                                    cache);
            }
        }
    }
}

void Source::fillEndChunk(const density::Interpreter& interpreter,
                          const settings::NoiseRouter& router, const std::int32_t chunkX,
                          const std::int32_t chunkZ, const std::int32_t minQuartY,
                          std::span<const data::ResourceLocation*> into,
                          density::Interpreter::CornerCache& cache) {
    // Every quart column of a chunk is the same chunk, so one answer serves
    // a whole layer: `at` would read the same sample sixteen times over.
    if (EndRule::isCentral(chunkX, chunkZ)) {
        std::ranges::fill(into, &EndRule::central());
        return;
    }
    // And when nothing beneath `erosion` reads y — vanilla's End routes
    // cache_2d(end_islands) there — one answer serves the whole chunk: the
    // value cannot differ from layer to layer, so reading it again could
    // only cost time. Interpreter::isColumnInvariant is conservative, so a
    // y-varying erosion (a datapack's: end-biome-router-probe.sh's
    // gradients, or a 3D noise) is read at every layer's own y. This trusts
    // it outright, so a node type that reads y and is reported invariant
    // shows up here as one biome per chunk — old_blended_noise and
    // weird_scaled_sampler once were; tests/unit/biome_source_test.cpp holds
    // this against `at`, quart by quart.
    const density::NodeIndex erosion = router.at(settings::RouterEntry::Erosion);
    const bool sameEveryLayer = interpreter.isColumnInvariant(erosion);
    const data::ResourceLocation* biome = nullptr;
    const std::size_t layers = into.size() / kQuartsPerLayer;
    std::size_t index = 0;
    for (std::size_t layer = 0; layer < layers; ++layer) {
        if (biome == nullptr || !sameEveryLayer) {
            const std::int32_t blockY =
                (minQuartY + static_cast<std::int32_t>(layer)) * kQuartWidth;
            biome = &EndRule::outer(
                interpreter.evaluate(erosion, EndRule::samplePoint(chunkX, blockY, chunkZ), cache));
        }
        for (std::size_t column = 0; column < kQuartsPerLayer; ++column) {
            into[index++] = biome;
        }
    }
}

} // namespace stratum::biome
