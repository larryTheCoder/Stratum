// Stratum — the aquifer's random source, against deepslate's.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The cell centres come from a per-cell generator: the world seed forked into
// a 128-bit base, salted with the MD5 of `minecraft:aquifer`, forked again,
// and XORed with a mix of the cell's index (SPEC §11, "The jitter draw,
// recovered"). The known answers in aquifer_lattice_test.cpp are this build's
// own output, so they hold that derivation still but cannot say it is right;
// what ties it to vanilla is vanilla_aquifer_jitter_test.cpp, which needs the
// server's probe corpora and so never runs in CI.
//
// These vectors come from an implementation that is not this one: deepslate,
// run as a black box (tools/vectors/deepslate_aquifer_vectors.mjs). They cover
// only the part of the derivation where deepslate was found to agree with the
// server — the base (salt, both forks, the MD5 halves' order) and the mix's y
// term — because its PositionalRandom.at evaluates the mix without wrapping,
// which the server refutes, and its aquifer does not reproduce the server at
// all. Every cell therefore sits on the y axis, where nothing wraps.
//
// What this does NOT pin, and says so below rather than leaving it to be
// discovered: the mix's x and z terms and its wraps, arithmetic against
// logical shift, and the jitter's bounds (10, 9, 10) and draw order. The
// vectors' bounded draws were taken with this project's own bounds, so they
// hold `jitterOf`'s wiring to that reading, not the reading to vanilla. None
// of those has a CI oracle; the server-backed conformance cases pin them.
#include "deepslate_aquifer_vectors.inc"

#include <stratum/aquifer/lattice.hpp>
#include <stratum/javamath.hpp>
#include <stratum/rng/xoroshiro128.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <string_view>
#include <utility>

namespace {

using stratum::rng::Seed128;
using stratum::rng::Xoroshiro128PlusPlus;

/// The derivation with every knob an ablation turns. Default-constructed it is
/// the shipped one; the liveness case requires that it then reproduces the
/// library's own result, so the ablations perturb the real derivation and not
/// a lookalike.
struct Derivation {
    std::string_view salt = "minecraft:aquifer";
    bool firstFork = true;
    bool secondFork = true;
    bool thirdFork = false;
    bool swapHalves = false;
    int shift = 16;
    bool logicalShift = false;
};

[[nodiscard]] Seed128 forked(const Seed128 state) noexcept {
    Xoroshiro128PlusPlus source{state};
    // Sequenced: both draws come from one generator, so the order is part of
    // the answer.
    const auto lo = static_cast<std::uint64_t>(source.nextLong());
    const auto hi = static_cast<std::uint64_t>(source.nextLong());
    return Seed128{.lo = lo, .hi = hi};
}

[[nodiscard]] Seed128 baseOf(const std::int64_t seed, const Derivation& d) noexcept {
    const Seed128 root = d.firstFork ? stratum::rng::XoroshiroPositionalFactory{seed}.base()
                                     : stratum::rng::upgradeSeedTo128Bit(seed);
    Seed128 salt = stratum::rng::seedFromHashOf(d.salt);
    if (d.swapHalves) {
        std::swap(salt.lo, salt.hi);
    }
    Seed128 base{.lo = root.lo ^ salt.lo, .hi = root.hi ^ salt.hi};
    if (d.secondFork) {
        base = forked(base);
    }
    if (d.thirdFork) {
        base = forked(base);
    }
    return base;
}

/// The position mix with its shift exposed. Mirrors `rng::positionSeed`: the
/// x term is a 32-bit product, sign-extended, and every step wraps.
[[nodiscard]] std::uint64_t mixOf(const std::int32_t x, const std::int32_t y, const std::int32_t z,
                                  const Derivation& d) noexcept {
    const auto xTerm = static_cast<std::uint64_t>(static_cast<std::int64_t>(
        static_cast<std::int32_t>(static_cast<std::uint32_t>(x) * UINT32_C(3129871))));
    std::uint64_t value =
        xTerm ^ (static_cast<std::uint64_t>(static_cast<std::int64_t>(z)) * UINT64_C(116129781)) ^
        static_cast<std::uint64_t>(static_cast<std::int64_t>(y));
    value = (value * value * UINT64_C(42317861)) + (value * UINT64_C(11));
    const auto mixed = static_cast<std::int64_t>(value);
    return static_cast<std::uint64_t>(d.logicalShift ? stratum::javamath::ushr(mixed, d.shift)
                                                     : stratum::javamath::shr(mixed, d.shift));
}

[[nodiscard]] std::array<std::uint64_t, 3> longsOf(const AquiferRandomVector& v,
                                                   const Derivation& d) noexcept {
    const Seed128 base = baseOf(v.seed, d);
    Xoroshiro128PlusPlus source{Seed128{.lo = base.lo ^ mixOf(v.cx, v.cy, v.cz, d), .hi = base.hi}};
    std::array<std::uint64_t, 3> longs{};
    for (auto& value : longs) {
        value = static_cast<std::uint64_t>(source.nextLong());
    }
    return longs;
}

/// How many vectors a derivation gets wrong: base or longs.
[[nodiscard]] int mismatchesOf(const Derivation& d) noexcept {
    int wrong = 0;
    for (const auto& v : kDeepslateAquiferRandomVectors) {
        const Seed128 base = baseOf(v.seed, d);
        const bool baseRight = base.lo == v.baseLo && base.hi == v.baseHi;
        wrong += static_cast<int>(!baseRight || longsOf(v, d) != v.longs);
    }
    return wrong;
}

[[nodiscard]] int vectorCount() noexcept {
    return static_cast<int>(kDeepslateAquiferRandomVectors.size());
}

} // namespace

