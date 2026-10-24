// Stratum — column invariance, held to the bit on vanilla's own routers.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// A CornerCache keeps a node's value per (x, z), window and read context
// wherever Interpreter::isColumnInvariant says the node has one value down its
// column (and it reads no cell structure): the climate the biome source reads
// at every quart layer, `jagged` and the offset splines under every terrain
// corner, the End's island field. That is a structural proof — y reaches an
// invariant noise only as y * 0.0, a zero of either sign, which the octaves
// absorb into their origins — and this holds it to the bit where it matters:
// every node of every vanilla router the rule calls invariant, at 100 y from
// INT32_MIN to INT32_MAX over columns at the origin, at negative coordinates
// and far out, read with no window in the Block context and through the
// column's chunk window detached, as the aquifer reads. Where one y refuses,
// every y must (vanilla's `jagged` does, past chunk 175 770): a value that
// throws is never kept, and an invariant node's refusal must be invariant too.

#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/graph.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/javamath.hpp>
#include <stratum/settings/noise_settings.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using stratum::data::Pack;
using stratum::data::ResourceLocation;
using stratum::density::CellGeometry;
using stratum::density::EvalError;
using stratum::density::FlatCacheWindow;
using stratum::density::Interpreter;
using stratum::density::NodeIndex;
using stratum::density::NoiseRegistry;
using stratum::density::Point;
using stratum::density::ReadContext;

[[nodiscard]] std::filesystem::path versionDir() {
    return std::filesystem::path(STRATUM_FIXTURES_DIR) / "1.21.11";
}

[[nodiscard]] bool haveFixtures() {
    return std::filesystem::is_directory(versionDir() / "worldgen" / "noise_settings");
}

/// One read's outcome: the value's bits, or that it refused.
struct Outcome {
    std::optional<std::uint64_t> bits;

    [[nodiscard]] bool operator==(const Outcome&) const noexcept = default;
};

/// The chunk window of the chunk holding (@p x, @p z): terrain::ChunkFiller's.
[[nodiscard]] FlatCacheWindow windowAround(std::int32_t x, std::int32_t z) {
    const std::int32_t baseX = stratum::javamath::floorDiv(x, std::int32_t{16}) * 16;
    const std::int32_t baseZ = stratum::javamath::floorDiv(z, std::int32_t{16}) * 16;
    return FlatCacheWindow{.minX = baseX, .maxX = baseX + 19, .minZ = baseZ, .maxZ = baseZ + 19};
}

} // namespace

TEST_CASE("every node of vanilla's routers called column-invariant has one bit pattern down its "
          "column",
          "[conformance][density][memo]") {
    if (!haveFixtures()) {
        SKIP("no worldgen fixtures under " << STRATUM_FIXTURES_DIR << "; run tools/fetch-vanilla");
    }
    const Pack pack = Pack::open(versionDir() / "worldgen");
    const stratum::settings::LoadedSettings loaded = stratum::settings::loadAll(pack);
    const stratum::density::Graph& graph = loaded.graph;

    std::vector<std::int32_t> ys{
        std::numeric_limits<std::int32_t>::min(), -2049, -64, -63, -1, 0, 1, 62, 63, 319, 320, 4095,
        std::numeric_limits<std::int32_t>::max()};
    for (std::int32_t y = -1999; ys.size() < 100U; y += 53) {
        ys.push_back(y);
    }
    const std::vector<std::pair<std::int32_t, std::int32_t>> columns{
        {0, 0}, {-1, -1}, {-12345, 6789}, {100003, -77}, {-2000001, 1999999}, {2900005, 7}};

    std::size_t dimensions = 0;
    for (const char* name : {"minecraft:overworld", "minecraft:amplified", "minecraft:large_biomes",
                             "minecraft:end", "minecraft:caves", "minecraft:floating_islands"}) {
        CAPTURE(name);
        const stratum::settings::NoiseSettings& settings =
            loaded.settings.at(ResourceLocation::parse(name));
        const std::vector<NodeIndex> roots(settings.router.entries.begin(),
                                           settings.router.entries.end());
        const std::vector<ResourceLocation> wanted = graph.noisesReachableFrom(roots);
        const NoiseRegistry noises = NoiseRegistry::create(
            pack, wanted, 42,
            settings.legacyRandomSource ? stratum::density::RandomSource::Legacy
                                        : stratum::density::RandomSource::Xoroshiro);
        const Interpreter interpreter(graph, noises,
                                      CellGeometry{.width = settings.geometry.cellWidth(),
                                                   .height = settings.geometry.cellHeight()});
        Interpreter::CornerCache detachedCache(interpreter.cacheSize(),
                                               Interpreter::CornerCache::Sharing::LastCell);

        std::set<NodeIndex> reached;
        for (const NodeIndex root : roots) {
            for (const NodeIndex node : graph.reachableFrom(root)) {
                reached.insert(node);
            }
        }

        std::size_t invariant = 0;
        std::size_t reads = 0;
        std::size_t refusedColumns = 0;
        std::size_t differing = 0;
        std::string firstDiffering = "none";
        for (const NodeIndex node : reached) {
            if (!interpreter.isColumnInvariant(node)) {
                continue;
            }
            ++invariant;
            for (const auto& [x, z] : columns) {
                const FlatCacheWindow window = windowAround(x, z);
                const auto read = [&](std::int32_t y, bool detached) {
                    const Point at{.x = x, .y = y, .z = z};
                    try {
                        const double value =
                            detached ? interpreter.evaluate(node, at, detachedCache, window,
                                                            ReadContext::Detached)
                                     : interpreter.evaluate(node, at);
                        return Outcome{.bits = std::bit_cast<std::uint64_t>(value)};
                    } catch (const EvalError&) {
                        return Outcome{.bits = std::nullopt};
                    }
                };
                for (const bool detached : {false, true}) {
                    const Outcome atZero = read(0, detached);
                    refusedColumns += atZero.bits.has_value() ? 0U : 1U;
                    for (const std::int32_t y : ys) {
                        ++reads;
                        if (read(y, detached) != atZero) {
                            if (differing++ == 0U) {
                                firstDiffering = "node " + std::to_string(node) + " at (" +
                                                 std::to_string(x) + ", " + std::to_string(y) +
                                                 ", " + std::to_string(z) + ")" +
                                                 (detached ? " detached" : "");
                            }
                        }
                    }
                }
            }
        }
        CAPTURE(invariant, reads, refusedColumns, firstDiffering);
        CHECK(differing == 0U);
        // Not a test of nothing: each dimension has invariant nodes to hold.
        CHECK(invariant > 0U);
        ++dimensions;
    }
    CHECK(dimensions == 6U);
}
