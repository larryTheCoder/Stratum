// Stratum — turning a density field into blocks.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum/aquifer/substance.hpp>
#include <stratum/javamath.hpp>
#include <stratum/ore/vein.hpp>
#include <stratum/terrain/filler.hpp>
#include <stratum/terrain/surface_column.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <optional>
#include <span>
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

/// The six blocks the vein system places. Block identifiers only — the one
/// class of Mojang-derived table this repo commits — and confirmed against
/// the server rather than transcribed: iron's whole range sits below y = 0,
/// so its ore is never anything but the deepslate variant.
[[nodiscard]] const std::array<settings::BlockState, 6>& veinBlocks() {
    const auto named = [](const char* name) {
        return settings::BlockState{.name = data::ResourceLocation::parse(name), .properties = {}};
    };
    // Order is load-bearing: `veinBlock` indexes into this by metal (copper's
    // three, then iron's) and then by filler/ore/raw within the metal.
    static const std::array<settings::BlockState, 6> kBlocks{named("minecraft:granite"),
                                                             named("minecraft:copper_ore"),
                                                             named("minecraft:raw_copper_block"),
                                                             named("minecraft:tuff"),
                                                             named("minecraft:deepslate_iron_ore"),
                                                             named("minecraft:raw_iron_block")};
    return kBlocks;
}

[[nodiscard]] const settings::BlockState& veinBlock(const ore::Vein& vein) {
    const std::size_t base = vein.type == ore::VeinType::Copper ? 0U : 3U;
    switch (vein.block) {
        case ore::VeinBlock::Filler:
            return veinBlocks()[base];
        case ore::VeinBlock::Ore:
            return veinBlocks()[base + 1U];
        case ore::VeinBlock::RawBlock:
            return veinBlocks()[base + 2U];
        case ore::VeinBlock::None:
            break;
    }
    // Unreachable: fill() only asks once `placed()` is true. Throwing rather
    // than returning stone keeps a future caller that forgets that from
    // silently painting the world with the default block.
    throw FillError("veinBlock asked for a block for a position the vein system did not place");
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

/// The palette indices of the blocks one fill places, kept by the ADDRESS of
/// the state placed. Every state the fill places lives in storage that
/// outlives the fill and never changes — air() and lava(), the vein blocks,
/// the dimension's own default block and fluid, the surface rules' blocks
/// and bandlands' table — so one address is one value for the whole fill,
/// and the index ChunkBuffer::intern gave it the first time is the index it
/// would give it every time: the palette only grows, and holds each value
/// once. A state not seen yet is interned by value then, in the order it is
/// first placed, which is the palette order a value-by-value set() made.
///
/// Bounded: past kCapacity distinct addresses a state is interned by value
/// on every placement, which is what set() always did.
class StablePalette {
public:
    template<typename Intern>
    [[nodiscard]] std::uint16_t indexOf(const settings::BlockState& block, Intern&& intern) {
        for (std::size_t i = 0; i < size_; ++i) {
            if (keys_[i] == &block) {
                return values_[i];
            }
        }
        const std::uint16_t index = intern(block);
        if (size_ < kCapacity) {
            keys_[size_] = &block;
            values_[size_] = index;
            ++size_;
        }
        return index;
    }

private:
    static constexpr std::size_t kCapacity = 32;

    std::array<const settings::BlockState*, kCapacity> keys_{};
    std::array<std::uint16_t, kCapacity> values_{};
    std::size_t size_ = 0;
};

/// Snaps down onto the biome grid: vanilla resolves one biome per 4x4x4
/// cell and surface rules read that grid rather than resampling per block
/// (measured in `vanilla_biomes_test.cpp` — the lower corner of the cell).
[[nodiscard]] std::int32_t quartSnap(const std::int32_t value) noexcept {
    return javamath::floorDiv(value, std::int32_t{4}) * std::int32_t{4};
}

} // namespace

// THE CATEGORY. The surface pass's WRITE does not rely on this — it writes
// over `default_block` only (`applySurfaceRules`) — but its two stone-depth
// runs and its water height do, and the server treats lava there exactly as
// water: measured on `tools/analysis/aquifer-lavarun-probe.sh` (SPEC §11),
// where this called lava Solid and so counted it toward both runs. Here, next
// to `air()` and `lava()`, so the blocks it compares against are the very
// ones the first pass writes.
Category categorize(const settings::BlockState& block, const settings::NoiseSettings& settings) {
    if (block == air()) {
        return Category::Air;
    }
    if (block == settings.defaultFluid || block == lava()) {
        return Category::Fluid;
    }
    return Category::Solid;
}

void SurfaceColumn::read(const std::span<const Category> categories, const std::int32_t minY) {
    const std::size_t height = categories.size();
    minY_ = minY;
    above_.resize(height);
    below_.resize(height);
    waterHeight_.reset();
    top_ = minY - 1;
    // Top down: the run counting from the world's top, and the water height
    // latched at the first fluid block met descending. Air resets the run;
    // fluid — water or lava alike — neither breaks it nor counts toward it,
    // and lava latches the height as water does (surface::Context's own doc,
    // measured).
    std::int32_t run = 0;
    for (std::size_t i = height; i-- > 0;) {
        const Category category = categories[i];
        const std::int32_t y = minY + static_cast<std::int32_t>(i);
        if (category == Category::Air) {
            run = 0;
        } else {
            if (top_ < minY) {
                top_ = y;
            }
            if (category == Category::Solid) {
                ++run;
            } else if (!waterHeight_.has_value()) {
                waterHeight_ = y + 1;
            }
        }
        above_[i] = run;
    }
    // The run counted from the world's floor, for `surface_type: ceiling` —
    // and NOT the mirror image of the one above: bottom up, fluid RESETS the
    // run exactly as air does, water as much as lava. Measured
    // (aquifer-lavarun-probe.sh, SPEC §11): stone over an enclosed pool reads
    // depth 0 from the pool's roof up, on every column of every pool, where a
    // run that skipped the fluid — the filler's reading until then, for water
    // too — left it stone.
    run = 0;
    for (std::size_t i = 0; i < height; ++i) {
        run = categories[i] == Category::Solid ? run + 1 : 0;
        below_[i] = run;
    }
}

