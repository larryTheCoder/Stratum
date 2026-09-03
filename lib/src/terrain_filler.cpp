// Stratum — turning a density field into blocks.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum/javamath.hpp>
#include <stratum/terrain/filler.hpp>

#include <algorithm>
#include <cstddef>
#include <string>

namespace stratum::terrain {

namespace {

constexpr int kChunkWidth = 16;

/// Air, for everything above sea level that the density did not fill. Not a
/// field of the noise settings: vanilla has no `default_air`, it simply
/// places nothing, and "nothing" in a block array is this.
[[nodiscard]] const settings::BlockState& air() {
    static const settings::BlockState kAir{.name = data::ResourceLocation{"minecraft", "air"},
                                           .properties = {}};
    return kAir;
}

} // namespace

ChunkBuffer::ChunkBuffer(const settings::NoiseGeometry& geometry)
    : minY_(geometry.minY), height_(geometry.height) {
    if (height_ <= 0) {
        throw FillError("a dimension of height " + std::to_string(height_) +
                        " has no blocks to fill");
    }
    palette_.push_back(air());
    blocks_.assign(
        static_cast<std::size_t>(kChunkWidth) * kChunkWidth * static_cast<std::size_t>(height_), 0);
}

std::size_t ChunkBuffer::indexOf(int localX, std::int32_t y, int localZ) const {
    if (localX < 0 || localX >= kChunkWidth || localZ < 0 || localZ >= kChunkWidth) {
        throw FillError("block (" + std::to_string(localX) + ", " + std::to_string(localZ) +
                        ") is outside the chunk; local coordinates run 0 to 15");
    }
    if (y < minY_ || y >= minY_ + height_) {
        throw FillError("y " + std::to_string(y) + " is outside this dimension, which runs " +
                        std::to_string(minY_) + " to " + std::to_string(minY_ + height_ - 1));
    }
    const auto layer = static_cast<std::size_t>(y - minY_);
    return (((layer * kChunkWidth) + static_cast<std::size_t>(localZ)) * kChunkWidth) +
           static_cast<std::size_t>(localX);
}

const settings::BlockState& ChunkBuffer::at(int localX, std::int32_t y, int localZ) const {
    return palette_[blocks_[indexOf(localX, y, localZ)]];
}

std::uint16_t ChunkBuffer::intern(const settings::BlockState& block) {
    const auto found = std::ranges::find(palette_, block);
    if (found != palette_.end()) {
        return static_cast<std::uint16_t>(found - palette_.begin());
    }
    palette_.push_back(block);
    return static_cast<std::uint16_t>(palette_.size() - 1);
}

void ChunkBuffer::set(int localX, std::int32_t y, int localZ, const settings::BlockState& block) {
    const std::size_t index = indexOf(localX, y, localZ);
    blocks_[index] = intern(block);
}

ChunkFiller::ChunkFiller(const density::Graph& graph, const density::NoiseRegistry& noises,
                         const settings::NoiseSettings& settings)
    : settings_(&settings),
      interpreter_(graph, noises,
                   density::CellGeometry{.width = settings.geometry.cellWidth(),
                                         .height = settings.geometry.cellHeight()}),
      finalDensity_(settings.router.at(settings::RouterEntry::FinalDensity)) {}

ChunkFiller ChunkFiller::compile(const density::Graph& graph, const density::NoiseRegistry& noises,
                                 const settings::NoiseSettings& settings) {
    // Refused, not approximated. A dimension with aquifers does not decide its
    // blocks from the density alone, and filling it as though it did produces
    // a world that generates and is wrong — which SPEC §8 treats as the most
    // severe class of bug there is. Measured on one golden seed, the
    // difference is 1.12% of all blocks, four fifths of it water that should
    // have been air.
    if (settings.aquifersEnabled) {
        throw FillError("this dimension sets aquifers_enabled, and this build does not implement "
                        "the aquifer fill decision (SPEC §11). Its blocks are not a function of "
                        "final_density alone, so filling it from the density would flood every "
                        "cave in the world; refusing instead");
    }
    if (settings.oreVeinsEnabled) {
        throw FillError("this dimension sets ore_veins_enabled, and this build does not place ore "
                        "veins (SPEC §10, M3); refusing rather than generating a world missing "
                        "them silently");
    }

    ChunkFiller filler(graph, noises, settings);
    // Raised here, at compile, rather than on the first block of the first
    // chunk: a caller that cannot generate should learn so before it starts.
    filler.interpreter_.requireEvaluable(filler.finalDensity_);
    return filler;
}

void ChunkFiller::fill(std::int32_t chunkX, std::int32_t chunkZ, ChunkBuffer& into) const {
    const settings::NoiseGeometry& geometry = settings_->geometry;
    if (into.minY() != geometry.minY || into.height() != geometry.height) {
        throw FillError("this buffer is " + std::to_string(into.height()) + " blocks from " +
                        std::to_string(into.minY()) + ", and the dimension is " +
                        std::to_string(geometry.height) + " from " + std::to_string(geometry.minY));
    }

    const std::int32_t baseX = chunkX * kChunkWidth;
    const std::int32_t baseZ = chunkZ * kChunkWidth;
    const std::int32_t cellWidth = geometry.cellWidth();
    const std::int32_t cellHeight = geometry.cellHeight();
    const std::int32_t topY = geometry.minY + geometry.height;

    density::Interpreter::CornerCache cache(interpreter_.cacheSize());

    // Cell by cell, then block by block within the cell. The order is the
    // whole point: every block of a cell shares the eight corner values
    // `interpolated` needs, and visiting them together is what lets the cache
    // hold. Column-major order would evict on every block and cost 87 times
    // as much, which is measured rather than guessed.
    for (std::int32_t cellZ = 0; cellZ < kChunkWidth; cellZ += cellWidth) {
        for (std::int32_t cellX = 0; cellX < kChunkWidth; cellX += cellWidth) {
            for (std::int32_t cellBottom = geometry.minY; cellBottom < topY;
                 cellBottom += cellHeight) {
                const std::int32_t cellTop = std::min(cellBottom + cellHeight, topY);
                for (std::int32_t y = cellBottom; y < cellTop; ++y) {
                    for (std::int32_t localZ = cellZ;
                         localZ < std::min(cellZ + cellWidth, std::int32_t{kChunkWidth});
                         ++localZ) {
                        for (std::int32_t localX = cellX;
                             localX < std::min(cellX + cellWidth, std::int32_t{kChunkWidth});
                             ++localX) {
                            const double density = interpreter_.evaluate(
                                finalDensity_,
                                density::Point{.x = baseX + localX, .y = y, .z = baseZ + localZ},
                                cache);

                            // `sea_level` is EXCLUSIVE: with vanilla's 63 the
                            // water stops at 62 and 63 is the first air. That
                            // is measured, not read — an inclusive comparison
                            // puts one extra water block on top of every
                            // column in the world, which is 256 a chunk and
                            // exactly what the golden comparison found.
                            // sea_level is EXCLUSIVE: with vanilla's 63 the
                            // water stops at 62 and 63 is the first air.
                            // Measured, not read — an inclusive comparison
                            // puts one extra water block on top of every
                            // column, 256 a chunk, and that is exactly what
                            // the golden comparison found.
                            const settings::BlockState* block = &air();
                            if (density > 0.0) {
                                block = &settings_->defaultBlock;
                            } else if (y < settings_->seaLevel) {
                                block = &settings_->defaultFluid;
                            }
                            into.set(static_cast<int>(localX), y, static_cast<int>(localZ), *block);
                        }
                    }
                }
            }
        }
    }
}

} // namespace stratum::terrain
