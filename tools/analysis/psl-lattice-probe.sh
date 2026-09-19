#!/usr/bin/env bash
# Stratum — the lattice a varying preliminary_surface_level is sampled on.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/psl-lattice-probe.sh --accept-eula [seed] [second-seed]
#
# WHY THIS EXISTS. `aps-boundary-probe.sh` measured what
# `above_preliminary_surface` compares y against, and left one thing open: a
# `preliminary_surface_level` that VARIES in space does not reach the
# condition per column. Driven by a three-valued `range_choice` (-40 / 0 / 60)
# it comes back as 101 distinct integers with no gaps, so something coarser
# than a column samples it and something blends between those samples. This
# probe family measures that sampling's PITCH and its ANCHOR — and measures
# them SEPARATELY, because a joint fit of two parameters to one field is
# exactly the shape of the two wrong universal claims this question has
# already collected.
#
# THE READOUT is `aps-boundary-probe.sh`'s, unchanged: `raw_final_density` is
# the constant 1 so every column is solid floor to roof, the whole surface
# rule is `{condition: above_preliminary_surface, then_run: diamond_block}`,
# and the band's LOWER EDGE is the condition's boundary at single-block
# resolution in every column. `psl = boundary - surfaceDepth + 8` recovers the
# integer the server used, per column, from the measured boundary law.
#
# THE LEVER IS TRANSLATION, and it is what separates pitch from anchor.
# Every `s_cNN` dimension drives the entry with the SAME field shifted NN
# blocks in x — a `shifted_noise` whose `shift_x` is the constant NN, so the
# shift is exact arithmetic rather than a different noise. Then:
#
#   * if the server reads the entry per column, psl_NN(x, z) == psl_0(x+NN, z)
#     for EVERY NN, because shifting the input shifts the output;
#   * if it samples on a lattice of pitch P, that identity holds exactly when
#     P divides NN and fails otherwise — the field moves, the lattice does
#     not. A staircase in NN, with a denominator, and it assumes nothing at
#     all about how the samples are blended.
#
# The shifts are 0..8, 12, 16 and 32, which separate every pitch in
# {1, 2, 4, 8, 16, 32} from every other by which of them match. `t_cNN` does
# the same in z, so a pitch that differs by axis cannot hide; `t_c00` is
# deliberately a duplicate of `s_c00` and must agree on every column, which is
# the control that the readout itself is deterministic.
#
# THE ANCHOR is then one scan over one parameter at the measured pitch, and
# the joint grid search is kept only as a check on it.
#
# THE OTHER TWO QUESTIONS, each with the family that answers it:
#
#   f_*  Where the FLOOR falls. Arms inside one unit interval (-0.5/+0.5 and
#        -0.25/+0.75) make "blend then floor" and "floor then blend" give
#        wildly different counts of psl == 0: blending first crosses the
#        integer only at the top corner, blending after crosses it halfway
#        (or a quarter of the way) along every ramp. With integer arms the
#        two are indistinguishable, which is why every earlier probe of this
#        entry could not have told them apart.
#   g_*  The field's own geometry, at xz_scale 2 and 0.5. A lattice's ramps
#        are P blocks wide whatever the field does; a per-column read has no
#        ramps at any scale. This is what keeps "pitch" from being a property
#        of the one noise the probe happens to drive it with.
#
# And two controls: `a_3arm` is the exact three-valued field `probes/apsb`'s
# `v_psl` used, so the new world reproduces the old finding rather than
# replacing it, and `c_k0` pins the entry at the constant 0, where the
# boundary must be exactly `surfaceDepth - 8` and any disagreement means the
# harness is measuring itself.
#
# TWO SEEDS. A per-column field fitted at one seed is exactly the kind of
# thing that matches by luck (MEMORY: single-seed coincidence risk), so the
# second spec re-runs the decisive subset at another seed.
#
# AND NEGATIVE COORDINATES. A third spec runs the same subset at chunk -12,
# where the lattice's cell index is the one thing a probe confined to r.0.0
# cannot see: floorDiv and a truncating division agree on every column with
# x, z >= 0 and disagree on most columns below zero.
#
# WHAT EACH SPEC ACTUALLY COVERS, which is not the forceloaded square. Every
# dimension forceloads 8x8 chunks — 128 blocks a side — and the server
# generates a border of chunks around it; the readback reads the copied
# region file, so it gets the square plus as much of that border as shares
# the file. Measured, not assumed:
#
#   psllat, psllat2   forceloaded at chunk 0,   read back x, z in [0, 191]
#                     (36864 columns; the border below zero lies in r.-1.-1,
#                     which is not copied)
#   psllat3           forceloaded at chunk -12, read back x, z in [-256, -1]
#                     (65536 columns; its border lies inside r.-1.-1 too)
#
# Read back by `psl-lattice-analyze.cpp`; scored in
# `tests/conformance/vanilla_psl_lattice_test.cpp`.
#
# Nothing this writes is committed: worlds are Mojang-derived (SPEC §12).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${repo_root}"

accept_eula=0
args=()
for arg in "$@"; do
    case "${arg}" in
        --accept-eula) accept_eula=1 ;;
        *) args+=("${arg}") ;;
    esac
done
seed="${args[0]:-42}"
second_seed="${args[1]:-31337}"

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

python3 - "${work}" <<'PY'
import json, os, sys

work = sys.argv[1]

SOLID = {"type": "minecraft:constant", "argument": 1.0}
MARK = {"type": "minecraft:condition",
        "if_true": {"type": "minecraft:above_preliminary_surface"},
        "then_run": {"type": "minecraft:block",
                     "result_state": {"Name": "minecraft:diamond_block"}}}


