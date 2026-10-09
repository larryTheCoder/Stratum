// Stratum — the hand-over of fluid updates from generation workers.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Fixture-free: what is under test is the outbox's own contract — replace on
// put, remove on take, nothing created by asking, and an outbox that lives
// exactly as long as something attached to it — and that it holds under the
// threads PocketMine-MP actually runs it on.
#include <stratum_pmmp/fluid_updates.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using stratum::pmmp::FluidUpdate;
using stratum::pmmp::FluidUpdateOutbox;
using stratum::pmmp::FluidUpdateOutboxes;
using stratum::pmmp::fluidUpdateWorldKey;

namespace {

/// Through a cast: MSVC warns (C4242, an error here) on brace-initialising
/// FluidUpdate's uint8_t members from an int.
[[nodiscard]] FluidUpdate at(int x, std::int32_t y, int z) {
    return FluidUpdate{
        .localX = static_cast<std::uint8_t>(x), .y = y, .localZ = static_cast<std::uint8_t>(z)};
}

} // namespace

TEST_CASE("an outbox hands each chunk's updates over once", "[pmmp]") {
    FluidUpdateOutbox outbox;
    const std::vector<FluidUpdate> first = {at(3, -12, 15), at(0, 40, 2)};
    outbox.put(-2, 5, first);
    CHECK(outbox.chunksHeld() == 1U);
    CHECK(outbox.take(5, -2).empty()); // x and z are not interchangeable
    CHECK(outbox.take(-2, 5) == first);
    CHECK(outbox.take(-2, 5).empty()); // taken means gone
    CHECK(outbox.chunksHeld() == 0U);

    // A chunk generated again replaces what its first generation left, so a
    // retried population never schedules a position twice.
    outbox.put(7, 7, first);
    const std::vector<FluidUpdate> second = {at(1, 1, 1)};
    outbox.put(7, 7, second);
    CHECK(outbox.take(7, 7) == second);

    // A chunk without fluid holds nothing — and clears what an earlier
    // generation of it left.
    outbox.put(1, 1, first);
    outbox.put(1, 1, {});
    CHECK(outbox.chunksHeld() == 0U);
    CHECK(outbox.take(1, 1).empty());
}

TEST_CASE("an outbox is shared while attached and gone once nothing holds it", "[pmmp]") {
    FluidUpdateOutboxes outboxes;
    const std::string world = fluidUpdateWorldKey("/worlds/a/stratum-pipeline.blob",
                                                  "minecraft:overworld", "minecraft:overworld", 42);
    // Asking never creates one: a main thread asking about a world no
    // worker has opened gets nothing.
    CHECK(outboxes.find(world) == nullptr);

    auto worker = outboxes.attach(world);
    auto otherWorker = outboxes.attach(world);
    CHECK(worker == otherWorker);
    worker->put(0, 0, {at(4, 60, 4)});
    REQUIRE(outboxes.find(world) != nullptr);
    CHECK(outboxes.find(world)->take(0, 0).size() == 1U);

    // Another world — a different blob path, the same pack and seed — is
    // another outbox.
    const std::string sibling = fluidUpdateWorldKey(
        "/worlds/b/stratum-pipeline.blob", "minecraft:overworld", "minecraft:overworld", 42);
    auto siblingWorker = outboxes.attach(sibling);
    CHECK(siblingWorker != worker);

    worker.reset();
    otherWorker.reset();
    CHECK(outboxes.find(world) == nullptr); // the world's generators are gone
    CHECK(outboxes.find(sibling) == siblingWorker);
}

TEST_CASE("world keys cannot collide by moving text between their parts", "[pmmp]") {
    CHECK(fluidUpdateWorldKey("a", "bc", "d", 1) != fluidUpdateWorldKey("ab", "c", "d", 1));
    CHECK(fluidUpdateWorldKey("a", "b", "c", 12) != fluidUpdateWorldKey("a", "b", "c1", 2));
    CHECK(fluidUpdateWorldKey("a", "b", "c", -1) != fluidUpdateWorldKey("a", "b", "c", 1));
    CHECK(fluidUpdateWorldKey("a", "b", "c", 7) == fluidUpdateWorldKey("a", "b", "c", 7));
}

TEST_CASE("workers put while the main thread takes, and nothing is lost or doubled", "[pmmp]") {
    // PocketMine-MP's shape: several generation workers encoding chunks and
    // the main thread taking them as they are populated, all at once.
    FluidUpdateOutboxes outboxes;
    const std::string world = fluidUpdateWorldKey("w", "s", "b", 1);
    const auto main = outboxes.attach(world);
    constexpr int kWorkers = 4;
    constexpr int kChunksEach = 200;
    std::vector<std::thread> workers;
    workers.reserve(kWorkers);
    for (int w = 0; w < kWorkers; ++w) {
        workers.emplace_back([&outboxes, &world, w] {
            const auto outbox = outboxes.attach(world);
            for (int i = 0; i < kChunksEach; ++i) {
                // Chunk (w, i) holds i % 7 + 1 updates.
                outbox->put(
                    w, i,
                    std::vector<FluidUpdate>(static_cast<std::size_t>((i % 7) + 1), at(0, i, 0)));
            }
        });
    }
    std::size_t taken = 0;
    std::vector<bool> seen(static_cast<std::size_t>(kWorkers * kChunksEach), false);
    std::size_t chunksTaken = 0;
    while (chunksTaken < seen.size()) {
        for (int w = 0; w < kWorkers; ++w) {
            for (int i = 0; i < kChunksEach; ++i) {
                const std::vector<FluidUpdate> updates = main->take(w, i);
                if (updates.empty()) {
                    continue;
                }
                const auto at = static_cast<std::size_t>((w * kChunksEach) + i);
                CHECK_FALSE(seen[at]);
                seen[at] = true;
                ++chunksTaken;
                CHECK(updates.size() == static_cast<std::size_t>((i % 7) + 1));
                taken += updates.size();
            }
        }
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    std::size_t expected = 0;
    for (int i = 0; i < kChunksEach; ++i) {
        expected += static_cast<std::size_t>((i % 7) + 1);
    }
    CHECK(taken == expected * kWorkers);
    CHECK(main->chunksHeld() == 0U);
}