TEST_CASE("the aquifer's random source matches deepslate's on every vector",
          "[aquifer][rng][oracle]") {
    int checked = 0;
    for (const auto& v : kDeepslateAquiferRandomVectors) {
        INFO("seed " << v.seed << ", cell " << v.cx << " " << v.cy << " " << v.cz);

        // The library's own path: the named derivation, then one generator per
        // cell from it.
        const auto source = stratum::rng::positionalSourceFor(v.seed, "minecraft:aquifer");
        CHECK(source.base() == Seed128{.lo = v.baseLo, .hi = v.baseHi});
        auto generator = source.at(v.cx, v.cy, v.cz);
        std::array<std::uint64_t, 3> longs{};
        for (auto& value : longs) {
            value = static_cast<std::uint64_t>(generator.nextLong());
        }
        CHECK(longs == v.longs);

        // And the aquifer's wiring of it: the same base, the three draws in
        // the order and bounds this project measured, and the centre built
        // from them.
        const stratum::aquifer::CentreSource centres{v.seed};
        CHECK(centres.base() == source.base());
        CHECK(centres.jitterOf(v.cx, v.cy, v.cz) ==
              stratum::aquifer::Jitter{v.draws[0], v.draws[1], v.draws[2]});
        CHECK(centres.centreOf(v.cx, v.cy, v.cz) ==
              stratum::aquifer::CellIndex{(16 * v.cx) + v.draws[0], (12 * v.cy) + v.draws[1],
                                          (16 * v.cz) + v.draws[2]});
        ++checked;
    }
    CHECK(checked == 72);
}

TEST_CASE("the vectors reach the parts of the derivation they claim to, and no others",
          "[aquifer][rng][oracle]") {
    std::set<std::int64_t> seeds;
    std::set<std::int32_t> cells;
    bool negativeY = false;
    for (const auto& v : kDeepslateAquiferRandomVectors) {
        seeds.insert(v.seed);
        cells.insert(v.cy);
        negativeY = negativeY || v.cy < 0;
        // Off the y axis deepslate's mix is not the server's (the driver's
        // header), so no vector may sit there.
        CHECK(v.cx == 0);
        CHECK(v.cz == 0);
        CHECK(v.draws[0] >= 0);
        CHECK(v.draws[0] < stratum::aquifer::kJitterBoundX);
        CHECK(v.draws[1] >= 0);
        CHECK(v.draws[1] < stratum::aquifer::kJitterBoundY);
        CHECK(v.draws[2] >= 0);
        CHECK(v.draws[2] < stratum::aquifer::kJitterBoundZ);
    }
    CHECK(seeds.size() == 9U);
    CHECK(seeds.contains(0));
    CHECK(seeds.contains(-1));
    CHECK(seeds.contains(std::numeric_limits<std::int64_t>::max()));
    CHECK(seeds.contains(std::numeric_limits<std::int64_t>::min()));
    CHECK(cells.size() * seeds.size() == kDeepslateAquiferRandomVectors.size());
    CHECK(cells.contains(0));
    CHECK(negativeY);

    // No two cells of one seed share a generator: y and -y usually mix to one
    // value, and a set that held both would be one vector written twice.
    for (const std::int64_t seed : seeds) {
        std::set<std::array<std::uint64_t, 3>> distinct;
        for (const auto& v : kDeepslateAquiferRandomVectors) {
            if (v.seed == seed) {
                distinct.insert(v.longs);
            }
        }
        CHECK(distinct.size() == cells.size());
    }
}

