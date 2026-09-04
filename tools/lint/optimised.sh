#!/usr/bin/env bash
# The unit tests, built optimised. Run before pushing.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
# WHY THIS EXISTS. `ctest --preset dev` is a debug build, and a debug build
# forgives undefined behaviour that an optimised one does not. That is not
# hypothetical here: binding a reference into the temporary proxy
# `nlohmann::json::items()` returns passed every debug test on three platforms
# and failed both release legs in CI, and nothing local would have said so.
#
# NOT built with -Werror, unlike CI's release legs, and deliberately: GCC 15
# raises -Wnull-dereference inside nlohmann/json.hpp itself under optimisation,
# which CI's older compiler does not. Failing locally on a third-party header
# would train people to skip this. tools/lint/warnings.sh covers the warning
# set, in debug, where it is clean.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${repo_root}"

build_dir="${1:-build/optimised}"
cmake -B "${build_dir}" --preset release -DSTRATUM_WERROR=OFF >/dev/null
cmake --build "${build_dir}" >/dev/null
ctest --test-dir "${build_dir}" -L unit --output-on-failure
echo "optimised: the unit suite passes with optimisation on"
