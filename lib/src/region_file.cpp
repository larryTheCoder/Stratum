// Stratum — Anvil region (.mca) container reader.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum/javamath.hpp>
#include <stratum/region/region_file.hpp>

// ZLIB_CONST makes z_stream::next_in a `const Bytef*`, so the input buffer
// can stay const instead of being cast, which -Wcast-qual would reject.
#define ZLIB_CONST
#include <zlib.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace stratum::region {

namespace {

/// Everything in a region file is big-endian, which is not the host order on
/// any platform we target.
[[nodiscard]] std::uint32_t readBigEndian32(const std::byte* data) noexcept {
    return (static_cast<std::uint32_t>(data[0]) << 24U) |
           (static_cast<std::uint32_t>(data[1]) << 16U) |
           (static_cast<std::uint32_t>(data[2]) << 8U) | static_cast<std::uint32_t>(data[3]);
}

[[nodiscard]] std::uint32_t readBigEndian24(const std::byte* data) noexcept {
    return (static_cast<std::uint32_t>(data[0]) << 16U) |
           (static_cast<std::uint32_t>(data[1]) << 8U) | static_cast<std::uint32_t>(data[2]);
}

[[nodiscard]] std::string describe(std::int32_t chunkX, std::int32_t chunkZ) {
    return "chunk (" + std::to_string(chunkX) + ", " + std::to_string(chunkZ) + ")";
}

[[nodiscard]] std::string compressionName(std::uint8_t id) {
    switch (id) {
        case 1:
            return "gzip (1)";
        case 2:
            return "zlib (2)";
        case 3:
            return "uncompressed (3)";
        case 4:
            return "LZ4 (4)";
        case 127:
            return "custom (127)";
        default:
            return "unknown (" + std::to_string(id) + ")";
    }
}

/// Inflates a zlib- or gzip-framed payload. windowBits selects the framing:
/// 15 for zlib, 15 + 16 for gzip.
[[nodiscard]] std::vector<std::byte> inflatePayload(const std::byte* data, std::size_t size,
                                                    int windowBits, const std::string& what) {
    z_stream stream{};
    if (inflateInit2(&stream, windowBits) != Z_OK) {
        throw FormatError(what + ": could not initialise the inflate stream");
    }

    // The stream is owned for the rest of this function; every exit path
    // below runs inflateEnd first.
    stream.next_in = reinterpret_cast<const Bytef*>(data);
    stream.avail_in = static_cast<uInt>(size);

    std::vector<std::byte> output;
    // Chunk NBT inflates to a few hundred KiB; start there and grow.
    std::array<std::byte, 64U * 1024U> buffer{};

    int status = Z_OK;
    do {
        stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
        stream.avail_out = static_cast<uInt>(buffer.size());

        status = ::inflate(&stream, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END && status != Z_BUF_ERROR) {
            const std::string message = stream.msg != nullptr ? stream.msg : "corrupt stream";
            inflateEnd(&stream);
            throw FormatError(what + ": inflate failed (" + message + ")");
        }

        const std::size_t produced = buffer.size() - static_cast<std::size_t>(stream.avail_out);
        output.insert(output.end(), buffer.begin(),
                      buffer.begin() + static_cast<std::ptrdiff_t>(produced));

        if (status == Z_BUF_ERROR && produced == 0U) {
            inflateEnd(&stream);
            throw FormatError(what + ": inflate stalled before the end of the stream");
        }
    } while (status != Z_STREAM_END);

    inflateEnd(&stream);
    return output;
}

} // namespace

RegionFile::RegionFile(std::vector<std::byte> bytes, std::string name)
    : bytes_(std::move(bytes)), name_(std::move(name)) {
    validateHeader();
}

RegionFile RegionFile::open(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw FormatError("cannot open region file: " + path.string());
    }

    stream.seekg(0, std::ios::end);
    const std::streamoff size = stream.tellg();
    if (size < 0) {
        throw FormatError("cannot determine the size of: " + path.string());
    }
    stream.seekg(0, std::ios::beg);

    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (size > 0 &&
        !stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) {
        throw FormatError("short read on region file: " + path.string());
    }

    return RegionFile(std::move(bytes), path.filename().string());
}

RegionFile RegionFile::fromBytes(std::vector<std::byte> bytes, std::string name) {
    return RegionFile(std::move(bytes), std::move(name));
}

std::size_t RegionFile::indexFor(std::int32_t chunkX, std::int32_t chunkZ) noexcept {
    // floorMod, not %: chunk (-1, -1) belongs at index (31, 31) of the
    // region to the north-west, and `%` would put it at (-1, -1).
    const std::int32_t localX = javamath::floorMod(chunkX, kChunksPerAxis);
    const std::int32_t localZ = javamath::floorMod(chunkZ, kChunksPerAxis);
    return static_cast<std::size_t>(localX) +
           (static_cast<std::size_t>(localZ) * static_cast<std::size_t>(kChunksPerAxis));
}

