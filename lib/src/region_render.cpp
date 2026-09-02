// Stratum — region rendering.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum/chunk/chunk.hpp>
#include <stratum/javamath.hpp>
#include <stratum/nbt/reader.hpp>
#include <stratum/render/region_render.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace stratum::render {

namespace {

constexpr std::size_t kPixelsPerAxis =
    static_cast<std::size_t>(region::kChunksPerAxis) * chunk::kSectionSize;

/// Absent or undecodable columns keep this colour: a deliberate "nothing was
/// read here", distinct from any height or biome.
constexpr std::array<std::uint8_t, 3> kBackground{16U, 16U, 24U};

/// FNV-1a. Integer-only and fixed-width, so the palette is identical on
/// every platform — a colour that changed between machines would make
/// rendered output useless for comparison.
[[nodiscard]] std::uint32_t hashName(std::string_view name) noexcept {
    std::uint32_t hash = 2166136261U;
    for (const char character : name) {
        hash ^= static_cast<std::uint32_t>(static_cast<unsigned char>(character));
        hash *= 16777619U;
    }
    return hash;
}

/// A distinct, reasonably bright colour per name.
[[nodiscard]] std::array<std::uint8_t, 3> colourFor(std::string_view name) noexcept {
    const std::uint32_t hash = hashName(name);
    // Keep each channel in [64, 255] so nothing lands on the background.
    const auto channel = [](std::uint32_t value) {
        return static_cast<std::uint8_t>(64U + (value % 192U));
    };
    return {channel(hash), channel(hash >> 8U), channel(hash >> 16U)};
}

struct Column {
    bool present = false;
    int height = 0;
    std::string surfaceBlock;
    std::string surfaceBiome;
};

[[nodiscard]] std::string biomeAt(const chunk::Chunk& chunk, int localX, int y, int localZ) {
    for (const chunk::Section& section : chunk.sections()) {
        if (section.y != javamath::floorDiv(y, chunk::kSectionSize)) {
            continue;
        }
        if (section.biomes.empty()) {
            return {};
        }
        // Biomes are stored per 4x4x4 cell, so 64 to a section.
        const std::size_t cellX = static_cast<std::size_t>(localX) / 4U;
        const std::size_t cellY =
            static_cast<std::size_t>(javamath::floorMod(y, chunk::kSectionSize)) / 4U;
        const std::size_t cellZ = static_cast<std::size_t>(localZ) / 4U;
        const std::size_t index = (cellY * 16U) + (cellZ * 4U) + cellX;
        if (index < section.biomes.size()) {
            return section.biomePalette[section.biomes[index]];
        }
    }
    return {};
}

} // namespace

bool parseMode(std::string_view name, Mode& mode) noexcept {
    if (name == "heightmap") {
        mode = Mode::Heightmap;
        return true;
    }
    if (name == "biome") {
        mode = Mode::Biome;
        return true;
    }
    if (name == "blocks") {
        mode = Mode::Blocks;
        return true;
    }
    return false;
}

std::string_view modeName(Mode mode) noexcept {
    switch (mode) {
        case Mode::Heightmap:
            return "heightmap";
        case Mode::Biome:
            return "biome";
        case Mode::Blocks:
            return "blocks";
    }
    return "unknown";
}

image::Image renderRegion(const region::RegionFile& region, Mode mode) {
    std::vector<Column> columns(kPixelsPerAxis * kPixelsPerAxis);

    int lowest = std::numeric_limits<int>::max();
    int highest = std::numeric_limits<int>::min();

    for (std::int32_t chunkZ = 0; chunkZ < region::kChunksPerAxis; ++chunkZ) {
        for (std::int32_t chunkX = 0; chunkX < region::kChunksPerAxis; ++chunkX) {
            if (!region.hasChunk(chunkX, chunkZ)) {
                continue;
            }

            chunk::Chunk decoded;
            try {
                const nbt::Document document = nbt::read(region.readChunk(chunkX, chunkZ));
                decoded = chunk::Chunk::decode(document.root);
            } catch (const std::exception&) {
                // Left as background: a chunk that could not be read is shown
                // as absent rather than drawn from a guess.
                continue;
            }

            for (int localZ = 0; localZ < chunk::kSectionSize; ++localZ) {
                for (int localX = 0; localX < chunk::kSectionSize; ++localX) {
                    const std::optional<int> height = decoded.highestNonAir(localX, localZ);
                    if (!height.has_value()) {
                        continue;
                    }

                    const std::size_t pixelX =
                        (static_cast<std::size_t>(chunkX) * chunk::kSectionSize) +
                        static_cast<std::size_t>(localX);
                    const std::size_t pixelZ =
                        (static_cast<std::size_t>(chunkZ) * chunk::kSectionSize) +
                        static_cast<std::size_t>(localZ);

                    Column& column = columns[(pixelZ * kPixelsPerAxis) + pixelX];
                    column.present = true;
                    column.height = *height;
                    lowest = std::min(lowest, *height);
                    highest = std::max(highest, *height);

                    if (mode == Mode::Blocks) {
                        const chunk::BlockState* state = decoded.blockAt(localX, *height, localZ);
                        if (state != nullptr) {
                            column.surfaceBlock = state->name;
                        }
                    } else if (mode == Mode::Biome) {
                        column.surfaceBiome = biomeAt(decoded, localX, *height, localZ);
                    }
                }
            }
        }
    }

    image::Image image(kPixelsPerAxis, kPixelsPerAxis);
    const int span = (highest > lowest) ? (highest - lowest) : 1;

    for (std::size_t y = 0; y < kPixelsPerAxis; ++y) {
        for (std::size_t x = 0; x < kPixelsPerAxis; ++x) {
            const Column& column = columns[(y * kPixelsPerAxis) + x];
            if (!column.present) {
                image.setPixel(x, y, kBackground[0], kBackground[1], kBackground[2]);
                continue;
            }

            switch (mode) {
                case Mode::Heightmap: {
                    // Scaled across the region's own range, so a flat world
                    // is not a black square.
                    const int offset = column.height - lowest;
                    const auto shade = static_cast<std::uint8_t>(40 + ((offset * 215) / span));
                    image.setPixel(x, y, shade, shade, shade);
                    break;
                }
                case Mode::Biome: {
                    const auto colour = colourFor(column.surfaceBiome);
                    image.setPixel(x, y, colour[0], colour[1], colour[2]);
                    break;
                }
                case Mode::Blocks: {
                    const auto colour = colourFor(column.surfaceBlock);
                    image.setPixel(x, y, colour[0], colour[1], colour[2]);
                    break;
                }
            }
        }
    }

    return image;
}

} // namespace stratum::render
