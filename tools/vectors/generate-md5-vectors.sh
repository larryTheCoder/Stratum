#!/usr/bin/env bash
# Stratum — MD5 known-answer vectors, from md5sum.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
# md5sum (coreutils) is an independent implementation, which is exactly what
# a known-answer vector needs. The inputs cover RFC 1321's own test suite,
# the padding boundaries (55, 56, 63, 64, 65 bytes — where the tail spills
# into a second block), and the octave names vanilla salts its noise with.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
output="${1:-${repo_root}/tests/unit/md5_vectors.inc}"

digest_of() {
    if command -v md5sum >/dev/null 2>&1; then
        printf '%s' "$1" | md5sum | cut -d' ' -f1
    else
        printf '%s' "$1" | md5 -q   # macOS
    fi
}

inputs=(
    ""
    "a"
    "abc"
    "message digest"
    "abcdefghijklmnopqrstuvwxyz"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
    "12345678901234567890123456789012345678901234567890123456789012345678901234567890"
)

# Padding boundaries: one byte either side of every block edge that matters.
for length in 54 55 56 57 63 64 65 119 120 127 128; do
    inputs+=("$(printf 'x%.0s' $(seq 1 "${length}"))")
done

# Every octave salt vanilla uses.
for octave in $(seq -12 0); do
    inputs+=("octave_${octave}")
done

# Names shaped like the resource locations later derivations will hash.
inputs+=("minecraft:overworld" "minecraft:terrain" "minecraft:aquifer_barrier" "minecraft:ore_veininess")

{
    echo "// GENERATED FILE — DO NOT EDIT BY HAND."
    echo "//"
    echo "// MD5 known-answer vectors produced by md5sum, an independent"
    echo "// implementation. Regenerate with tools/vectors/generate-md5-vectors.sh."
    echo
    echo "#include <array>"
    echo "#include <string_view>"
    echo
    echo "// clang-format off"
    echo
    echo "struct Md5Vector {"
    echo "    std::string_view input;"
    echo "    std::string_view digest;"
    echo "};"
    echo
    echo "constexpr auto kMd5Vectors = std::to_array<Md5Vector>({"
    for input in "${inputs[@]}"; do
        printf '    {"%s", "%s"},\n' "${input}" "$(digest_of "${input}")"
    done
    echo "});"
    echo
    echo "// clang-format on"
} > "${output}"

echo "==> wrote ${output} ($(grep -c '^    {' "${output}") vectors)" >&2
