// Stratum — ore veins: the gate, the three draws, and the flag coupling.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The per-block agreement against real server output lives in
// `tests/conformance/vanilla_ore_vein_test.cpp`, which needs probe worlds.
// This file is fixture-free: known-answer vectors for the RNG the derivation
// rests on, and the deterministic gate's own boundaries, which are the pieces
// a wrong edge case would silently shift.
#include <stratum/ore/vein.hpp>
#include <stratum/rng/xoroshiro128.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstdint>

using stratum::ore::clearsRichness;
using stratum::ore::inAnyVeinRange;
using stratum::ore::Vein;
using stratum::ore::VeinBlock;
using stratum::ore::VeinInputs;
using stratum::ore::VeinSource;
using stratum::ore::veinsPlaceBlocks;
using stratum::ore::VeinType;

namespace {

/// Inputs that clear every deterministic gate, so a test can vary one thing.
[[nodiscard]] VeinInputs passing(const double toggle) {
    return VeinInputs{.toggle = toggle, .ridged = -1.0, .gap = 0.0};
}

} // namespace

TEST_CASE("the ore source's draws match known-answer vectors", "[ore][vein][rng]") {
    // `positionalSourceFor(seed, "minecraft:ore").at(x, y, z)`, first three
    // nextFloat()s. The salt is the whole derivation: a wrong one moves every
    // vein in the world, and nothing else in this file would notice. Taken
    // from this implementation once it read 100.000% against the server on
    // 91245 real blocks (SPEC §11), so these pin the mechanism against drift
    // rather than merely against itself.
    struct Vector {
        std::int64_t seed;
        std::int32_t x, y, z;
        float first, second, third;
    };

    static constexpr Vector kVectors[] = {
        {0LL, 0, 0, 0, 0.892419636F, 0.960065186F, 0.90817225F},
        {0LL, 75, 19, 88, 0.449496031F, 0.394712806F, 0.335043967F},
        {0LL, -13, -47, 260, 0.19639343F, 0.154126883F, 0.451847255F},
        {0LL, 1, -60, -1, 0.753855169F, 0.618357182F, 0.110221386F},
        {42LL, 0, 0, 0, 0.395392776F, 0.237860024F, 0.975757658F},
        {42LL, 75, 19, 88, 0.967148244F, 0.115772724F, 0.154608607F},
        {42LL, -13, -47, 260, 0.215975821F, 0.300225139F, 0.395461559F},
        {100LL, 0, 0, 0, 0.447641373F, 0.0494456887F, 0.617500722F},
        {100LL, 255, 50, 255, 0.898220241F, 0.120529652F, 0.0537125468F},
        {-1LL, 0, 0, 0, 0.765336275F, 0.896994948F, 0.0848363638F},
        {-1LL, -13, -47, 260, 0.0707936287F, 0.920744777F, 0.543898821F},
        {1234LL, 1, -60, -1, 0.284154177F, 0.995274603F, 0.839390934F},
    };
    // Compared as bit patterns, not as floats: these are known-answer
    // vectors, so "equal to the last bit" is exactly the claim, and
    // `-Wfloat-equal` is right that `==` on floats usually is not.
    const auto bits = [](const float value) { return std::bit_cast<std::uint32_t>(value); };
    for (const Vector& v : kVectors) {
        auto generator =
            stratum::rng::positionalSourceFor(v.seed, "minecraft:ore").at(v.x, v.y, v.z);
        CHECK(bits(generator.nextFloat()) == bits(v.first));
        CHECK(bits(generator.nextFloat()) == bits(v.second));
        CHECK(bits(generator.nextFloat()) == bits(v.third));
    }
}

TEST_CASE("only the two vein ranges can hold a vein", "[ore][vein]") {
    CHECK_FALSE(inAnyVeinRange(-61));
    CHECK(inAnyVeinRange(-60));
    CHECK(inAnyVeinRange(50));
    CHECK_FALSE(inAnyVeinRange(51));
    // The dead zone between iron's top and copper's bottom is INSIDE the
    // cheap range check — it is the richness gate that rejects it, which is
    // why both have to exist.
    CHECK(inAnyVeinRange(-4));
}

TEST_CASE("the deterministic gate matches the measured ranges", "[ore][vein]") {
    SECTION("the sign of vein_toggle picks the metal, and the metal the range") {
        // Copper wants y in [0, 50] AND a positive toggle; a rich negative
        // toggle at the same y is not an iron candidate, it is nothing.
        CHECK(clearsRichness(25, 1.0));
        CHECK_FALSE(clearsRichness(25, -1.0));
        // Iron wants y in [-60, -8] and a toggle <= 0.
        CHECK(clearsRichness(-30, -1.0));
        CHECK_FALSE(clearsRichness(-30, 1.0));
    }

    SECTION("the dead zone between the ranges produces neither") {
        for (std::int32_t y = -7; y < 0; ++y) {
            CHECK_FALSE(clearsRichness(y, 1.0));
            CHECK_FALSE(clearsRichness(y, -1.0));
        }
    }

    SECTION("the ranges are inclusive at both ends") {
        CHECK(clearsRichness(0, 1.0));
        CHECK(clearsRichness(50, 1.0));
        CHECK_FALSE(clearsRichness(51, 1.0));
        CHECK(clearsRichness(-60, -1.0));
        CHECK(clearsRichness(-8, -1.0));
        CHECK_FALSE(clearsRichness(-61, -1.0));
    }

    SECTION("richness is 0.6 at a limit and 0.4 twenty blocks in") {
        // At the limit itself nothing under 0.6 survives...
        CHECK(clearsRichness(0, 0.6));
        CHECK_FALSE(clearsRichness(0, 0.59));
        // ...and 20 blocks inside, the bar has fallen the whole way to 0.4.
        CHECK(clearsRichness(20, 0.4));
        CHECK_FALSE(clearsRichness(20, 0.39));
        // Halfway along the ramp it is halfway down, and it does NOT keep
        // falling past 20 blocks in.
        CHECK(clearsRichness(10, 0.5));
        CHECK_FALSE(clearsRichness(10, 0.49));
        CHECK(clearsRichness(25, 0.4));
        CHECK_FALSE(clearsRichness(25, 0.39));
    }

    SECTION("the ramp is measured from whichever limit is nearer") {
        // Copper's top end, counted down from 50 rather than up from 0.
        CHECK(clearsRichness(50, 0.6));
        CHECK_FALSE(clearsRichness(50, 0.59));
        CHECK(clearsRichness(45, 0.55));
        CHECK_FALSE(clearsRichness(45, 0.54));
    }
}

