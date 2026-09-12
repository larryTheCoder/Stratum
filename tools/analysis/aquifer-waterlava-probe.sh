#!/usr/bin/env bash
# Stratum — the band directly above the global lava sea, aimed at Q6.3.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/aquifer-waterlava-probe.sh --accept-eula [seed]
#
# WHY THIS EXISTS. The clean-room spec's Q6.3 (spec/aquifer-spec.md) names an
# exception to the barrier: where the NEAREST source reads water and the
# GLOBAL picker one block below reads lava, the block is water outright — no
# pressure, no barrier noise. `Global` is Q1.2's trivial picker, a function of
# y alone: lava below `min(-54, sea_level)`, the default fluid at and above
# it. And Q2.4 already hands every block BELOW that line to the lava sea
# before the lattice is consulted. Put together, the exception can only ever
# fire on ONE ROW — `y == min(-54, sea_level)` exactly, the first row above
# the sea — which is why no other probe in this project has ever seen it:
# `aquifer-barrier-probe.sh` sets `min_y` -48 specifically to keep the lava
# sea OUT of its world, and every other aquifer probe held the row at its
# uninformative configuration or read the fluid boundary, not the barrier.
#
# WHAT THIS BUILDS. The barrier probe's own configuration — REAL
# `minecraft:aquifer_barrier` / `..._fluid_level_floodedness` /
# `..._fluid_level_spread` noises, `preliminary_surface_level` a constant 96,
# `lava` a constant 0.0 so every source above the sea reads water, a constant
# `raw_final_density` swept across the same three values — but at `min_y`
# -64, so the world REACHES the lava sea and y = -54 is a real row with lava
# beneath it. Three density constants, because Q6.3 sits BEFORE Q6.6's
# `D + s*Π > 0` and so must be independent of `D` if the spec's ordering is
# right; a fourth arm drops `sea_level` to -70 (`min_y` -80) so the row the
# exception fires on has to MOVE with `lambda = min(-54, sea_level)` rather
# than stay pinned at -54 — the same ablation Q1.2's own `verify-by` names.
#
# WHAT IS READ. `tools/analysis/aquifer-waterlava-analyze.cpp` and
# `tests/conformance/vanilla_aquifer_waterlava_test.cpp` walk the rows at and
# just above the sea, call this build's own `aquifer::computeSubstance` end
# to end, and separately call the bare `placesBarrier` the substance decision
# used to fall straight through to — so what is scored is exactly "where the
# OLD logic writes stone and the server does not", block by block, with the
# rows above the sea as a control where the two must agree.
#
# THE OBSIDIAN TRAP IS NOT A PROBLEM HERE, and it is worth saying why: water
# resting on the lava sea turns the LAVA beneath it to obsidian, not the
# water above it to anything, and a barrier block is stone that no fluid tick
# can create or destroy. So `stone` at the row is the aquifer's own barrier
# and nothing else, whatever the fluids did to each other afterwards; the
# analyzer only ever asks stone-or-not.
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
# The spec's NAME is the output directory, and density-probe.sh keeps one
# world per name — so the seed goes into the name, or a second seed would
# overwrite the first (the barrier probe's single `barrier3way` has exactly
# that limitation, and its conformance test reads the manifest to cope).
spec="${work}/waterlava_s${seed}.json"

python3 - "${spec}" <<'PY'
import json, sys

REAL_BARRIER = {"type": "minecraft:noise", "noise": "minecraft:aquifer_barrier",
                "xz_scale": 1.0, "y_scale": 0.5}
REAL_FLOODEDNESS = {"type": "minecraft:noise",
                    "noise": "minecraft:aquifer_fluid_level_floodedness",
                    "xz_scale": 1.0, "y_scale": 0.67}
REAL_SPREAD = {"type": "minecraft:noise", "noise": "minecraft:aquifer_fluid_level_spread",
               "xz_scale": 1.0, "y_scale": 0.7142857142857143}

NO_SURFACE = {"type": "minecraft:condition",
              "if_true": {"type": "minecraft:y_above",
                          "anchor": {"absolute": 2000},
                          "surface_depth_multiplier": 0, "add_stone_depth": False},
              "then_run": {"type": "minecraft:block",
                           "result_state": {"Name": "minecraft:stone"}}}

def aquifer(name, density, sea_level, min_y, height):
    # min_y and height must both be multiples of 16 or the server refuses the
    # dimension type; -54 itself is therefore not a floor a world can have.
    assert min_y % 16 == 0 and height % 16 == 0, (name, min_y, height)
    return {"name": name, "min_y": min_y, "height": height,
            "raw_final_density": {"type": "minecraft:constant", "argument": density},
            "aquifers_enabled": True, "sea_level": sea_level,
            "default_fluid": {"Name": "minecraft:water", "Properties": {"level": "0"}},
            "size_vertical": 2, "surface_rule": NO_SURFACE,
            "router": {"barrier": REAL_BARRIER, "lava": 0.0,
                       "preliminary_surface_level": 96.0,
                       "fluid_level_floodedness": REAL_FLOODEDNESS,
                       "fluid_level_spread": REAL_SPREAD}}

spec = [
    # The lava sea at its shipped height: lambda = -54, the row is y = -54.
    aquifer("sea63_d_neg1_0", -1.0, 63, -64, 384),
    aquifer("sea63_d_neg0_3", -0.3, 63, -64, 384),
    aquifer("sea63_d_neg3_0", -3.0, 63, -64, 384),
    # The sea pulled below -54: lambda = sea_level = -70, and the row must
    # move to y = -70 while y = -54 becomes an ordinary row.
    aquifer("sea70_d_neg1_0", -1.0, -70, -80, 400),
]

json.dump(spec, open(sys.argv[1], "w"))
print(len(spec), "dimensions", file=sys.stderr)
PY

exec tools/analysis/density-probe.sh \
    $([[ ${accept_eula} -eq 1 ]] && printf -- --accept-eula) \
    --spec "${spec}" --seed "${seed}"
