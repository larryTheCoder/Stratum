// Stratum — the worlds a PocketMine-MP process shares: opened, pinned,
// refused, and written, under the threads PocketMine-MP runs them on.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Fixture-free: the pipeline is one this test writes and freezes itself — a
// flat dimension whose router is constants — because what is under test is
// what the zend module's registry and pins promise, not what the terrain
// looks like (ext/zend/tests/ and the conformance suites hold vanilla's):
//
//   * one compiled dimension per world for every thread that opens it, and
//     none kept once nothing holds it;
//   * shared by the blob's every byte, so a blob restamped with another
//     engine version, or damaged under an intact header, is refused while
//     its twin is open — not handed the twin's dimension (SPEC §6);
//   * refused by name, at open, for what PocketMine-MP cannot hold;
//   * a pinned world outliving every generator, and the file it was read
//     from, until it is unpinned;
//   * a pipeline written whole or not at all, and read whole while one is
//     written over it.

#include "support/temp_path.hpp"

#include <stratum/data/pack.hpp>
#include <stratum/freeze/pipeline.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/version.hpp>
#include <stratum/world/dimension.hpp>

#include <nlohmann/json.hpp>
#include <stratum_pmmp/dimension_registry.hpp>
#include <stratum_pmmp/fluid_updates.hpp>
#include <stratum_pmmp/worlds.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using stratum::pmmp::compileForPocketMine;
using stratum::pmmp::DimensionRegistry;
using stratum::pmmp::FluidUpdate;
using stratum::pmmp::OpenWorld;
using stratum::pmmp::readPipelineFile;
using stratum::pmmp::WorldOptions;
using stratum::pmmp::Worlds;
using stratum::pmmp::writePipelineFile;

namespace {

/// A worldgen tree, a frozen pipeline made from it, and a scratch directory
/// to put blobs in, all removed afterwards.
class Scratch {
public:
    Scratch() : root_(stratum::test::tempPath("stratum-worlds-test")) {
        std::filesystem::remove_all(root_);
        for (const char* registry : {"density_function", "noise", "noise_settings", "biome"}) {
            std::filesystem::create_directories(root_ / "worldgen" / registry);
        }
        std::filesystem::create_directories(root_ / "biome_parameters" / "minecraft");
        std::filesystem::create_directories(root_ / "blobs");

        // Two settings: one inside PocketMine-MP's y -64..320, one that
        // reaches below it.
        write("noise_settings", "overworld", settings(-64).dump());
        write("noise_settings", "deep", settings(-128).dump());
        write("biome", "plains", R"({"temperature":0.8})");
        // Not a vanilla biome, so not in the Bedrock table: a datapack's.
        write("biome", "moon", R"({"temperature":0.1})");
        writeList("overworld", R"({"biomes":[
            {"biome":"minecraft:plains","parameters":{"temperature":[-1.0,1.0],
             "humidity":[-1.0,1.0],"continentalness":[-1.0,1.0],"erosion":[-1.0,1.0],
             "depth":0.0,"weirdness":[-1.0,1.0],"offset":0.0}}]})");
        writeList("lunar", R"({"biomes":[
            {"biome":"minecraft:plains","parameters":{"temperature":[-1.0,0.0],
             "humidity":[-1.0,1.0],"continentalness":[-1.0,1.0],"erosion":[-1.0,1.0],
             "depth":0.0,"weirdness":[-1.0,1.0],"offset":0.0}},
            {"biome":"minecraft:moon","parameters":{"temperature":[0.0,1.0],
             "humidity":[-1.0,1.0],"continentalness":[-1.0,1.0],"erosion":[-1.0,1.0],
             "depth":0.0,"weirdness":[-1.0,1.0],"offset":0.0}}]})");

        blob_ = stratum::freeze::write(stratum::freeze::resolve(
            stratum::data::Pack::open(root_ / "worldgen"), root_ / "biome_parameters"));
    }

    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    Scratch(Scratch&&) = delete;
    Scratch& operator=(Scratch&&) = delete;

    ~Scratch() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    [[nodiscard]] const std::vector<std::byte>& blob() const noexcept { return blob_; }

    /// @p bytes written to blobs/@p name, its path returned.
    [[nodiscard]] std::filesystem::path put(std::string_view name,
                                            const std::vector<std::byte>& bytes) const {
        const std::filesystem::path path = blobs() / std::string(name);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        return path;
    }

    [[nodiscard]] std::filesystem::path blobs() const { return root_ / "blobs"; }

