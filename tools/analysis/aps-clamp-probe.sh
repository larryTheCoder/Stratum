#!/usr/bin/env bash
# Stratum — does above_preliminary_surface's surface depth carry a BOTTOM CLAMP?
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/aps-clamp-probe.sh --accept-eula [case...]
#
# with no case named it runs all three: s1, s2, s3 (see THE THREE CASES).
#
# THE QUESTION. `above_preliminary_surface`'s boundary is measured, to
# single-block resolution and against 52 probe dimensions, as
#
#     y >= floor(preliminary_surface_level) + surfaceDepth(x, z) - 8
#
# (SPEC §11, scored in tests/conformance/vanilla_above_preliminary_surface_test.cpp).
# What those 52 dimensions could NOT separate is whether the depth carries a
# bottom clamp: `- 8 + surfaceDepth` against `- 8 + max(0, surfaceDepth)`. The
# two agree on every column whose RETURNED depth is >= 0, and the cast
# truncates toward zero, so the whole question is the columns whose RAW depth
# reaches -1. In all 52 probe dimensions, and in all eight golden r.0.0
# regions, there is not one.
#
# WHY THIS PROBE AND NOT THE OVERWORLD. There IS one in vanilla — four, in
# fact: `aps-boundary-analyze sweep` over the eight golden seeds and
# x, z in [-4096, 4096) finds exactly four columns at depth -1, all at seed
# -4172144997902289642, in chunk (142, 117). Reading them in the OVERWORLD
# settles nothing, and that is measured rather than feared: all four are
# `warm_ocean` with the ocean floor at y = 36 and psl = 24, so the one block
# the candidates disagree about, y = psl - 9 = 15, is 21 blocks deep in stone.
# At `surfaceDepth == -1` every arm of the overworld's gated subtree declines
# there --
#
#   * arms 0, 2, 4 and 3.0 need a solid-run depth of 0 (the top of a run);
#   * arm 3.1 -- the whole grass/dirt/gravel/mud family -- is gated by
#     `stone_depth(floor, offset 0, add_surface_depth true)`, whose threshold
#     is `0 + surfaceDepth = -1`, and a run depth is never negative, so that
#     arm is OFF everywhere in such a column whatever the boundary says;
#   * arms 3.2 and 3.3 reach 5 and 29 blocks deep, but only in
#     warm_ocean/beach/snowy_beach and desert; 21 > 5 refutes the first here;
#   * arm 1 is badlands only, and `NOT(hole)` is false because `hole` is
#     exactly `depth <= 0`.
#
# -- so BOTH candidates predict the terrain filler's stone at y = 15, and the
# server placed stone. A confirmed non-result, run and recorded by
# `aps-boundary-analyze window`, not an excuse for not running it.
#
# WHAT THIS PROBE DOES INSTEAD. It removes the gated subtree from the question
# entirely. Each dimension here is solid from the world floor to its roof
# (`raw_final_density` is the constant 1), pins `preliminary_surface_level` to
# a constant, and carries ONE surface rule:
#
#     { condition: above_preliminary_surface, then_run: diamond_block }
#
# so the marker band's LOWER EDGE is the condition's own boundary, at
# single-block resolution, with nothing between the condition and the readout.
# `hole`, `stone_depth`, the biome and the materials tree are all absent. The
# surface-depth field is a function of the WORLD SEED and (x, z) alone, so at
# seed -4172144997902289642 the same four columns carry depth -1 in a probe
# dimension as in the overworld -- and this forceloads the chunk they are in
# rather than the world origin, which is what `--origin-chunk` was added to
# density-probe.sh for.
#
# THE PREDICTION, stated before the run. At a column with surfaceDepth == -1
# and psl pinned to the constant P, the band's lowest painted y is
#     P - 10  if the double is converted with floor (every separating raw here
#             is in (-1.14, -1.00), so floor gives -2),
#     P - 9   if it is truncated toward zero and NOT clamped,
#     P - 8   if it is clamped at 0.
# Three candidates, three different answers, one reading — and outside this
# tail `floor` and `(int)` are the same function, so this is the only place
# the first two are separable at all. At every neighbouring column, whose
# depth is >= 0, all three agree. So the single number this probe returns is
# the lower edge at the separating columns, and it can come back any of the
# three ways.
#
# THE CONTROLS, because one number read at four columns of one dimension is
# how this question has already been got wrong twice:
#   k_p100 / k_p40 / k_p0     three pinned psl values. A fixed offset that
#                             happened to look like a clamp would not track P.
#   k_m20                     a NEGATIVE psl, so the band sits below y = 0 and
#                             any sign handling in the boundary shows up.
#   k_p100_b                  the same as k_p100 in a second dimension, which
#                             separates "the reading is stable" from "one
#                             dimension did something".
#
# THE THREE CASES, because four separating columns at one seed is four
# samples at one seed. `aps-boundary-analyze sweep` over the eight golden
# seeds and x, z in [-16384, 16384) — 1073741824 columns each, 8589934592 in
# all — finds 98 columns at depth -1, on six of the eight seeds, in ten
# distinct regions. Three of those clusters are probed here, each a different
# seed and a different region:
#
#   s1  seed -4172144997902289642, chunk (140, 115), region r.4.3,
#       4 separating columns  (x 2282-2284, z 1879-1880)
#   s2  seed 42,                   chunk (198, 336), region r.6.10,
#       22 separating columns (x 3211-3218, z 5412-5418)
#   s3  seed -9223372036854775808, chunk (420, 856), region r.13.26,
#       23 separating columns (x 6778-6785, z 13744-13752)
#
# 49 separating columns in all, each read in five dimensions: 245 readings
# that the two candidates disagree about, against three independent seeds.
# The five entries are FOUR distinct dimension configurations plus one
# deliberate byte-identical repeat (k_p100_b repeats k_p100), in each of three
# worlds — 15 generated dimensions, 12 distinct (seed, psl) pairs.
#
# Every other column of every window is a control on which the two agree, and
# they are scored too. A window is not the forceloaded 8x8 block: the server
# takes a wider skirt of chunks past the `surface` stage than it takes to
# `full`, and what carries a marker band is every chunk that reached at least
# `carvers` — measured as 16x16 chunks, 65536 columns, for s1 and s2, and
# 16x12 = 49152 for s3, whose settle heuristic stopped four chunk rows short
# on the low-z edge. Measured total across the 15 dimensions: 901120 painted
# columns, 245 separating readings, 900875 controls. Re-measure with
# `aps-boundary-analyze clamp` rather than multiplying 65536 by 15.
#
# Nothing this writes is committed: worlds are Mojang-derived (SPEC §12).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${repo_root}"

