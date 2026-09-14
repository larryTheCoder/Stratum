// Stratum — compiling a dimension refuses a pipeline that does not carry it.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Fixture-free: that a compiled dimension generates vanilla's terrain is
// tests/conformance/vanilla_compiled_dimension_test.cpp's job. What belongs
// here is that a name the frozen pipeline does not carry is refused by name
// before anything is built.
#include <stratum/data/resource_location.hpp>
#include <stratum/freeze/pipeline.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/world/dimension.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using Catch::Matchers::ContainsSubstring;
using stratum::data::ResourceLocation;
using stratum::world::CompiledDimension;

TEST_CASE("a dimension the frozen pipeline does not carry is refused by name", "[world]") {
    const auto overworld = ResourceLocation::parse("minecraft:overworld");

    CHECK_THROWS_WITH(
        CompiledDimension::compile(stratum::freeze::Pipeline{}, overworld, overworld, 1),
        ContainsSubstring("no noise settings 'minecraft:overworld'"));

    stratum::freeze::Pipeline withSettings;
    withSettings.settings.emplace(overworld, stratum::settings::NoiseSettings{});
    CHECK_THROWS_WITH(CompiledDimension::compile(std::move(withSettings), overworld, overworld, 1),
                      ContainsSubstring("no biome parameter list 'minecraft:overworld'"));
}