private:
    [[nodiscard]] static nlohmann::json settings(int minY) {
        nlohmann::json router = nlohmann::json::object();
        for (std::size_t i = 0; i < stratum::settings::kRouterEntryCount; ++i) {
            router[std::string(stratum::settings::routerEntryName(
                static_cast<stratum::settings::RouterEntry>(i)))] = 0.0;
        }
        return {
            {"default_block", {{"Name", "minecraft:stone"}}},
            {"default_fluid", {{"Name", "minecraft:water"}, {"Properties", {{"level", "0"}}}}},
            {"sea_level", 63},
            {"disable_mob_generation", false},
            {"aquifers_enabled", false},
            {"ore_veins_enabled", false},
            {"legacy_random_source", false},
            {"noise",
             {{"min_y", minY}, {"height", 384}, {"size_horizontal", 1}, {"size_vertical", 2}}},
            {"noise_router", router},
            {"spawn_target", nlohmann::json::array()},
            {"surface_rule",
             {{"type", "minecraft:block"}, {"result_state", {{"Name", "minecraft:stone"}}}}},
        };
    }

    void write(const char* registry, std::string_view name, std::string_view json) const {
        std::ofstream out(root_ / "worldgen" / registry / (std::string(name) + ".json"));
        out << json;
    }

    void writeList(std::string_view name, std::string_view json) const {
        std::ofstream out(root_ / "biome_parameters" / "minecraft" / (std::string(name) + ".json"));
        out << json;
    }

    std::filesystem::path root_;
    std::vector<std::byte> blob_;
};

[[nodiscard]] WorldOptions overworld(const std::filesystem::path& blob, std::int64_t seed) {
    return {.blobPath = blob.string(),
            .noiseSettings = "minecraft:overworld",
            .biomeSource = "minecraft:overworld",
            .seed = seed};
}

/// The header's engine version, one higher: the bytes `freeze::write` stamps
/// at offset 12, after the magic and the container format.
[[nodiscard]] std::vector<std::byte> restamped(std::vector<std::byte> blob) {
    blob.at(12) = std::byte{static_cast<std::uint8_t>(stratum::kPipelineEngineVersion + 1)};
    return blob;
}

/// The last payload byte changed and the header left as it was, content
/// hash included.
[[nodiscard]] std::vector<std::byte> damaged(std::vector<std::byte> blob) {
    blob.back() ^= std::byte{0x01};
    return blob;
}

/// Through a cast: MSVC warns (C4242, an error here) on brace-initialising
/// FluidUpdate's uint8_t members from an int.
[[nodiscard]] FluidUpdate at(int x, std::int32_t y, int z) {
    return FluidUpdate{
        .localX = static_cast<std::uint8_t>(x), .y = y, .localZ = static_cast<std::uint8_t>(z)};
}

[[nodiscard]] std::vector<std::string> filesIn(const std::filesystem::path& directory) {
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        names.push_back(entry.path().filename().string());
    }
    return names;
}

} // namespace

TEST_CASE("a dimension PocketMine-MP cannot hold is refused by name when it is opened", "[pmmp]") {
    const Scratch scratch;
    const std::vector<std::byte>& blob = scratch.blob();
    const auto open = [&blob](const char* settings, const char* source) {
        return compileForPocketMine(blob, settings, source, 1);
    };

    CHECK(open("minecraft:overworld", "minecraft:overworld")->geometry().minY == -64);
    // Below PocketMine-MP's 24 sub-chunks: refused here, where encodeChunk
    // would only have refused it at the first chunk.
    CHECK_THROWS_WITH(
        open("minecraft:deep", "minecraft:overworld"),
        ContainsSubstring("y -128 to 256 does not fit PocketMine-MP's 24 sub-chunks"));
    // A list naming a biome with no Bedrock id: refused before any chunk
    // lands on it.
    CHECK_THROWS_WITH(open("minecraft:overworld", "minecraft:lunar"),
                      ContainsSubstring("biome parameter list 'minecraft:lunar' names "
                                        "'minecraft:moon', which has no Bedrock id"));
    // compile's own refusals, unchanged and first.
    CHECK_THROWS_WITH(open("minecraft:nowhere", "minecraft:lunar"),
                      ContainsSubstring("no noise settings 'minecraft:nowhere'"));
    CHECK_THROWS_WITH(open("minecraft:overworld", "minecraft:nowhere"),
                      ContainsSubstring("no biome parameter list 'minecraft:nowhere'"));
    CHECK_THROWS_WITH(open("minecraft:overworld", "minecraft:fixed"),
                      ContainsSubstring("is not implemented by this build"));
    // And freeze::read's.
    CHECK_THROWS_WITH(
        compileForPocketMine(restamped(blob), "minecraft:overworld", "minecraft:overworld", 1),
        ContainsSubstring("frozen by pipeline engine version"));
    CHECK_THROWS_WITH(compileForPocketMine(std::vector<std::byte>(4), "minecraft:overworld",
                                           "minecraft:overworld", 1),
                      ContainsSubstring("not a frozen pipeline"));
}

