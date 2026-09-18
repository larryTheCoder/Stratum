// Stratum — ore veins: the three draws and the gate in front of them.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#include <stratum/ore/vein.hpp>

#include <algorithm>
#include <cmath>

namespace stratum::ore {

namespace {

/// Vanilla's `clampedMap`: `v` from [`inLo`, `inHi`] onto [`outLo`, `outHi`],
/// clamped at both ends. Written out rather than reached for elsewhere
/// because the ore roll is the only caller here and the clamp direction is
/// part of what was measured.
[[nodiscard]] double clampedMap(const double v, const double inLo, const double inHi,
                                const double outLo, const double outHi) noexcept {
    const double t = std::clamp((v - inLo) / (inHi - inLo), 0.0, 1.0);
    return outLo + (t * (outHi - outLo));
}

/// The two metals' y-ranges, inclusive at both ends. The gap between -8 and 0
/// is real: no vein of either kind is ever placed there, which is measured
/// rather than inferred from the ranges happening not to overlap.
constexpr std::int32_t kCopperLowY = 0;
constexpr std::int32_t kCopperHighY = 50;
constexpr std::int32_t kIronLowY = -60;
constexpr std::int32_t kIronHighY = -8;

/// The richness threshold: 0.6 at either end of a metal's range, falling
/// linearly to 0.4 once 20 blocks inside it.
constexpr double kRichnessAtLimit = 0.6;
constexpr double kRichnessSpan = 0.2;
constexpr std::int32_t kRichnessRampBlocks = 20;

/// The three thresholds, each bracketed against the server rather than
/// rounded to (see vein.hpp).
constexpr float kMembershipChance = 0.7F;
constexpr float kRawChance = 0.02F;
constexpr double kGapFloor = -0.3;
constexpr double kOreChanceInLo = 0.4;
constexpr double kOreChanceInHi = 0.6;
constexpr double kOreChanceOutLo = 0.1;
constexpr double kOreChanceOutHi = 0.3;

} // namespace

bool clearsRichness(const std::int32_t y, const double toggle) noexcept {
    // The sign of `vein_toggle` picks the metal, and the metal picks the
    // range — so a position inside copper's range with a negative toggle is
    // not an iron candidate, it is no candidate at all.
    const bool isCopper = y >= kCopperLowY && y <= kCopperHighY && toggle > 0.0;
    const bool isIron = y >= kIronLowY && y <= kIronHighY && toggle <= 0.0;
    if (!isCopper && !isIron) {
        return false;
    }
    const std::int32_t lower = isCopper ? kCopperLowY : kIronLowY;
    const std::int32_t upper = isCopper ? kCopperHighY : kIronHighY;
    const std::int32_t distFromLimit = std::min(y - lower, upper - y);
    const double required =
        kRichnessAtLimit - (kRichnessSpan * std::min(distFromLimit, kRichnessRampBlocks) /
                            static_cast<double>(kRichnessRampBlocks));
    return std::abs(toggle) >= required;
}

VeinSource::VeinSource(const std::int64_t worldSeed)
    : source_(rng::positionalSourceFor(worldSeed, "minecraft:ore")) {}

Vein VeinSource::at(const std::int32_t x, const std::int32_t y, const std::int32_t z,
                    const VeinInputs& inputs) const {
    if (!clearsRichness(y, inputs.toggle)) {
        return {};
    }
    // Necessary for any vein block at all, with zero exceptions measured —
    // and read AFTER the richness gate because it is a second router
    // evaluation the caller has already paid for only if we get this far.
    if (inputs.ridged >= 0.0) {
        return {};
    }
    const VeinType type = y >= kCopperLowY ? VeinType::Copper : VeinType::Iron;

    // One generator, three draws, in this order. Sequenced through named
    // locals rather than nested in one expression: which draw comes first is
    // part of the answer, and C++ would not otherwise promise it.
    auto generator = source_.at(x, y, z);

    if (generator.nextFloat() >= kMembershipChance) {
        return {};
    }

    const float oreRoll = generator.nextFloat();
    const double oreChance = clampedMap(std::abs(inputs.toggle), kOreChanceInLo, kOreChanceInHi,
                                        kOreChanceOutLo, kOreChanceOutHi);
    // The draw above is taken whatever the gap says. Unobservable either way
    // — nothing downstream reads this position's stream — and vein.hpp's
    // header records that rather than letting the choice pass silently.
    if (oreRoll >= static_cast<float>(oreChance) || inputs.gap <= kGapFloor) {
        return {.block = VeinBlock::Filler, .type = type};
    }
    if (generator.nextFloat() < kRawChance) {
        return {.block = VeinBlock::RawBlock, .type = type};
    }
    return {.block = VeinBlock::Ore, .type = type};
}

} // namespace stratum::ore
