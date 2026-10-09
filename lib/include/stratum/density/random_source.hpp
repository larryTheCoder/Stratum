// Stratum — which generator a dimension declares.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Its own header because two unrelated consumers need it and neither should
// pull in the other: the noise registry (noise_registry.hpp), which seeds a
// dimension's named noises, and the aquifer's cell centres
// (aquifer/lattice.hpp), which draw a positional random from the same
// declared source and name no noise at all.
#pragma once

#include <cstdint>

namespace stratum::density {

/// Which generator seeds a dimension's noises. A noise settings entry
/// chooses one for all of them at once, through `legacy_random_source`, so
/// the same `minecraft:temperature` is a different noise in the overworld
/// than it is in the Nether — and a registry can only hold one of the two.
///
/// Named rather than a bool, and required rather than defaulted, because a
/// default is exactly how this went wrong: the registry used to be built
/// per pack and always with Xoroshiro, which was right for three of
/// vanilla's seven dimensions and silently wrong for the other four.
enum class RandomSource : std::uint8_t {
    /// Xoroshiro128++ through the positional factory, salted with the MD5 of
    /// each noise's identifier. What a dimension declaring
    /// `legacy_random_source: false` uses, which is the overworld and its
    /// two variants.
    Xoroshiro,
    /// The Java LCG. What the Nether, the End, caves and floating islands
    /// use. The nameless `old_blended_noise` is derived — the world seed
    /// handed straight to the LCG — and so are the three CLIMATE noises,
    /// `minecraft:temperature`, `vegetation` and `offset`, by the rule
    /// cubiomes documents for the Nether and the goldens confirm (SPEC §11).
    /// Every other NAMED noise is not: nothing available says how a name
    /// becomes an LCG seed, and those are refused. The aquifer's cell centres
    /// are measured under it (rng::LegacyPositionalSource, SPEC §11).
    Legacy,
};

} // namespace stratum::density