TEST_CASE("the registry shares by every byte, so a restamped or damaged twin is refused",
          "[pmmp]") {
    const Scratch scratch;
    DimensionRegistry registry;
    const auto held =
        registry.open(scratch.blob(), "minecraft:overworld", "minecraft:overworld", 7);
    CHECK(registry.open(scratch.blob(), "minecraft:overworld", "minecraft:overworld", 7) == held);
    CHECK(registry.open(scratch.blob(), "minecraft:overworld", "minecraft:overworld", 8) != held);

    // The same payload, and so the same content hash in the header, under
    // another engine version: keyed by that hash it was handed `held`, and
    // generated a world this build may not reproduce. Keyed by every byte it
    // misses, and freeze::read refuses it.
    CHECK_THROWS_WITH(
        registry.open(restamped(scratch.blob()), "minecraft:overworld", "minecraft:overworld", 7),
        ContainsSubstring("frozen by pipeline engine version"));
    // A payload damaged under an intact header, the same.
    CHECK_THROWS_WITH(
        registry.open(damaged(scratch.blob()), "minecraft:overworld", "minecraft:overworld", 7),
        ContainsSubstring("does not match its own content hash"));
    // And neither displaced the dimension it was not given.
    CHECK(registry.open(scratch.blob(), "minecraft:overworld", "minecraft:overworld", 7) == held);
}

TEST_CASE("the registry keeps nothing nobody holds", "[pmmp]") {
    const Scratch scratch;
    DimensionRegistry registry;
    const std::weak_ptr<const stratum::world::CompiledDimension> first =
        registry.open(scratch.blob(), "minecraft:overworld", "minecraft:overworld", 1);
    CHECK(first.expired()); // the registry itself holds no dimension
    CHECK(registry.entries() == 1U);
    // A refused open adds nothing.
    CHECK_THROWS(registry.open(scratch.blob(), "minecraft:deep", "minecraft:overworld", 1));
    CHECK(registry.entries() == 1U);
    // The next entry prunes the expired one.
    const auto second =
        registry.open(scratch.blob(), "minecraft:overworld", "minecraft:overworld", 2);
    CHECK(registry.entries() == 1U);
}