void RegionFile::validateHeader() const {
    // An empty region file is legal: vanilla creates one before any chunk in
    // it has been written.
    if (bytes_.empty()) {
        return;
    }

    if (bytes_.size() < kHeaderBytes) {
        throw FormatError(name_ + ": truncated header, " + std::to_string(bytes_.size()) +
                          " bytes but a region header is " + std::to_string(kHeaderBytes));
    }

    const std::size_t sectorsInFile = bytes_.size() / kSectorBytes;
    for (std::size_t index = 0; index < static_cast<std::size_t>(kChunksPerRegion); ++index) {
        const std::byte* entry = bytes_.data() + (index * 4U);
        const std::uint32_t offset = readBigEndian24(entry);
        const std::uint8_t count = static_cast<std::uint8_t>(entry[3]);

        if (offset == 0U && count == 0U) {
            continue; // absent, which is the common case
        }
        if (offset == 0U || count == 0U) {
            throw FormatError(name_ + ": sector table entry " + std::to_string(index) +
                              " is half-empty (offset " + std::to_string(offset) + ", count " +
                              std::to_string(count) + ")");
        }
        if (offset < 2U) {
            throw FormatError(name_ + ": sector table entry " + std::to_string(index) +
                              " points at sector " + std::to_string(offset) +
                              ", which is inside the header");
        }
        if (static_cast<std::size_t>(offset) + count > sectorsInFile) {
            throw FormatError(name_ + ": sector table entry " + std::to_string(index) + " spans " +
                              std::to_string(offset) + ".." + std::to_string(offset + count) +
                              " but the file holds only " + std::to_string(sectorsInFile) +
                              " sectors");
        }
    }
}

ChunkLocation RegionFile::location(std::int32_t chunkX, std::int32_t chunkZ) const {
    if (bytes_.empty()) {
        return {};
    }
    const std::byte* entry = bytes_.data() + (indexFor(chunkX, chunkZ) * 4U);
    return ChunkLocation{readBigEndian24(entry), static_cast<std::uint8_t>(entry[3])};
}

bool RegionFile::hasChunk(std::int32_t chunkX, std::int32_t chunkZ) const {
    return location(chunkX, chunkZ).present();
}

std::int32_t RegionFile::timestamp(std::int32_t chunkX, std::int32_t chunkZ) const {
    if (bytes_.empty()) {
        return 0;
    }
    const std::byte* entry = bytes_.data() + kSectorBytes + (indexFor(chunkX, chunkZ) * 4U);
    return static_cast<std::int32_t>(readBigEndian32(entry));
}

std::size_t RegionFile::chunkCount() const noexcept {
    if (bytes_.empty()) {
        return 0;
    }
    std::size_t count = 0;
    for (std::size_t index = 0; index < static_cast<std::size_t>(kChunksPerRegion); ++index) {
        const std::byte* entry = bytes_.data() + (index * 4U);
        if (readBigEndian24(entry) != 0U || static_cast<std::uint8_t>(entry[3]) != 0U) {
            ++count;
        }
    }
    return count;
}

std::vector<std::byte> RegionFile::readChunk(std::int32_t chunkX, std::int32_t chunkZ) const {
    const std::string what = name_ + ": " + describe(chunkX, chunkZ);

    const ChunkLocation slot = location(chunkX, chunkZ);
    if (!slot.present()) {
        throw FormatError(what + " is not present in this region");
    }

    const std::size_t start = static_cast<std::size_t>(slot.sectorOffset) * kSectorBytes;
    const std::size_t span = static_cast<std::size_t>(slot.sectorCount) * kSectorBytes;
    // validateHeader() already proved the span is inside the file.
    if (span < 5U) {
        throw FormatError(what + ": sector span is too small to hold a chunk header");
    }

    const std::uint32_t declared = readBigEndian32(bytes_.data() + start);
    if (declared == 0U) {
        throw FormatError(what + ": declares a zero-length payload");
    }
    // The declared length counts the compression byte as well as the payload.
    if (static_cast<std::size_t>(declared) + 4U > span) {
        throw FormatError(what + ": declares " + std::to_string(declared) + " bytes but its " +
                          std::to_string(slot.sectorCount) + " sector(s) hold only " +
                          std::to_string(span - 4U));
    }

    const std::uint8_t rawScheme = static_cast<std::uint8_t>(bytes_[start + 4U]);

    // The high bit means the payload lives beside the region in an .mcc file
    // because it did not fit in 255 sectors.
    if ((rawScheme & 0x80U) != 0U) {
        throw FormatError(what + ": stored externally in an .mcc file (scheme " +
                          compressionName(static_cast<std::uint8_t>(rawScheme & 0x7FU)) +
                          " with the external flag set), which is not supported yet");
    }

    const std::byte* payload = bytes_.data() + start + 5U;
    const std::size_t payloadSize = static_cast<std::size_t>(declared) - 1U;

    switch (static_cast<Compression>(rawScheme)) {
        case Compression::Zlib:
            return inflatePayload(payload, payloadSize, 15, what);
        case Compression::Gzip:
            return inflatePayload(payload, payloadSize, 15 + 16, what);
        case Compression::None:
            return std::vector<std::byte>(payload, payload + payloadSize);
        case Compression::Lz4:
        case Compression::Custom:
        default:
            break;
    }

    throw FormatError(what + ": compression scheme " + compressionName(rawScheme) +
                      " is not supported");
}

} // namespace stratum::region
