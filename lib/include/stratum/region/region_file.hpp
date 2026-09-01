// Stratum — Anvil region (.mca) container reader.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Reads the region container only: the sector table, the per-chunk framing
// and the decompression. What comes back is a chunk's raw NBT payload;
// interpreting that is the NBT reader's job.
//
// This exists for the conformance harness (SPEC §7): `cli diff` compares
// vanilla's region output against ours block-for-block in Java block space.
// A malformed or unsupported file therefore raises FormatError naming what
// was wrong and where. Skipping a chunk that could not be read would turn a
// parity failure into a green run, which SPEC §8 treats as the most severe
// class of bug.
//
// Format reference: the Anvil region format as documented on minecraft.wiki.
// No Mojang code was consulted.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace stratum::region {

/// A region file covers 32x32 chunks, stored in 4 KiB sectors after an
/// 8 KiB header (4 KiB of locations, then 4 KiB of timestamps).
inline constexpr int kChunksPerAxis = 32;
inline constexpr int kChunksPerRegion = kChunksPerAxis * kChunksPerAxis;
inline constexpr std::size_t kSectorBytes = 4096;
inline constexpr std::size_t kHeaderBytes = 2 * kSectorBytes;

/// Raised for anything malformed or unsupported. Always names the region and
/// the chunk involved: a conformance diff that cannot read its input must
/// say so loudly rather than report agreement.
class FormatError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// The compression byte that precedes each chunk's payload.
enum class Compression : std::uint8_t {
    Gzip = 1,
    Zlib = 2,
    None = 3,
    Lz4 = 4,
    Custom = 127,
};

/// A chunk's position in the sector table. A chunk is absent when both
/// fields are zero.
struct ChunkLocation {
    std::uint32_t sectorOffset = 0;
    std::uint8_t sectorCount = 0;

    [[nodiscard]] constexpr bool present() const noexcept {
        return sectorOffset != 0 || sectorCount != 0;
    }
};

class RegionFile {
public:
    /// Reads and validates a region file. Throws FormatError if the header
    /// is malformed or the sector table points outside the file.
    [[nodiscard]] static RegionFile open(const std::filesystem::path& path);

    /// Same, for bytes already in memory. @p name is used in error messages.
    [[nodiscard]] static RegionFile fromBytes(std::vector<std::byte> bytes, std::string name);

    /// Index into the sector table for a chunk at world chunk coordinates.
    /// Uses floorMod, so negative coordinates map the way Java's do — the
    /// landmine that would otherwise mirror the world across the origin.
    [[nodiscard]] static std::size_t indexFor(std::int32_t chunkX, std::int32_t chunkZ) noexcept;

    [[nodiscard]] bool hasChunk(std::int32_t chunkX, std::int32_t chunkZ) const;

    /// Seconds since the epoch, as vanilla recorded them. Zero for a chunk
    /// that is not present.
    [[nodiscard]] std::int32_t timestamp(std::int32_t chunkX, std::int32_t chunkZ) const;

    [[nodiscard]] ChunkLocation location(std::int32_t chunkX, std::int32_t chunkZ) const;

    /// The chunk's decompressed NBT payload. Throws FormatError when the
    /// chunk is absent, its framing is inconsistent, or its compression
    /// scheme is one we do not implement.
    [[nodiscard]] std::vector<std::byte> readChunk(std::int32_t chunkX, std::int32_t chunkZ) const;

    /// How many of the 1024 slots hold a chunk.
    [[nodiscard]] std::size_t chunkCount() const noexcept;

    [[nodiscard]] const std::string& name() const noexcept { return name_; }

private:
    RegionFile(std::vector<std::byte> bytes, std::string name);

    void validateHeader() const;

    std::vector<std::byte> bytes_;
    std::string name_;
};

} // namespace stratum::region
