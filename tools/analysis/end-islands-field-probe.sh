#!/usr/bin/env bash
# Stratum — reading `minecraft:end_islands` straight off the server.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/end-islands-field-probe.sh --accept-eula [seed] \
#       [--at CHUNK_X,CHUNK_Z]
#
# THE QUESTION. `end_islands`' CENTRAL term is settled — the End generates
# block-for-block against eight golden regions and against a negative-quadrant
# probe (golden_end_test.cpp). Its OUTER term is not: scored against the
# r.64.0 probe it lands at 87.9% of blocks at seed 0, against an 85.7% base
# rate for predicting pure void, which is to say the island POSITIONS are
# essentially uncorrelated with vanilla's. Something in the outer term — its
# simplex seeding, its `< -0.9` gate, its steepness hash, its cell grid — is
# wrong, and terrain cannot say which: the End's own final_density buries the
# field under `old_blended_noise` and then saturates the surface height
# wherever the field is merely large.
#
# THE READOUT is density-probe.sh's, so the field arrives unburied: the
# dimension's whole final_density is
#
#     K * flat_cache(scale * end_islands) + y_clamped_gradient(+1 .. -1)
#
# and each cell-corner column's terrain height inverts to a reading of
# `end_islands` at that column. See that script's header. `--at` is what makes
# this probe possible at all: the outer term cannot fire within 1024 blocks of
# the origin, and every probe before this one generated at the origin.
#
# WHAT MAKES IT A CONTROL. Each world carries the same function twice, once in
# a dimension declaring `legacy_random_source` and once in one that does not.
# The End is legacy; if the two read the SAME field, then `end_islands` does
# not go through the dimension's declared random source at all, and its seed
# comes from somewhere else — which is itself the answer to half the question.
#
# TWO SCALES per dimension, because the inversion's resolution is
# 2 / height / (K * scale): at scale 1 it resolves the field to 0.015, which
# is 1.9 blocks of island height, and at scale 3 to 0.0050, which is 0.64 —
# but scale 3 clips, since the field's own range is [-0.84, 0.5625]. A reading
# that only survives at one scale is a reading that survived quantisation.
#
# Read back by tools/analysis/end-islands-analyze.cpp. Nothing it writes is
# committed: worlds are Mojang-derived (SPEC §12).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${repo_root}"

accept_eula=0
at="2048,0"
args=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --accept-eula) accept_eula=1; shift ;;
        --at)          at="${2:?--at needs CHUNK_X,CHUNK_Z}"; shift 2 ;;
        *)             args+=("$1"); shift ;;
    esac
done
seed="${args[0]:-42}"

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
# The spec's basename becomes the fixture directory, so the seed and the
# window both go in it: one probe per (seed, window), and neither is
# recoverable from the regions themselves.
spec="${work}/endfield_s${seed}_${at//,/_}.json"

python3 - "${spec}" <<'PY'
import json, sys

ISLANDS = {"type": "minecraft:end_islands"}


def dimension(name, legacy, scale):
    return {"name": name,
            "legacy_random_source": legacy,
            "function": {"type": "minecraft:mul",
                         "argument1": scale,
                         "argument2": ISLANDS}}


spec = [
    dimension("leg_fine", True, 3.0),
    dimension("leg_coarse", True, 1.0),
    dimension("mod_fine", False, 3.0),
    dimension("mod_coarse", False, 1.0),
]
json.dump(spec, open(sys.argv[1], "w"), indent=1)
print(len(spec), "dimensions", file=sys.stderr)
PY

exec tools/analysis/density-probe.sh \
    $([[ ${accept_eula} -eq 1 ]] && printf -- --accept-eula) \
    --spec "${spec}" --seed "${seed}" --origin-chunk "${at%%,*}" "${at##*,}"
