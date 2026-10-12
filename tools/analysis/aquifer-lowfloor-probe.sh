#!/usr/bin/env bash
# Stratum — an aborted scan's status below a sea under the lava level.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/aquifer-lowfloor-probe.sh --accept-eula [seed]
#
# WHY THIS EXISTS. A cell whose scan of `preliminary_surface_level` aborts (a
# sample below lambda - 8) and that does not take the sea reads a floor:
# `cellLevel` returned lambda for it, typed as the cell's own fluid, on three
# paths — the aborting near-surface floor, an aborted cell off the
# near-surface path (pipeline engine v5) and an aborted cell under the
# deep-dark override (v6). The clean-room spec's Q5.3(b) gives every one of
# them the global picker's status at the submerged surface instead, which
# below lambda is Q1.1's A_lava = (-54, lava). At any sea at or above -54
# those are one level, and no block, barrier or fluid-update mark can tell
# the two types apart (SPEC §11). Below it lambda is `sea_level`, and the
# readings part in blocks: lowsea's `a_lo` showed the near-surface floor
# holding lava to -55 at sea -70, and nothing had measured the other two.
#
# Every dimension holds density -1 (no terrain), vanilla's own barrier noise,
# `lava` 0.0 and spread 0.0; erosion and depth stay at density-probe.sh's 0
# except in lf_dd.
#
#   lf_v5   sea -70; psl -88 where stratum:probe_noise (xz_scale 0.25) is
#           under -0.5, else -20; floodedness 0. A cell whose anchor reads
#           -20 with a -88 in its window aborts OFF the near-surface path
#           (v5's floor); one whose anchor reads -88 is on it (the near-
#           surface floor, or the sea above -68). Nothing else holds fluid.
#   lf_v5w  the same at floodedness 0.6: cells that do not abort take the
#           ladder (water, or lava when centred below -70), so the floors
#           meet water bodies and the barrier's mixed-type constant.
#   lf_dd   the same at floodedness 0.9 under Q5.9's override (erosion -0.5,
#           depth 1.0): cells that do not abort are dry, and the aborted
#           ones are v6's floor.
#   lf_f60  sea -60, psl -75 everywhere: every scan aborts, so this is the
#           near-surface floor alone at a second sea level, where the
#           literal -54 and lambda + 16 (the same number at -70) part.
#   lf_s63  sea 63; psl -70 or 96 on the same field, floodedness 0.9: the
#           control, where every reading of the floor is one level and the
#           case requires them to agree on every block and mark.
#   lfr     the readout: the same field as a +-1 indicator, so terrain height
#           names the arm per column (as aquifer-nsfloor-probe.sh's).
#
# Two of the five aquifer dimensions hold water above lambda (lf_v5w,
# lf_s63), so the server gets the 10 GB heap the water probes do.
#
# The spec's NAME carries the seed (density-probe.sh names its output after
# the spec), so a second seed lands beside the first instead of on it.
#
# Read by tests/conformance/vanilla_aquifer_lowfloor_test.cpp. Nothing this
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
spec="${work}/lowfloor_s${seed}.json"

python3 - "${spec}" <<'PY'
import json, sys

SCALE, THRESHOLD = 0.25, -0.5

REAL_BARRIER = {"type": "minecraft:noise", "noise": "minecraft:aquifer_barrier",
                "xz_scale": 1.0, "y_scale": 0.5}

NO_SURFACE = {"type": "minecraft:condition",
              "if_true": {"type": "minecraft:y_above",
                          "anchor": {"absolute": 2000},
                          "surface_depth_multiplier": 0, "add_stone_depth": False},
              "then_run": {"type": "minecraft:block",
                           "result_state": {"Name": "minecraft:stone"}}}

def two_valued(low, high):
    noise = {"type": "minecraft:noise", "noise": "stratum:probe_noise",
             "xz_scale": SCALE, "y_scale": 0.0}
    return {"type": "minecraft:range_choice", "input": noise,
            "min_inclusive": -1000.0, "max_exclusive": THRESHOLD,
            "when_in_range": low, "when_out_of_range": high}

def aquifer(name, sea, psl, floodedness, min_y=-128, deep_dark=False):
    router = {"barrier": REAL_BARRIER, "lava": 0.0,
              "preliminary_surface_level": psl,
              "fluid_level_floodedness": floodedness,
              "fluid_level_spread": 0.0}
    if deep_dark:
        router.update({"erosion": -0.5, "depth": 1.0})
    return {"name": name, "min_y": min_y, "height": 256,
            "raw_final_density": {"type": "minecraft:constant", "argument": -1.0},
            "aquifers_enabled": True, "sea_level": sea,
            "default_fluid": {"Name": "minecraft:water", "Properties": {"level": "0"}},
            "size_vertical": 2, "surface_rule": NO_SURFACE, "router": router}

spec = [
    aquifer("lf_v5", -70, two_valued(-88.0, -20.0), 0.0),
    aquifer("lf_v5w", -70, two_valued(-88.0, -20.0), 0.6),
    aquifer("lf_dd", -70, two_valued(-88.0, -20.0), 0.9, deep_dark=True),
    aquifer("lf_f60", -60, -75.0, 0.0),
    aquifer("lf_s63", 63, two_valued(-70.0, 96.0), 0.9, min_y=-64),
    {"name": "lfr", "function": two_valued(-1.0, 1.0)},
]

json.dump(spec, open(sys.argv[1], "w"))
print(len(spec), "dimensions", file=sys.stderr)
PY

# A frozen world keeps every fluid tick it schedules: two of these hold water
# above lambda, so the heap is the water probes' (aquifer-nsfloor-probe.sh).
# Overridable, as there.
export STRATUM_PROBE_XMX="${STRATUM_PROBE_XMX:-10G}"
exec tools/analysis/density-probe.sh \
    $([[ ${accept_eula} -eq 1 ]] && printf -- --accept-eula) \
    --spec "${spec}" --seed "${seed}"
