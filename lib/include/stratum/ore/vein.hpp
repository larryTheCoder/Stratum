// Stratum — ore veins: the copper and iron ribbons that replace stone deep
// underground.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// WHAT THIS IS. Three router entries — `vein_toggle`, `vein_ridged`,
// `vein_gap` — and one salted positional RNG decide, per block, whether a
// solid block becomes part of an ore vein and what it turns into. Unlike the
// aquifer this has no clean-room brief (`spec/` carries no ore-vein spec), so
// every number here was confirmed against the vanilla server rather than read
// from a document: see `tools/analysis/ore-vein-probe.sh` for the probe and
// SPEC §11 for the counts.
//
// THE DERIVATION, confirmed per block on 79790 of 79790 candidates across 11
// contributing seeds and both vein types — 36725 from the discovery probes
// (`probes/orevein-multi`) and 43065 from nine worlds generated afterwards
// and never used to fit anything (`probes/orevein-heldout`). 100.000% on
// every seed alone and on copper and iron alone.
//
// A position must first clear a purely DETERMINISTIC gate (no RNG at all):
//
//   * `y` in [0, 50] with `vein_toggle > 0`   -> a candidate COPPER vein;
//     `y` in [-60, -8] with `vein_toggle <= 0` -> a candidate IRON vein.
//     The dead zone [-8, 0) produces neither, whatever the toggle says.
//   * `|vein_toggle|` must clear a richness threshold that is 0.6 AT either
//     end of its range and falls linearly to 0.4 at 20 blocks inside it.
//   * `vein_ridged < 0`.
//
// Then ONE generator — `positionalSourceFor(worldSeed, "minecraft:ore")
// .at(x, y, z)` — supplies all three draws, in this order:
//
//   1. `nextFloat() < 0.7` or the block is left alone. Note the SENSE: 0.7
//      is the chance of being touched, not of being skipped. The earlier
//      search assumed a "30% membership roll" and swept thresholds at 0.3
//      only, which is why ~1226 salt candidates were refuted without the
//      right salt ever being scored properly: at the CORRECT salt a 0.3
//      threshold still reads only 60.3%, an unremarkable near-miss, while
//      0.7 reads 100%. The measured marginal touch rate — 69.729% on the
//      discovery set — was in the record the whole time.
//   2. `nextFloat() < clampedMap(|vein_toggle|, 0.4, 0.6 -> 0.1, 0.3)` AND
//      `vein_gap > -0.3` gives ore; anything else gives filler.
//   3. only after an ore: `nextFloat() < 0.02` upgrades it to the raw-metal
//      block.
//
// Because every position gets its OWN `.at(x, y, z)` generator, there is no
// stream ordering between blocks: a position's three draws cannot be
// perturbed by what any other position did. That is what makes this safe to
// evaluate lazily, block by block, in whatever order the filler visits.
//
// THRESHOLDS BRACKETED, NOT ASSUMED. Over the combined 79790 candidates the
// largest draw on a touched block and the smallest on an untouched one pin
// the membership threshold into (0.6999681, 0.7000110]; 0.6973 reads 99.752%
// and 0.71 reads 99.022%, so 0.7 is measured rather than rounded to. The raw
// roll brackets to (0.0199925, 0.0200130] the same way.
//
// THE AQUIFER COUPLING, measured. Ore veins only place anything when
// `aquifers_enabled` is ALSO true: a probe with veins on and aquifers off
// returned 6291456 of 6291456 blocks as plain stone, with the vein inputs
// themselves clearing their gates on a measurable fraction of positions.
// `veinsPlaceBlocks` is that observation, not a guess about why.
//
// WHAT THIS DELIBERATELY DOES NOT DECIDE. Two things the probe cannot see,
// recorded rather than guessed (SPEC §11):
//   * `nextFloat()` vs `nextDouble()`. One `nextLong()` backs both and no row
//     of the 79790 lands between the two precisions, so both read 100.000%.
//     Separating them needs a probe that takes a FOURTH draw.
//   * whether draw 2 is consumed when `vein_gap <= -0.3`. Nothing downstream
//     reads the stream at this position, so both variants also read 100.000%.
//     `decide` takes the draw unconditionally, which is the simpler reading;
//     no observable depends on it.
#pragma once

#include <stratum/rng/xoroshiro128.hpp>

#include <cstdint>

namespace stratum::ore {

/// Which metal a candidate position belongs to. Fixed by `y` and the sign of
/// `vein_toggle` together, never by the RNG.
enum class VeinType : std::uint8_t { Copper, Iron };

/// What the vein system puts at a position. `None` means "leave whatever was
/// already there" — the overwhelmingly common answer, including for every
/// position outside the two y-ranges.
enum class VeinBlock : std::uint8_t { None, Filler, Ore, RawBlock };

/// One position's answer. `type` is meaningless when `block` is `None`.
struct Vein {
    VeinBlock block = VeinBlock::None;
    VeinType type = VeinType::Copper;

    [[nodiscard]] constexpr bool placed() const noexcept { return block != VeinBlock::None; }
};

/// The three router values a decision reads, evaluated by the caller because
/// only the caller owns the interpreter and its corner cache.
struct VeinInputs {
    double toggle = 0.0;
    double ridged = 0.0;
    double gap = 0.0;
};

/// The lowest and highest `y` any vein can occupy, over both metals. Checked
/// before anything else so a caller can skip the router reads outright for
/// the ~70% of a 384-block column that can never hold a vein.
inline constexpr std::int32_t kLowestVeinY = -60;
inline constexpr std::int32_t kHighestVeinY = 50;

/// Whether `y` is inside either metal's range at all — the cheapest possible
/// rejection, and true for 111 of vanilla's 384 block heights.
[[nodiscard]] constexpr bool inAnyVeinRange(const std::int32_t y) noexcept {
    return y >= kLowestVeinY && y <= kHighestVeinY;
}

/// Whether `y` and `toggle` together clear the type/sign correspondence and
/// the richness threshold — everything deterministic except `vein_ridged`,
/// which is a separate router read and so worth deferring past this.
///
/// Confirmed with zero exceptions on every real vein block measured; a caller
/// that skips this and rolls anyway would place veins in the dead zone
/// between the ranges and at richness values vanilla never accepts.
[[nodiscard]] bool clearsRichness(std::int32_t y, double toggle) noexcept;

/// Whether this dimension's flags let the vein system place anything.
///
/// Both flags, not just `ore_veins_enabled`: the coupling in this file's
/// header is measured, and a dimension with veins on and aquifers off really
/// does come back as unbroken stone. Returning false here reproduces that
/// exactly rather than approximating it.
[[nodiscard]] constexpr bool veinsPlaceBlocks(const bool oreVeinsEnabled,
                                              const bool aquifersEnabled) noexcept {
    return oreVeinsEnabled && aquifersEnabled;
}

/// The per-world RNG behind all three draws: `positionalSourceFor(worldSeed,
/// "minecraft:ore")`, built once per world rather than per block because the
/// MD5 of the salt and the two seeding draws do not depend on position.
class VeinSource {
public:
    explicit VeinSource(std::int64_t worldSeed);

    /// What the vein system puts at this position, given its three router
    /// values. Runs the deterministic gate itself, so a caller may hand it
    /// any position inside `inAnyVeinRange`; positions outside that are the
    /// caller's to skip, since evaluating the router there is the cost this
    /// is trying to avoid.
    [[nodiscard]] Vein at(std::int32_t x, std::int32_t y, std::int32_t z,
                          const VeinInputs& inputs) const;

private:
    rng::PositionalSource source_;
};

} // namespace stratum::ore
