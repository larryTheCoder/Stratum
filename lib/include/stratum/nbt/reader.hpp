// Stratum — NBT reader.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Parses uncompressed NBT bytes, as produced by
// stratum::region::RegionFile::readChunk or by inflating a .nbt file.
// Format reference: minecraft.wiki. No Mojang code was consulted.
//
// The reader is deliberately strict. It parses files written by another
// implementation, so every length is checked against what is actually left
// in the buffer before anything is allocated, and anything unrecognised is
// an error naming the byte offset. A parser that guessed its way past a
// damaged region would let the conformance harness report parity it never
// verified (SPEC §8).

#pragma once

#include <stratum/nbt/tag.hpp>

#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>

namespace stratum::nbt {

/// Raised for malformed input. The message always carries the byte offset.
class ParseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct ReadLimits {
    /// Guards against a corrupt file whose nesting would exhaust the stack.
    /// Vanilla's own limit is 512; chunk NBT nests only a handful deep.
    std::size_t maxDepth = 512;
};

struct Document {
    /// The root tag's name. Empty in every file vanilla writes, but part of
    /// the format rather than something to assume away.
    std::string rootName;
    Tag root;
    /// How many bytes the root consumed. Callers that expect the buffer to
    /// hold exactly one document can compare this against its size.
    std::size_t bytesConsumed = 0;
};

/// Reads one named root tag. The root must be a compound, which is what the
/// file format specifies.
[[nodiscard]] Document read(std::span<const std::byte> bytes, const ReadLimits& limits = {});

} // namespace stratum::nbt
