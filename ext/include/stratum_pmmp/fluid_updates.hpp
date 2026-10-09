// Stratum — fluid updates, carried from PocketMine-MP's generation workers to
// the thread that can schedule them.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Plain C++ with no PHP in it, like chunk_encoder.hpp: the zend shim holds
// one process-wide FluidUpdateOutboxes and marshals, and everything about the
// hand-over is testable without a PHP runtime.
//
// WHY THIS EXISTS. A Java server keeps a generated chunk's fluid updates in
// its `PostProcessing` lists and ticks them once the chunk loads; that is
// what makes aquifer water and lava flow (spec Q8.1, SPEC §11's Q8 entry).
// PocketMine-MP has no such list and no way to carry one with a chunk: a
// generator runs on a worker thread with nothing but a `ChunkManager`, and
// what comes back to the main thread is `FastChunkSerializer::
// serializeTerrain()` — sub-chunks and the populated flag, nothing else
// (read from PocketMine-MP 5.44.4's source, SPEC §11's M5 entries). Only the
// main thread's `World::scheduleDelayedBlockUpdate()` can make a liquid
// tick. The one thing every thread of the process shares is this extension,
// so the worker leaves the positions here when it encodes a chunk and the
// plugin takes them when PocketMine-MP announces the chunk populated.
//
// One lock per chunk, taken after generation and never during it — the same
// shape as the zend module's registry of compiled dimensions — so nothing
// here touches generation's hot path (CLAUDE.md's determinism rules).
#pragma once

#include <stratum/terrain/filler.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace stratum::pmmp {

using FluidUpdate = terrain::ChunkBuffer::FluidUpdate;

/// One world's fluid updates, chunk by chunk, from the moment a worker
/// encodes the chunk until the main thread takes them. Safe to use from any
/// thread.
class FluidUpdateOutbox {
public:
    /// Holds @p updates for chunk (@p chunkX, @p chunkZ), replacing anything
    /// already held for it: a chunk generated again (PocketMine-MP retries a
    /// population whose neighbours changed under it) generates the same
    /// updates, and must not be scheduled twice. An empty @p updates holds
    /// nothing, so a chunk without fluid costs no entry.
    void put(std::int32_t chunkX, std::int32_t chunkZ, std::vector<FluidUpdate> updates);

    /// Removes and returns what is held for chunk (@p chunkX, @p chunkZ), in
    /// the order it was put; empty when nothing is.
    [[nodiscard]] std::vector<FluidUpdate> take(std::int32_t chunkX, std::int32_t chunkZ);

    /// How many chunks currently hold updates.
    [[nodiscard]] std::size_t chunksHeld() const;

private:
    mutable std::mutex mutex_;
    std::map<std::pair<std::int32_t, std::int32_t>, std::vector<FluidUpdate>> held_;
};

/// Every world's outbox in the process, keyed by a string naming the world.
///
/// An outbox lives exactly as long as something attached to it: the zend
/// module attaches one to every `Stratum\Dimension` a generator opens, so a
/// world's outbox — and any updates of chunks generated but never populated
/// — goes when PocketMine-MP drops the world's generators at unload.
/// `find()` never creates one: the main thread asking about a world no
/// worker has opened gets nothing rather than an outbox nobody will fill.
class FluidUpdateOutboxes {
public:
    /// The outbox for @p world, created if nothing holds one.
    [[nodiscard]] std::shared_ptr<FluidUpdateOutbox> attach(const std::string& world);

    /// The outbox for @p world while anything holds it; nullptr otherwise.
    [[nodiscard]] std::shared_ptr<FluidUpdateOutbox> find(const std::string& world) const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::weak_ptr<FluidUpdateOutbox>> outboxes_;
};

/// The key the zend module files a world's outbox under: the generator
/// options PocketMine-MP hands every generator of the world, and its seed,
/// joined so that no two distinct tuples meet. Deliberately NOT the blob's
/// content hash the compiled dimension is shared under: two worlds frozen
/// from one pack with one seed share a compiled dimension, but each has its
/// own blob path (`WorldFactory` writes one into each world's folder), so
/// each gets its own outbox and neither takes the other's updates.
[[nodiscard]] std::string fluidUpdateWorldKey(const std::string& blobPath,
                                              const std::string& noiseSettings,
                                              const std::string& biomeParameterList,
                                              std::int64_t seed);

} // namespace stratum::pmmp
