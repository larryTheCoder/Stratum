#!/usr/bin/env bash
# clang-format check (default) or apply (--fix) over first-party sources.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${repo_root}"

CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"
if ! command -v "${CLANG_FORMAT}" >/dev/null 2>&1; then
    echo "no ${CLANG_FORMAT} found; formatting was NOT checked. Install" >&2
    echo "clang-format, or set CLANG_FORMAT=/path/to/clang-format." >&2
    # 77 is what CTest reads as "skipped" (SKIP_RETURN_CODE, see
    # tests/CMakeLists.txt): a developer without the tool gets a visible skip
    # rather than a green run that checked nothing. In CI it is still a
    # non-zero exit, so the lint job fails, which is what should happen there.
    exit 77
fi

# The VERSION matters as much as the presence. CI pins clang-format to one
# exact release (CLANG_FORMAT_VERSION in .github/workflows/ci.yml, installed
# via pipx in the lint job) because clang-format's line-wrapping decisions
# differ between releases — including between minors of the same major. The
# build-matrix jobs run this same script through `ctest`'s lint.format with
# whatever clang-format the runner image has on PATH, and at f792e64 that
# binary rejected three lines the pinned one accepted, turning seven legs red
# for a disagreement between two formatters rather than a defect in the tree.
# A check by a different version is not a weaker check, it is not a check:
# skip visibly (77) and leave the pinned lint job as the authority. Set
# CLANG_FORMAT=/path/to/the/pinned/binary to run it here.
pinned="$(grep -oE 'CLANG_FORMAT_VERSION: *"[0-9.]+"' .github/workflows/ci.yml | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
actual="$("${CLANG_FORMAT}" --version | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
if [[ -n "${pinned}" && "${actual}" != "${pinned}" ]]; then
    echo "${CLANG_FORMAT} is clang-format ${actual}, but CI pins ${pinned}; formatting was" >&2
    echo "NOT checked, since a different release wraps differently. Install the pin with" >&2
    echo "  pipx install \"clang-format==${pinned}\"" >&2
    echo "or set CLANG_FORMAT=/path/to/clang-format-${pinned}." >&2
    exit 77
fi

# tools/analysis is mostly one-shot investigation code that nothing builds,
# and formatting it wholesale would rewrite a dozen files nobody is touching.
# What IS listed here is the part of it that became a build target: a file the
# project compiles, warns on and tests is a file the project formats too. It
# was added when legacy-seed-analyze.cpp stopped being compiled by hand from a
# comment in its own header (see tools/analysis/CMakeLists.txt) -- until then
# it was outside every gate, and a claim in it about agreeing with the library
# could go stale with nothing to catch it. Add to this list whenever another
# analysis tool is brought under the build.
analysis_targets=(
    tools/analysis/legacy-seed-analyze.cpp
    tools/analysis/legacy-goldens-biome-analyze.cpp
    tools/analysis/legacy-goldens-surface-analyze.cpp
    tools/analysis/legacy-goldens-surface-decoder.hpp
)

# A read loop rather than mapfile: macOS still ships bash 3.2, where
# mapfile does not exist.
files=()
while IFS= read -r file; do
    files+=("${file}")
done < <(find lib cli ext ext-nukkit tests \
    \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.inl' \) \
    -not -path '*/_deps/*' 2>/dev/null | sort; \
    printf '%s\n' "${analysis_targets[@]}")

if [[ ${#files[@]} -eq 0 ]]; then
    echo "no C++ sources to format"
    exit 0
fi

if [[ "${1:-}" == "--fix" ]]; then
    "${CLANG_FORMAT}" -i --style=file "${files[@]}"
    echo "formatted ${#files[@]} file(s)"
else
    "${CLANG_FORMAT}" --dry-run --Werror --style=file "${files[@]}"
    echo "format clean: ${#files[@]} file(s)"
fi
