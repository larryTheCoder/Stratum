#!/usr/bin/env bash
# Stratum — what floors a source's level at lambda: an aborted scan, or a cap
# below lambda?
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/aquifer-capfloor-probe.sh --accept-eula [seed]
#
# WHY THIS EXISTS. On aquifer-nsfloor-probe.sh's worlds the barrier is exact
# only if every source level below lambda weighs as lambda in its pressure
# term — yet on the water/lava and deep-floor worlds the unfloored ladder is
# exact. The difference between those corpora is the psl scan: every
# sub-lambda level on the near-surface worlds comes from a cell whose scan
# ABORTED (a sample below lambda - 8), and no cell on the others aborts. Two
# readings fit both, and they part only where a cell's scan minimum sits
# between lambda - 8 and lambda — below lambda, but not low enough to abort:
#
#   * the floor follows the abort;
#   * the floor follows the cap (the scan minimum) being below lambda.
#
# So the field here is CONSTANT, at three heights:
#
#   cf58   psl -58: cap below lambda (-54), scan not aborted — the arm that
#          separates the two readings.
#   cf66   psl -66: aborted, so both readings floor (control).
#   cf200  psl 200: neither floors (control, the deep-floor probe's own).
#   cf58d  psl -58 again, at density -0.05: where a small level change flips
#          a verdict, so the two readings' few-block difference shows.
#
# Floodedness is 0.6, strictly between the level rule's two gates, so cells
# below the near-surface band take the LADDER; the spread is the deep-floor
# probe's fast-sampled one, so neighbouring cells sit on different rungs and
# pairs with different levels — the only kind the pressure term weighs —
# are common. The barrier is vanilla's own noise; `lava` is 0.0.
#
# Its conformance case lands with the change it decides (SPEC §11, "The
# aborting near-surface floor is lambda"). Nothing this writes is committed:
# worlds are Mojang-derived (SPEC §12).
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
spec="${work}/capfloor_s${seed}.json"

python3 - "${spec}" <<'PY'
import json, sys

SEA = 63
MIN_Y = -64
HEIGHT = 384

REAL_BARRIER = {"type": "minecraft:noise", "noise": "minecraft:aquifer_barrier",
                "xz_scale": 1.0, "y_scale": 0.5}
FAST_SPREAD = {"type": "minecraft:mul", "argument1": 4.0,
               "argument2": {"type": "minecraft:noise",
                             "noise": "minecraft:aquifer_fluid_level_spread",
                             "xz_scale": 4.0,
                             "y_scale": 0.7142857142857143 * 4.0}}
LADDER_FLOODEDNESS = 0.6

NO_SURFACE = {"type": "minecraft:condition",
              "if_true": {"type": "minecraft:y_above",
                          "anchor": {"absolute": 2000},
                          "surface_depth_multiplier": 0, "add_stone_depth": False},
              "then_run": {"type": "minecraft:block",
                           "result_state": {"Name": "minecraft:stone"}}}

def aquifer(name, psl, density=-1.0):
    return {"name": name, "min_y": MIN_Y, "height": HEIGHT,
            "raw_final_density": {"type": "minecraft:constant", "argument": density},
            "aquifers_enabled": True, "sea_level": SEA,
            "default_fluid": {"Name": "minecraft:water", "Properties": {"level": "0"}},
            "size_vertical": 2, "surface_rule": NO_SURFACE,
            "router": {"barrier": REAL_BARRIER, "lava": 0.0,
                       "preliminary_surface_level": psl,
                       "fluid_level_floodedness": LADDER_FLOODEDNESS,
                       "fluid_level_spread": FAST_SPREAD}}

spec = [aquifer("cf58", -58.0), aquifer("cf66", -66.0), aquifer("cf200", 200.0),
        # The same surface at a density barely below zero, as the deep-floor
        # probe's d005 arm: there a small change in either level of a pair
        # flips the barrier's verdict, which at -1.0 it almost never does.
        aquifer("cf58d", -58.0, -0.05)]

json.dump(spec, open(sys.argv[1], "w"))
print(len(spec), "dimensions", file=sys.stderr)
PY

# A frozen world keeps every fluid tick it schedules, and these worlds are
# water from wall to wall: at density-probe.sh's 6 GB default the server runs
# out of heap before the regions settle. Overridable, as there.
export STRATUM_PROBE_XMX="${STRATUM_PROBE_XMX:-10G}"
exec tools/analysis/density-probe.sh \
    $([[ ${accept_eula} -eq 1 ]] && printf -- --accept-eula) \
    --spec "${spec}" --seed "${seed}"