std::int32_t SurfaceColumn::stoneDepthAbove(const std::int32_t y) const {
    return above_.at(static_cast<std::size_t>(static_cast<std::int64_t>(y) - minY_));
}

std::int32_t SurfaceColumn::stoneDepthBelow(const std::int32_t y) const {
    return below_.at(static_cast<std::size_t>(static_cast<std::int64_t>(y) - minY_));
}

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

void ChunkBuffer::refuseOutside(int localX, std::int32_t y, int localZ) const {
    if (localX < 0 || localX >= kChunkWidth || localZ < 0 || localZ >= kChunkWidth) {
        throw FillError("block (" + std::to_string(localX) + ", " + std::to_string(localZ) +
                        ") is outside the chunk; local coordinates run 0 to 15");
    }
    throw FillError("y " + std::to_string(y) + " is outside this dimension, which runs " +
                    std::to_string(minY_) + " to " + std::to_string(minY_ + height_ - 1));
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

void ChunkBuffer::clear() {
    // Entry 0 is air in every buffer (the constructor's), so zeroing the
    // indices is "every block air" whatever else the palette held.
    palette_.erase(palette_.begin() + 1, palette_.end());
    std::ranges::fill(blocks_, std::uint16_t{0});
    fluidUpdates_.clear();
}

void ChunkBuffer::markFluidUpdate(int localX, std::int32_t y, int localZ) {
    static_cast<void>(indexOf(localX, y, localZ)); // the bounds check, nothing more
    fluidUpdates_.push_back(FluidUpdate{.localX = static_cast<std::uint8_t>(localX),
                                        .y = y,
                                        .localZ = static_cast<std::uint8_t>(localZ)});
}

void ChunkBuffer::orderFluidUpdatesByCells(const std::int32_t cellWidth) {
    if (cellWidth < 1) {
        throw FillError("cannot order fluid updates by cells " + std::to_string(cellWidth) +
                        " blocks wide");
    }
    // Local x and z are 0..15, so plain division and remainder are exact
    // here; y is the only signed coordinate, and it is only compared.
    const auto key = [cellWidth](const FluidUpdate& update) {
        const std::int32_t x = update.localX;
        const std::int32_t z = update.localZ;
        return std::tuple{x / cellWidth, z / cellWidth, -update.y, x % cellWidth, z % cellWidth};
    };
    std::ranges::sort(fluidUpdates_, [&key](const FluidUpdate& lhs, const FluidUpdate& rhs) {
        return key(lhs) < key(rhs);
    });
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
    std::optional<biome::Source> source;
    if (biomeParameters != nullptr) {
        source = biome::Source::multiNoise(*biomeParameters);
    }
    return compileWith(graph, noises, settings, surfaceRules, source, biomeTemperatures);
}

ChunkFiller ChunkFiller::compile(const density::Graph& graph, const density::NoiseRegistry& noises,
                                 const settings::NoiseSettings& settings,
                                 const surface::RuleGraph* surfaceRules,
                                 const biome::Source& biomeSource,
                                 const biome::TemperatureTable* biomeTemperatures) {
    return compileWith(graph, noises, settings, surfaceRules, biomeSource, biomeTemperatures);
}

ChunkFiller ChunkFiller::compileWith(const density::Graph& graph,
                                     const density::NoiseRegistry& noises,
                                     const settings::NoiseSettings& settings,
                                     const surface::RuleGraph* surfaceRules,
                                     const std::optional<biome::Source>& biomeSource,
                                     const biome::TemperatureTable* biomeTemperatures) {
    // WIRED (SPEC §10 milestone MA, §11): the cell lattice, the centre
    // jitter, the fluid level rule (including its ocean branch and the
    // depth path's anchor gate), source selection (selection.hpp), and the
    // three-source barrier predicate (barrier.hpp) are all measured and
    // called from here now, via `aquifer::computeSubstance`.
    //
    // The global lava sea (Q2.4), the water-over-lava exception on its top
    // row (Q6.3) and Pi's mixed-fluid-type branch (Q6.4 — every ranked
    // source is typed, and a water/lava junction pushes with a constant)
    // are in `computeSubstance` too, all measured against the server
    // (`aquifer/substance.hpp`'s and `aquifer/barrier.hpp`'s own headers
    // carry the numbers).
    ChunkFiller filler(graph, noises, settings);
    // Raised here, at compile, rather than on the first block of the first
    // chunk: a caller that cannot generate should learn so before it starts.
    filler.interpreter_.requireEvaluable(filler.finalDensity_);

    // Ore veins, wired (SPEC §10, M3; SPEC §11), on `ore_veins_enabled`
    // ALONE since pipeline engine v14. They were gated on `aquifers_enabled`
    // too, from one probe world that came back unbroken stone with aquifers
    // off; its seed's window holds no position the vein gate admits, so the
    // library predicts it stone with aquifers on as well, and the contrast
    // that showed veins had moved the seed with the flag. With aquifers off
    // the recipe at a seed that has veins is the aquifers-on world block for
    // block, and so are the legacy probe's aquifers-off arms under both
    // sources, so the gate is gone (ore/vein.hpp).
    //
    // NOT refused under a legacy source any more. The vein source is a
    // positional random drawn through the dimension's declared source, not
    // from any named noise, so it never reaches NoiseRegistry::create's
    // `wanted` list; it was refused by name here while nothing said how it
    // draws under the legacy source. `tools/analysis/legacy-orevein-probe.sh`
    // asked the server: the legacy positional primitive salted
    // "minecraft:ore", nextFloat thrice, exact on every block of its legacy
    // arms (SPEC §11, "Ore veins under the legacy source"). So the source is
    // built from the registry's own, as the aquifer's centres are below.
    if (settings.oreVeinsEnabled) {
        filler.oreVeins_.emplace(noises.worldSeed(), noises.source());
        for (const settings::RouterEntry entry :
             {settings::RouterEntry::VeinToggle, settings::RouterEntry::VeinRidged,
              settings::RouterEntry::VeinGap}) {
            filler.interpreter_.requireEvaluable(settings.router.at(entry));
        }
    }

    // The block kernels of the entries fill() reads at every block, each
    // after its entry was required evaluable (density::Interpreter::
    // BlockKernel says what lowers; where nothing does, fill() walks the
    // graph as before).
    filler.finalDensityKernel_ = filler.interpreter_.blockKernel(filler.finalDensity_);
    if (settings.oreVeinsEnabled) {
        filler.veinToggleKernel_ =
            filler.interpreter_.blockKernel(settings.router.at(settings::RouterEntry::VeinToggle));
        filler.veinRidgedKernel_ =
            filler.interpreter_.blockKernel(settings.router.at(settings::RouterEntry::VeinRidged));
        filler.veinGapKernel_ =
            filler.interpreter_.blockKernel(settings.router.at(settings::RouterEntry::VeinGap));
    }

    if (settings.aquifersEnabled) {
        // NOT refused under a legacy source, as the veins above are not. The
        // lattice's centre jitter is a positional random from the
        // dimension's declared source, and no vanilla legacy dimension
        // enables aquifers — but the server generates one when a datapack
        // asks, and `tools/analysis/legacy-aquifer-probe.sh` asked: the
        // legacy derivation is measured (SPEC §11, "The aquifer under the
        // legacy source") and `aquiferCentres_` below is built from the
        // registry's own source. A legacy dimension whose aquifer router
        // names vanilla's aquifer noises is still refused, by
        // NoiseRegistry::create, for its noises' seeding.
        //
        // The aquifer types a source as the dimension's default fluid or as
        // lava, and two of its rules are written for WATER specifically:
        // Q6.3's exception is water resting on the lava sea, and Q6.4's
        // mixed-type constant is lava against water. With any other default
        // fluid those two readings part from "default fluid", and no
        // world has ever been measured to say which the server takes —
        // every vanilla dimension that enables aquifers uses water. Refused
        // by name rather than guessed (SPEC §8).
        if (settings.defaultFluid.name != data::ResourceLocation::parse("minecraft:water")) {
            throw FillError("aquifers_enabled with default_fluid " +
                            settings.defaultFluid.name.toString() +
                            " is refused: the aquifer's water-over-lava exception and its "
                            "lava-against-water pressure are measured only for a water "
                            "default fluid, and no vanilla dimension enables aquifers with "
                            "any other");
        }
        // The salted positional source (SPEC §4) is per-world, not per-block
        // — built once here from the registry's own seed AND its own random
        // source rather than re-derived on every call to fill().
        filler.aquiferCentres_.emplace(noises.worldSeed(), noises.source());
        for (const settings::RouterEntry entry :
             {settings::RouterEntry::Barrier, settings::RouterEntry::FluidLevelFloodedness,
              settings::RouterEntry::FluidLevelSpread, settings::RouterEntry::Lava,
              settings::RouterEntry::PreliminarySurfaceLevel, settings::RouterEntry::Erosion,
              settings::RouterEntry::Depth}) {
            // Erosion and depth for Q5.9's deep-dark override, read at every
            // source centre — required whenever aquifers are, not only when a
            // surface tree happens to read the biome.
            filler.interpreter_.requireEvaluable(settings.router.at(entry));
        }
        // Where the barrier is read. computeSubstance skips the read where
        // Q6.2 decides without it, which no block can see — provided the
        // read has no effect but its value. Two node types make that
        // unprovable, and a barrier reaching either keeps every read it
        // always had (aquifer::BarrierReads::Always):
        //
        //   * `interpolated`. The barrier is the one aquifer read made in
        //     the BLOCK context, so its cells' corners are read from, and
        //     written to, the CornerCache the terrain's own reads share —
        //     keyed by cell, not by the flat_cache window a corner was read
        //     through. Every other aquifer read is detached and never
        //     touches the cache.
        //   * `find_top_surface`, which refuses a scan longer than its step
        //     bound when it is evaluated, on a value — a chunk that threw
        //     because of one skippable read must still throw.
        //
        // Vanilla's barrier is a bare noise, so every vanilla preset reads it
        // only where it is weighed.
        const std::vector<density::NodeIndex> barrierTree =
            graph.reachableFrom(settings.router.at(settings::RouterEntry::Barrier));
        filler.barrierReadsAlways_ =
            std::ranges::any_of(barrierTree, [&graph](const density::NodeIndex index) {
                const density::NodeType type = graph.node(index).type;
                return type == density::NodeType::Interpolated ||
                       type == density::NodeType::FindTopSurface;
            });
        // And how the fill's CornerCache shares corners. The barrier is the
        // one read this fill makes through its chunk's window in the Block
        // context; the terrain's and the veins' are read through none, and
        // the rest are detached and never blend. So a cell corner is read
        // through two windows only where the barrier reaches an
        // `interpolated` — and the window can change a corner's value only
        // where that argument reaches a `flat_cache`. There the cache keeps
        // what every fill did before the corner lattice: each argument's
        // last cell, whichever window filled it (Sharing::LastCell), so the
        // blocks stay the same bit for bit. No vanilla preset and no probe
        // arm is that pack: vanilla's barrier is a bare noise.
        const bool barrierCornersReadWindow =
            std::ranges::any_of(barrierTree, [&graph](const density::NodeIndex index) {
                const density::Node& node = graph.node(index);
                if (node.type != density::NodeType::Interpolated || node.arguments.empty()) {
                    return false;
                }
                const std::vector<density::NodeIndex> argumentTree =
                    graph.reachableFrom(node.arguments.front());
                return std::ranges::any_of(argumentTree, [&graph](const density::NodeIndex inner) {
                    return graph.node(inner).type == density::NodeType::FlatCache;
                });
            });
        if (barrierCornersReadWindow) {
            filler.cornerSharing_ = density::Interpreter::CornerCache::Sharing::LastCell;
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
        // biome source `biome` itself does — a tree naming only
        // `temperature` still needs one, even though it never names `biome`
        // directly.
        const bool needsBiomeIdentity = needs.biome || needs.temperature;
        if (needsBiomeIdentity && !biomeSource.has_value()) {
            filler.surfaceRulesBlockedBy_.emplace_back(
                "minecraft:biome (this dimension's rules read the biome, directly or through a "
                "biome's declared temperature, and no biome source — a biome::ParameterList or "
                "a biome::Source — was supplied to ChunkFiller::compile)");
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
        if (surface::readsSurfaceDepth(*surfaceRules) &&
            noises.find(data::ResourceLocation::parse("minecraft:surface")) == nullptr) {
            // Same shape as the bandlands case below, and reached by more
            // trees than it looks: `above_preliminary_surface` reads a
            // surface depth (SPEC §11) though neither its name nor its
            // schema says so, and `hole` and a depth-adding `stone_depth`
            // always did. Without this the tree compiled and then threw at
            // the first block.
            filler.surfaceRulesBlockedBy_.emplace_back(
                "minecraft:surface (this dimension's rules read a column's surface depth, and no "
                "minecraft:surface noise was built into the registry supplied to "
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
            filler.biomeSource_ = biomeSource;
            filler.biomeTemperatures_ = biomeTemperatures;
            if (needs.preliminarySurface) {
                filler.interpreter_.requireEvaluable(
                    settings.router.at(settings::RouterEntry::PreliminarySurfaceLevel));
            }
            if (needsBiomeIdentity && biomeSource.has_value()) {
                // What the source reads and nothing more: the End's reads
                // erosion alone, so its other climate entries need not be
                // evaluable for its rules to run.
                for (const settings::RouterEntry entry : biomeSource->routerEntries()) {
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
    if (!density::chunkInDomain(chunkX, chunkZ)) {
        throw FillError("chunk (" + std::to_string(chunkX) + ", " + std::to_string(chunkZ) +
                        ") is outside the ±" + std::to_string(density::kMaxChunkCoordinate) +
                        " chunks whose block arithmetic stays in 32 bits; this build refuses "
                        "it (SPEC §8)");
    }
    const settings::NoiseGeometry& geometry = settings_->geometry;
    if (into.minY() != geometry.minY || into.height() != geometry.height) {
        throw FillError("this buffer is " + std::to_string(into.height()) + " blocks from " +
                        std::to_string(into.minY()) + ", and the dimension is " +
                        std::to_string(geometry.height) + " from " + std::to_string(geometry.minY));
    }
    // The pass below writes every block, but only appends to the palette and
    // to the fluid updates: a reused buffer would otherwise carry an earlier
    // chunk's marks into this one's list.
    into.clear();

    const std::int32_t baseX = chunkX * kChunkWidth;
    const std::int32_t baseZ = chunkZ * kChunkWidth;
    const std::int32_t cellWidth = geometry.cellWidth();
    const std::int32_t cellHeight = geometry.cellHeight();
    const std::int32_t topY = geometry.minY + geometry.height;

    density::Interpreter::CornerCache cache(interpreter_.cacheSize(), cornerSharing_);
    // A chunk touches dozens of distinct cell centres, not thousands of
    // blocks' worth of them — see aquifer::StatusCache's own doc.
    aquifer::StatusCache aquiferStatusCache;

    // Each block is written as its palette index (StablePalette), at a
    // position the loops below keep inside the chunk and the dimension —
    // the buffer's geometry was checked against the dimension's above — so
    // what set() did per block, a bounds check and a search of the palette
    // by value, is done once per distinct state.
    StablePalette stablePalette;
    const auto intern = [&into](const settings::BlockState& block) { return into.intern(block); };
    // The last block's state and its index (the block loop's write).
    const settings::BlockState* lastBlock = nullptr;
    std::uint16_t lastIndex = 0;
    // air()'s one state, looked up once rather than once a block.
    const settings::BlockState* const airBlock = &air();

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
    // Every aquifer read goes through THIS chunk's flat_cache window: a
    // source centre in a neighbouring chunk reads its router values where it
    // is, not at a 4x4 corner this chunk's grid does not hold. Measured
    // through Q5.9 (`deepDarkAt` below), the one read in the vanilla presets
    // that can tell, and through every entry a datapack can wrap; see
    // `flatCacheWindow`.
    //
    // And every read but the barrier's is DETACHED: it is a point this block
    // reads somewhere else — a source's centre, its contracted spread and
    // lava indices, its surface anchors — so an `interpolated` there reads
    // its argument at that point rather than blending over a cell. The
    // barrier is read at the block itself and blends like terrain does.
    // Measured, both halves (density::ReadContext, SPEC §11).
    const density::FlatCacheWindow window = flatCacheWindow(chunkX, chunkZ);
    const auto aquiferRead = [&](density::NodeIndex node, std::int32_t x, std::int32_t y,
                                 std::int32_t z) {
        return interpreter_.evaluate(node, density::Point{.x = x, .y = y, .z = z}, cache, window,
                                     density::ReadContext::Detached);
    };
    const auto barrierAt = [&](std::int32_t x, std::int32_t y, std::int32_t z) {
        return interpreter_.evaluate(barrierNode, density::Point{.x = x, .y = y, .z = z}, cache,
                                     window, density::ReadContext::Block);
    };
    const auto floodednessAt = [&](std::int32_t x, std::int32_t y, std::int32_t z) {
        return aquiferRead(floodednessNode, x, y, z);
    };
    const auto spreadAt = [&](std::int32_t x, std::int32_t y, std::int32_t z) {
        return aquiferRead(spreadNode, x, y, z);
    };
    const auto lavaAt = [&](std::int32_t x, std::int32_t y, std::int32_t z) {
        return aquiferRead(lavaNode, x, y, z);
    };
    // The raw entry at each scan sample's own column — NOT through the
    // 16-block lattice the surface rule reads this same entry through
    // (`preliminarySurfaceIn`, below). Two readings of one entry, on purpose:
    // head to head, the server sides with the lattice on no block where the
    // two part (aquifer::readPreliminarySurface, SPEC §11).
    //
    // Memoized by position for this chunk: each read is a whole
    // `find_top_surface` column scan, and the y_skip samples and the
    // sources' anchors and windows land on the same 4-aligned positions
    // again and again. Exact by construction: a detached read is a function
    // of its position and this chunk's window alone, never of the
    // CornerCache (aquifer::MemoizedRead).
    aquifer::MemoizedRead pslMemo([&](std::int32_t x, std::int32_t y, std::int32_t z) {
        return aquiferRead(pslNode, x, y, z);
    });
    const auto pslAt = [&pslMemo](std::int32_t x, std::int32_t y, std::int32_t z) {
        return pslMemo(x, y, z);
    };
    const density::NodeIndex erosionNode =
        settings_->aquifersEnabled ? settings_->router.at(settings::RouterEntry::Erosion)
                                   : density::NodeIndex{};
    const density::NodeIndex depthNode = settings_->aquifersEnabled
                                             ? settings_->router.at(settings::RouterEntry::Depth)
                                             : density::NodeIndex{};
    const auto deepDarkAt = [&](std::int32_t x, std::int32_t y, std::int32_t z) {
        return aquifer::isDeepDark(aquiferRead(erosionNode, x, y, z),
                                   aquiferRead(depthNode, x, y, z));
    };

    // Q2.3/Q2.5: above `y_skip` the local aquifer is never consulted and the
    // global picker decides. One height per chunk, from the highest floored
    // `preliminary_surface_level` over the chunk's lattice rectangle, sampled
    // every four blocks with both endpoints included.
    const std::int32_t ySkipLevel = settings_->aquifersEnabled
                                        ? aquifer::chunkYSkip(pslAt, baseX, baseZ)
                                        : std::numeric_limits<std::int32_t>::max();

    // Only ever read when oreVeins_ holds a source, same as the aquifer's
    // five above, and looked up once per fill() rather than per block.
    const density::NodeIndex veinToggleNode =
        oreVeins_.has_value() ? settings_->router.at(settings::RouterEntry::VeinToggle)
                              : density::NodeIndex{};
    const density::NodeIndex veinRidgedNode =
        oreVeins_.has_value() ? settings_->router.at(settings::RouterEntry::VeinRidged)
                              : density::NodeIndex{};
    const density::NodeIndex veinGapNode =
        oreVeins_.has_value() ? settings_->router.at(settings::RouterEntry::VeinGap)
                              : density::NodeIndex{};

    // Cell by cell, then block by block within the cell. The order is the
    // whole point: every block of a cell shares the eight corner values
    // `interpolated` needs, and visiting them together is what lets the cache
    // hold. Column-major order would evict on every block and cost 87 times
    // as much, which is measured rather than guessed.
    //
    // And a cell's blocks of one y are a run of the kernels (density::
    // Interpreter::evaluate(kernel, blocks, lanes, cache)): `final_density`
    // at every one, then `vein_toggle` at those it makes solid inside the
    // vein range — each a block the loop below reads it at, solid ground
    // being the default block whatever the aquifer says. A run stands for
    // those evaluations bit for bit, made before the aquifer's reads in
    // between rather than among them, which changes no value under the
    // corner memo (the only sharing it runs under) and leaves every refusal
    // to the block's own evaluation (it refuses nothing itself). Where it
    // does not run, the loop evaluates block by block, as before.
    density::Interpreter::BlockKernel::Lanes densityLanes;
    density::Interpreter::BlockKernel::Lanes toggleLanes;
    constexpr auto kRunLanes = static_cast<std::int32_t>(density::Interpreter::BlockKernel::kLanes);
    for (std::int32_t cellZ = 0; cellZ < kChunkWidth; cellZ += cellWidth) {
        for (std::int32_t cellX = 0; cellX < kChunkWidth; cellX += cellWidth) {
            const std::int32_t cellEndX = std::min(cellX + cellWidth, std::int32_t{kChunkWidth});
            const std::int32_t cellEndZ = std::min(cellZ + cellWidth, std::int32_t{kChunkWidth});
            // A run is as many of the cell's rows along x as its lanes hold.
            const std::int32_t runWidth = cellEndX - cellX;
            const std::int32_t runRows = std::max(std::int32_t{1}, kRunLanes / runWidth);
            for (std::int32_t cellBottom = geometry.minY; cellBottom < topY;
                 cellBottom += cellHeight) {
                const std::int32_t cellTop = std::min(cellBottom + cellHeight, topY);
                for (std::int32_t y = cellBottom; y < cellTop; ++y) {
                    // What a block's y alone decides, once a y: whether the
                    // aquifer is asked below a non-positive density, what
                    // stands there when it is not, and whether veins are
                    // read on solid ground, and at what richness.
                    //
                    // Not asked — aquifers off, or (Q2.3) above y_skip —
                    // the global picker stands there, the same in both:
                    // lava under min(-54, sea_level), default_fluid under
                    // sea_level, air above. Aquifers off still has that
                    // global lava sea (spec Q1.2, Q2.1): measured on the
                    // aquifer-free probe (tools/analysis/aquifer-free-
                    // probe.sh), 1005 positions at y -58..-55 the server
                    // filled with lava, all of which this used to write as
                    // water — invisible to every category-only comparison,
                    // where both are "fluid". And `sea_level` is EXCLUSIVE:
                    // with vanilla's 63 the water stops at 62 and 63 is the
                    // first air. That is measured, not read — an inclusive
                    // comparison puts one extra water block on top of every
                    // column in the world, which is 256 a chunk and exactly
                    // what the golden comparison found.
                    const bool consultsAquifer =
                        aquiferCentres_.has_value() && aquifer::consultsLattice(y, ySkipLevel);
                    const settings::BlockState* unconsulted = airBlock;
                    if (y < aquifer::lambdaLevel(settings_->seaLevel)) {
                        unconsulted = &lava();
                    } else if (y < settings_->seaLevel) {
                        unconsulted = &settings_->defaultFluid;
                    }
                    const bool readsVeins = oreVeins_.has_value() && ore::inAnyVeinRange(y);
                    const ore::RichnessGate richness(y);
                    std::int32_t runZ = cellZ;
                    bool densities = false;
                    bool toggles = false;
                    for (std::int32_t localZ = cellZ; localZ < cellEndZ; ++localZ) {
                        if (localZ == cellZ || localZ - runZ == runRows) {
                            runZ = localZ;
                            const density::Interpreter::BlockKernel::Blocks run{
                                .first =
                                    density::Point{.x = baseX + cellX, .y = y, .z = baseZ + localZ},
                                .width = runWidth,
                                .depth = std::min(runRows, cellEndZ - localZ),
                                .lanes = ~0U};
                            densities = finalDensityKernel_.has_value() &&
                                        interpreter_.evaluate(*finalDensityKernel_, run,
                                                              densityLanes, cache);
                            toggles = false;
                            if (densities && oreVeins_.has_value() &&
                                veinToggleKernel_.has_value() && ore::inAnyVeinRange(y)) {
                                std::uint32_t solid = 0;
                                const std::size_t count = static_cast<std::size_t>(run.width) *
                                                          static_cast<std::size_t>(run.depth);
                                for (std::size_t lane = 0; lane < count; ++lane) {
                                    solid |= (densityLanes.value(lane) > 0.0 ? 1U : 0U) << lane;
                                }
                                if (solid != 0U) {
                                    density::Interpreter::BlockKernel::Blocks solidRun = run;
                                    solidRun.lanes = solid;
                                    toggles = interpreter_.evaluate(*veinToggleKernel_, solidRun,
                                                                    toggleLanes, cache);
                                }
                            }
                        }
                        // This row's first lane and first block in the
                        // buffer, from which each block along x is the next.
                        const std::size_t rowLane = static_cast<std::size_t>(localZ - runZ) *
                                                    static_cast<std::size_t>(runWidth);
                        const std::size_t rowIndex = into.linearIndex(cellX, y, localZ);
                        for (std::int32_t localX = cellX; localX < cellEndX; ++localX) {
                            const auto offset = static_cast<std::size_t>(localX - cellX);
                            const std::size_t lane = rowLane + offset;
                            const double density =
                                densities ? densityLanes.value(lane)
                                          : evaluateAtBlock(finalDensityKernel_, finalDensity_,
                                                            density::Point{.x = baseX + localX,
                                                                           .y = y,
                                                                           .z = baseZ + localZ},
                                                            cache);

                            // Non-solid ground the aquifer is not asked about
                            // is `unconsulted`, the global picker (above).
                            const settings::BlockState* block = unconsulted;
                            const std::int32_t blockX = baseX + localX;
                            const std::int32_t blockZ = baseZ + localZ;
                            if (density > 0.0) {
                                // Q2.2: unconditional, whatever the aquifer
                                // would otherwise say — it only ever
                                // replaces what would otherwise be
                                // non-solid, or adds solid via the barrier.
                                block = &settings_->defaultBlock;
                            } else if (consultsAquifer) {
                                block = airBlock;
                                const aquifer::AquiferQuery query{.x = blockX,
                                                                  .y = y,
                                                                  .z = blockZ,
                                                                  .density = density,
                                                                  .seaLevel = settings_->seaLevel};
                                const aquifer::SubstanceAt result = aquifer::computeSubstance(
                                    *aquiferCentres_, query, aquiferStatusCache, barrierAt,
                                    floodednessAt, spreadAt, lavaAt, pslAt, deepDarkAt,
                                    barrierReadsAlways_ ? aquifer::BarrierReads::Always
                                                        : aquifer::BarrierReads::WhereWeighed);
                                switch (result.substance) {
                                    case aquifer::Substance::Solid:
                                        block = &settings_->defaultBlock;
                                        break;
                                    case aquifer::Substance::Fluid:
                                        block = (result.fluidType == aquifer::FluidType::Lava)
                                                    ? &lava()
                                                    : &settings_->defaultFluid;
                                        if (result.fluidUpdate) {
                                            into.markFluidUpdate(localX, y, localZ);
                                        }
                                        break;
                                    case aquifer::Substance::Air:
                                        break;
                                }
                            }

                            // Ore veins, last and only over solid ground.
                            // Measured: on a probe whose density crosses zero
                            // INSIDE both vein ranges, the server placed zero
                            // vein blocks on 25509 positions it left as air,
                            // though the chain would have claimed 17813 of
                            // them — while every position it left solid,
                            // including the ones the aquifer's own barrier
                            // turned solid against a negative density, came
                            // back exact on 11455 of 11455.
                            if (readsVeins && block == &settings_->defaultBlock) {
                                // One router entry at a time, each behind the
                                // gate the last one opened: `vein_toggle`
                                // rejects the overwhelming majority of
                                // positions on its own, and every one of the
                                // three costs a full density evaluation.
                                // From the run where it read this block: a
                                // solid density's, made solid before the
                                // aquifer is asked.
                                const density::Point here{.x = blockX, .y = y, .z = blockZ};
                                const double toggle =
                                    toggles && density > 0.0
                                        ? toggleLanes.value(lane)
                                        : evaluateAtBlock(veinToggleKernel_, veinToggleNode, here,
                                                          cache);
                                if (richness.clears(toggle)) {
                                    const double ridged = evaluateAtBlock(
                                        veinRidgedKernel_, veinRidgedNode, here, cache);
                                    if (ridged < 0.0) {
                                        const ore::Vein vein = oreVeins_->at(
                                            blockX, y, blockZ,
                                            ore::VeinInputs{.toggle = toggle,
                                                            .ridged = ridged,
                                                            .gap = evaluateAtBlock(veinGapKernel_,
                                                                                   veinGapNode,
                                                                                   here, cache)});
                                        if (vein.placed()) {
                                            block = &veinBlock(vein);
                                        }
                                    }
                                }
                            }
                            // The palette is asked only when the state
                            // changes from the last block's: it answers one
                            // state with one index, and the first ask has
                            // interned it.
                            if (block != lastBlock) {
                                lastBlock = block;
                                lastIndex = stablePalette.indexOf(*block, intern);
                            }
                            into.blocks_[rowIndex + offset] = lastIndex;
                        }
                    }
                }
            }
        }
    }
    // This loop runs z-major and bottom-up, which is what keeps the corner
    // cache warm; the server's fill runs x-major and top-down, and its
    // `PostProcessing` lists hold the marks in that order.
    into.orderFluidUpdatesByCells(cellWidth);

    if (surfaceExecutor_.has_value()) {
        applySurfaceRules(chunkX, chunkZ, into);
    }
}

const data::ResourceLocation&
ChunkFiller::surfaceBiomeAt(const density::Point quartCorner,
                            density::Interpreter::CornerCache& cache) const {
    // Its own function, and checked here rather than trusted from compile():
    // compile() leaves no tree that reads the biome without a source, and a
    // read without one would have nothing honest to return.
    if (!biomeSource_.has_value()) {
        throw FillError(
            "this dimension's surface rules read the biome, and it has no biome source");
    }
    return biomeSource_->at(interpreter_, settings_->router, quartCorner, cache);
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

    // The pass reads and writes the buffer by palette index, at positions
    // its loops keep inside the chunk and the dimension (fill() checked the
    // buffer's geometry). A block's category is a pure function of its state
    // (categorize), so it is worked out once per palette entry, the entries
    // this pass adds included, rather than once per block. And the palette
    // holds each state once (ChunkBuffer::intern), so a position holds the
    // default block exactly when it holds the default block's index — found
    // once, after the first pass: an index this pass adds is a block it
    // placed over a default block, so if the first pass placed none this pass
    // places nothing and the default block never enters the palette.
    std::vector<Category> categoryOf;
    const auto categoryAt = [&](const int localX, const std::int32_t y, const int localZ) {
        const std::uint16_t index = into.blocks_[into.linearIndex(localX, y, localZ)];
        while (categoryOf.size() <= index) {
            categoryOf.push_back(categorize(into.palette_[categoryOf.size()], *settings_));
        }
        return categoryOf[index];
    };
    std::int32_t defaultIndex = -1;
    for (std::size_t i = 0; i < into.palette_.size(); ++i) {
        if (into.palette_[i] == settings_->defaultBlock) {
            defaultIndex = static_cast<std::int32_t>(i);
            break;
        }
    }
    StablePalette stablePalette;
    const auto intern = [&into](const settings::BlockState& block) { return into.intern(block); };

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
                    if (categoryAt(localX, y, localZ) != Category::Air) {
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

    // Scratch for one column's categories and the reads drawn from them
    // (`SurfaceColumn`: the two stone-depth runs, the water height, the
    // topmost non-air block), sized once and overwritten whole by every
    // column rather than reallocated 256 times a chunk. Each block is
    // categorised ONCE here, where the three loops this replaced each did it
    // again.
    std::vector<Category> categories(static_cast<std::size_t>(geometry.height));
    SurfaceColumn column;

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

    // `preliminary_surface_level` reaches `above_preliminary_surface` through
    // a 16-block lattice, not per column: sampled at the lattice corners,
    // each sample floored, blended linearly, floored again — measured, SPEC
    // §11. The lattice is anchored at the world origin through floorDiv and
    // its pitch is a chunk's width, so a chunk is exactly one cell and its
    // four samples are the chunk's own corner and its three +16 neighbours.
    // Four evaluations a chunk where the per-column read made 256 — and the
    // per-column read was the WRONG value: `ChunkFiller::preliminarySurfaceIn`
    // existed and was measured on 1871872 columns (vanilla_psl_lattice_test)
    // while this pass still read the raw entry at every column, so the
    // engine itself never reproduced what that test credited it with.
    static_assert(kPreliminarySurfacePitch == kChunkWidth,
                  "one lattice cell per chunk is what makes four samples enough");
    std::array<double, 4> pslCorners{};
    if (surfaceNeedsPreliminarySurface_) {
        const auto pslSample = [&](const std::int32_t dx, const std::int32_t dz) {
            return interpreter_.evaluate(
                settings_->router.at(settings::RouterEntry::PreliminarySurfaceLevel),
                density::Point{.x = baseX + dx, .y = 0, .z = baseZ + dz}, surfaceCache);
        };
        pslCorners = {pslSample(0, 0), pslSample(kPreliminarySurfacePitch, 0),
                      pslSample(0, kPreliminarySurfacePitch),
                      pslSample(kPreliminarySurfacePitch, kPreliminarySurfacePitch)};
        // preliminarySurfaceIn's precondition, refused by name where it does
        // not hold: a router's `preliminary_surface_level` is any function,
        // and a floor that is not an int32 has no defined conversion (SPEC §8).
        for (const double corner : pslCorners) {
            const double floored = std::floor(corner);
            if (std::isnan(floored) || floored < -2147483648.0 || floored > 2147483647.0) {
                throw FillError("chunk (" + std::to_string(chunkX) + ", " + std::to_string(chunkZ) +
                                ") reads preliminary_surface_level " + std::to_string(corner) +
                                " at a lattice corner, whose floor is not a 32-bit integer; this "
                                "build refuses it (SPEC §8)");
            }
        }
    }

    // Cached across the WHOLE chunk, not just down one column: a chunk is
    // only 4 quart-cells wide, so up to 16 of its 256 columns share the same
    // one — and without this, each repeated a from-scratch linear search
    // over the biome parameter table (7593 rows for the real overworld)
    // rather than reusing the first column's answer. Measured
    // (tools/analysis/generate-world.cpp): this took a real chunk's second
    // pass from about 8 seconds to about 0.5.
    //
    // Each entry keeps the biome as the source's own identifier (stable for
    // the source's life, which outlives this filler) and, when the rules read
    // `temperature`, that biome's declared temperature: looked up once, on
    // the block that first resolves the quart, which is the first block that
    // ever asked for it. A biome the table does not hold is refused there, as
    // it always was; every later lookup of one it holds returned the same
    // float, and is not repeated.
    struct QuartBiome {
        const data::ResourceLocation* id = nullptr;
        float temperature = 0.0F;
    };

    std::map<std::tuple<std::int32_t, std::int32_t, std::int32_t>, QuartBiome> biomeCache;

    // What the rules read that is constant down a column — the surface depth
    // above all, which vanilla's overworld reads on every default block it
    // walks — kept from the first block of each column that reads it
    // (surface::ColumnMemo: exact, refusals included). It forgets on its own
    // whenever a block of another column is asked about.
    surface::ColumnMemo columnMemo;

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
                // FLOORED at each sample and after the blend, not truncated:
                // the two are only separable on a router that hands this
                // entry a negative fraction, which vanilla's own
                // `find_top_surface` never does, so it is a data-pack-only
                // distinction — and a measured one (preliminarySurfaceIn).
                context.preliminarySurface = preliminarySurfaceIn(pslCorners, localX, localZ);
            }

            // The column's reads — both stone-depth runs, the water height
            // and the topmost non-air block — counted by `SurfaceColumn`
            // (surface_column.hpp carries the measured rules), the same code
            // the golden decoder reconstructs them with.
            for (std::int32_t y = minY; y < topY; ++y) {
                categories[static_cast<std::size_t>(y - minY)] = categoryAt(localX, y, localZ);
            }
            column.read(categories, minY);

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
            // ONLY THE DEFAULT BLOCK IS A SURFACE-RULE POSITION. A rule's
            // block replaces `default_block` and nothing else: not fluid, not
            // air, not lava the aquifer placed, not a vein block. Measured on
            // the eight golden overworld regions (every second chunk, 6619641
            // positions where the first pass holds water and 31491 where it
            // holds lava): vanilla changes NONE of the water — not the open
            // water reached straight from the sky, which an earlier reading
            // here left in reach of the rules, and not water below solid rock.
            // The same predicate is what the ore-vein placement probe measured
            // first, from the other side: an unconditional rule left all 12934
            // vein blocks untouched and repainted all 5548 default-block
            // positions (SPEC §11).
            //
            // What this replaced, and what it cost. The pass used to rewrite
            // every position of the column's first non-solid stretch plus
            // every Solid one, exempting only the six vein blocks — and
            // `categorize` then called the aquifer's lava Solid, because lava
            // is not the overworld's `default_fluid`. So the overworld's own
            // unconditioned `deepslate` gradient (true at y <= 0) turned every
            // aquifer lava block into deepslate, 127531 of the goldens' 127700
            // lava blocks, and painted the bottom of open water in the
            // gradient's band, 4215 blocks; the first pass had all of those
            // right. Nothing caught it, because every aquifer probe ran with
            // surface rules off and the one surface-on aquifer case held no
            // lava (golden_overworld_test.cpp now holds both).
            //
            // THE SCAN STILL STARTS AT THE COLUMN'S TOPMOST NON-AIR BLOCK. Air
            // is never the default block, so this is a bound on work rather
            // than on correctness now; the End's unconditioned end_stone rule
            // (golden_end_test.cpp, 2097152 of 2097152) is safe under either.
            // A column of nothing but air leaves `scanFrom` below `minY` and
            // the loop runs zero times.
            const std::int32_t scanFrom = column.top();
            for (std::int32_t y = scanFrom; y >= minY; --y) {
                if (into.blocks_[into.linearIndex(localX, y, localZ)] != defaultIndex) {
                    continue;
                }
                if (needsBiomeIdentity) {
                    // The context's biome and temperature change only where
                    // the quart does: assigned there, and left standing for
                    // the blocks below it in the same quart.
                    const std::int32_t quartY = quartSnap(y);
                    if (!biomeQuartYKnown || biomeQuartY != quartY) {
                        const auto key = std::tuple{qx, quartY, qz};
                        auto cached = biomeCache.find(key);
                        if (cached == biomeCache.end()) {
                            QuartBiome resolved;
                            resolved.id = &surfaceBiomeAt(
                                density::Point{.x = qx, .y = quartY, .z = qz}, surfaceCache);
                            if (surfaceNeedsTemperature_) {
                                resolved.temperature = biomeTemperatures_->at(*resolved.id);
                            }
                            cached = biomeCache.emplace(key, resolved).first;
                        }
                        if (surfaceNeedsBiome_) {
                            context.biome = *cached->second.id;
                        }
                        if (surfaceNeedsTemperature_) {
                            context.biomeTemperature = cached->second.temperature;
                        }
                        biomeQuartY = quartY;
                        biomeQuartYKnown = true;
                    }
                }

                context.y = y;
                context.stoneDepthAbove = column.stoneDepthAbove(y);
                context.stoneDepthBelow = column.stoneDepthBelow(y);
                context.waterHeight = column.waterHeight();

                if (const settings::BlockState* placed = executor.apply(context, columnMemo);
                    placed != nullptr) {
                    into.blocks_[into.linearIndex(localX, y, localZ)] =
                        stablePalette.indexOf(*placed, intern);
                }
            }
        }
    }
}

} // namespace stratum::terrain
