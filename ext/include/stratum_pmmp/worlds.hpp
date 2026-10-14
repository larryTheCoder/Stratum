// Stratum — every world the process generates, as its generators hold it,
// and as PocketMine-MP's main thread pins it from world load to unload.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Plain C++ with no PHP in it: the zend shim holds one process-wide Worlds
// and marshals (`Stratum\Dimension::open`, `pinWorld`, `unpinWorld`,
// `takeFluidUpdates`); everything here is testable without a PHP runtime.
//
// WHY A WORLD IS PINNED. A world's state in this process is its compiled
// dimension (dimension_registry.hpp) and its fluid-update outbox
// (fluid_updates.hpp), and both live exactly as long as something holds
// them. Unpinned, the only holders are generators, one per PocketMine-MP
// worker thread — and PocketMine-MP's memory manager shuts idle workers
// down (its periodic garbage collection), dropping every generator on them.
// With the last one goes the outbox, and with it the fluid updates of every
// chunk generated but not yet populated; and the next generator reads the
// blob from disk again, whatever is in that file by then. So the plugin
// pins each Stratum world on the main thread when it loads and unpins it
// when it unloads (ext/plugin/src/StratumWorlds.php). While pinned:
//
//   * the outbox outlives any number of worker restarts, so no held update
//     is lost to one;
//   * `open` hands every generator the state the pin was made with, without
//     reading the file at all: a worker that restarts generates from what
//     the world loaded with, never from a blob edited or replaced since.
//
// Pins are counted, so two loaded worlds whose options and seed coincide (a
// copied world folder whose options still name the original's blob) do not
// unpin each other.
//
// WHERE A BLOB IS. A world's options name its frozen pipeline by a path,
// and PocketMine-MP stores the options inside the world. Absolute, that
// path pinned the world to the directory it was created in: moved or
// renamed, the server refused it at load, and a moved default world
// stopped the server at startup. So a blob path may be relative — the
// plugin's WorldFactory writes `worlds/<name>/stratum-pipeline.blob` — and
// is then read against the server's data path, which the plugin sets once,
// on the main thread, before any world loads (`setDataPath`). One value for
// the whole process: PocketMine-MP's worker threads are threads of the same
// process, so a generator on any of them reads the same base. An absolute
// path, as every world made before this has, is read as it is. Every
// function here resolves a blob path the same way before it reads, keys,
// pins or takes anything, so a relative path and the absolute one it names
// are the same world.
#pragma once

#include <stratum/world/dimension.hpp>

#include <stratum_pmmp/dimension_registry.hpp>
#include <stratum_pmmp/fluid_updates.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace stratum::pmmp {

/// What PocketMine-MP hands every generator of a world, as the plugin
/// reads it: the frozen pipeline's path (absolute, or relative to the
/// server's data path), the dimension's noise settings and biome source,
/// and the world's seed.
struct WorldOptions {
    std::string blobPath;
    std::string noiseSettings;
    std::string biomeSource;
    std::int64_t seed = 0;

    /// fluidUpdateWorldKey of these options.
    [[nodiscard]] std::string key() const;
};

/// A world as a generator holds it.
struct OpenWorld {
    std::shared_ptr<const world::CompiledDimension> dimension;
    /// Where the generator leaves each chunk's fluid updates.
    std::shared_ptr<FluidUpdateOutbox> fluidUpdates;
};

/// Every world in the process. Safe to use from any thread.
class Worlds {
public:
    /// The directory a relative blob path is read against: the server's
    /// data path. Set on the main thread before any world loads; setting the
    /// same path again changes nothing. Throws std::runtime_error, naming
    /// the path, for one that is not absolute, and for a different one while
    /// any world is pinned — whose relative blob path would then name
    /// another file than the one it was pinned with.
    void setDataPath(const std::filesystem::path& dataPath);

    /// @p blobPath as every function here reads it: an absolute path as it
    /// is, a relative one against the data path, both lexically normal.
    /// Touches no file. Throws std::runtime_error, naming the path, for a
    /// relative one before setDataPath.
    [[nodiscard]] std::filesystem::path resolvePipelinePath(const std::string& blobPath) const;

    /// The world as a generator opens it. Pinned, the state the pin holds,
    /// and the file is not read. Otherwise the blob is read now and the
    /// dimension shared through the registry (dimension_registry.hpp), with
    /// the world's outbox attached. Throws what resolvePipelinePath,
    /// compileForPocketMine and readPipelineFile throw.
    [[nodiscard]] OpenWorld open(const WorldOptions& options);

    /// Pins the world: opens it as an unpinned generator would — reading
    /// the blob now, so every refusal is made here and thrown, and nothing
    /// is pinned — and holds that state until as many unpins as pins. A
    /// world already pinned is counted once more and keeps the state it was
    /// first pinned with; its file is not read again.
    void pin(const WorldOptions& options);

    /// One pin fewer. The state itself goes once neither a pin nor a
    /// generator holds it. Returns false, and does nothing, for a world that
    /// is not pinned — a relative blob path before setDataPath among them,
    /// since nothing could have pinned it.
    bool unpin(const WorldOptions& options);

    /// Whether the world is pinned (false for a relative blob path before
    /// setDataPath).
    [[nodiscard]] bool pinned(const WorldOptions& options) const;

    /// Removes and returns what generators left for chunk (@p chunkX,
    /// @p chunkZ) of the world (FluidUpdateOutbox::take); empty when nothing
    /// holds the world's outbox, as nothing can for a relative blob path
    /// before setDataPath. Opens and compiles nothing.
    [[nodiscard]] std::vector<FluidUpdate>
    takeFluidUpdates(const WorldOptions& options, std::int32_t chunkX, std::int32_t chunkZ);

    /// The registry behind `open`, for tests.
    [[nodiscard]] const DimensionRegistry& dimensions() const noexcept { return dimensions_; }

private:
    struct Pin {
        OpenWorld world;
        std::size_t count = 0;
    };

    /// @p blobPath resolved, or nothing for a relative one before
    /// setDataPath.
    [[nodiscard]] std::optional<std::filesystem::path>
    tryResolve(const std::string& blobPath) const;

    /// @p options with their blob path resolved (resolvePipelinePath).
    [[nodiscard]] WorldOptions resolved(const WorldOptions& options) const;

    /// The same, or nothing for a relative blob path before setDataPath —
    /// which names no world anything could have opened or pinned.
    [[nodiscard]] std::optional<WorldOptions> resolvedIfAny(const WorldOptions& options) const;

    /// Opens @p options, whose blob path is resolved, from the file.
    [[nodiscard]] OpenWorld openFile(const WorldOptions& options, const std::string& key);

    DimensionRegistry dimensions_;
    FluidUpdateOutboxes outboxes_;
    mutable std::mutex pinsMutex_;
    std::map<std::string, Pin> pins_;
    mutable std::mutex dataPathMutex_;
    std::filesystem::path dataPath_;
};

} // namespace stratum::pmmp
