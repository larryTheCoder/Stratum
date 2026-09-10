// Stratum — turning a density field into blocks.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum/javamath.hpp>
#include <stratum/terrain/filler.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

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

/// Solid, fluid or air — what the FIRST pass decided, read back for the
/// second. Not stored anywhere: rederived from the block a position already
/// holds, which is exact because only these three ever come out of it.
enum class Category : std::uint8_t { Air, Fluid, Solid };

[[nodiscard]] Category categorize(const settings::BlockState& block,
                                  const settings::NoiseSettings& settings) {
    if (block == air()) {
        return Category::Air;
    }
    if (block == settings.defaultFluid) {
        return Category::Fluid;
    }
    return Category::Solid;
}

/// What one dimension's surface rules ask of the caller, beyond the block
/// the first pass already placed. Scanning the tree once at compile is
/// cheaper than guessing, and safer: a field left unpopulated because a
/// caller assumed it was not needed is exactly the "Context missing a field"
/// bug `surface::Executor` is built to make impossible.
struct SurfaceNeeds {
    bool biome = false;
    bool temperature = false;
    bool preliminarySurface = false;
    bool steep = false;
    bool bandlands = false;
};

[[nodiscard]] SurfaceNeeds surfaceNeedsOf(const surface::RuleGraph& graph) {
    SurfaceNeeds needs;
    for (surface::ConditionIndex i = 0; i < graph.conditionCount(); ++i) {
        switch (graph.condition(i).type) {
            case surface::ConditionType::Biome:
                needs.biome = true;
                break;
            case surface::ConditionType::Temperature:
                needs.temperature = true;
                break;
            case surface::ConditionType::AbovePreliminarySurface:
                needs.preliminarySurface = true;
                break;
            case surface::ConditionType::Steep:
                needs.steep = true;
                break;
            default:
                break;
        }
    }
    for (surface::RuleIndex i = 0; i < graph.ruleCount(); ++i) {
        if (graph.rule(i).type == surface::RuleType::Bandlands) {
            needs.bandlands = true;
            break;
        }
    }
    return needs;
}

