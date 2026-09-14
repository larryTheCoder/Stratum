// Stratum — the Nukkit binding's own generation pipeline.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// This is the boundary `ext-nukkit/src/jni_bridge.cpp` wraps in JNI, not the
// JNI surface itself — plain C++, buildable and testable without a JVM.
// Splitting it this way is the same shape `ext/`'s own "marshaling only, no
// generation logic" rule takes for PocketMine-MP: this file calls straight
// into `lib/`'s existing compile-then-fill pipeline
// (`terrain::ChunkFiller`), the same one `tools/analysis/generate-world.cpp`
// already exercises end to end, and adds exactly two things a Java caller
// needs that `lib/` does not provide on its own — a Bedrock biome id per
// cell (via `stratum::mapping`, already built and reused verbatim rather
// than re-derived) and Nukkit's own block id (see `resolveNukkitFullId`'s
// own doc for why that one is a refusal, not an answer, today).
//
// SCOPED TO THE OVERWORLD, matching `tools/analysis/generate-world.cpp`'s
// own scope: `legacy_random_source` (M4) still blocks the density chain for
// the Nether, the End, caves and floating islands, so there is nothing this
// pipeline could compile for them yet. `Pipeline::compile` names the
// dimension in its signature anyway, rather than hard-coding it invisibly,
// so extending this to another dimension is a matter of removing a check in
// `pipeline.cpp`, not changing this interface.
#pragma once

#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/settings/noise_settings.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>

namespace stratum::nukkit {

/// Raised by `resolveNukkitFullId` for a Java block state this build cannot
/// yet place into a Nukkit chunk, and by `Pipeline::compile` for a dimension
/// this pipeline does not support yet. Named, not a crash and not a
/// silently-wrong block (SPEC §8) — `jni_bridge.cpp` turns this into a
/// thrown Java exception rather than letting it cross the JNI boundary as a
/// native fault.
class NukkitError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// One dimension's terrain, compiled once and reused for every chunk and
/// every generation worker thread Nukkit hands it to.
///
/// Immutable after `compile()` — the same invariant `terrain::ChunkFiller`
/// itself already documents and relies on (CLAUDE.md's determinism rules:
/// "Pipeline objects are immutable after compile"). Nukkit's own generation
/// model (confirmed by reading its source, not assumed) gives every async
/// worker thread its own `Generator` instance via a `ThreadLocal`, created
/// once and reused for that thread's whole lifetime — so the natural home
/// for a `Pipeline` is one shared instance per world, constructed when the
/// first `Generator` initializes and read by every thread afterward, not
/// one per thread. `fill()` needs no caller-provided scratch state to do
/// that safely: `terrain::ChunkFiller::fill()` already builds whatever
/// per-call cache it needs internally, and this class's own extra step
/// (the biome quart-grid walk) does the same, mirroring
/// `tools/analysis/generate-world.cpp`'s own per-chunk cache lifetime
/// rather than inventing a different one.
class Pipeline {
public:
    /// Compiles @p dimension from the datapack rooted at @p packDir for
    /// @p worldSeed. @p packDir is the pinned version's own root —
    /// `worldgen/` and `biome_parameters/` as siblings underneath it,
    /// exactly what `tools/fetch-vanilla` leaves behind and what
    /// `tools/analysis/generate-world.cpp` already reads the same way —
    /// not the `worldgen/` tree itself. Throws `NukkitError` naming the
    /// dimension if it is not `minecraft:overworld` — the only one this
    /// pipeline can compile today — and whatever `lib/`'s own loaders throw
    /// (`PackError`, `FillError`, ...) for a malformed or unsupported pack,
    /// unchanged: this layer adds no tolerance `lib/` does not already
    /// have.
    [[nodiscard]] static std::unique_ptr<Pipeline> compile(const std::filesystem::path& packDir,
                                                           const data::ResourceLocation& dimension,
                                                           std::int64_t worldSeed);

    ~Pipeline();
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
    Pipeline(Pipeline&&) = delete;
    Pipeline& operator=(Pipeline&&) = delete;

    /// This dimension's vertical extent, for a caller sizing its own
    /// section-count-aware output buffers before calling `fill()`.
    [[nodiscard]] std::int32_t minY() const noexcept;
    [[nodiscard]] std::int32_t height() const noexcept;

    /// Fills one chunk. Both spans are caller-owned and caller-sized —
    /// `16*16*height()` for `fullBlockIds`, `4*4*(height()/4)` for
    /// `biomeIds` (the quart grid this engine's own biome search already
    /// uses) — so `jni_bridge.cpp` can size them from Java-side arrays
    /// without an extra copy in either direction.
    ///
    /// Order within each span is y-major, matching Nukkit's own
    /// `PalettedBlockStorage` layout (confirmed by reading
    /// `cn.nukkit.level.format.leveldb.structure.LevelDBChunk` and
    /// `cn.nukkit.level.util.PalettedBlockStorage`, not assumed): for
    /// blocks, `index = (ly * 16 + lz) * 16 + lx` within each of
    /// `height() / 16` sections, sections ordered bottom to top; for
    /// biomes, the same shape one quart-section at a time.
    ///
    /// Throws `NukkitError` — naming the block, by its Java identifier —
    /// the first time it meets a block `resolveNukkitFullId` cannot place,
    /// which is every real column right now (see that function's own doc).
    /// The scan order is bottom-to-top, so today that is always the very
    /// first block visited, but that is incidental to the mapping being
    /// entirely unbuilt rather than a guarantee: once real blocks resolve
    /// and only some fail, an exception may leave both output spans
    /// PARTIALLY written, in an unspecified state neither committed to nor
    /// rolled back. A caller must treat any thrown `fill()` as having
    /// produced nothing usable — read the spans only after a call that did
    /// not throw — the same discipline any exception-returning API already
    /// requires, not a special case of this one.
    void fill(std::int32_t chunkX, std::int32_t chunkZ, std::span<std::int32_t> fullBlockIds,
              std::span<std::int32_t> biomeIds) const;

private:
    Pipeline();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Nukkit's own "full id" for one Java block state — `id << DATA_BITS |
/// meta`, `BlockID.java`'s int constants for `id` — or a refusal.
///
/// STILL OPEN (PROGRESS.md's M5 section). The planned resolution (SPEC §9)
/// is `lib/mapping/`'s shared Java → Bedrock blockstate table, fed through
/// Nukkit's own `BlockStateMapping` — which lives on the Java side, so this
/// C++ function is expected to be replaced by a Java-built lookup that
/// `fill()` only indexes. Always throws `NukkitError` today, naming
/// @p block's own identifier — a refusal rather than a guess (SPEC §8).
[[nodiscard]] std::int32_t resolveNukkitFullId(const settings::BlockState& block);

} // namespace stratum::nukkit