TEST_CASE("every wrong derivation the search refuted misses the vectors, where they can see it",
          "[aquifer][rng][oracle]") {
    // Liveness: the knob model, at the shipped setting, is the library's own
    // derivation — and the ablations SPEC §11 lists as refuted on the
    // server's blocks are refuted here too, by an independent implementation
    // and with no fixture. A base or salt ablation changes the whole base, so
    // every vector misses.
    REQUIRE(mismatchesOf(Derivation{}) == 0);

    CHECK(mismatchesOf(Derivation{.salt = "minecraft:ore"}) == vectorCount());
    CHECK(mismatchesOf(Derivation{.salt = "aquifer"}) == vectorCount());
    CHECK(mismatchesOf(Derivation{.salt = "minecraft:aquifer_barrier"}) == vectorCount());
    CHECK(mismatchesOf(Derivation{.firstFork = false}) == vectorCount());
    CHECK(mismatchesOf(Derivation{.secondFork = false}) == vectorCount());
    CHECK(mismatchesOf(Derivation{.thirdFork = true}) == vectorCount());
    CHECK(mismatchesOf(Derivation{.swapHalves = true}) == vectorCount());

    // A shift of 15 or 17 cannot reach the origin cell, whose mix is 0 under
    // any shift; every other cell's vector must miss.
    const auto originVectors = static_cast<int>(
        std::count_if(kDeepslateAquiferRandomVectors.begin(), kDeepslateAquiferRandomVectors.end(),
                      [](const AquiferRandomVector& v) { return v.cy == 0; }));
    CHECK(originVectors == 9);
    CHECK(mismatchesOf(Derivation{.shift = 15}) == vectorCount() - originVectors);
    CHECK(mismatchesOf(Derivation{.shift = 17}) == vectorCount() - originVectors);

    // PINNED AS INVISIBLE. On the y axis the mixed value is never negative
    // before the shift, so a logical shift parts from the arithmetic one
    // nowhere here: these vectors cannot tell them apart, and only the
    // server's blocks have (SPEC §11). A cell that could would sit off the
    // axis, where deepslate's mix is not vanilla's.
    CHECK(mismatchesOf(Derivation{.logicalShift = true}) == 0);
}

TEST_CASE("the bounded draws pin jitterOf's wiring, and only that", "[aquifer][rng][oracle]") {
    // Taking the draws in another order, or with other bounds, from the same
    // generator does not reproduce the recorded ones — so a change to either
    // in `jitterOf` fails here, in CI. That is a guard on this build's
    // reading, not evidence for it: the recorded draws were asked for with
    // that reading (the driver's BOUNDS). Vanilla's bounds and order are
    // pinned by the server-backed conformance cases alone.
    const auto drawsOf = [](const AquiferRandomVector& v, const std::array<std::size_t, 3>& order,
                            const std::array<std::int32_t, 3>& bounds) {
        auto generator =
            stratum::rng::positionalSourceFor(v.seed, "minecraft:aquifer").at(v.cx, v.cy, v.cz);
        std::array<std::int32_t, 3> draws{};
        for (const std::size_t axis : order) {
            draws.at(axis) = generator.nextInt(bounds.at(axis));
        }
        return draws;
    };
    const auto wrong = [&](const std::array<std::size_t, 3>& order,
                           const std::array<std::int32_t, 3>& bounds) {
        int count = 0;
        for (const auto& v : kDeepslateAquiferRandomVectors) {
            count += static_cast<int>(drawsOf(v, order, bounds) != v.draws);
        }
        return count;
    };

    REQUIRE(wrong({0, 1, 2}, {10, 9, 10}) == 0);
    // At least 65 of 72 miss under every other order.
    for (const auto& order : std::array<std::array<std::size_t, 3>, 5>{
             {{0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}}}) {
        CHECK(wrong(order, {10, 9, 10}) > vectorCount() / 2);
    }
    // At least 34 of 72 miss under every one of these; the floor leaves room.
    for (const auto& bounds : std::array<std::array<std::int32_t, 3>, 7>{{{10, 10, 10},
                                                                          {9, 9, 9},
                                                                          {10, 8, 10},
                                                                          {11, 9, 11},
                                                                          {9, 9, 10},
                                                                          {10, 9, 9},
                                                                          {16, 12, 16}}}) {
        CHECK(wrong({0, 1, 2}, bounds) > vectorCount() / 4);
    }
}