TEST_CASE("workers open and close the same and different worlds at once", "[pmmp]") {
    // PocketMine-MP's shape: a generator per worker thread, every worker
    // opening every world, and workers coming and going.
    const Scratch scratch;
    const std::filesystem::path blobA = scratch.put("a.blob", scratch.blob());
    // Another path to the same bytes: another world (its own outbox), the
    // same compiled dimension.
    const std::filesystem::path blobB = scratch.put("b.blob", scratch.blob());
    Worlds worlds;
    const std::vector<WorldOptions> options = {overworld(blobA, 1), overworld(blobA, 2),
                                               overworld(blobB, 1)};

    constexpr int kWorkers = 8;
    // Phase one: every worker opens every world and holds it until all have,
    // so every holder of a world is open at once and must share.
    std::vector<std::vector<OpenWorld>> held(kWorkers);
    {
        std::vector<std::thread> workers;
        workers.reserve(kWorkers);
        for (int w = 0; w < kWorkers; ++w) {
            workers.emplace_back([&worlds, &options, &held, w] {
                for (const WorldOptions& world : options) {
                    held[static_cast<std::size_t>(w)].push_back(worlds.open(world));
                }
            });
        }
        for (std::thread& worker : workers) {
            worker.join();
        }
    }
    for (const std::vector<OpenWorld>& worker : held) {
        REQUIRE(worker.size() == options.size());
        for (std::size_t i = 0; i < options.size(); ++i) {
            CHECK(worker[i].dimension == held[0][i].dimension);
            CHECK(worker[i].fluidUpdates == held[0][i].fluidUpdates);
        }
    }
    CHECK(held[0][0].dimension != held[0][1].dimension);       // another seed
    CHECK(held[0][0].dimension == held[0][2].dimension);       // the same bytes
    CHECK(held[0][0].fluidUpdates != held[0][2].fluidUpdates); // another world
    CHECK(worlds.dimensions().entries() == 2U);
    held.clear();

    // Phase two: workers open and drop worlds over and over, each a
    // different mix, while one pins and unpins a fourth. Counted here and
    // checked after the join: Catch2's assertions are not thread-safe.
    std::atomic<int> wrong{0};
    {
        std::vector<std::thread> workers;
        workers.reserve(kWorkers + 1);
        for (int w = 0; w < kWorkers; ++w) {
            workers.emplace_back([&worlds, &options, &wrong, w] {
                for (int i = 0; i < 50; ++i) {
                    const WorldOptions& world =
                        options[static_cast<std::size_t>(i + w) % options.size()];
                    const OpenWorld opened = worlds.open(world);
                    opened.fluidUpdates->put(w, i, {at(1, i, 2)});
                    if (opened.dimension->geometry().height != 384) {
                        ++wrong;
                    }
                }
            });
        }
        workers.emplace_back([&worlds, &blobB, &wrong] {
            for (int i = 0; i < 50; ++i) {
                worlds.pin(overworld(blobB, 9));
                if (!worlds.unpin(overworld(blobB, 9))) {
                    ++wrong;
                }
            }
        });
        for (std::thread& worker : workers) {
            worker.join();
        }
    }
    CHECK(wrong == 0);
    // Nothing holds any of them now: one more open prunes every other entry.
    const OpenWorld last = worlds.open(options[0]);
    CHECK(worlds.dimensions().entries() == 1U);
    CHECK_FALSE(worlds.pinned(overworld(blobB, 9)));
}

TEST_CASE("a pinned world outlives its generators and the file it was read from", "[pmmp]") {
    const Scratch scratch;
    const std::filesystem::path blob = scratch.put("world.blob", scratch.blob());
    const WorldOptions world = overworld(blob, 42);
    Worlds worlds;

    CHECK_FALSE(worlds.unpin(world));
    worlds.pin(world);
    CHECK(worlds.pinned(world));

    // A worker generates a chunk and is shut down while idle, before the
    // chunk is populated: its updates are still there to take.
    const auto* pinnedDimension = [&] {
        const OpenWorld worker = worlds.open(world);
        worker.fluidUpdates->put(3, -4, {at(5, 60, 6)});
        return worker.dimension.get();
    }();
    CHECK(worlds.takeFluidUpdates(world, 3, -4).size() == 1U);

    // The blob is replaced on disk while the world is loaded. A worker that
    // restarts gets the state the world loaded with; the file is not read.
    static_cast<void>(scratch.put("world.blob", std::vector<std::byte>(16)));
    {
        const OpenWorld restarted = worlds.open(world);
        CHECK(restarted.dimension.get() == pinnedDimension);
    }

    // Counted: a second pin of the same world needs a second unpin.
    worlds.pin(world);
    CHECK(worlds.unpin(world));
    CHECK(worlds.pinned(world));
    CHECK(worlds.unpin(world));
    CHECK_FALSE(worlds.pinned(world));
    CHECK_FALSE(worlds.unpin(world));

    // Unpinned, with no generator: nothing held, and the next open reads the
    // file — which now refuses.
    CHECK(worlds.takeFluidUpdates(world, 3, -4).empty());
    CHECK_THROWS_WITH(worlds.open(world), ContainsSubstring("not a frozen pipeline"));

    // A pin that cannot open throws and pins nothing.
    CHECK_THROWS_WITH(worlds.pin(world), ContainsSubstring("not a frozen pipeline"));
    CHECK_FALSE(worlds.pinned(world));
    CHECK_THROWS_WITH(worlds.pin(overworld(scratch.blobs() / "missing.blob", 1)),
                      ContainsSubstring("cannot read the frozen pipeline at"));
}

