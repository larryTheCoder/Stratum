// Stratum — reading a whole file in a test.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The one way the tests read a file whole, by its size. The shorter spelling,
// a container built from a pair of std::istreambuf_iterator, is the read CI's
// release legs rejected until afe4566 removed it: GCC 13 at -O2 (ci-release,
// -Werror, ubuntu-24.04) cannot prove the stream buffer non-null through
// istreambuf_iterator::operator++ and reports -Wnull-dereference inside
// <streambuf>, which -Werror makes fatal. GCC 15 and Clang 21 are silent, so
// nothing local catches a new copy of it; reading by size gives the optimiser
// nothing to doubt.
#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <string>

namespace stratum::test {

/// Every byte of @p path, or nothing if it cannot be opened or read in full.
/// The handle is closed before this returns, so the caller may remove the
/// file at once — Windows refuses to delete a file that is still open.
[[nodiscard]] inline std::optional<std::string> readFileBytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::nullopt;
    }
    stream.seekg(0, std::ios::end);
    const std::streamoff size = stream.tellg();
    if (size < 0) {
        return std::nullopt;
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    stream.seekg(0, std::ios::beg);
    stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (stream.gcount() != static_cast<std::streamsize>(bytes.size())) {
        return std::nullopt;
    }
    return bytes;
}

} // namespace stratum::test
