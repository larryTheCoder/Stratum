#!/usr/bin/env bash
# Stratum — does Q5.8's lava override reach a near-surface sea?
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/aquifer-fluidnear-probe.sh --accept-eula [seed]
#
# WHY THIS EXISTS. `fluidTypeOf` turns a source to lava when its level is at
# most -10 and `|lava| > 0.3` (both measured: aquifer-fluidtype-probe.sh). No
# probe has asked whether that applies to the level rule's two short-circuit
# sea outcomes, because neither can sit at or under -10 at the shipped sea:
#
#   (i)  the near-surface return: a cell close under a submerged surface takes
#        `sea_level` from that surface (`LevelOrigin::NearSurfaceSea`);
#   (ii) the aborted scan's sea: a cell more than twenty blocks above an
#        aborting surface takes `sea_level` (the clean-room spec's Q5.3(a)).
#
# The clean-room spec's Q5.3 returns the global picker's status from both,
# before Q5.8 types anything — so both would be water whatever `lava` says.
# The build typed both lava. Five readings, all separable here:
#
#   R0  the override reaches both (the build before this probe);
#   R1  it reaches neither (Q5.3 read literally);
#   R2  it reaches only sources more than twenty blocks above the surface
#       (Q5.3(a)'s class, which includes every aborted sea);
#   R3  the reverse;
#   R4  the override on these seas keys on the surface (psl <= -10) rather
#       than on the level.
#
# The server takes R1 on every source block of seeds 42 and 31337 (SPEC §11,
# "Nor the lava override"), and the build now does.
#
# THE FIELD. Every router entry is a constant: density -1.0 (no terrain),
# barrier -2.0 (no barrier forms), floodedness -2.0 (nothing floods off the
# near-surface path), spread 0.0. So above lambda the world is the
# near-surface seas and nothing else, and every one of them in a dimension
# carries the same `lava`: R0 and R1 disagree on EVERY fluid block of a lava
# arm. Each lava arm has a lava-0.0 twin, which must agree on water against
# air before any type is scored — the level of a near-surface sea at these
# sea levels has never been measured either.
#
# fluidnear_s<seed>, the near-surface return:
#   nb20   sea -20, psl -40, lava  0.5   mostly within twenty of the surface
#   nb20z  sea -20, psl -40, lava  0.0   its control
#   nb20n  sea -20, psl -40, lava -0.5   the absolute value on this path
#   nb10   sea -10, psl -30, lava  0.5   the ceiling's inclusive edge
#   nb9    sea  -9, psl -29, lava  0.5   above the ceiling: separates R4
#   nab12  sea -12, psl -50, lava  0.5   both classes in balance
#
# fluidnearb_s<seed>, a low sea, the aborted scan, and a positive control:
#   nb40   sea -40, psl -60, lava  0.5   near-surface centres down to -63
#   nb40z  sea -40, psl -60, lava  0.0   its control
#   ab20   sea -20, psl -64, lava  0.5   aborts (-64 < -62); centres above
#                                        -44 take the sea
#   ab20z  sea -20, psl -64, lava  0.0   its control
#   q20    sea -20, psl  96, lava  0.5   floodedness 0.9: the cell's own sea
#                                        branch, lava under every reading
#
# Read by tests/conformance/vanilla_aquifer_fluidnear_test.cpp. Nothing this
# writes is committed: worlds are Mojang-derived (SPEC §12).
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
# Two corpora, one server run each: a frozen world keeps every fluid tick it
# schedules (aquifer-capfloor-probe.sh ran a 10 GB heap out on five
# water-filled dimensions).
spec="${work}/fluidnear_s${seed}.json"
spec_b="${work}/fluidnearb_s${seed}.json"

python3 - "${spec}" "${spec_b}" <<'PY'
import json, sys

MIN_Y = -64
HEIGHT = 384

NO_SURFACE = {"type": "minecraft:condition",
              "if_true": {"type": "minecraft:y_above",
                          "anchor": {"absolute": 2000},
                          "surface_depth_multiplier": 0, "add_stone_depth": False},
              "then_run": {"type": "minecraft:block",
                           "result_state": {"Name": "minecraft:stone"}}}

def dim(name, sea, psl, lava, flood=-2.0):
    return {"name": name, "min_y": MIN_Y, "height": HEIGHT,
            "raw_final_density": {"type": "minecraft:constant", "argument": -1.0},
            "aquifers_enabled": True, "sea_level": sea,
            "default_fluid": {"Name": "minecraft:water", "Properties": {"level": "0"}},
            "size_vertical": 2, "surface_rule": NO_SURFACE,
            "router": {"barrier": -2.0, "lava": lava,
                       "preliminary_surface_level": psl,
                       "fluid_level_floodedness": flood,
                       "fluid_level_spread": 0.0}}

spec = [dim("nb20", -20, -40.0, 0.5), dim("nb20z", -20, -40.0, 0.0),
        dim("nb20n", -20, -40.0, -0.5), dim("nb10", -10, -30.0, 0.5),
        dim("nb9", -9, -29.0, 0.5), dim("nab12", -12, -50.0, 0.5)]
spec_b = [dim("nb40", -40, -60.0, 0.5), dim("nb40z", -40, -60.0, 0.0),
          dim("ab20", -20, -64.0, 0.5), dim("ab20z", -20, -64.0, 0.0),
          dim("q20", -20, 96.0, 0.5, flood=0.9)]

json.dump(spec, open(sys.argv[1], "w"))
json.dump(spec_b, open(sys.argv[2], "w"))
print(len(spec), "+", len(spec_b), "dimensions", file=sys.stderr)
PY

# A frozen world keeps every fluid tick it schedules: at density-probe.sh's
# 6 GB default a fluid-filled run can exhaust the heap. Overridable, as there.
export STRATUM_PROBE_XMX="${STRATUM_PROBE_XMX:-10G}"
for one in "${spec}" "${spec_b}"; do
    tools/analysis/density-probe.sh \
        $([[ ${accept_eula} -eq 1 ]] && printf -- --accept-eula) \
        --spec "${one}" --seed "${seed}"
done