/// Snaps down onto the biome grid: vanilla resolves one biome per 4x4x4
/// cell and surface rules read that grid rather than resampling per block
/// (measured in `vanilla_biomes_test.cpp` — the lower corner of the cell).
[[nodiscard]] std::int32_t quartSnap(const std::int32_t value) noexcept {
    return javamath::floorDiv(value, std::int32_t{4}) * std::int32_t{4};
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
                                 const settings::NoiseSettings& settings,
                                 const surface::RuleGraph* surfaceRules,
                                 const biome::ParameterList* biomeParameters) {
    // Refused, not approximated. A dimension with aquifers does not decide its
    // blocks from the density alone, and filling it as though it did produces
    // a world that generates and is wrong — which SPEC §8 treats as the most
    // severe class of bug there is. Measured on one golden seed, the
    // difference is 1.12% of all blocks, four fifths of it water that should
    // have been air.
    // What is settled: the cell lattice, the centre jitter, the fluid level
    // rule including its ocean branch, the barrier predicate, and every one of
    // the surface scan's reads — the last of which was re-derived at twenty
    // feature scales from half a block to a hundred and comes back the same
    // thirteen positions every time. The surface is no longer why this
    // refuses; that sentence stood here for a day and was wrong.
    //
    // What is not, and any one of these is enough: the depth path's gate on
    // the anchor rests on a single instrument, and it decides whether a cell
    // floods; three measured corrections to the level rule are unverified, one
    // of which fires at ordinary sea levels; about 13% of the server's real
    // barriers come from a third source this build cannot see; and the `lava`
    // router entry has never been measured by anybody, so a correct level
    // still writes the wrong block. Filling now would generate a world that is
    // wrong without failing — the most severe class in SPEC §8. Measured on
    // one golden seed, aquifers move 1.12% of all blocks, four fifths of it
    // water that should have been air.
    if (settings.aquifersEnabled) {
        throw FillError("this dimension sets aquifers_enabled, and this build does not yet "
                        "implement the aquifer fill decision (SPEC §10 milestone MA, §11). Its "
                        "geometry, its fluid levels, its source selection, its fluid type and its "
                        "surface reads are derived, but its barrier is refuted as a general model "
                        "and three corrections to the level rule are still single-sourced; "
                        "refusing rather than generating a world that is quietly wrong");
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

    if (surfaceRules != nullptr) {
        // Same policy `surface::Executor::compile` enforces on its own — a
        // tree with one unrunnable construct is refused whole — reached from
        // the graph directly rather than by catching ExecutionError, so the
        // reasons stay a list a caller can inspect instead of one exception
        // message glued together out of several.
        filler.surfaceRulesBlockedBy_ = surfaceRules->unrunnable();
        const SurfaceNeeds needs = surfaceNeedsOf(*surfaceRules);
        if (needs.biome && biomeParameters == nullptr) {
            filler.surfaceRulesBlockedBy_.emplace_back(
                "minecraft:biome (this dimension's rules read the biome, and no "
                "biome::ParameterList was supplied to ChunkFiller::compile)");
        }
        if (needs.temperature) {
            // Not a construct — the executor runs `temperature` fine, given
            // a biome's own declared value. Nothing in this build resolves
            // one from a biome's identifier yet (SPEC §11), and 0.0F is not
            // an honest stand-in: it would run the condition against the
            // wrong number rather than not run it, which is the class of
            // wrong SPEC §8 exists to keep out.
            filler.surfaceRulesBlockedBy_.emplace_back(
                "minecraft:temperature (this build has no source yet for a biome's own "
                "declared temperature)");
        }
        if (needs.bandlands &&
            noises.find(data::ResourceLocation::parse("minecraft:clay_bands_offset")) == nullptr) {
            // Also not a construct — `bandlands` itself runs fine
            // (spec/bandlands-spec.md, SPEC §11) — but Executor::compile
            // would otherwise throw NoiseError reaching for a noise nobody
            // asked the registry to build, and that reads as a crash rather
            // than an honest "blocked, here is why".
            filler.surfaceRulesBlockedBy_.emplace_back(
                "minecraft:bandlands (this dimension's rules use bandlands, and no "
                "minecraft:clay_bands_offset noise was built into the registry supplied to "
                "ChunkFiller::compile)");
        }

        if (filler.surfaceRulesBlockedBy_.empty()) {
            filler.surfaceNeedsBiome_ = needs.biome;
            filler.surfaceNeedsPreliminarySurface_ = needs.preliminarySurface;
            filler.surfaceNeedsSteep_ = needs.steep;
            filler.biomeParameters_ = biomeParameters;
            if (needs.preliminarySurface) {
                filler.interpreter_.requireEvaluable(
                    settings.router.at(settings::RouterEntry::PreliminarySurfaceLevel));
            }
            if (needs.biome) {
                for (const settings::RouterEntry entry :
                     {settings::RouterEntry::Temperature, settings::RouterEntry::Vegetation,
                      settings::RouterEntry::Continents, settings::RouterEntry::Erosion,
                      settings::RouterEntry::Depth, settings::RouterEntry::Ridges}) {
                    filler.interpreter_.requireEvaluable(settings.router.at(entry));
                }
            }
            filler.surfaceExecutor_.emplace(surface::Executor::compile(
                *surfaceRules, noises.worldSeed(), settings.geometry, &noises, settings.seaLevel));
        }
    }

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

    if (surfaceExecutor_.has_value()) {
        applySurfaceRules(chunkX, chunkZ, into);
    }
}

void ChunkFiller::applySurfaceRules(const std::int32_t chunkX, const std::int32_t chunkZ,
                                    ChunkBuffer& into) const {
    if (!surfaceExecutor_.has_value()) {
        // fill() only calls this once runsSurfaceRules() is true; checked
        // again here rather than trusting that from a distance.
        return;
    }
    const surface::Executor& executor = *surfaceExecutor_;
    const settings::NoiseGeometry& geometry = settings_->geometry;
    const std::int32_t baseX = chunkX * kChunkWidth;
    const std::int32_t baseZ = chunkZ * kChunkWidth;
    const std::int32_t minY = geometry.minY;
    const std::int32_t topY = geometry.minY + geometry.height;

    // WORLD_SURFACE per column of this chunk, for `steep`'s neighbours —
    // clamped inside the chunk by fillSteepNeighbours(), which is what keeps
    // this from ever touching a column outside it. Fluid counts as surface,
    // matching the heightmap `steep` was measured against, not OCEAN_FLOOR.
    std::array<std::int32_t, static_cast<std::size_t>(kChunkWidth) * kChunkWidth> worldSurface{};
    if (surfaceNeedsSteep_) {
        for (int localZ = 0; localZ < kChunkWidth; ++localZ) {
            for (int localX = 0; localX < kChunkWidth; ++localX) {
                std::int32_t height = minY - 1;
                for (std::int32_t y = topY - 1; y >= minY; --y) {
                    if (categorize(into.at(localX, y, localZ), *settings_) != Category::Air) {
                        height = y;
                        break;
                    }
                }
                worldSurface[(static_cast<std::size_t>(localZ) * kChunkWidth) +
                             static_cast<std::size_t>(localX)] = height;
            }
        }
    }
    const auto heightAt = [&](const std::int32_t worldX, const std::int32_t worldZ) {
        const auto lx = static_cast<std::size_t>(worldX - baseX);
        const auto lz = static_cast<std::size_t>(worldZ - baseZ);
        return worldSurface[(lz * kChunkWidth) + lx];
    };

    // Scratch for the two stone-depth runs, sized once and overwritten whole
    // by every column rather than reallocated 256 times a chunk.
    std::vector<std::int32_t> stoneDepthAbove(static_cast<std::size_t>(geometry.height));
    std::vector<std::int32_t> stoneDepthBelow(static_cast<std::size_t>(geometry.height));

    for (int localZ = 0; localZ < kChunkWidth; ++localZ) {
        for (int localX = 0; localX < kChunkWidth; ++localX) {
            const std::int32_t x = baseX + localX;
            const std::int32_t z = baseZ + localZ;

            surface::Context context;
            context.x = x;
            context.z = z;
            if (surfaceNeedsSteep_) {
                surface::fillSteepNeighbours(context, heightAt);
            }
            if (surfaceNeedsPreliminarySurface_) {
                context.preliminarySurface = static_cast<std::int32_t>(interpreter_.evaluate(
                    settings_->router.at(settings::RouterEntry::PreliminarySurfaceLevel),
                    density::Point{.x = x, .y = 0, .z = z}));
            }

            // Top-down: the stone-depth run counting from the world's top,
            // and the water height latched at the first fluid block met
            // descending. Air resets the run; fluid neither breaks it nor
            // counts toward it (surface::Context's own doc, measured).
            std::optional<std::int32_t> waterHeight;
            {
                std::int32_t run = 0;
                for (std::int32_t y = topY - 1; y >= minY; --y) {
                    const Category category = categorize(into.at(localX, y, localZ), *settings_);
                    if (category == Category::Air) {
                        run = 0;
                    } else if (category == Category::Solid) {
                        ++run;
                    }
                    stoneDepthAbove[static_cast<std::size_t>(y - minY)] = run;
                    if (category == Category::Fluid && !waterHeight.has_value()) {
                        waterHeight = y + 1;
                    }
                }
            }
            // The same run counted from the world's floor, for
            // `surface_type: ceiling`.
            {
                std::int32_t run = 0;
                for (std::int32_t y = minY; y < topY; ++y) {
                    const Category category = categorize(into.at(localX, y, localZ), *settings_);
                    if (category == Category::Air) {
                        run = 0;
                    } else if (category == Category::Solid) {
                        ++run;
                    }
                    stoneDepthBelow[static_cast<std::size_t>(y - minY)] = run;
                }
            }

            // The biome grid is quarter-resolution and constant within a
            // cell, so this is resolved once per four y levels rather than
            // once a block. A sentinel bool rather than optional<int32_t>:
            // every read of the cached value is already guarded by it, and
            // spelling that as has_value()/operator* left the guard too
            // indirect for bugprone-unchecked-optional-access to see.
            bool biomeQuartYKnown = false;
            std::int32_t biomeQuartY = 0;
            data::ResourceLocation biomeId{"minecraft", "plains"};
            for (std::int32_t y = topY - 1; y >= minY; --y) {
                if (surfaceNeedsBiome_) {
                    const std::int32_t quartY = quartSnap(y);
                    if (!biomeQuartYKnown || biomeQuartY != quartY) {
                        const std::int32_t qx = quartSnap(x);
                        const std::int32_t qz = quartSnap(z);
                        const density::Point at{.x = qx, .y = quartY, .z = qz};
                        const biome::ClimateSample sample{
                            .temperature = interpreter_.evaluate(
                                settings_->router.at(settings::RouterEntry::Temperature), at),
                            .humidity = interpreter_.evaluate(
                                settings_->router.at(settings::RouterEntry::Vegetation), at),
                            .continentalness = interpreter_.evaluate(
                                settings_->router.at(settings::RouterEntry::Continents), at),
                            .erosion = interpreter_.evaluate(
                                settings_->router.at(settings::RouterEntry::Erosion), at),
                            .depth = interpreter_.evaluate(
                                settings_->router.at(settings::RouterEntry::Depth), at),
                            .weirdness = interpreter_.evaluate(
                                settings_->router.at(settings::RouterEntry::Ridges), at)};
                        biomeId = biomeParameters_->find(sample);
                        biomeQuartY = quartY;
                        biomeQuartYKnown = true;
                    }
                    context.biome = biomeId;
                }

                context.y = y;
                context.stoneDepthAbove = stoneDepthAbove[static_cast<std::size_t>(y - minY)];
                context.stoneDepthBelow = stoneDepthBelow[static_cast<std::size_t>(y - minY)];
                context.waterHeight = waterHeight;

                if (const settings::BlockState* placed = executor.apply(context);
                    placed != nullptr) {
                    into.set(localX, y, localZ, *placed);
                }
            }
        }
    }
}

} // namespace stratum::terrain
