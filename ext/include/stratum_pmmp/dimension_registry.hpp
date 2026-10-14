// Stratum — frozen pipelines on disk, and the compiled dimensions the whole
// process shares.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Plain C++ with no PHP in it, like chunk_encoder.hpp and fluid_updates.hpp:
// the zend shim holds one process-wide registry (worlds.hpp) and marshals,
// and what is shared, when, and what is refused is testable without a PHP
// runtime, threads included.
//
// SHARED, NOT RE-CHECKED: PocketMine-MP builds one generator per worker
// thread, and a compiled dimension is immutable (SPEC §4.1), so every thread
// opening the same world shares one. What makes two opens "the same" is the
// blob's whole bytes — header and payload, by MD5 — and the dimension's
// options. Not the header's content hash, which covers the payload alone:
// keyed by that, a blob stamped with another engine version, format or
// Minecraft pin, or one whose payload was damaged under an intact header,
// matched a dimension already compiled and was never given to
// `freeze::read`, which is where all of those are refused (SPEC §6). Keyed
// by every byte, a hit means bytes identical to a blob `freeze::read`
// accepted; anything else misses, and is read — and refused — afresh.
//
// The registry holds weak references: a dimension lives as long as some
// thread holds it, and an entry whose dimension has gone is pruned as the
// next one is added, so the map never holds more than the dimensions open
// now plus the one being opened.
#pragma once

#include <stratum/hash/md5.hpp>
#include <stratum/world/dimension.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <tuple>
#include <vector>

namespace stratum::pmmp {

/// The bytes of the frozen pipeline at @p path. Throws std::runtime_error
/// naming the path if it is not a regular file that can be read whole.
/// Its size and its bytes come from one open of the file, so a
/// writePipelineFile to @p path landing meanwhile yields the file it
/// replaced or the new one, whole, and never a refusal.
[[nodiscard]] std::vector<std::byte> readPipelineFile(const std::filesystem::path& path);

/// Writes @p blob to @p path so that a reader finds the file it replaced or
/// the new one, whole, and never a part of either: the bytes go to a fresh
/// file beside it, which is then renamed over @p path. Throws
/// std::runtime_error naming the path, leaving @p path as it was and no
/// partial file behind, if any step fails.
void writePipelineFile(const std::filesystem::path& path, std::span<const std::byte> blob);

/// Compiles the dimension @p noiseSettings / @p biomeSource of the frozen
/// pipeline @p blob for @p worldSeed, refusing — by name, before any chunk
/// is asked for — everything that can be decided here:
///
///   * whatever `freeze::read` refuses: no blob at all, a container format,
///     pipeline engine version or Minecraft pin this build does not
///     reproduce, a content hash that does not match (SPEC §6);
///   * whatever `world::BiomeSource::parse` and
///     `world::CompiledDimension::compile` refuse: a source type this build
///     does not implement, settings or a parameter list the pipeline does
///     not carry, a legacy-random-source dimension, a router entry the
///     interpreter cannot evaluate;
///   * and what PocketMine-MP cannot hold, which would otherwise surface at
///     the first chunk: a geometry outside its 24 sub-chunks
///     (`requireFitsChunk`), and a multi-noise parameter list naming a
///     biome with no Bedrock id. The End's source names only vanilla's own
///     End biomes, every one of which the mapping table carries.
///
/// NOT checked here, because nothing in lib/ can enumerate them: the block
/// states the dimension can place. `mapping::javaBlockStateId` still refuses
/// one it does not know, at the chunk that holds it.
///
/// Every refusal is a std::runtime_error subclass whose message says what is
/// wrong. No refusal depends on @p worldSeed.
[[nodiscard]] std::unique_ptr<const world::CompiledDimension>
compileForPocketMine(std::span<const std::byte> blob, const std::string& noiseSettings,
                     const std::string& biomeSource, std::int64_t worldSeed);

/// One compiled dimension per (blob bytes, noise settings, biome source,
/// seed) for the whole process. Safe to use from any thread; locked while
/// looking up and while compiling, never while a dimension generates.
class DimensionRegistry {
public:
    /// The dimension @p blob compiles to (compileForPocketMine), shared with
    /// every other holder of the same bytes and options: compiled only if
    /// none is held now. Throws what compileForPocketMine throws.
    [[nodiscard]] std::shared_ptr<const world::CompiledDimension>
    open(std::span<const std::byte> blob, const std::string& noiseSettings,
         const std::string& biomeSource, std::int64_t worldSeed);

    /// How many entries the registry holds, expired ones not yet pruned
    /// included. For tests.
    [[nodiscard]] std::size_t entries() const;

private:
    using Key = std::tuple<hash::Md5Digest, std::string, std::string, std::int64_t>;

    mutable std::mutex mutex_;
    std::map<Key, std::weak_ptr<const world::CompiledDimension>> dimensions_;
};

} // namespace stratum::pmmp
