#!/usr/bin/env bash
# Stratum — the probe that reaches Q6.4's `/10` arm, the barrier's FLOOR.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/aquifer-deepfloor-probe.sh --accept-eula [seed]
#
# WHY THIS EXISTS, and why every earlier barrier probe could not do it.
# Q6.4's fourth divisor (`u = (3 + t)/10` for `3 + t <= 0`) went unmeasured
# for four campaigns, recorded each time as "0 uses across the barrier-probe
# seeds". That was read as a world-shape problem and it was not one. Two
# things were in the way, and only the second is about worlds at all.
#
#   1. A GUARD IN THIS BUILD'S OWN CODE. `termFires` used to refuse any pair
#      of sources that READ THE SAME THING at the block (`aFluid == bFluid`),
#      so `levelPressure` was only ever reached by a pair that disagreed.
#      On that domain `t >= 0.5` for every integer `(L_A, L_B, y)` — a
#      disagreement means `min(L) <= y < max(L)`, hence `|h| <= r - 0.5` —
#      so `3 + t >= 3.5 > 0` and the `/10` arm could not fire for ANY input.
#      No probe could have reached past it. The guard has no counterpart in
#      spec/aquifer-spec.md's Q6.4 or Q6.6, and the worlds this script
#      builds are what refuted it: see barrier.hpp's own header.
#
#   2. THE SEA FLATTENS EVERY LEVEL. With the guard gone the `/10` arm is
#      reachable in principle — it needs two sources that both read the SAME
#      fluid with DIFFERENT levels, four or more blocks under the lower one.
#      But `aquifer-barrier-probe.sh`'s worlds hand nearly every flooded cell
#      `sea_level` itself, so neighbouring cells hold the SAME level, `Δ = 0`,
#      and `Π` is zero before any arm is chosen. On `barrier3way` that leaves
#      the arm entered a few hundred times and DECIDING nothing: substituting
#      `/3` for `/10` there flips exactly 0 blocks.
#
# THE ONE KNOB THAT FIXES IT is `fluid_level_floodedness`, pinned to a
# CONSTANT 0.6 — strictly between `kFloodedLocalThreshold` (0.4) and
# `kFloodedSeaThreshold` (0.8), the two gates lattice.hpp measures to a
# ten-thousandth. Below 0.4 a cell is dry; above 0.8 it takes the sea;
# BETWEEN them it takes the LADDER, `40·band + phase + spreadOffset(spread)`,
# which differs from its neighbours by rung and by band. That converts a
# world of identical sea levels into a world of differing ladder levels,
# which is the entire trick. `fluid_level_spread` is kept as vanilla's own
# noise (it is what supplies the rung diversity) but sampled four times as
# fast on the `deep` arms, so adjacent cells land on different rungs rather
# than on smooth gradients.
#
# THE REST IS HELD STILL ON PURPOSE. `lava` stays 0.0 — a lava/water pair
# below both levels takes Q6.4's `kMixedTypePressure` constant and hides the
# level formula entirely, which would defeat the probe. `min_y` -48 clears
# the global picker's unconditional lava floor at `min(-54, sea_level)`, so
# no row belongs to Q2.4 instead of to the barrier. `sea_level` 63 and a
# constant `preliminary_surface_level` (200 on the deep arms, 96 on the
# mild ones) keep every cell off the near-surface and aborting branches.
# `raw_final_density` is a spatial constant swept across four values,
# because the arm's firing threshold moves with `|D|` and the small
# magnitudes are where a negative `u` can still cross it.
#
# THE CONTROL ARM (`ctlreal_d03`) restores REAL floodedness and the
# unscaled spread, changing nothing else. It reproduces `barrier3way`'s own
# numbers exactly, which is what says the recipe distorts only the level
# diversity it was built to create and not the rest of the lattice.
#
# `tools/analysis/aquifer-deepfloor-analyze.cpp` reads the result.
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

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
spec="${work}/aqdeep.json"

python3 - "${spec}" <<'PY'
import json, sys

SEA = 63
MIN_Y = -48
HEIGHT = 320

REAL_BARRIER = {"type": "minecraft:noise", "noise": "minecraft:aquifer_barrier",
                "xz_scale": 1.0, "y_scale": 0.5}
REAL_FLOODEDNESS = {"type": "minecraft:noise",
                    "noise": "minecraft:aquifer_fluid_level_floodedness",
                    "xz_scale": 1.0, "y_scale": 0.67}
REAL_SPREAD = {"type": "minecraft:noise", "noise": "minecraft:aquifer_fluid_level_spread",
               "xz_scale": 1.0, "y_scale": 0.7142857142857143}

# The same noise sampled four times as fast, then scaled by four: adjacent
# cells land on DIFFERENT rungs of the ladder rather than on a smooth
# gradient, which is what makes a both-fluid pair with Δ != 0 common.
FAST_SPREAD = {"type": "minecraft:mul", "argument1": 4.0,
               "argument2": {"type": "minecraft:noise",
                             "noise": "minecraft:aquifer_fluid_level_spread",
                             "xz_scale": 4.0,
                             "y_scale": 0.7142857142857143 * 4.0}}

# Strictly between kFloodedLocalThreshold (0.4) and kFloodedSeaThreshold
# (0.8), so every cell takes the LADDER rather than the sea or nothing.
LADDER_FLOODEDNESS = 0.6

NO_SURFACE = {"type": "minecraft:condition",
              "if_true": {"type": "minecraft:y_above",
                          "anchor": {"absolute": 2000},
                          "surface_depth_multiplier": 0, "add_stone_depth": False},
              "then_run": {"type": "minecraft:block",
                           "result_state": {"Name": "minecraft:stone"}}}

def aquifer(name, density, psl, floodedness, spread):
    return {"name": name, "min_y": MIN_Y, "height": HEIGHT,
            "raw_final_density": {"type": "minecraft:constant", "argument": density},
            "aquifers_enabled": True, "sea_level": SEA,
            "default_fluid": {"Name": "minecraft:water", "Properties": {"level": "0"}},
            "size_vertical": 2, "surface_rule": NO_SURFACE,
            "router": {"barrier": REAL_BARRIER, "lava": 0.0,
                       "preliminary_surface_level": psl,
                       "fluid_level_floodedness": floodedness,
                       "fluid_level_spread": spread}}

def deep(name, density):
    return aquifer(name, density, 200.0, LADDER_FLOODEDNESS, FAST_SPREAD)

def mild(name, density):
    return aquifer(name, density, 96.0, LADDER_FLOODEDNESS, REAL_SPREAD)

spec = [
    # The four that create the /10 population, across the densities whose
    # firing thresholds differ most.
    deep("deep_d005", -0.05),
    deep("deep_d01", -0.1),
    deep("deep_d03", -0.3),
    deep("deep_d10", -1.0),
    # The ladder without the fast spread: fewer deep pairs, and a check that
    # the rescaling is not itself doing the work.
    mild("mild_d01", -0.1),
    mild("mild_d03", -0.3),
    # The control: vanilla's own floodedness and spread, nothing else moved.
    aquifer("ctlreal_d03", -0.3, 96.0, REAL_FLOODEDNESS, REAL_SPREAD),
]

json.dump(spec, open(sys.argv[1], "w"))
print(len(spec), "dimensions", file=sys.stderr)
PY

exec tools/analysis/density-probe.sh \
    $([[ ${accept_eula} -eq 1 ]] && printf -- --accept-eula) \
    --spec "${spec}" --seed "${seed}"
