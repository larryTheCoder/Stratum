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

# A read loop rather than mapfile: macOS still ships bash 3.2, where
# mapfile does not exist.
files=()
while IFS= read -r file; do
    files+=("${file}")
done < <(find lib cli ext tests \
    \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.inl' \) \
    -not -path '*/_deps/*' 2>/dev/null | sort)

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