TEST_CASE("ore veins need aquifers enabled too", "[ore][vein]") {
    // Measured, not assumed: a probe with veins on and aquifers off came back
    // as 6291456 of 6291456 plain stone blocks (SPEC §11).
    CHECK(veinsPlaceBlocks(true, true));
    CHECK_FALSE(veinsPlaceBlocks(true, false));
    CHECK_FALSE(veinsPlaceBlocks(false, true));
    CHECK_FALSE(veinsPlaceBlocks(false, false));
}

TEST_CASE("a position failing the gate is never touched, whatever the RNG says", "[ore][vein]") {
    const VeinSource source(42);
    // vein_ridged >= 0 is the one gate that is not about y or the toggle, and
    // it is necessary with zero exceptions across every real vein block
    // measured.
    CHECK_FALSE(source.at(0, 25, 0, VeinInputs{.toggle = 1.0, .ridged = 0.0, .gap = 0.0}).placed());
    CHECK_FALSE(source.at(0, 25, 0, VeinInputs{.toggle = 1.0, .ridged = 0.5, .gap = 0.0}).placed());
    // ...and it is only that: flip it negative and the same position is live.
    CHECK(source.at(0, 25, 0, VeinInputs{.toggle = 1.0, .ridged = -0.001, .gap = 0.0}).placed());
}

TEST_CASE("the metal follows y, not the RNG", "[ore][vein]") {
    const VeinSource source(100);
    for (std::int32_t y = 0; y <= 50; ++y) {
        const Vein vein = source.at(7, y, 9, passing(1.0));
        if (vein.placed()) {
            CHECK(vein.type == VeinType::Copper);
        }
    }
    for (std::int32_t y = -60; y <= -8; ++y) {
        const Vein vein = source.at(7, y, 9, passing(-1.0));
        if (vein.placed()) {
            CHECK(vein.type == VeinType::Iron);
        }
    }
}

TEST_CASE("vein_gap at or below -0.3 forces filler over ore", "[ore][vein]") {
    const VeinSource source(42);
    // A toggle of 1.0 maps to the TOP of the ore chance (0.3), so a position
    // that comes out as ore with an open gap is the sharpest test that the
    // gap alone closed it.
    int oreWithOpenGap = 0;
    int oreWithClosedGap = 0;
    for (std::int32_t x = 0; x < 400; ++x) {
        const VeinInputs open{.toggle = 1.0, .ridged = -1.0, .gap = 0.0};
        const VeinInputs closed{.toggle = 1.0, .ridged = -1.0, .gap = -0.3};
        const VeinBlock a = source.at(x, 25, 0, open).block;
        const VeinBlock b = source.at(x, 25, 0, closed).block;
        oreWithOpenGap += (a == VeinBlock::Ore || a == VeinBlock::RawBlock) ? 1 : 0;
        oreWithClosedGap += (b == VeinBlock::Ore || b == VeinBlock::RawBlock) ? 1 : 0;
        // Closing the gap can only ever turn ore into filler, never the other
        // way, and never change whether the block was touched at all.
        CHECK((b == VeinBlock::None) == (a == VeinBlock::None));
    }
    CHECK(oreWithOpenGap > 0);
    CHECK(oreWithClosedGap == 0);
}

TEST_CASE("the three draws come from one generator, in order", "[ore][vein][rng]") {
    // The membership roll is draw 1, the ore roll draw 2 and the raw roll
    // draw 3 of the SAME `.at(x, y, z)` stream. Reproducing that by hand here
    // is what would catch a future refactor that gave any of them its own
    // generator — which would still look random, still read ~70/30/2 in
    // aggregate, and be wrong on every block.
    const std::int64_t seed = 424242;
    const VeinSource source(seed);
    const auto raw = stratum::rng::positionalSourceFor(seed, "minecraft:ore");
    int touched = 0;
    int ore = 0;
    for (std::int32_t x = 0; x < 500; ++x) {
        const VeinInputs inputs{.toggle = 0.5, .ridged = -1.0, .gap = 0.0};
        auto generator = raw.at(x, 25, 0);
        const bool expectTouched = generator.nextFloat() < 0.7F;
        // |toggle| 0.5 sits halfway along [0.4, 0.6], so the ore chance is
        // halfway along [0.1, 0.3].
        const bool expectOre = expectTouched && generator.nextFloat() < 0.2F;
        const bool expectRaw = expectOre && generator.nextFloat() < 0.02F;

        const Vein vein = source.at(x, 25, 0, inputs);
        CHECK(vein.placed() == expectTouched);
        CHECK((vein.block == VeinBlock::Ore || vein.block == VeinBlock::RawBlock) == expectOre);
        CHECK((vein.block == VeinBlock::RawBlock) == expectRaw);
        touched += expectTouched ? 1 : 0;
        ore += expectOre ? 1 : 0;
    }
    // Guards against the loop above passing vacuously.
    CHECK(touched > 0);
    CHECK(ore > 0);
}
