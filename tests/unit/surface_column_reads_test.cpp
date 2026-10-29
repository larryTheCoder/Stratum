// Stratum — SurfaceColumn's one reading body, against the two walks it folds.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// terrain::SurfaceColumn counts a column's two stone-depth runs, its water
// height and its topmost non-air block in one body (readWith), which read()
// runs over a span and terrain::ChunkFiller runs straight off its buffer. It
// walks UP first — the floor's run, and the topmost non-air and fluid
// entries as the last of each met ascending — and then DOWN from the topmost
// non-air entry only. The reference here is the reading that body replaced,
// restated as it was: a descending walk over the whole column for the top
// run, the top and the water height, then an ascending one for the floor run.
// surface_column_test.cpp holds the measured rules by known answers; this
// holds the two orders to each other over random columns.
#include <stratum/terrain/surface_column.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

namespace {

using stratum::terrain::Category;
using stratum::terrain::SurfaceColumn;

struct Reads {
    std::vector<std::int32_t> above;
    std::vector<std::int32_t> below;
    std::optional<std::int32_t> waterHeight;
    std::int32_t top = 0;
};

/// The descending-then-ascending reading, step for step.
[[nodiscard]] Reads twoWalks(const std::vector<Category>& categories, std::int32_t minY) {
    Reads reads;
    const std::size_t height = categories.size();
    reads.above.resize(height);
    reads.below.resize(height);
    reads.top = minY - 1;
    std::int32_t run = 0;
    for (std::size_t i = height; i-- > 0;) {
        const Category category = categories[i];
        const std::int32_t y = minY + static_cast<std::int32_t>(i);
        if (category == Category::Air) {
            run = 0;
        } else {
            if (reads.top < minY) {
                reads.top = y;
            }
            if (category == Category::Solid) {
                ++run;
            } else if (!reads.waterHeight.has_value()) {
                reads.waterHeight = y + 1;
            }
        }
        reads.above[i] = run;
    }
    run = 0;
    for (std::size_t i = 0; i < height; ++i) {
        run = categories[i] == Category::Solid ? run + 1 : 0;
        reads.below[i] = run;
    }
    return reads;
}

class Dice {
public:
    explicit Dice(std::uint64_t seed) : state_(seed) {}

    [[nodiscard]] std::uint32_t below(std::uint32_t bound) {
        state_ = (state_ * UINT64_C(6364136223846793005)) + UINT64_C(1442695040888963407);
        return static_cast<std::uint32_t>(state_ >> 33U) % bound;
    }

private:
    std::uint64_t state_;
};

/// A column built in runs, each run one category, so that runs, pools,
/// pockets and open sky all occur; some columns are all air, some have no
/// air at the top at all.
[[nodiscard]] std::vector<Category> randomColumn(Dice& dice, std::size_t height) {
    std::vector<Category> categories;
    categories.reserve(height);
    const std::uint32_t style = dice.below(8);
    while (categories.size() < height) {
        Category category = Category::Solid;
        switch (style == 0U ? 0U : dice.below(style == 1U ? 2U : 3U)) {
            case 0:
                category = style == 0U ? Category::Air : Category::Solid;
                break;
            case 1:
                category = Category::Fluid;
                break;
            default:
                category = Category::Air;
                break;
        }
        const std::size_t length = 1U + dice.below(24);
        for (std::size_t i = 0; i < length && categories.size() < height; ++i) {
            categories.push_back(category);
        }
    }
    return categories;
}

} // namespace

TEST_CASE("SurfaceColumn reads every column as the descending and ascending walks did",
          "[terrain][surface]") {
    Dice dice(20261029);
    SurfaceColumn spanRead;
    SurfaceColumn lambdaRead;
    SurfaceColumn boundedRead;
    std::size_t withWater = 0;
    std::size_t allAir = 0;
    for (std::size_t trial = 0; trial < 4000; ++trial) {
        const std::size_t height = trial < 3 ? trial : 1U + dice.below(400);
        const std::int32_t minY = static_cast<std::int32_t>(dice.below(200)) - 100;
        const std::vector<Category> categories = randomColumn(dice, height);
        const Reads expected = twoWalks(categories, minY);
        CAPTURE(trial, height, minY);

        // Each reader reused across columns of every height, as the filler's
        // one SurfaceColumn is.
        spanRead.read(categories, minY);
        lambdaRead.readWith(height, minY, [&](std::size_t i) { return categories[i]; });
        // Told where the air above the column starts — one past its topmost
        // non-air entry, as the filler finds it — it never asks above that.
        const auto airFrom = static_cast<std::size_t>(expected.top - minY + 1);
        std::size_t askedAbove = 0;
        boundedRead.readWith(height, minY, airFrom, [&](std::size_t i) {
            askedAbove += i >= airFrom ? 1U : 0U;
            return categories[i];
        });
        CHECK(askedAbove == 0U);
        for (const SurfaceColumn* reads : {&spanRead, &lambdaRead, &boundedRead}) {
            REQUIRE(reads->top() == expected.top);
            REQUIRE(reads->waterHeight() == expected.waterHeight);
            for (std::size_t i = 0; i < height; ++i) {
                const std::int32_t y = minY + static_cast<std::int32_t>(i);
                REQUIRE(reads->stoneDepthAbove(y) == expected.above[i]);
                REQUIRE(reads->stoneDepthBelow(y) == expected.below[i]);
            }
        }
        withWater += expected.waterHeight.has_value() ? 1U : 0U;
        allAir += expected.top < minY ? 1U : 0U;
    }
    CHECK(withWater > 1000U);
    CHECK(allAir > 100U);
}

TEST_CASE("SurfaceColumn's accessors still refuse a y outside the column read",
          "[terrain][surface]") {
    SurfaceColumn reads;
    const std::vector<Category> categories{Category::Solid, Category::Fluid, Category::Air};
    reads.read(categories, 10);
    CHECK(reads.stoneDepthBelow(10) == 1);
    CHECK_THROWS_AS(reads.stoneDepthAbove(9), std::out_of_range);
    CHECK_THROWS_AS(reads.stoneDepthAbove(13), std::out_of_range);
    CHECK_THROWS_AS(reads.stoneDepthBelow(13), std::out_of_range);
}
