#!/usr/bin/env bash
# Stratum — the lava override's level ceiling, pinned and shown absolute.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/aquifer-fluidceiling-probe.sh --accept-eula [seed]
#
# WHY THIS EXISTS. `fluidTypeOf` turns a source to lava when `|lava| > 0.3`
# and its level is at most -10 (fluid_type.hpp). aquifer-fluidtype-probe.sh
# settled the 0.3 strictness and read -11 and -10 as lava and -8 as water,
# but no ladder configuration it tried put a source at exactly -9: with a
# constant `fluid_level_spread` the ladder's levels are a mod-3 lattice
# pinned to the rung, and its -9 rung sat a whole band above the observable
# one (fluid_type.hpp's header has the arithmetic). This probe reaches -9 by
# the two routes that lattice does not constrain, and asks the further
# question a single sea level cannot: is the ceiling an absolute constant, or
# relative to `sea_level` (or to lambda, min(-54, sea_level))? At the shipped
# sea 63 a rule `L <= sea_level - 73` is indistinguishable from `L <= -10`.
#
# Every router entry is a constant, the density is -1 (no terrain), `lava` is
# 0.5 (past the threshold under either strictness), the barrier -2.0 (no
# barrier noise ever wins), so each dimension's whole fluid body above lambda
# is ONE source level and ONE type, and the server's block says which. Three
# corpora, one server run each:
#
#   fluidceiling_s<seed>, arm Q, the sea branch: floodedness 0.9 is past the
#     0.8 sea gate, so every cell's level IS `sea_level`, and psl 96 keeps
#     the ocean branch and the near-surface path out of it. `sea_level` -12
#     to -7 walks the level through the bracket one block at a time:
#     q_sea_m12 .. q_sea_m7.
#   fluidceilingp_s<seed>, arm P, the psl cap at `sea_level` -16: floodedness
#     0.5 takes the ladder, spread 6.0 lifts every rung far above the cap, so
#     the level is floor(psl) on every cell; psl -12 to -7 walks it:
#     p_psl_m12 .. p_psl_m7. The ocean gate (sea - 8 = -24) is below every
#     psl here, so the ocean branch stays out of it as psl 96 does in arm Q.
#   fluidceilingl_s<seed>, arm P', arm P at `sea_level` -70, which moves
#     lambda to -70 and leaves the absolute level where it was: p70_psl_m12,
#     _m10, _m9, _m8. min_y -192 keeps lambda inside the world.
#
# A sea-relative ceiling would move arm Q's transition off the bracket
# entirely (the level IS the sea) and arm P's to -89; a lambda-relative one
# would move arm P''s to -26. On record (SPEC §11, "The level ceiling, pinned
# at -10"), seeds 42, 7 and 999, every dimension 16384 of 16384 columns with
# 0 of the other fluid: -12/-11/-10 lava and -9/-8/-7 water on Q and P,
# -12/-10 lava and -9/-8 water on P'.
#
# These arms were aquifer-fluidtype-probe.sh's group D, run as one 22-
# dimension world. Two of its arms are not carried: R reproduced three
# ladder readings that probe's own fluidtype corpus already holds, entry for
# entry at the same seed (b_0_9, b_1_2, c_neg10); S read -9 off the ladder at
# sea 63 only indirectly, as a lava count, in worlds of water from wall to
# wall.
#
# THE HEAP. A frozen world keeps every fluid tick it schedules, so no run
# here holds more than three dimensions with water above lambda (the water
# probes' limit is four; aquifer-capfloor-probe.sh ran 10 GB out on five),
# and the heap is theirs: 10 GB, overridable as in density-probe.sh.
#
# The spec names carry the seed (density-probe.sh names its output after the
# spec), so a second seed lands beside the first. Read by
# tests/conformance/vanilla_aquifer_fluid_ceiling_test.cpp. Nothing this
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
spec_q="${work}/fluidceiling_s${seed}.json"
spec_p="${work}/fluidceilingp_s${seed}.json"
spec_l="${work}/fluidceilingl_s${seed}.json"

python3 - "${spec_q}" "${spec_p}" "${spec_l}" <<'PY'
import json, sys

LAVA = 0.5     # past 0.3 under either strictness: only the level decides

NO_SURFACE = {"type": "minecraft:condition",
              "if_true": {"type": "minecraft:y_above",
                          "anchor": {"absolute": 2000},
                          "surface_depth_multiplier": 0, "add_stone_depth": False},
              "then_run": {"type": "minecraft:block",
                           "result_state": {"Name": "minecraft:stone"}}}

def dim(name, sea, psl, flood, spread, min_y=-64, height=384):
    return {"name": name, "min_y": min_y, "height": height,
            "raw_final_density": {"type": "minecraft:constant", "argument": -1.0},
            "aquifers_enabled": True, "sea_level": sea,
            "default_fluid": {"Name": "minecraft:water", "Properties": {"level": "0"}},
            "size_vertical": 2, "surface_rule": NO_SURFACE,
            "router": {"barrier": -2.0, "lava": LAVA,
                       "preliminary_surface_level": psl,
                       "fluid_level_floodedness": flood,
                       "fluid_level_spread": spread}}

# Arm Q: the level is sea_level.
arm_q = [dim("q_sea_m%d" % -sea, sea, 96.0, 0.9, 0.0) for sea in range(-12, -6)]
# Arm P: the level is floor(psl), at sea -16.
arm_p = [dim("p_psl_m%d" % -psl, -16, float(psl), 0.5, 6.0) for psl in range(-12, -6)]
# Arm P': the same at sea -70, lambda -70.
arm_l = [dim("p70_psl_m%d" % -psl, -70, float(psl), 0.5, 6.0, min_y=-192)
         for psl in (-12, -10, -9, -8)]

for path, arm in zip(sys.argv[1:4], (arm_q, arm_p, arm_l)):
    json.dump(arm, open(path, "w"))
print(len(arm_q), "+", len(arm_p), "+", len(arm_l), "dimensions", file=sys.stderr)
PY

export STRATUM_PROBE_XMX="${STRATUM_PROBE_XMX:-10G}"
for one in "${spec_q}" "${spec_p}" "${spec_l}"; do
    tools/analysis/density-probe.sh \
        $([[ ${accept_eula} -eq 1 ]] && printf -- --accept-eula) \
        --spec "${one}" --seed "${seed}"
done
