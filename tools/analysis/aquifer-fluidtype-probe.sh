#!/usr/bin/env bash
# Stratum — an aquifer probe isolating the `lava` fluid-type rule's two
# remaining open questions (SPEC §10 MA blocker 4).
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/aquifer-fluidtype-probe.sh --accept-eula [seed]
#
# WHY THIS EXISTS. `fluid_type.hpp` settled the rule's shape, its threshold
# (0.3), its sample pitch (64/40) and its comparison (absolute value) against
# 3125 cells on the `elava` probe arm — but two things were marked rather
# than measured, because every one of those cells came from vanilla's own
# `lava` NOISE, never from a value the probe controls directly:
#
#   (a) Strictness at exactly 0.3: is the gate `magnitude > 0.3` or
#       `magnitude >= 0.3`? Every measured cell's `lava` sat wherever the
#       noise put it, never AT the threshold itself.
#   (b) The level ceiling: bracketed to [-14, -5] and no tighter, because at
#       `preliminary_surface_level` 96 the corpus's own levels skip that
#       range entirely — the SAME psl this probe also uses, deliberately,
#       so `baseLevel`'s own doc (psl 56/80/96/128/160 give topmost levels
#       56/80/96/128/160) is known to apply cleanly here.
#
# A FIRST ATTEMPT AT THIS PROBE GOT THE SHAPE WRONG, and it is worth keeping
# the failure on record. It drove `preliminary_surface_level` far NEGATIVE
# (-200) at the default `sea_level` (63) to force a deep level the way
# aquifer-lowsea-probe.sh's own "b_floor" dimension does — but that
# dimension holds `sea_level` at -70 specifically so psl -200 sits only 130
# blocks below it; here, at sea_level 63, psl -200 sits 263 blocks below
# sea_level - kOceanGateOffset (55), which unconditionally fires the OCEAN
# branch (lattice.hpp) and floods every column to `sea_level` regardless of
# `fluid_level_floodedness` or anything the level ladder would otherwise
# say. Every "strictness" dimension came back 100% water with the level
# pinned at 63 — the level condition alone excluded lava, so the `lava`
# sweep measured nothing. The fix is not a deeper psl; it is to stay OUT of
# the ocean branch entirely (psl 96 > sea_level - 8, so depth is never even
# evaluated) and reach a deep level through `fluid_level_spread` instead,
# which the ladder's own math supports directly.
#
# THE SHARED TRICK, as in the other aquifer-*-probe.sh scripts: `barrier`
# held far enough negative that no barrier ever wins, and
# `fluid_level_floodedness` at 0.5 — strictly between the two gates (0.4
# local, 0.8 sea; lattice.hpp) — so every cell takes the LADDER.
#
# GROUP A — strictness. `fluid_level_spread` fixed at -1.0, which floors to
# offset -12 (spreadOffset's own doc), landing the -20-rung cells at level
# -32 — comfortably inside [-14, -5] from either end whichever the true
# ceiling turns out to be — crossed with a `lava` sweep straddling 0.3 as
# tightly as a double allows: the two doubles adjacent to 0.3 itself
# (`math.nextafter`), 0.3 exactly, and coarser points either side for
# context.
#
# GROUP B — the level ceiling. `lava` held at 0.5 (comfortably past the
# threshold either strictness reading gives, so ONLY the level condition is
# in play) crossed with `fluid_level_spread` swept from -1.0 to 1.0 in
# steps of 0.1 — every achievable offset bucket the ladder's own floor-based
# formula produces, read off whichever cell-y band each dimension's column
# actually carries (a void column spans many cell-y bands at once, so one
# dimension samples several ladder rungs for free).
#
# GROUP D — the level ceiling, PINNED rather than bracketed. Selected with
# `--group d`, and it writes to a DIFFERENT output directory
# (probes/fluidceiling_s<seed>) because density-probe.sh names that directory
# after this spec's basename: running group D under the old name would
# overwrite probes/fluidtype, which still holds the group A/B evidence behind
# the 0.3-strictness result.
#
# WHY GROUPS B AND C COULD NOT REACH -10 OR -9, now arithmetic rather than a
# mystery. The ladder level is `40*floorDiv(centreY,40) + 20 + 3*floorDiv(
# floor(10*spread), 3)`, so for a constant `fluid_level_spread` the offset K
# is uniform world-wide and the reachable levels are a mod-3 lattice pinned to
# the rung: `L = base + K`. Reaching -9 needs `K = -29 - 40*yi` divisible by
# three, i.e. `yi = 1 (mod 3)`. Group C's `c_neg9` picked `yi = 1` — base 60 —
# whose cells are centred in y in [40,80), an entire rung ABOVE the observable
# band. Its -9 sources therefore own no block below their own level and place
# nothing; every cell that DOES own a block in -53..0 lands on base -20 or -60
# (levels -89 / -129), below lambda, hence dry. What is left in the world is
# the global lava sea alone — the "collapse to the lava-sea floor" this probe's
# first pass recorded without explaining. `c_neg10` (base 20, `yi = 0`) is the
# mirror image and works, which is why it produced 68 cells rather than none.
#
# The same arithmetic says the gap is structural: with a real
# `fluid_level_spread` noise (|s| <= 1) the offset is confined to [-12, +9], so
# the reachable uncapped levels are {-72..-51} u {-32..-11} u {8..29} u ... and
# -10/-9 fall in the hole between -11 and +8. No golden and no real-noise world
# can decide this; only a configuration that leaves the ladder does.
#
# So group D reaches -9 by the two routes that are NOT mod-3 constrained:
#
#   arm Q (PRIMARY, the sea branch): `fluid_level_floodedness` 0.9, past the
#     0.8 sea gate, so every cell's level IS `sea_level` exactly. Sweeping
#     `sea_level` -12..-7 walks the level one block at a time straight through
#     the open bracket. psl 96 keeps the ocean branch and the near-surface
#     path out of it, exactly as groups A and B do.
#   arm P (CONTROL, the psl cap): floodedness 0.5 so the cell takes the
#     ladder, `fluid_level_spread` 6.0 so every rung lands far above the cap,
#     and the level is then `floor(psl)` on every cell. `sea_level` -16 puts
#     the ocean gate at -24, below every psl this arm uses, so the ocean
#     branch stays out of it just as psl 96 does at the shipped sea.
#     Arm P exists to catch a ceiling that is really sea-RELATIVE: at the
#     shipped sea 63 a rule `L <= sea_level - 73` is indistinguishable from
#     `L <= -10`, and only a run at a different sea can tell them apart.
#   arm P' (CONTROL, lambda-relative): arm P at `sea_level` -70, which moves
#     lambda to -70 while leaving the absolute level where it was.
#   arm S (CROSS-CHECK): the shipped sea 63 and the LADDER, on the rung the
#     first attempt should have used — base -60 (`yi = -2`, K = +51 from
#     spread 5.2) gives level -9 on cells centred in [-80,-40), which do own
#     blocks in the observable band. Read as a lava/obsidian count above
#     lambda rather than as a body top, because such a source floods its whole
#     territory and leaves no body top at its level.
#   arm R (REPRODUCTION, in-run): spread 0.9, 1.2 and -2.9 reproduce the three
#     readings already on record (-11 LAVA, -8 water, -10 LAVA) inside the
#     same world and the same seed as the new arms.
#
# Nothing this writes is committed: worlds are Mojang-derived (SPEC §12).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${repo_root}"

