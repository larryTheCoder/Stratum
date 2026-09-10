// Stratum — a biome's own declared temperature.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// TemperatureTable reads every worldgen/biome entry a Pack holds; the real
// biomes are Mojang-derived and never committed (SPEC §12), so what is
// checked here is the reading, against files this test writes itself. The
// real ones are exercised wherever the conformance suite runs `temperature`
// end to end.

#include <stratum/biome/temperature_table.hpp>
#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

using Catch::Matchers::ContainsSubstring;
using stratum::biome::TemperatureError;
using stratum::biome::TemperatureTable;
using stratum::data::Pack;
using stratum::data::ResourceLocation;

namespace {

/// A `worldgen/biome/*.json` tree small enough to write by hand, in the
/// layout `Pack::openWorldgenTree` reads — rooted so `biome/plains.json`
/// resolves the same way `worldgen/biome/plains.json` would in a data pack.
class TempBiomeTree {
public:
    TempBiomeTree() : path_(std::filesystem::temp_directory_path() / uniqueName()) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_ / "biome");
    }

    TempBiomeTree(const TempBiomeTree&) = delete;
    TempBiomeTree& operator=(const TempBiomeTree&) = delete;

    ~TempBiomeTree() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const TempBiomeTree& define(std::string_view name, const nlohmann::json& json) const {
        std::ofstream out(path_ / "biome" / (std::string(name) + ".json"));
        out << json.dump();
        return *this;
    }

    [[nodiscard]] Pack pack() const { return Pack::openWorldgenTree(path_); }

private:
    [[nodiscard]] static std::string uniqueName() {
        static int counter = 0;
        return "stratum-temperature-test-" + std::to_string(++counter);
    }

    std::filesystem::path path_;
};

/// Exact comparison by bits: the project keeps -Wfloat-equal on, and these
/// are exact constants rather than computed results, so bit equality is the
/// honest way to say so.
[[nodiscard]] std::uint32_t bits(float value) noexcept {
    return std::bit_cast<std::uint32_t>(value);
}

} // namespace

TEST_CASE("a biome's declared temperature is read from its own entry", "[biome][temperature]") {
    const TempBiomeTree tree;
    tree.define("plains", nlohmann::json{{"temperature", 0.5}});
    tree.define("desert", nlohmann::json{{"temperature", 2.0}});
    const TemperatureTable table = TemperatureTable::fromPack(tree.pack());

    CHECK(table.size() == 2U);
    CHECK(bits(table.at(ResourceLocation::parse("minecraft:plains"))) == bits(0.5F));
    CHECK(bits(table.at(ResourceLocation::parse("minecraft:desert"))) == bits(2.0F));
}

TEST_CASE("a biome missing temperature is refused, by name", "[biome][temperature]") {
    const TempBiomeTree tree;
    tree.define("mystery", nlohmann::json{{"downfall", 0.4}});
    CHECK_THROWS_WITH(TemperatureTable::fromPack(tree.pack()),
                      ContainsSubstring("mystery") && ContainsSubstring("temperature"));
}

TEST_CASE("a biome whose temperature is not a number is refused", "[biome][temperature]") {
    const TempBiomeTree tree;
    tree.define("odd", nlohmann::json{{"temperature", "warm"}});
    CHECK_THROWS_WITH(TemperatureTable::fromPack(tree.pack()),
                      ContainsSubstring("odd") && ContainsSubstring("temperature"));
}

TEST_CASE("a biome outside the table is refused, not defaulted", "[biome][temperature]") {
    const TempBiomeTree tree;
    tree.define("plains", nlohmann::json{{"temperature", 0.5}});
    const TemperatureTable table = TemperatureTable::fromPack(tree.pack());
    CHECK_THROWS_WITH(table.at(ResourceLocation::parse("minecraft:desert")),
                      ContainsSubstring("minecraft:desert"));
}
