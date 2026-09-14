// Stratum — the Nukkit binding's own generation pipeline.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#include <stratum/data/pack.hpp>
#include <stratum/freeze/pipeline.hpp>
#include <stratum/javamath.hpp>
#include <stratum/mapping/biome.hpp>
#include <stratum/terrain/filler.hpp>
#include <stratum/world/dimension.hpp>

#include <stratum_nukkit/pipeline.hpp>

#include <vector>

namespace stratum::nukkit {

namespace {

constexpr int kQuartWidth = 4;
constexpr int kQuartsPerChunkEdge = 4;

} // namespace

/// Everything is in `world::CompiledDimension`, the core both native
/// bindings generate through — including the in-place construction order its
/// ChunkFiller's pointers depend on. What is left here is Nukkit's side of
/// the boundary: geometry checks and mapping the Java output.
struct Pipeline::Impl {
    std::unique_ptr<world::CompiledDimension> dimension;
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
    // Still the live pack, not a world's frozen blob (SPEC §6): the Nukkit
    // binding has no world-creation hook storing one yet. It resolves the
    // same pipeline a blob would carry, through the same builder.
    auto pipeline = std::unique_ptr<Pipeline>(new Pipeline());
    pipeline->impl_ = std::make_unique<Impl>();
    pipeline->impl_->dimension = world::CompiledDimension::compile(
        freeze::resolve(data::Pack::open(packDir / "worldgen"), packDir / "biome_parameters"),
        dimension, dimension, worldSeed);
    return pipeline;
}

std::int32_t Pipeline::minY() const noexcept {
    return impl_->dimension->geometry().minY;
}

std::int32_t Pipeline::height() const noexcept {
    return impl_->dimension->geometry().height;
}

void Pipeline::fill(const std::int32_t chunkX, const std::int32_t chunkZ,
                    const std::span<std::int32_t> fullBlockIds,
                    const std::span<std::int32_t> biomeIds) const {
    const settings::NoiseGeometry& geometry = impl_->dimension->geometry();
    constexpr int kChunkWidth = 16;

    // Checked before anything expensive runs, not after: a caller that
    // sized its arrays wrong should not first pay for a whole chunk's
    // worth of terrain it was never going to be able to hand back.
    if (fullBlockIds.size() != static_cast<std::size_t>(kChunkWidth) * kChunkWidth *
                                   static_cast<std::size_t>(geometry.height)) {
        throw NukkitError("stratum-nukkit: fullBlockIds is the wrong size for this dimension's "
                          "height — caller and pipeline disagree on geometry");
    }
    const int quartsPerColumn = javamath::floorDiv(geometry.height, kQuartWidth);
    if (biomeIds.size() != static_cast<std::size_t>(quartsPerColumn) * kQuartsPerChunkEdge *
                               static_cast<std::size_t>(kQuartsPerChunkEdge)) {
        throw NukkitError("stratum-nukkit: biomeIds is the wrong size for this dimension's "
                          "height — caller and pipeline disagree on geometry");
    }

    terrain::ChunkBuffer buffer(geometry);
    impl_->dimension->fillBlocks(chunkX, chunkZ, buffer);

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

    std::vector<const data::ResourceLocation*> javaBiomes(biomeIds.size());
    impl_->dimension->fillBiomes(chunkX, chunkZ, javaBiomes);
    for (std::size_t index = 0; index < javaBiomes.size(); ++index) {
        const std::optional<std::int32_t> bedrockId = mapping::bedrockBiomeId(*javaBiomes[index]);
        if (!bedrockId.has_value()) {
            throw NukkitError("stratum-nukkit: biome '" + javaBiomes[index]->toString() +
                              "' has no Bedrock id in this build's generated table (a "
                              "custom/datapack biome — the nearest-biome fallback is not "
                              "implemented yet, see lib/mapping/README.md)");
        }
        biomeIds[index] = *bedrockId;
    }
}

std::int32_t resolveNukkitFullId(const settings::BlockState& block) {
    throw NukkitError("stratum-nukkit: no Nukkit block id mapping for '" + block.name.toString() +
                      "' — the Java-state-to-Nukkit-legacy-id table has not been sourced yet "
                      "(PROGRESS.md's M5 section); refusing rather than guessing (SPEC §8)");
}

} // namespace stratum::nukkit
