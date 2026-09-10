// Stratum — NBT writer.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The exact inverse of reader.hpp's read(): serialises a Tag tree to raw NBT
// bytes, uncompressed. Whatever compression a container format wants — zlib
// framing inside a region's chunk, gzip framing for a level.dat — is applied
// separately, the same way read() is always handed already-decompressed
// bytes rather than doing that itself.
//
// Format reference: minecraft.wiki. No Mojang code was consulted.

#pragma once

#include <stratum/nbt/tag.hpp>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace stratum::nbt {

/// Raised when a Tag tree cannot be written — today, only for a root that is
/// not a Compound.
class WriteError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Writes @p root, named @p rootName, as one NBT document. @p root must be a
/// Compound — the format's own rule, the same one read() enforces on the way
/// in — so that write(read(bytes)) and read(write(...)) are genuine inverses
/// of each other rather than agreeing only on well-formed input.
[[nodiscard]] std::vector<std::byte> write(const std::string& rootName, const Tag& root);

} // namespace stratum::nbt
