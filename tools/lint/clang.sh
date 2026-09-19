#!/usr/bin/env bash
# The project warning set under Clang, as errors — what CI's clang legs see.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
# Run before pushing. tools/lint/warnings.sh builds with whatever compiler the
# preset picks up, which on this tree is GCC, and GCC's warning set is not
# Clang's. The two disagree exactly where it has already turned CI's clang
# legs red: a defaulted `operator==` over a struct of two floats draws
# -Wfloat-equal from Clang and nothing at all from GCC. Isolated and measured
# here under the project set, clang++ 21.1.8 raised it twice and failed, and
# g++ 15.2.0 compiled the same file silently. Until this script the local
# gates were GCC -Werror, clang-tidy and the optimised run, and not one of
# them compiles the tree with Clang — so nothing local could have said so.
#
# A separate build directory on purpose, for the same reason warnings.sh uses
# one: a tree configured for one compiler cannot be rebuilt with another, and
# the ordinary loop in build/dev stays undisturbed and fast.
#
# The -D flags are re-passed on every run, not only when the directory is
# new, and the cache is then read back. `cmake -B <dir>` over an existing
# cache is a reconfigure, but a CHANGE of CMAKE_CXX_COMPILER makes CMake
# delete that cache and run configure again, and on that forced re-run the
# dev preset's own STRATUM_WERROR=OFF wins over the -D on the command line.
# Measured: the first run of this script against a directory configured for
# GCC came out with clang++ and WITHOUT -Werror, built, and would have
# printed the clean line for a build it did not gate. So after configuring,
# the script checks STRATUM_WERROR in CMakeCache.txt, configures once more if
# it is not ON — the second pass sticks, since the compiler no longer changes
# — and refuses to build if it still is not. A gate that accepts whatever
# cache it finds is a gate that reports success for a build it did not run.
#
# The Clang major need not be CI's. Majors differ in which warnings they
# raise, so a clean run here is evidence, not proof, and CI's clang legs stay
# the authority; the point is that some Clang sees this tree before a push,
# which is exactly what was missing above.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${repo_root}"

CLANGXX="${CLANGXX:-clang++}"
if ! command -v "${CLANGXX}" >/dev/null 2>&1; then
    echo "no ${CLANGXX} found; the Clang warning set was NOT checked. Install" >&2
    echo "clang, or set CLANGXX=/path/to/clang++." >&2
    # 77 is what CTest reads as "skipped", the same convention format.sh and
    # tidy.sh use: a developer without the compiler gets a visible skip rather
    # than a green run that compiled nothing.
    exit 77
fi

version="$("${CLANGXX}" --version | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"

build_dir="${1:-build/clang-check}"
configure() {
    cmake -B "${build_dir}" --preset dev \
        -DCMAKE_CXX_COMPILER="${CLANGXX}" -DSTRATUM_WERROR=ON >/dev/null
}
werror_on() {
    grep -qx 'STRATUM_WERROR:BOOL=ON' "${build_dir}/CMakeCache.txt"
}
configure
# A compiler change wiped the cache and the preset's OFF won; see the header.
werror_on || configure
if ! werror_on; then
    echo "${build_dir} did not take STRATUM_WERROR=ON; delete it and re-run." >&2
    exit 1
fi
cmake --build "${build_dir}"
echo "clang clean: the project set builds with -Werror under clang++ ${version}"