TEST_CASE("a frozen pipeline is written whole or not at all", "[pmmp]") {
    const Scratch scratch;
    const std::filesystem::path path = scratch.blobs() / "stratum-pipeline.blob";
    const std::vector<std::byte> before(100, std::byte{0x11});
    writePipelineFile(path, before);
    CHECK(readPipelineFile(path) == before);

    // Replacing it leaves the new bytes and nothing beside them.
    writePipelineFile(path, scratch.blob());
    CHECK(readPipelineFile(path) == scratch.blob());
    CHECK(filesIn(scratch.blobs()) == std::vector<std::string>{"stratum-pipeline.blob"});

    // A write that cannot land leaves what was there, and no partial file.
    const std::filesystem::path directory = scratch.blobs() / "a-directory";
    std::filesystem::create_directory(directory);
    CHECK_THROWS_WITH(writePipelineFile(directory, before),
                      ContainsSubstring("cannot write the frozen pipeline to"));
    CHECK(std::filesystem::is_directory(directory));
    CHECK_THROWS_WITH(writePipelineFile(scratch.blobs() / "no-such-dir" / "x.blob", before),
                      ContainsSubstring("cannot write the frozen pipeline to"));
    CHECK(filesIn(scratch.blobs()).size() == 2U);
    CHECK(readPipelineFile(path) == scratch.blob());

    // Reading refuses what is no file, by name.
    CHECK_THROWS_WITH(readPipelineFile(directory),
                      ContainsSubstring("cannot read the frozen pipeline at"));
    CHECK_THROWS_WITH(readPipelineFile(scratch.blobs() / "missing.blob"),
                      ContainsSubstring("cannot read the frozen pipeline at"));
}

// POSIX only. On Windows, renaming over a file that another handle holds
// open is refused (the C++ runtime's streams do not share delete access), so
// there a write that lands while a reader reads is refused instead, by
// writePipelineFile's own refusal, leaving the file as it was: the
// interleaving raced here does not arise.
#if !defined(_WIN32)
TEST_CASE("a reader racing a writer reads one whole blob or the other and is never refused",
          "[pmmp]") {
    // Two blobs of different sizes written over one path, again and again,
    // while readers read it: a worker opening an unpinned world while the
    // main thread freezes over its blob. The sizes differ so that a size
    // taken from one file and bytes from the next refuse the read (the next
    // is shorter) or return a blob neither write wrote (it is longer). With
    // the size taken from the path before opening it, the reader did both,
    // hundreds of times in every run of this case.
    const Scratch scratch;
    const std::filesystem::path path = scratch.blobs() / "stratum-pipeline.blob";
    const std::vector<std::byte> shorter(std::size_t{16} * 1024, std::byte{0x11});
    const std::vector<std::byte> longer(std::size_t{48} * 1024, std::byte{0x22});
    writePipelineFile(path, shorter);

    constexpr int kWrites = 2000;
    constexpr int kReaders = 4;
    std::atomic<bool> writing{true};
    std::atomic<int> writesRefused{0};
    std::atomic<int> reads{0};
    std::atomic<int> readsRefused{0};
    std::atomic<int> readsTorn{0};
    std::vector<std::thread> threads;
    threads.reserve(kReaders + 1);
    for (int r = 0; r < kReaders; ++r) {
        threads.emplace_back([&] {
            while (writing.load()) {
                try {
                    const std::vector<std::byte> read = readPipelineFile(path);
                    if (read != shorter && read != longer) {
                        ++readsTorn;
                    }
                } catch (const std::runtime_error&) {
                    ++readsRefused;
                }
                ++reads;
            }
        });
    }
    threads.emplace_back([&] {
        for (int i = 0; i < kWrites; ++i) {
            try {
                writePipelineFile(path, i % 2 == 0 ? longer : shorter);
            } catch (const std::runtime_error&) {
                ++writesRefused;
            }
        }
        writing = false;
    });
    for (std::thread& thread : threads) {
        thread.join();
    }
    CHECK(writesRefused == 0);
    CHECK(reads > 0);
    CHECK(readsRefused == 0);
    CHECK(readsTorn == 0);
    CHECK(readPipelineFile(path) == shorter);
    CHECK(filesIn(scratch.blobs()) == std::vector<std::string>{"stratum-pipeline.blob"});
}
#endif
