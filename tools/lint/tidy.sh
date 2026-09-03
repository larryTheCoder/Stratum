#!/usr/bin/env bash
# clang-tidy over first-party sources, with CI's exact invocation.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
# Not wired into `ctest --preset dev`: it takes minutes where the rest of that
# preset takes seconds, and a dev loop that slow gets skipped. Run it before
# pushing. Format is wired in (lint.format) because it costs a quarter of a
# second; this does not.
#
# The version matters. clang-tidy 18, 19 and 21 each report a different set of
# findings on this tree, so "clean locally" means nothing unless the major
# matches what CI pins. Without root, get the pinned one with:
#
#   apt-get download clang-tidy-18 libclang-cpp18 libllvm18
#   for d in *.deb; do dpkg -x "$d" root; done
#   CLANG_TIDY=$PWD/root/usr/lib/llvm-18/bin/clang-tidy \
#   LD_LIBRARY_PATH=$PWD/root/usr/lib/llvm-18/lib tools/lint/tidy.sh
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${repo_root}"

CLANG_TIDY="${CLANG_TIDY:-clang-tidy}"
if ! command -v "${CLANG_TIDY}" >/dev/null 2>&1; then
    echo "no ${CLANG_TIDY} found; clang-tidy was NOT run. See the header of" >&2
    echo "this script for how to get the pinned version without root." >&2
    exit 77
fi

version="$("${CLANG_TIDY}" --version | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
expected="$(grep -oE 'clang-tidy-[0-9]+' .github/workflows/ci.yml | head -1 | cut -d- -f3)"
if [[ "${version%%.*}" != "${expected}" ]]; then
    echo "warning: this is clang-tidy ${version}, but CI pins major ${expected}." >&2
    echo "         Findings differ between majors; expect CI to disagree." >&2
fi

# Same build directory and same source list CI uses.
cmake --preset dev -B build/tidy -DSTRATUM_BUILD_TESTS=OFF >/dev/null

sources=()
while IFS= read -r file; do
    sources+=("${file}")
done < <(find lib cli -name '*.cpp' -not -path '*/_deps/*' | sort)

if [[ ${#sources[@]} -eq 0 ]]; then
    echo "no first-party sources to analyse"
    exit 0
fi

"${CLANG_TIDY}" -p build/tidy --warnings-as-errors='*' "${sources[@]}"
echo "clang-tidy clean: ${#sources[@]} file(s)"
