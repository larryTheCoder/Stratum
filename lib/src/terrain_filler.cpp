// Stratum — turning a density field into blocks.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum/aquifer/substance.hpp>
#include <stratum/javamath.hpp>
#include <stratum/terrain/filler.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <tuple>
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

/// The aquifer's lava reading. Not `default_fluid` — that is the dimension's
/// own choice for the OTHER fluid type, and Q6.7's own "the barrier block is
/// the caller's default block" has no analogue for lava: it is always the
/// literal block, whatever `default_fluid` says.
[[nodiscard]] const settings::BlockState& lava() {
    static const settings::BlockState kLava{.name = data::ResourceLocation{"minecraft", "lava"},
                                            .properties = {{"level", "0"}}};
    return kLava;
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
                                 const biome::ParameterList* biomeParameters,
                                 const biome::TemperatureTable* biomeTemperatures) {
    // WIRED (SPEC §10 milestone MA, §11): the cell lattice, the centre
    // jitter, the fluid level rule (including its ocean branch and the
    // depth path's anchor gate), source selection (selection.hpp), and the
    // three-source barrier predicate (barrier.hpp) are all measured and
    // called from here now, via `aquifer::computeSubstance`.
    //
    // TWO GAPS ARE STILL CARRIED RATHER THAN GUESSED, and
    // `aquifer/substance.hpp`'s own header has the numbers behind both: Q6.3's
    // water-over-lava exception is not applied, so a water block directly
    // above the global lava floor may get a barrier the real server would
    // not place; and Pi's mixed-fluid-type branch is not applied, so a
    // junction between a water body and a lava body is decided as though
    // both were the same type. Neither is guessed at, and both are narrow —
    // every barrier probe this project has run holds `lava` constant
    // specifically to keep the second one out of scope.
    if (settings.oreVeinsEnabled) {
        throw FillError("this dimension sets ore_veins_enabled, and this build does not place ore "
                        "veins (SPEC §10, M3); refusing rather than generating a world missing "
                        "them silently");
    }

    ChunkFiller filler(graph, noises, settings);
    // Raised here, at compile, rather than on the first block of the first
    // chunk: a caller that cannot generate should learn so before it starts.
    filler.interpreter_.requireEvaluable(filler.finalDensity_);

    if (settings.aquifersEnabled) {
        // The salted positional source (SPEC §4) is per-world, not per-block
        // — built once here from the registry's own seed rather than
        // re-derived on every call to fill().
        filler.aquiferCentres_.emplace(noises.worldSeed());
        for (const settings::RouterEntry entry :
             {settings::RouterEntry::Barrier, settings::RouterEntry::FluidLevelFloodedness,
              settings::RouterEntry::FluidLevelSpread, settings::RouterEntry::Lava,
              settings::RouterEntry::PreliminarySurfaceLevel}) {
            filler.interpreter_.requireEvaluable(settings.router.at(entry));
        }
    }

    if (surfaceRules != nullptr) {
        // Same policy `surface::Executor::compile` enforces on its own — a
        // tree with one unrunnable construct is refused whole — reached from
        // the graph directly rather than by catching ExecutionError, so the
        // reasons stay a list a caller can inspect instead of one exception
        // message glued together out of several.
        filler.surfaceRulesBlockedBy_ = surfaceRules->unrunnable();
        const SurfaceNeeds needs = surfaceNeedsOf(*surfaceRules);
        // `temperature` needs to know WHICH biome a block sits in before it
        // can look up that biome's declared value, so it leans on the same
        // climate search `biome` itself does — a tree naming only
        // `temperature` still needs a ParameterList, even though it never
        // names `biome` directly.
        const bool needsBiomeIdentity = needs.biome || needs.temperature;
        if (needsBiomeIdentity && biomeParameters == nullptr) {
            filler.surfaceRulesBlockedBy_.emplace_back(
                "minecraft:biome (this dimension's rules read the biome, directly or through a "
                "biome's declared temperature, and no biome::ParameterList was supplied to "
                "ChunkFiller::compile)");
        }
        if (needs.temperature && biomeTemperatures == nullptr) {
            // Not a construct — the executor runs `temperature` fine, given
            // a biome's own declared value; 0.0F is not an honest stand-in
            // for one, since it would run the condition against the wrong
            // number rather than not run it, which is the class of wrong
            // SPEC §8 exists to keep out.
            filler.surfaceRulesBlockedBy_.emplace_back(
                "minecraft:temperature (this dimension's rules read a biome's declared "
                "temperature, and no biome::TemperatureTable was supplied to "
                "ChunkFiller::compile)");
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
            filler.surfaceNeedsTemperature_ = needs.temperature;
            filler.surfaceNeedsPreliminarySurface_ = needs.preliminarySurface;
            filler.surfaceNeedsSteep_ = needs.steep;
            filler.biomeParameters_ = biomeParameters;
            filler.biomeTemperatures_ = biomeTemperatures;
            if (needs.preliminarySurface) {
                filler.interpreter_.requireEvaluable(
                    settings.router.at(settings::RouterEntry::PreliminarySurfaceLevel));
            }
            if (needsBiomeIdentity) {
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
    // A chunk touches dozens of distinct cell centres, not thousands of
    // blocks' worth of them — see aquifer::LevelCache's own doc.
    aquifer::LevelCache aquiferLevelCache;

    // Only ever populated when settings_->aquifersEnabled; the five lambdas
    // below capture these by reference and are only ever called from inside
    // that same condition, so they never see a moved-from or absent
    // optional. Looked up once per fill() call rather than per block — the
    // NodeIndex is the same for the whole dimension.
    const density::NodeIndex barrierNode =
        settings_->aquifersEnabled ? settings_->router.at(settings::RouterEntry::Barrier)
                                   : density::NodeIndex{};
    const density::NodeIndex floodednessNode =
        settings_->aquifersEnabled
            ? settings_->router.at(settings::RouterEntry::FluidLevelFloodedness)
            : density::NodeIndex{};
    const density::NodeIndex spreadNode =
        settings_->aquifersEnabled ? settings_->router.at(settings::RouterEntry::FluidLevelSpread)
                                   : density::NodeIndex{};
    const density::NodeIndex lavaNode = settings_->aquifersEnabled
                                            ? settings_->router.at(settings::RouterEntry::Lava)
                                            : density::NodeIndex{};
    const density::NodeIndex pslNode =
        settings_->aquifersEnabled
            ? settings_->router.at(settings::RouterEntry::PreliminarySurfaceLevel)
            : density::NodeIndex{};
    const auto barrierAt = [&](std::int32_t x, std::int32_t y, std::int32_t z) {
        return interpreter_.evaluate(barrierNode, density::Point{.x = x, .y = y, .z = z}, cache);
    };
    const auto floodednessAt = [&](std::int32_t x, std::int32_t y, std::int32_t z) {
        return interpreter_.evaluate(floodednessNode, density::Point{.x = x, .y = y, .z = z},
                                     cache);
    };
    const auto spreadAt = [&](std::int32_t x, std::int32_t y, std::int32_t z) {
        return interpreter_.evaluate(spreadNode, density::Point{.x = x, .y = y, .z = z}, cache);
    };
    const auto lavaAt = [&](std::int32_t x, std::int32_t y, std::int32_t z) {
        return interpreter_.evaluate(lavaNode, density::Point{.x = x, .y = y, .z = z}, cache);
    };
    const auto pslAt = [&](std::int32_t x, std::int32_t y, std::int32_t z) {
        return interpreter_.evaluate(pslNode, density::Point{.x = x, .y = y, .z = z}, cache);
    };

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
                            const std::int32_t blockX = baseX + localX;
                            const std::int32_t blockZ = baseZ + localZ;
                            if (density > 0.0) {
                                // Q2.2: unconditional, whatever the aquifer
                                // would otherwise say — it only ever
                                // replaces what would otherwise be
                                // non-solid, or adds solid via the barrier.
                                block = &settings_->defaultBlock;
                            } else if (aquiferCentres_.has_value()) {
                                const aquifer::AquiferQuery query{.x = blockX,
                                                                  .y = y,
                                                                  .z = blockZ,
                                                                  .density = density,
                                                                  .seaLevel = settings_->seaLevel};
                                const aquifer::SubstanceAt result = aquifer::computeSubstance(
                                    *aquiferCentres_, query, aquiferLevelCache, barrierAt,
                                    floodednessAt, spreadAt, lavaAt, pslAt);
                                switch (result.substance) {
                                    case aquifer::Substance::Solid:
                                        block = &settings_->defaultBlock;
                                        break;
                                    case aquifer::Substance::Fluid:
                                        block = (result.fluidType == aquifer::FluidType::Lava)
                                                    ? &lava()
                                                    : &settings_->defaultFluid;
                                        break;
                                    case aquifer::Substance::Air:
                                        break;
                                }
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

    // One cache for every density read this whole second pass makes —
    // `preliminarySurface` and the six climate router entries alike — shared
    // across every column of the chunk, the same way the first pass's own
    // cache is. `interpolated` nodes recompute their eight corners from
    // scratch with no cache, measured at 87 times slower over a cell
    // (ChunkFiller::fill's own comment); a tree that reads `biome` or
    // `temperature` calls the climate router for every one of 256 columns,
    // so leaving this uncached here cost the same multiple across the whole
    // chunk rather than one cell.
    density::Interpreter::CornerCache surfaceCache(interpreter_.cacheSize());

    // Cached across the WHOLE chunk, not just down one column: a chunk is
    // only 4 quart-cells wide, so up to 16 of its 256 columns share the same
    // one — and without this, each repeated a from-scratch linear search
    // over the biome parameter table (7593 rows for the real overworld)
    // rather than reusing the first column's answer. Measured
    // (tools/analysis/generate-world.cpp): this took a real chunk's second
    // pass from about 8 seconds to about 0.5.
    std::map<std::tuple<std::int32_t, std::int32_t, std::int32_t>, data::ResourceLocation>
        biomeCache;

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
                    density::Point{.x = x, .y = 0, .z = z}, surfaceCache));
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
            // once a block — and `temperature` rides the same grid, since it
            // has to know which biome a block sits in before it can look up
            // that biome's own declared value. A sentinel bool rather than
            // optional<int32_t>: every read of the cached value is already
            // guarded by it, and spelling that as has_value()/operator* left
            // the guard too indirect for bugprone-unchecked-optional-access
            // to see.
            const bool needsBiomeIdentity = surfaceNeedsBiome_ || surfaceNeedsTemperature_;
            const std::int32_t qx = quartSnap(x);
            const std::int32_t qz = quartSnap(z);
            bool biomeQuartYKnown = false;
            std::int32_t biomeQuartY = 0;
            data::ResourceLocation biomeId{"minecraft", "plains"};
            for (std::int32_t y = topY - 1; y >= minY; --y) {
                // A FLUID position only stays eligible for surface rules
                // while it is part of the column's FIRST (topmost, reached
                // straight from the sky) fluid body — `stoneDepthAbove == 0`,
                // meaning no solid has been crossed yet on the way down. A
                // deeper, isolated cave-void's fluid inherits a nonzero run
                // carried over from the solid rock above it (Context's own
                // doc: fluid neither breaks a stone-depth run nor counts
                // toward it), and real vanilla never rewrites that fluid at
                // all — confirmed against the real server (tools/analysis,
                // see SPEC §11): a datapack whose ENTIRE surface_rule is the
                // overworld's own bare, unconditioned `deepslate`
                // vertical_gradient — no above_preliminary_surface, no
                // bedrock floor, nothing else gating it — still leaves
                // exactly the same 475 fluid blocks untouched that vanilla's
                // full 287-rule tree does, in the same aquifer-free probe
                // golden_fill_test.cpp reads, and every one of those 475 is
                // inside a fluid-filled void with solid stone already
                // crossed above it. The topmost/open-water case (real ocean
                // straight from the sky, `stoneDepthAbove == 0` throughout)
                // is left reachable, which is what a rule keyed on `water`
                // — freezing ice onto a lake's own surface — needs.
                const Category category = categorize(into.at(localX, y, localZ), *settings_);
                if (category == Category::Fluid &&
                    stoneDepthAbove[static_cast<std::size_t>(y - minY)] != 0) {
                    continue;
                }
                if (needsBiomeIdentity) {
                    const std::int32_t quartY = quartSnap(y);
                    if (!biomeQuartYKnown || biomeQuartY != quartY) {
                        const auto key = std::tuple{qx, quartY, qz};
                        const auto cached = biomeCache.find(key);
                        if (cached != biomeCache.end()) {
                            biomeId = cached->second;
                        } else {
                            const density::Point at{.x = qx, .y = quartY, .z = qz};
                            const biome::ClimateSample sample{
                                .temperature = interpreter_.evaluate(
                                    settings_->router.at(settings::RouterEntry::Temperature), at,
                                    surfaceCache),
                                .humidity = interpreter_.evaluate(
                                    settings_->router.at(settings::RouterEntry::Vegetation), at,
                                    surfaceCache),
                                .continentalness = interpreter_.evaluate(
                                    settings_->router.at(settings::RouterEntry::Continents), at,
                                    surfaceCache),
                                .erosion = interpreter_.evaluate(
                                    settings_->router.at(settings::RouterEntry::Erosion), at,
                                    surfaceCache),
                                .depth = interpreter_.evaluate(
                                    settings_->router.at(settings::RouterEntry::Depth), at,
                                    surfaceCache),
                                .weirdness = interpreter_.evaluate(
                                    settings_->router.at(settings::RouterEntry::Ridges), at,
                                    surfaceCache)};
                            biomeId = biomeParameters_->find(sample);
                            biomeCache.emplace(key, biomeId);
                        }
                        biomeQuartY = quartY;
                        biomeQuartYKnown = true;
                    }
                    if (surfaceNeedsBiome_) {
                        context.biome = biomeId;
                    }
                    if (surfaceNeedsTemperature_) {
                        context.biomeTemperature = biomeTemperatures_->at(biomeId);
                    }
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