accept_eula=0
group=abc
args=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --accept-eula) accept_eula=1; shift ;;
        --group) group="${2:?--group needs abc or d}"; shift 2 ;;
        *) args+=("$1"); shift ;;
    esac
done
seed="${args[0]:-42}"
case "${group}" in
    abc|d) ;;
    *) echo "--group must be abc (strictness + the ceiling sweep) or d (the ceiling, pinned)" >&2; exit 2 ;;
esac

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
# density-probe.sh names its output directory after this basename, so the
# group D run MUST NOT be called fluidtype: that would overwrite the group
# A/B dump the 0.3-strictness result rests on.
if [[ "${group}" == "d" ]]; then
    spec="${work}/fluidceiling_s${seed}.json"
else
    spec="${work}/fluidtype.json"
fi

python3 - "${spec}" "${group}" <<'PY'
import json, math, sys

SEA = 63
PSL = 96.0     # matches baseLevel's own doc; comfortably clear of the ocean
               # branch (psl - sea_level = 33 > -kOceanGateOffset).
MIN_Y = -64
HEIGHT = 384

NO_SURFACE = {"type": "minecraft:condition",
              "if_true": {"type": "minecraft:y_above",
                          "anchor": {"absolute": 2000},
                          "surface_depth_multiplier": 0, "add_stone_depth": False},
              "then_run": {"type": "minecraft:block",
                           "result_state": {"Name": "minecraft:stone"}}}

def dim(name, spread, lava, sea=SEA, psl=PSL, flood=0.5, min_y=MIN_Y, height=HEIGHT):
    return {"name": name, "min_y": min_y, "height": height,
            "raw_final_density": {"type": "minecraft:constant", "argument": -1.0},
            "aquifers_enabled": True, "sea_level": sea,
            "default_fluid": {"Name": "minecraft:water", "Properties": {"level": "0"}},
            "size_vertical": 2, "surface_rule": NO_SURFACE,
            "router": {"barrier": -2.0, "lava": lava,
                       "preliminary_surface_level": psl,
                       "fluid_level_floodedness": flood,
                       "fluid_level_spread": spread}}

