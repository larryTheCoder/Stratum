#!/usr/bin/env bash
# The project warning set, as errors — the way CI's build legs see it.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
# Run before pushing. `cmake --build --preset dev` uses STRATUM_WERROR=OFF, so
# the warnings are printed and then scroll away; CI sets it ON and the same
# code is a hard failure. That gap has turned main red once, on -Wfloat-equal
# in a test that compared exactly-representable doubles with `==` — legal
# arithmetic, banned operator, and the warning had gone by in an earlier
# incremental build.
#
# A separate build directory on purpose: it does not disturb build/dev, so the
# ordinary loop stays fast and this stays honest.
#
# The pre-push routine is four scripts: this one, tools/lint/clang.sh,
# tools/lint/tidy.sh and tools/lint/optimised.sh — the project warning set as
# errors here, the same set under Clang next, clang-tidy in the third and the
# unit tests built optimised in the fourth. The Clang one is not redundant
# with this one: -Wfloat-equal on a defaulted `operator==` over a struct of
# floats has failed CI's clang legs while this script, in GCC, stayed green —
# GCC does not raise it there at all. `ctest --preset dev` covers formatting
# and the tests in DEBUG, and none of those four.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${repo_root}"

build_dir="${1:-build/werror}"
cmake -B "${build_dir}" --preset dev -DSTRATUM_WERROR=ON >/dev/null
cmake --build "${build_dir}"
echo "warnings clean: the project set builds with -Werror"
