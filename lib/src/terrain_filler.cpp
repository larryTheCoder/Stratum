// Stratum — turning a density field into blocks.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum/aquifer/substance.hpp>
#include <stratum/javamath.hpp>
#include <stratum/ore/vein.hpp>
#include <stratum/terrain/filler.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
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

/// Solid, fluid or air — what the FIRST pass decided, read back for the
/// second. Not stored anywhere: rederived from the block a position already
/// holds. Fluid is `default_fluid` AND the aquifer's literal lava, the only
/// two fluids the first pass writes; the ore veins' blocks are Solid. The
/// surface pass's WRITE does not rely on this — it writes over
/// `default_block` only (`applySurfaceRules`) — but its two stone-depth runs
/// and its water height do, and the server treats lava there exactly as
/// water: measured on `tools/analysis/aquifer-lavarun-probe.sh` (SPEC §11),
/// where this called lava Solid and so counted it toward both runs.
enum class Category : std::uint8_t { Air, Fluid, Solid };

[[nodiscard]] Category categorize(const settings::BlockState& block,
                                  const settings::NoiseSettings& settings) {
    if (block == air()) {
        return Category::Air;
    }
    if (block == settings.defaultFluid || block == lava()) {
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

std::uint16_t ChunkBuffer::paletteIndexAt(int localX, std::int32_t y, int localZ) const {
    return blocks_[indexOf(localX, y, localZ)];
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

void ChunkBuffer::markFluidUpdate(int localX, std::int32_t y, int localZ) {
    static_cast<void>(indexOf(localX, y, localZ)); // the bounds check, nothing more
    fluidUpdates_.push_back(FluidUpdate{.localX = static_cast<std::uint8_t>(localX),
                                        .y = y,
                                        .localZ = static_cast<std::uint8_t>(localZ)});
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

    // Ore veins, wired (SPEC §10, M3; SPEC §11). Both flags, not just
    // `ore_veins_enabled`: the coupling is measured, and a dimension with
    // veins on and aquifers off really does come back as unbroken stone, so
    // leaving the source unbuilt here reproduces the server exactly rather
    // than approximating it.
    // THE LEGACY REFUSAL, BY NAME OF THE CONSTRUCT (SPEC §8, §11). The vein
    // source is a positional random derived from the world seed through the
    // DIMENSION'S declared random source, not from any named noise, so it
    // never reaches NoiseRegistry::create's `wanted` list. Under a legacy
    // source this build has only the modern derivation, and it is UNTESTED
    // there: every vanilla legacy dimension has `ore_veins_enabled` false, so
    // no oracle for it exists on disk. It is the same primitive whose gradient
    // use is measured WRONG under a legacy source, so running it would be the
    // plausible-but-wrong world SPEC §8 forbids.
    //
    // On the FLAG, deliberately, and outside the veinsPlaceBlocks gate below.
    // With aquifers off the vein source is never built and no random is ever
    // drawn — a legacy pack with veins alone fills a chunk bit-identical to
    // the undoctored one (measured: FNV-1a 2b9f4b6af4ed174a both ways). But
    // `validatePack` warns on the flag alone, and SPEC §11 says each of the
    // three constructs is refused by name; a refusal that fires on one path
    // and not the other is the disagreement this build exists to avoid.
    if (settings.oreVeinsEnabled && noises.source() == density::RandomSource::Legacy) {
        throw FillError(density::legacyConstructRefusal(
            "ore_veins_enabled",
            "the vein source is a positional random drawn from that source, and no vanilla "
            "legacy dimension enables veins, so nothing on disk can say what it should be"));
    }
    if (ore::veinsPlaceBlocks(settings.oreVeinsEnabled, settings.aquifersEnabled)) {
        filler.oreVeins_.emplace(noises.worldSeed());
        for (const settings::RouterEntry entry :
             {settings::RouterEntry::VeinToggle, settings::RouterEntry::VeinRidged,
              settings::RouterEntry::VeinGap}) {
            filler.interpreter_.requireEvaluable(settings.router.at(entry));
        }
    }

    if (settings.aquifersEnabled) {
        // Same refusal, same reason (SPEC §8, §11): the aquifer lattice's
        // centre jitter is a positional random from the dimension's declared
        // random source and names no noise. UNTESTED under a legacy source
        // for the same reason as the veins — every vanilla legacy dimension
        // has `aquifers_enabled` false.
        if (noises.source() == density::RandomSource::Legacy) {
            throw FillError(density::legacyConstructRefusal(
                "aquifers_enabled",
                "the lattice's centre jitter is a positional random drawn from that source, and "
                "no vanilla legacy dimension enables aquifers, so nothing on disk can say what "
                "it should be"));
        }
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
        // — built once here from the registry's own seed rather than
        // re-derived on every call to fill().
        filler.aquiferCentres_.emplace(noises.worldSeed());
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
        if (noises.source() == density::RandomSource::Legacy) {
            // THE LEGACY REFUSAL, BY NAME OF THE CONSTRUCT (SPEC §8, §11) —
            // and it THROWS, exactly as the aquifer and ore-vein refusals
            // above do. A first version of this recorded the refusal as an
            // entry in blockedBy_ instead, which is the shape this function
            // uses for "a caller did not supply something". That was the
            // wrong shape for "this dimension is underivable": nothing on the
            // public path (`world::CompiledDimension::compile`, which every
            // native binding generates through) consults blockedBy_, so a
            // doctored legacy pack with a gradient compiled clean and filled
            // 13549 blocks with its surface rules silently dropped — the
            // best-effort partial load SPEC §8 forbids. Measured, not
            // supposed; the conformance case below drives that pack through
            // CompiledDimension and asserts the throw.
            //
            // MEASURED WRONG, not merely unverified: this build's Xoroshiro
            // vertical_gradient against the golden NETHER's bedrock agrees
            // at chance, while the identical code on the modern overworld is
            // exact — see tests/conformance/
            // vanilla_legacy_gradient_gap_test.cpp.
            const std::vector<std::string> names = surface::verticalGradientNames(*surfaceRules);
            if (!names.empty()) {
                std::string list;
                for (const std::string& name : names) {
                    list += (list.empty() ? "'" : ", '") + name + "'";
                }
                throw FillError(density::legacyConstructRefusal(
                    "minecraft:vertical_gradient",
                    "its random_name(s) " + list +
                        " salt this dimension's own random source, which declares "
                        "legacy_random_source; this build can only derive the modern one, and "
                        "for this construct that derivation is measured to agree with vanilla "
                        "at chance"));
            }
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
    // blocks' worth of them — see aquifer::StatusCache's own doc.
    aquifer::StatusCache aquiferStatusCache;

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
    const auto pslAt = [&](std::int32_t x, std::int32_t y, std::int32_t z) {
        return aquiferRead(pslNode, x, y, z);
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
                            } else if (aquiferCentres_.has_value() &&
                                       !aquifer::consultsLattice(y, ySkipLevel)) {
                                // Q2.3: the global picker, as with aquifers
                                // off (below) — lava under min(-54,
                                // sea_level), default_fluid under sea_level,
                                // air above.
                                if (y < aquifer::lambdaLevel(settings_->seaLevel)) {
                                    block = &lava();
                                } else if (y < settings_->seaLevel) {
                                    block = &settings_->defaultFluid;
                                }
                            } else if (aquiferCentres_.has_value()) {
                                const aquifer::AquiferQuery query{.x = blockX,
                                                                  .y = y,
                                                                  .z = blockZ,
                                                                  .density = density,
                                                                  .seaLevel = settings_->seaLevel};
                                const aquifer::SubstanceAt result = aquifer::computeSubstance(
                                    *aquiferCentres_, query, aquiferStatusCache, barrierAt,
                                    floodednessAt, spreadAt, lavaAt, pslAt, deepDarkAt);
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
                            } else if (y < aquifer::lambdaLevel(settings_->seaLevel)) {
                                // Aquifers off still has a global lava sea:
                                // non-solid ground below min(-54, sea_level)
                                // is lava, not default_fluid (spec Q1.2,
                                // Q2.1). Measured on the aquifer-free probe
                                // (tools/analysis/aquifer-free-probe.sh):
                                // 1005 positions at y -58..-55 the server
                                // filled with lava, all of which this branch
                                // used to write as water — invisible to every
                                // category-only comparison, where both are
                                // "fluid".
                                block = &lava();
                            } else if (y < settings_->seaLevel) {
                                block = &settings_->defaultFluid;
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
                            if (oreVeins_.has_value() && block == &settings_->defaultBlock &&
                                ore::inAnyVeinRange(y)) {
                                // One router entry at a time, each behind the
                                // gate the last one opened: `vein_toggle`
                                // rejects the overwhelming majority of
                                // positions on its own, and every one of the
                                // three costs a full density evaluation.
                                const density::Point here{.x = blockX, .y = y, .z = blockZ};
                                const double toggle =
                                    interpreter_.evaluate(veinToggleNode, here, cache);
                                if (ore::clearsRichness(y, toggle)) {
                                    const double ridged =
                                        interpreter_.evaluate(veinRidgedNode, here, cache);
                                    if (ridged < 0.0) {
                                        const ore::Vein vein = oreVeins_->at(
                                            blockX, y, blockZ,
                                            ore::VeinInputs{.toggle = toggle,
                                                            .ridged = ridged,
                                                            .gap = interpreter_.evaluate(
                                                                veinGapNode, here, cache)});
                                        if (vein.placed()) {
                                            block = &veinBlock(vein);
                                        }
                                    }
                                }
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
    }

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
                // FLOORED at each sample and after the blend, not truncated:
                // the two are only separable on a router that hands this
                // entry a negative fraction, which vanilla's own
                // `find_top_surface` never does, so it is a data-pack-only
                // distinction — and a measured one (preliminarySurfaceIn).
                context.preliminarySurface = preliminarySurfaceIn(pslCorners, localX, localZ);
            }

            // Top-down: the stone-depth run counting from the world's top,
            // and the water height latched at the first fluid block met
            // descending. Air resets the run; fluid — water or lava alike —
            // neither breaks it nor counts toward it, and lava latches the
            // height as water does (surface::Context's own doc, measured).
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
            // The run counted from the world's floor, for `surface_type:
            // ceiling` — and NOT the mirror image of the one above: bottom
            // up, fluid RESETS the run exactly as air does, water as much as
            // lava. Measured (aquifer-lavarun-probe.sh, SPEC §11): stone
            // over an enclosed pool reads depth 0 from the pool's roof up,
            // on every column of every pool, where a run that skipped the
            // fluid — this loop's reading until then, for water too — left
            // it stone.
            {
                std::int32_t run = 0;
                for (std::int32_t y = minY; y < topY; ++y) {
                    if (categorize(into.at(localX, y, localZ), *settings_) == Category::Solid) {
                        ++run;
                    } else {
                        run = 0;
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
            std::int32_t scanFrom = minY - 1;
            for (std::int32_t y = topY - 1; y >= minY; --y) {
                if (categorize(into.at(localX, y, localZ), *settings_) != Category::Air) {
                    scanFrom = y;
                    break;
                }
            }
            for (std::int32_t y = scanFrom; y >= minY; --y) {
                if (!(into.at(localX, y, localZ) == settings_->defaultBlock)) {
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