group = sys.argv[2] if len(sys.argv) > 2 else "abc"
spec = []

if group == "d":
    # Arm Q — the sea branch. floodedness 0.9 clears the 0.8 gate, so the
    # level IS sea_level and the sweep walks it one block at a time through
    # the whole open bracket. Levels: exactly `sea_level` on every cell
    # centred at or above lambda (-54).
    for sea in range(-12, -6):
        spec.append(dim("q_sea_m%d" % -sea, 0.0, 0.5, sea=sea, psl=96.0, flood=0.9))

    # Arm P — the psl cap, at a sea that is not the shipped one. spread 6.0
    # puts every rung 60 above its base, so `min(ladder, cap)` is the cap on
    # every cell and the level is floor(psl) world-wide.
    for psl in range(-12, -6):
        spec.append(dim("p_psl_m%d" % -psl, 6.0, 0.5, sea=-16, psl=float(psl)))

    # Arm P' — the same, at sea_level -70, which moves lambda to -70 and
    # leaves the absolute level alone. min_y follows aquifer-lowsea-probe.sh:
    # a sea below the world floor would make lambda unreachable.
    for psl in (-12, -10, -9, -8):
        spec.append(dim("p70_psl_m%d" % -psl, 6.0, 0.5, sea=-70, psl=float(psl),
                        min_y=-192, height=384))

    # Arm S — the shipped sea and the LADDER, on the rung that actually owns
    # blocks in the observable band (base -60, centres in [-80,-40)).
    for tag, spread in (("s63_m12", 4.9), ("s63_m9", 5.2), ("s63_m6", 5.5)):
        spec.append(dim(tag, spread, 0.5, psl=96.0))

    # Arm R — the three readings already on record, reproduced in this run.
    for tag, spread in (("r_b09", 0.9), ("r_b12", 1.2), ("r_c10", -2.9)):
        spec.append(dim(tag, spread, 0.5, psl=96.0))

    json.dump(spec, open(sys.argv[1], "w"))
    print(len(spec), "dimensions (group D)", file=sys.stderr)
    raise SystemExit(0)

# Group A: strictness at 0.3, level forced deep via spread (-1.0 -> -32 at
# the -20 rung), not via an out-of-range psl.
below = math.nextafter(0.3, -1.0)
above = math.nextafter(0.3, 2.0)
for tag, lava in (
    ("a_020", 0.20), ("a_025", 0.25), ("a_029", 0.29),
    ("a_0299", 0.299), ("a_below", below), ("a_exact", 0.3), ("a_above", above),
    ("a_0301", 0.301), ("a_031", 0.31), ("a_035", 0.35), ("a_040", 0.40),
):
    spec.append(dim(tag, -1.0, lava))

# Group B: the level ceiling, lava held well past threshold either way,
# spread swept across every achievable offset bucket. Extended past the
# +-1.0 a real `fluid_level_spread` noise ever reaches: nothing observed
# with normal spread lands a level in [-14, -5] at all (the -20 rung's own
# reachable range stops at -11, offset +9) — this reads the CODE's own
# comparison boundary further in, not a configuration real terrain can
# produce, exactly the kind of "extreme point pins the constant" probe the
# rest of this corpus already relies on (lattice.hpp's own strictness
# probes go past any per-cell threshold the same way).
s = -1.0
while s <= 2.0001:
    tag = ("b_%.1f" % s).replace("-", "m").replace(".", "_")
    spec.append(dim(tag, round(s, 1), 0.5))
    s += 0.1

# Group B continued: -11 (the -20 rung's own reachable ceiling, spread 0.9)
# came back LAVA, -8 (spread 1.2) came back water — bracketing the true
# ceiling to {-11, -10, -9}. Neither -10 nor -9 is reachable from the -20
# rung at all (its levels are -20 + 3k, which never lands on either); they
# ARE reachable from the rungs whose base is congruent to them mod 3 — base
# 20 (== 2 mod 3, like -10) and base 60 (== 0 mod 3, like -9) — at spread
# values far more extreme than the first sweep went.
spec.append(dim("c_neg10", -2.9, 0.5))  # base 20, offset -30 -> level -10
spec.append(dim("c_neg9", -6.8, 0.5))   # base 60, offset -69 -> level -9

json.dump(spec, open(sys.argv[1], "w"))
print(len(spec), "dimensions", file=sys.stderr)
PY

exec tools/analysis/density-probe.sh \
    $([[ ${accept_eula} -eq 1 ]] && printf -- --accept-eula) \
    --spec "${spec}" --seed "${seed}"
