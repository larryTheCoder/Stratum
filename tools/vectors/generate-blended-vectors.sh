#!/usr/bin/env bash
# Stratum — regenerate the legacy blended noise vectors from cubiomes.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
# cubiomes (MIT, Cubitect) is the noise and biome reference named in SPEC §2.
# It is fetched at a pinned commit and never vendored: this repository ships
# the driver, not somebody else's source tree.
set -euo pipefail

# Pinned to the same commit as the noise vectors, deliberately. Moving it is
# a decision, not a side effect of running this script.
readonly CUBIOMES_REPO="https://github.com/Cubitect/cubiomes.git"
readonly CUBIOMES_COMMIT="e61f90580cbdd883214a8054670dacae655e59c0"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
output="${1:-${repo_root}/tests/unit/blended_vectors.inc}"

work_dir="$(mktemp -d)"
trap 'rm -rf "${work_dir}"' EXIT

echo "==> fetching cubiomes at ${CUBIOMES_COMMIT}" >&2
git -C "${work_dir}" init -q cubiomes
git -C "${work_dir}/cubiomes" remote add origin "${CUBIOMES_REPO}"
git -C "${work_dir}/cubiomes" fetch -q --depth 1 origin "${CUBIOMES_COMMIT}"
git -C "${work_dir}/cubiomes" checkout -q FETCH_HEAD

actual="$(git -C "${work_dir}/cubiomes" rev-parse HEAD)"
if [[ "${actual}" != "${CUBIOMES_COMMIT}" ]]; then
    echo "error: expected cubiomes ${CUBIOMES_COMMIT} but got ${actual}" >&2
    exit 1
fi

# -ffp-contract=off for the same reason the engine builds with it (SPEC §5):
# a contracted multiply-add in the oracle would make the vectors describe a
# computation nobody performs, and the difference would then be blamed on us.
echo "==> building the vector driver" >&2
cc -O2 -std=c11 -ffp-contract=off -I"${work_dir}/cubiomes" \
    "${repo_root}/tools/vectors/blended_vectors.c" \
    "${work_dir}/cubiomes/noise.c" \
    "${work_dir}/cubiomes/biomenoise.c" \
    "${work_dir}/cubiomes/biomes.c" \
    "${work_dir}/cubiomes/layers.c" \
    "${work_dir}/cubiomes/util.c" \
    -lm -o "${work_dir}/blended_vectors"

echo "==> writing ${output}" >&2
"${work_dir}/blended_vectors" > "${output}"
echo "==> $(grep -c 'INT64_C' "${output}") vectors emitted" >&2