accept_eula=0
wanted=()
for arg in "$@"; do
    case "${arg}" in
        --accept-eula) accept_eula=1 ;;
        *) wanted+=("${arg}") ;;
    esac
done
[[ ${#wanted[@]} -gt 0 ]] || wanted=(s1 s2 s3)

# name : seed : origin chunk x : origin chunk z. The seed and the chunk are
# not defaults so much as the answer to "where is there a negative-depth
# column at all" — they come from `aps-boundary-analyze sweep`, and changing
# one without the other measures nothing.
cases=(
    "s1:-4172144997902289642:140:115"
    "s2:42:198:336"
    "s3:-9223372036854775808:420:856"
)

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

eula=()
[[ ${accept_eula} -eq 1 ]] && eula+=(--accept-eula)

for name in "${wanted[@]}"; do
    entry=""
    for candidate in "${cases[@]}"; do
        [[ "${candidate%%:*}" == "${name}" ]] && entry="${candidate}"
    done
    [[ -n "${entry}" ]] || { echo "unknown case: ${name} (have s1 s2 s3)" >&2; exit 2; }
    IFS=':' read -r _ seed origin_chunk_x origin_chunk_z <<< "${entry}"

    env SPEC_OUT="${work}/apsc_${name}.json" python3 - <<'PY'
import json, os

SOLID = {"type": "minecraft:constant", "argument": 1.0}
MARK = {"type": "minecraft:condition",
        "if_true": {"type": "minecraft:above_preliminary_surface"},
        "then_run": {"type": "minecraft:block",
                     "result_state": {"Name": "minecraft:diamond_block"}}}


def entry(name, psl):
    return {"name": name,
            "raw_final_density": SOLID,
            "surface_rule": MARK,
            "router": {"preliminary_surface_level":
                       {"type": "minecraft:constant", "argument": float(psl)}}}


spec = [entry("k_p100", 100), entry("k_p40", 40), entry("k_p0", 0),
        entry("k_m20", -20), entry("k_p100_b", 100)]

with open(os.environ["SPEC_OUT"], "w") as out:
    json.dump(spec, out)
PY

    echo "==> case ${name}: seed ${seed}, origin chunk (${origin_chunk_x}, ${origin_chunk_z})" >&2
    tools/analysis/density-probe.sh "${eula[@]}" --spec "${work}/apsc_${name}.json" \
        --seed "${seed}" --origin-chunk "${origin_chunk_x}" "${origin_chunk_z}"
done
