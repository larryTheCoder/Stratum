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
// Then ONE generator supplies all three draws, in this order —
// `positionalSourceFor(worldSeed, "minecraft:ore").at(x, y, z)` under the
// modern source, `legacyPositionalSourceFor(worldSeed, "minecraft:ore")
// .at(x, y, z)` (java.util.Random) under `legacy_random_source`:
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
// UNDER THE LEGACY SOURCE, measured (SPEC §11, "Ore veins under the legacy
// source"): the legacy positional primitive the aquifer's lattice and the
// gradient's coin use, salted "minecraft:ore", and the same three draws,
// each java.util.Random's nextFloat. Exact on every candidate of
// `tools/analysis/legacy-orevein-probe.sh`'s legacy arms — 1 703 936 a world
// at seeds 42, 31337, 42's 48-bit twin and 2, and held out at 8675309, 14
// and 15. Under java.util.Random a nextDouble takes two LCG steps where a
// nextFloat takes one, so the draw method of draws 1 and 2 shows in every
// draw after them: nextDouble for draw 1 reads 70%, for draw 2 99.2%. Draw
// 3's cannot show (its two precisions agree on the top 24 bits and nothing
// follows it).
//
// HOW A DRAW MEETS ITS THRESHOLD, measured on the one value where it shows.
// Every draw is a 24-bit nextFloat, and `f < c` read in float and read in
// double part only where (float) c rounds DOWN onto f itself:
//   * membership: f = 0.7F exactly (next(24) = 11744051) IS a member — the
//     comparison is the draw widened to double against 0.7. Five positions
//     in the probe windows, under both sources (legacy 31337, 14, 15;
//     Xoroshiro 2, twice), all but the first found from the RNG before their
//     worlds existed. Engine v13 compared in float and left all five as the
//     default block; the fix is pipeline engine v14.
//   * the ore roll: f = (float) c with c above it LOSES — the comparison is
//     in float. One position per source at seed 42, each with a toggle built
//     to put the chance a quarter of a float ulp above its draw.
//   * the raw roll: 0.02F is not on the 24-bit grid, so no draw can tell.
// Under Xoroshiro, seed 2's two membership positions also refute nextDouble
// for draw 1: its 53-bit value sits above 0.7 at both.
//
// THRESHOLDS BRACKETED, NOT ASSUMED. Over the combined 79790 candidates the
// largest draw on a touched block and the smallest on an untouched one pin
// the membership threshold into (0.6999681, 0.7000110]; 0.6973 reads 99.752%
// and 0.71 reads 99.022%, so 0.7 is measured rather than rounded to. The
// 0.7F bucket above narrows the bottom of it to 0.7F exclusive. The raw
// roll brackets to (0.0199925, 0.0200130] the same way.
//
// NO AQUIFER COUPLING — a measurement retracted. This header used to say
// veins place nothing unless `aquifers_enabled` is also true, from one probe
// world with veins on and aquifers off that came back 6291456 of 6291456
// stone. It does not replicate: the same recipe at the same seed (13579,
// vanilla's real vein entries, size_vertical 2) is block for block the
// aquifers-on world, 0 of 6291456 differing with 14373 vein blocks in it,
// and the legacy probe's `la` and `ma` arms (aquifers off) are their `lv`
// and `mv` exactly at four seeds, under both sources. Engine v14 places
// veins wherever `ore_veins_enabled` is set (SPEC §11).
//
// WHAT THIS DELIBERATELY DOES NOT DECIDE, recorded rather than guessed
// (SPEC §11), and two that were chosen first and measured since:
//   * draw 3's method, and draws 1 and 2's method under Xoroshiro beyond
//     the 0.7F bucket: one nextLong backs both precisions there, so they
//     part only where a 53-bit draw and its 24-bit truncation fall either
//     side of a threshold. The legacy source answers draws 1 and 2 for it.
//   * whether draw 2 is consumed when `vein_gap <= -0.3`. Nothing downstream
//     reads the stream at this position, so both variants also read 100.000%.
//     `drawVein` (ore_vein.cpp) takes the draw unconditionally, which is the
//     simpler reading; no observable depends on it.
//   * WHETHER A VEIN MAY REPLACE A FLUID — it may not, now measured.
//     `ChunkFiller` gates the vein call on `block == &settings_->defaultBlock`,
//     so a vein never replaces a block the aquifer placed as water or lava.
//     That was chosen first: the placement probe contained zero fluid
//     candidates (a single one would have broken its `solidAgree == solid`
//     assertion). The golden overworld regions settle it: wherever the first
//     pass places fluid inside the vein ranges and this chain would place a
//     vein block, the server kept the fluid, 21 of 21
//     (tests/conformance/golden_overworld_test.cpp).
//   * A NON-STONE default_block is replaced like stone, by the same blocks,
//     now measured: the legacy probe's netherrack and deepslate[axis=x] arms
//     hold every vein block of their stone control and their own
//     default_block everywhere else, 1 703 936 candidates a world at four
//     seeds.
#pragma once

#include <stratum/density/random_source.hpp>
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

/// The per-world RNG behind all three draws, from the random source the
/// dimension declares, built once per world rather than per block because
/// the salt and the seeding draws do not depend on position.
///
/// The source is REQUIRED, with no default, as it is for the aquifer's
/// `CentreSource` and the noise registry: a default is how a legacy
/// dimension once got the modern derivation without anyone asking for it.
class VeinSource {
public:
    /// Xoroshiro: `positionalSourceFor(worldSeed, "minecraft:ore")`.
    ///
    /// Legacy: `legacyPositionalSourceFor(worldSeed, "minecraft:ore")` —
    /// java.util.Random, two forks around String.hashCode, the position mix
    /// XORed into the stream seed: the primitive measured for the aquifer's
    /// lattice and the gradient's coin, and exact for veins on every block of
    /// `tools/analysis/legacy-orevein-probe.sh`'s legacy arms at seven
    /// seeds, four of them generated after the rule was frozen (SPEC §11,
    /// "Ore veins under the legacy source"). Only the low 48 bits of
    /// @p worldSeed reach it.
    explicit VeinSource(std::int64_t worldSeed, density::RandomSource source) noexcept;

    /// What the vein system puts at this position, given its three router
    /// values. Runs the deterministic gate itself, so a caller may hand it
    /// any position inside `inAnyVeinRange`; positions outside that are the
    /// caller's to skip, since evaluating the router there is the cost this
    /// is trying to avoid.
    [[nodiscard]] Vein at(std::int32_t x, std::int32_t y, std::int32_t z,
                          const VeinInputs& inputs) const;

    [[nodiscard]] constexpr density::RandomSource source() const noexcept { return source_; }

private:
    density::RandomSource source_;
    /// The Xoroshiro derivation's 128-bit base; zero under the legacy source.
    rng::Seed128 base_{};
    /// The legacy derivation's 64-bit stream seed; zero under Xoroshiro.
    std::int64_t legacySeed_ = 0;
};

} // namespace stratum::ore
