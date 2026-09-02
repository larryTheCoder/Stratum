// Stratum — synthetic region files for tests.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Real region files are Mojang-derived and never committed (SPEC §12), so
// tests frame their own: sector table, per-chunk header, compression.

#pragma once

#include "chunk_builder.hpp"

#include <stratum/region/region_file.hpp>

#define ZLIB_CONST
#include <zlib.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace stratum::test {

[[nodiscard]] inline std::vector<std::byte> deflateBytes(const std::vector<std::byte>& input,
                                                         int windowBits) {
    z_stream stream{};
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, windowBits, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        throw std::runtime_error("deflateInit2 failed");
    }

    std::vector<std::byte> output(deflateBound(&stream, static_cast<uLong>(input.size())) + 32U);
    stream.next_in = reinterpret_cast<const Bytef*>(input.data());
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<uInt>(output.size());

    const int status = deflate(&stream, Z_FINISH);
    const std::size_t produced = output.size() - static_cast<std::size_t>(stream.avail_out);
    deflateEnd(&stream);
    if (status != Z_STREAM_END) {
        throw std::runtime_error("deflate did not finish");
    }
    output.resize(produced);
    return output;
}

inline void writeBigEndian32(std::vector<std::byte>& out, std::size_t at, std::uint32_t value) {
    out[at] = static_cast<std::byte>((value >> 24U) & 0xFFU);
    out[at + 1U] = static_cast<std::byte>((value >> 16U) & 0xFFU);
    out[at + 2U] = static_cast<std::byte>((value >> 8U) & 0xFFU);
    out[at + 3U] = static_cast<std::byte>(value & 0xFFU);
}

/// Builds a syntactically valid region file, one framed chunk at a time.
class RegionBuilder {
public:
    RegionBuilder() : bytes_(region::kHeaderBytes, std::byte{0}) {}

    /// @p payload is the chunk body; @p scheme is the compression byte
    /// written before it, exactly as it appears on disk.
    void addFramed(std::int32_t chunkX, std::int32_t chunkZ, std::uint8_t scheme,
                   const std::vector<std::byte>& payload, std::int32_t timestamp = 1'700'000'000) {
        const std::size_t sectorOffset = bytes_.size() / region::kSectorBytes;
        const auto declared = static_cast<std::uint32_t>(payload.size() + 1U);

        std::vector<std::byte> framed(5U + payload.size());
        writeBigEndian32(framed, 0, declared);
        framed[4] = static_cast<std::byte>(scheme);
        if (!payload.empty()) {
            std::memcpy(framed.data() + 5, payload.data(), payload.size());
        }

        const std::size_t sectors =
            ((framed.size() + region::kSectorBytes) - 1U) / region::kSectorBytes;
        framed.resize(sectors * region::kSectorBytes, std::byte{0});
        bytes_.insert(bytes_.end(), framed.begin(), framed.end());

        const std::size_t index = region::RegionFile::indexFor(chunkX, chunkZ);
        writeBigEndian32(bytes_, index * 4U,
                         (static_cast<std::uint32_t>(sectorOffset) << 8U) |
                             static_cast<std::uint32_t>(sectors));
        writeBigEndian32(bytes_, region::kSectorBytes + (index * 4U),
                         static_cast<std::uint32_t>(timestamp));
    }

    /// zlib-framed, which is what vanilla writes by default.
    void addChunkPayload(std::int32_t chunkX, std::int32_t chunkZ,
                         const std::vector<std::byte>& raw) {
        addFramed(chunkX, chunkZ, 2, deflateBytes(raw, 15));
    }

    /// A chunk built from section specs, framed and compressed.
    void addChunk(std::int32_t chunkX, std::int32_t chunkZ,
                  const std::vector<SectionSpec>& sections) {
        const NbtWriter nbt = buildChunkNbt(chunkX, chunkZ, sections);
        addChunkPayload(chunkX, chunkZ, nbt.bytes());
    }

    /// Overwrites a sector-table entry, for the malformed-file cases.
    void forceLocation(std::int32_t chunkX, std::int32_t chunkZ, std::uint32_t sectorOffset,
                       std::uint8_t sectorCount) {
        const std::size_t index = region::RegionFile::indexFor(chunkX, chunkZ);
        writeBigEndian32(bytes_, index * 4U, (sectorOffset << 8U) | sectorCount);
    }

    [[nodiscard]] std::vector<std::byte>& bytes() noexcept { return bytes_; }

    [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept { return bytes_; }

    [[nodiscard]] region::RegionFile build(std::string name = "r.0.0.mca") const {
        return region::RegionFile::fromBytes(bytes_, std::move(name));
    }

    void writeTo(const std::filesystem::path& path) const {
        std::ofstream out(path, std::ios::binary);
        if (!out) {
            throw std::runtime_error("cannot write " + path.string());
        }
        out.write(reinterpret_cast<const char*>(bytes_.data()),
                  static_cast<std::streamsize>(bytes_.size()));
    }

private:
    std::vector<std::byte> bytes_;
};

} // namespace stratum::test