def shifted(scale, shift_x, shift_z):
    """The probe noise, translated by an exact number of blocks.

    `shifted_noise` samples at (x*xz_scale + shift_x, ...), so at xz_scale 1 a
    constant shift_x of NN is the same field read NN blocks along. That is the
    whole lever: the FIELD moves and any lattice the server samples it on does
    not."""
    return {"type": "minecraft:shifted_noise", "noise": "stratum:probe_noise",
            "xz_scale": scale, "y_scale": 0.0,
            "shift_x": {"type": "minecraft:constant", "argument": float(shift_x)},
            "shift_y": {"type": "minecraft:constant", "argument": 0.0},
            "shift_z": {"type": "minecraft:constant", "argument": float(shift_z)}}


def two_arm(scale, shift_x, shift_z, low, high, threshold=0.0):
    noise = shifted(scale, shift_x, shift_z)
    return {"type": "minecraft:range_choice", "input": noise,
            "min_inclusive": -1000.0, "max_exclusive": threshold,
            "when_in_range": low, "when_out_of_range": high}


def three_arm(scale, shift_x, shift_z, low, mid, high):
    """The field `probes/apsb`'s `v_psl` drove the entry with, so this world
    reproduces that measurement instead of only replacing it."""
    noise = shifted(scale, shift_x, shift_z)
    return {"type": "minecraft:range_choice", "input": noise,
            "min_inclusive": -1000.0, "max_exclusive": -0.25, "when_in_range": low,
            "when_out_of_range": {"type": "minecraft:range_choice", "input": noise,
                                  "min_inclusive": -1000.0, "max_exclusive": 0.25,
                                  "when_in_range": mid, "when_out_of_range": high}}


def entry(name, psl, **extra):
    probe = {"name": name, "raw_final_density": SOLID, "surface_rule": MARK,
             "router": {"preliminary_surface_level": psl}}
    probe.update(extra)
    return probe


# Which shifts. Chosen so that "which of these match the unshifted world" is a
# different set for every pitch in {1, 2, 4, 8, 16, 32}:
#   P=1 all; P=2 even; P=4 {4,8,12,16,32}; P=8 {8,16,32}; P=16 {16,32};
#   P=32 {32} — and a per-column read matches all of them including 1.
X_SHIFTS = [0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 32]
Z_SHIFTS = [0, 1, 2, 4, 8, 16]

LOW, HIGH = -40.0, 60.0

psllat = [entry("s_c%02d" % c, two_arm(1.0, c, 0, LOW, HIGH)) for c in X_SHIFTS]
psllat += [entry("t_c%02d" % c, two_arm(1.0, 0, c, LOW, HIGH)) for c in Z_SHIFTS]
# Where the floor falls: arms inside one unit interval.
psllat += [entry("f_half", two_arm(1.0, 0, 0, -0.5, 0.5)),
           entry("f_quart", two_arm(1.0, 0, 0, -0.25, 0.75))]
# The field's own geometry, so the pitch is not a property of one noise.
psllat += [entry("g_s2", two_arm(2.0, 0, 0, LOW, HIGH)),
           entry("g_s05", two_arm(0.5, 0, 0, LOW, HIGH))]
# Controls: the old three-valued field, and a constant.
psllat += [entry("a_3arm", three_arm(1.0, 0, 0, -40.0, 0.0, 60.0)),
           entry("c_k0", {"type": "minecraft:constant", "argument": 0.0})]

# The decisive subset at a second seed.
psllat2 = [entry("s_c%02d" % c, two_arm(1.0, c, 0, LOW, HIGH))
           for c in (0, 1, 2, 4, 8, 16)]
psllat2 += [entry("t_c04", two_arm(1.0, 0, 4, LOW, HIGH)),
            entry("f_half", two_arm(1.0, 0, 0, -0.5, 0.5)),
            entry("a_3arm", three_arm(1.0, 0, 0, -40.0, 0.0, 60.0))]

# NEGATIVE coordinates. An anchor at the world origin and an anchor at each
# chunk's own corner are the same statement while x, z >= 0, and floorDiv only
# parts company with a truncating division below zero — so this one forceloads
# the same subset at chunk -12, i.e. blocks -192..-65. The square and the
# border the server generates around it both sit inside r.-1.-1, so the
# readback covers x, z in [-256, -1].
psllat3 = [entry("s_c%02d" % c, two_arm(1.0, c, 0, LOW, HIGH)) for c in (0, 1, 2, 4, 8, 16)]
psllat3 += [entry("t_c04", two_arm(1.0, 0, 4, LOW, HIGH)),
            entry("f_half", two_arm(1.0, 0, 0, -0.5, 0.5)),
            entry("a_3arm", three_arm(1.0, 0, 0, -40.0, 0.0, 60.0)),
            entry("c_k0", {"type": "minecraft:constant", "argument": 0.0})]

for name, spec in (("psllat", psllat), ("psllat2", psllat2), ("psllat3", psllat3)):
    with open(os.path.join(work, name + ".json"), "w") as out:
        json.dump(spec, out)
    print(name, len(spec), "dimensions", file=sys.stderr)
PY

eula=()
[[ ${accept_eula} -eq 1 ]] && eula+=(--accept-eula)

tools/analysis/density-probe.sh "${eula[@]}" --spec "${work}/psllat.json" --seed "${seed}"
tools/analysis/density-probe.sh "${eula[@]}" --spec "${work}/psllat2.json" --seed "${second_seed}"
tools/analysis/density-probe.sh "${eula[@]}" --spec "${work}/psllat3.json" --seed "${seed}" \
    --origin -12
