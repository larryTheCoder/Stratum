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
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${repo_root}"

build_dir="${1:-build/werror}"
cmake -B "${build_dir}" --preset dev -DSTRATUM_WERROR=ON >/dev/null
cmake --build "${build_dir}"
echo "warnings clean: the project set builds with -Werror"
