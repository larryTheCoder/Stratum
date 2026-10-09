#!/usr/bin/env bash
# Stratum — regenerate the vectors taken from deepslate.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/vectors/generate-deepslate-vectors.sh [blended-output] [aquifer-output] \
#       [legacy-noise-output]
#
# Three files from one pinned install: old_blended_noise's vectors
# (deepslate_vectors.mjs), the aquifer's random source
# (deepslate_aquifer_vectors.mjs), and what deepslate builds for a named noise
# under legacy_random_source (deepslate_legacy_noise_vectors.mjs). Each output
# defaults to its committed path under tests/unit.
#
# deepslate (MIT, Misode) is installed from npm at a pinned version, run, and
# thrown away. Its source is never read and never vendored: this repository
# ships the drivers, not somebody else's code. See each driver's header for the
# provenance rule this sits under, and for how deepslate was checked — against
# the golden regions for its terrain, and against the server's probe corpora
# for its aquifer, which it does NOT reproduce — before anything was taken
# from it. The third file is not taken as vanilla's at all: it records what
# deepslate does, so that a claim about deepslate is checked against deepslate.
set -euo pipefail

# Pinned deliberately. A different version could change the vectors, and the
# vectors are what a candidate implementation is judged against.
readonly DEEPSLATE_VERSION="0.26.2"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
output="${1:-${repo_root}/tests/unit/deepslate_vectors.inc}"
aquifer_output="${2:-${repo_root}/tests/unit/deepslate_aquifer_vectors.inc}"
legacy_output="${3:-${repo_root}/tests/unit/deepslate_legacy_noise_vectors.inc}"

if ! command -v node >/dev/null 2>&1 || ! command -v npm >/dev/null 2>&1; then
    echo "error: node and npm are needed to run deepslate" >&2
    exit 2
fi

work_dir="$(mktemp -d)"
trap 'rm -rf "${work_dir}"' EXIT

echo "==> installing deepslate@${DEEPSLATE_VERSION}" >&2
(cd "${work_dir}" && npm init -y >/dev/null 2>&1 &&
    npm install --no-audit --no-fund "deepslate@${DEEPSLATE_VERSION}" >/dev/null 2>&1)

installed="$(node -e "console.log(require('${work_dir}/node_modules/deepslate/package.json').version)")"
if [[ "${installed}" != "${DEEPSLATE_VERSION}" ]]; then
    echo "error: expected deepslate ${DEEPSLATE_VERSION} but got ${installed}" >&2
    exit 1
fi

echo "==> writing ${output}" >&2
cp "${repo_root}/tools/vectors/deepslate_vectors.mjs" "${work_dir}/driver.mjs"
(cd "${work_dir}" && node driver.mjs) > "${output}"
echo "==> $(grep -c 'INT64_C' "${output}") vectors emitted" >&2

echo "==> writing ${aquifer_output}" >&2
cp "${repo_root}/tools/vectors/deepslate_aquifer_vectors.mjs" "${work_dir}/aquifer_driver.mjs"
(cd "${work_dir}" && node aquifer_driver.mjs) > "${aquifer_output}"
echo "==> $(grep -c 'INT64_C' "${aquifer_output}") vectors emitted" >&2

echo "==> writing ${legacy_output}" >&2
cp "${repo_root}/tools/vectors/deepslate_legacy_noise_vectors.mjs" "${work_dir}/legacy_driver.mjs"
(cd "${work_dir}" && node legacy_driver.mjs) > "${legacy_output}"
echo "==> $(grep -c 'INT64_C' "${legacy_output}") vectors emitted" >&2
