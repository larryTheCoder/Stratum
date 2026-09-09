#!/usr/bin/env bash
# Stratum — an aquifer probe with sea_level BELOW the lava level.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/aquifer-lowsea-probe.sh --accept-eula [seed]
#
# WHY THIS EXISTS. `kLavaLevel = -54` is a compile-time constant, but
# `lambdaLevel(seaLevel) = min(-54, seaLevel)` is not: it equals `sea_level`
# itself whenever `sea_level < -54`, and every "-54" this project's own
# comments call a LEVEL rather than a threshold is supposed to move with it
# (lattice.hpp). At every sea_level this project has ever generated a world
# with — 32 to 200 — that distinction is invisible, because lambda and
# kLavaLevel happen to coincide. This is the one world shape that pulls them
# apart, and it settles two things this project has carried as open:
#
#   (a) `kPslAbortBelow = -62.0`. It "coincides with kLavaLevel - 8", and that
#       identity is UNPROVEN — an arbitrary constant fits every measurement so
#       far equally well. If the true rule is `lambdaLevel(seaLevel) - 8`, the
#       abort boundary MOVES once sea_level drops below -54; if it is a bare
#       -62, it does not. Both readings were indistinguishable at every
#       sea_level tested before (SPEC records min_y invariance and sea_level
#       in {40,100,128,200} — all above -54).
#
#   (b) The aborting near-surface floor's own guard,
#       `cell.centreY >= kLavaLevel`, returning the literal `kLavaLevel` on
#       failure. One of three corrections the barrier campaign measured but
#       left unverified (SPEC §10 blocker 2): "the `centreY >= -54` conjunct
#       ... is wrong (needs an arm below -74 to see)". This world supplies
#       that arm, AND separates "the comparand should be lambda" from "the
#       return value should be lambda" from "neither" — the return value's
#       ambiguity collapses only because lambda equals sea_level here, which
#       is itself informative: if the observed floor tracks sea_level rather
#       than the literal -54, the return value needs fixing regardless of
#       which name it is given in code.
#
# GROUP A — the abort-threshold bisection. Eleven constant-psl dimensions,
# `preliminary_surface_level` pinned to values around the two live
# candidates: -62 (unmoved, if the constant is bare) and -78 (= sea_level - 8
# = oceanGate here, if the constant tracks lambda — see below for why those
# coincide). `floodedness` is pinned at 0.9, comfortably above BOTH of the
# level rule's gates, so the branch taken depends on nothing but whether the
# scan aborted: not aborted -> the block boundary sits at `sea_level`;
# aborted -> it sits at the ladder, which this sea_level/psl combination
# never places anywhere near -70. Reading the fluid/air transition directly
# off the generated blocks — the same technique `lattice.hpp`'s own header
# describes — needs no analysis code at all, only the boundary height.
#
# ONE STRUCTURAL FACT WORTH RECORDING BEFORE THE PROBE EVEN RUNS: whenever
# `sea_level < -54`, `oceanGate = sea_level - 8` is ALWAYS below -62 (since
# sea_level < -54 implies sea_level - 8 < -62 by arithmetic alone), and under
# the lambda-based hypothesis the abort threshold EQUALS oceanGate exactly
# (`lambdaLevel(seaLevel) - 8 = seaLevel - 8` once seaLevel < -54). So under
# EITHER live hypothesis, entering the depth path (`anchor < oceanGate`)
# forces `aborted = true` unconditionally in a sea_level < -54 world — the
# depth path's own "not aborted, flood via reach" branch is DEAD CODE here.
# That is not a flaw in this probe; it is a real property of the rule in this
# regime, and it is why Group A's floodedness=0.9 design works identically
# whether a given cell happens to be on the depth path or not.
#
# GROUP B — the near-surface floor's guard. One constant-psl=-200 dimension:
# deep enough to abort under any threshold hypothesis by a wide margin, which
# isolates THIS question from group A's. A single column's height profile
# sweeps many different cell centres for free — one cell per ~12-block layer
# — so no analysis code beyond reading the fluid/air boundary per layer is
# needed here either; interpreting WHICH centreY values it covers is what
# `tools/analysis/lowsea-psl-analyze.cpp` (built ad hoc against `lib/`) is
# for.
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
spec="${work}/lowsea.json"

python3 - "${spec}" <<'PY'
import json, sys

SEA = -70
MIN_Y = -192
HEIGHT = 384

NO_SURFACE = {"type": "minecraft:condition",
              "if_true": {"type": "minecraft:y_above",
                          "anchor": {"absolute": 2000},
                          "surface_depth_multiplier": 0, "add_stone_depth": False},
              "then_run": {"type": "minecraft:block",
                           "result_state": {"Name": "minecraft:stone"}}}

def aquifer(name, psl, floodedness):
    return {"name": name, "min_y": MIN_Y, "height": HEIGHT,
            "raw_final_density": {"type": "minecraft:constant", "argument": -1.0},
            "aquifers_enabled": True, "sea_level": SEA,
            "default_fluid": {"Name": "minecraft:water", "Properties": {"level": "0"}},
            "size_vertical": 2, "surface_rule": NO_SURFACE,
            "router": {"barrier": -2.0, "lava": 0.0,
                       "preliminary_surface_level": psl,
                       "fluid_level_floodedness": floodedness,
                       "fluid_level_spread": 0.0}}

spec = []
# Group A: abort-threshold bisection at floodedness 0.9 (comfortably above
# both gates, so the branch taken depends only on whether the scan aborted).
for tag, psl in (
    ("a_hi", -55.0),          # sanity: above both candidates, never aborts
    ("a_62m", -61.99), ("a_62", -62.00), ("a_62p", -62.01),   # classic pin
    ("a_65", -65.0), ("a_70", -70.0), ("a_75", -75.0),         # coarse sweep
    ("a_78m", -77.99), ("a_78", -78.00), ("a_78p", -78.01),    # oceanGate/lambda-8
    ("a_lo", -85.0),          # sanity: below both candidates, always aborts
):
    spec.append(aquifer(tag, psl, 0.9))

# Group B: the near-surface floor's guard, read as a column height profile.
spec.append(aquifer("b_floor", -200.0, 0.5))

json.dump(spec, open(sys.argv[1], "w"))
print(len(spec), "dimensions:", SEA, MIN_Y, HEIGHT, file=sys.stderr)
PY

exec tools/analysis/density-probe.sh \
    $([[ ${accept_eula} -eq 1 ]] && printf -- --accept-eula) \
    --spec "${spec}" --seed "${seed}"
