#!/usr/bin/env bash
# Stratum — measure this build's old_blended_noise against deepslate's.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/run-blended-probe.sh [seeds] [build-dir]
#
# Installs the pinned deepslate, compiles the native probe against the built
# library, samples both over the same grids and prints the comparison. Nothing
# it writes is committed; nothing Mojang-derived goes in or comes out.
#
# Forty-odd seeds is where the sd ratio's error bar gets below one part in a
# hundred. Ten will not separate 128 from 131 and should not be reported as
# though it had.
set -euo pipefail

readonly DEEPSLATE_VERSION="0.26.2"
seeds="${1:-41}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${2:-${repo_root}/build/dev}"
cd "${repo_root}"

for tool in node npm python3; do
    command -v "${tool}" >/dev/null 2>&1 || { echo "error: ${tool} is needed" >&2; exit 2; }
done
python3 -c 'import numpy' 2>/dev/null || { echo "error: numpy is needed" >&2; exit 2; }
[[ -d "${build_dir}/lib" ]] || { echo "error: no build at ${build_dir}" >&2; exit 2; }

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

echo "==> installing deepslate@${DEEPSLATE_VERSION}" >&2
(cd "${work}" && npm init -y >/dev/null 2>&1 &&
    npm install --no-audit --no-fund "deepslate@${DEEPSLATE_VERSION}" >/dev/null 2>&1)
installed="$(node -e "console.log(require('${work}/node_modules/deepslate/package.json').version)")"
[[ "${installed}" == "${DEEPSLATE_VERSION}" ]] ||
    { echo "error: got deepslate ${installed}, wanted ${DEEPSLATE_VERSION}" >&2; exit 1; }
cp tools/analysis/blended-probe.mjs "${work}/probe.mjs"

echo "==> compiling the native probe" >&2
"${CXX:-c++}" -std=c++20 -O2 -ffp-contract=off \
    -I lib/include -I "${build_dir}/lib/generated" \
    tools/analysis/blended-probe.cpp -L "${build_dir}/lib" -lstratum_core \
    -o "${work}/ours"

echo "==> sampling ${seeds} seed(s)" >&2
ours=() theirs=()
for ((seed = 0; seed < seeds; ++seed)); do
    (cd "${work}" && node probe.mjs lines smear1 "${seed}") > "${work}/t${seed}.txt"
    "${work}/ours" lines smear1 "${seed}" > "${work}/o${seed}.txt"
    ours+=("${work}/o${seed}.txt"); theirs+=("${work}/t${seed}.txt")
done

echo; echo "=== spectrum: is the octave schedule right? ==="
python3 tools/analysis/blended-compare.py spectra \
    "${ours[@]:0:3}" -- "${theirs[@]:0:3}"

echo; echo "=== scale: what is the normalisation? ==="
python3 tools/analysis/blended-compare.py scale "${ours[@]}" -- "${theirs[@]}"

echo; echo "=== height: how the spread varies with y ==="
(cd "${work}" && node probe.mjs planes overworld 0) > "${work}/tp.txt"
"${work}/ours" planes overworld 0 > "${work}/op.txt"
python3 tools/analysis/blended-compare.py planes "${work}/op.txt" -- "${work}/tp.txt"

# The multiplier is a free parameter in the oracle, so it can simply be swept.
# Multiplier 0 is the no-smearing control; 1 and 16 bracket the range over
# which vanilla's response is flat, which is itself the strongest constraint
# on where the multiplier can possibly enter.
echo; echo "=== smear: what does smear_scale_multiplier actually do? ==="
smear_ours=() smear_theirs=()
for ((seed = 0; seed < ${smear_seeds:-8}; ++seed)); do
    for m in 0 1 16; do
        (cd "${work}" && node probe.mjs columns "${m}" "${seed}") > "${work}/ts${seed}_${m}.txt"
        "${work}/ours" columns "${m}" "${seed}" > "${work}/os${seed}_${m}.txt"
        smear_theirs+=("${work}/ts${seed}_${m}.txt"); smear_ours+=("${work}/os${seed}_${m}.txt")
    done
done
python3 tools/analysis/blended-compare.py smear "${smear_ours[@]}" -- "${smear_theirs[@]}"

# The summary statistics above rank candidates; this one rejects them. Both
# multipliers are shown because 0 is the control: the jumps must be absent
# there and present at 1, or whatever produced them is not the smear.
echo; echo "=== fingerprint: discontinuities at integer y ==="
for m in 0 1; do
    (cd "${work}" && node probe.mjs fine "${m}" 0) > "${work}/fine_t_m${m}.txt"
    "${work}/ours" fine "${m}" 0 > "${work}/fine_o_m${m}.txt"
done
python3 tools/analysis/blended-compare.py fingerprint \
    "${work}/fine_o_m0.txt" "${work}/fine_o_m1.txt" -- \
    "${work}/fine_t_m0.txt" "${work}/fine_t_m1.txt"
