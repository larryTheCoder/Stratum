#!/usr/bin/env bash
# Stratum — a barrier probe with REAL barrier/floodedness/spread noise, aimed
# at MA blocker 3: the third source (SPEC §10).
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/aquifer-barrier-probe.sh --accept-eula [seed]
#
# WHY THIS EXISTS. `placesBarrier` (barrier.hpp) is exact on the nearest TWO
# sources — 251,658,240 blocks across 40 dimensions and six seeds, no known
# residual — but about 13% of the server's real barriers come from a THIRD
# source it cannot see at all. The clean-room spec's Q6.6 names the
# replacement shape (three ranked sources, an additive caller density `D`,
# and a pressure function `Π` read off Q6.4's own divisors), and this
# project's own algebraic check — not a probe, pure arithmetic — already
# closed part of it for free: at D = -1 (every Stratum aquifer probe's own
# density constant so far), Q6.4's formula with its stated divisors (1.5/2.5
# for the near-fluid branch, 3 for the near-air branch) reproduces the
# CURRENT, server-confirmed two-source rule EXACTLY over an exhaustive sweep
# of 39150 (Δ, y, separation, barrier) combinations — 0 mismatches. See
# lib/include/stratum/aquifer/barrier.hpp for the write-up.
#
# WHAT THAT CHECK COULD NOT REACH is the "/10" divisor branch
# (`3 + t <= 0`, only reachable when a source's own level sits far enough
# from the block that the near-air geometry runs deeply negative) and the
# whole three-way contribution itself — a two-source-only sweep can never
# produce a genuine third-source junction. Both need a real one.
#
# UNLIKE EVERY OTHER AQUIFER PROBE IN THIS PROJECT, `barrier`,
# `fluid_level_floodedness` and `fluid_level_spread` are NOT overridden with
# a constant or a synthetic field here — they are the REAL
# `minecraft:aquifer_barrier` / `minecraft:aquifer_fluid_level_floodedness` /
# `minecraft:aquifer_fluid_level_spread` noises, copied verbatim from the
# vanilla overworld's own `noise_router` (`tools/fetch-vanilla`'s own output,
# read but never committed — SPEC §12). Real noise is what makes real
# three-way junctions: a synthetic field built for one question (as the MA
# blocker 2 probes were) has no reason to produce the dense, irregular
# cell-to-cell variation vanilla's own noise does. `preliminary_surface_level`
# stays a constant 96 (comfortably above every cell's ocean gate at
# `sea_level` 63, so every cell takes the ordinary, non-aborting branch MA
# blockers 1 and 2 already closed) and `lava` stays 0.0 (every fluid reads as
# water, so this probe is blind to Q6.4's separate "one reads lava, the other
# water -> Π = 2.0" branch on purpose — that is a distinct, smaller question
# worth its own probe rather than folding in here).
#
# `raw_final_density` is a spatial CONSTANT, swept across three values so the
# additive-D shape (already "0 violations in 1053963 pairwise checks on two
# seeds" per barrier.hpp) gets exercised again on real three-way junctions
# specifically, not just pairs. `min_y` stays at -48 rather than this
# project's usual -64, clearing the global fluid picker's unconditional lava
# floor at `min(-54, sea_level)` entirely — this probe is about the barrier,
# not about fluid TYPE, and a lava contact would reintroduce the exact
# obsidian-conversion trap the MA blocker 2 analyzer's own header records.
#
# `tools/analysis/aquifer-barrier-analyze.cpp` reads the result: for every
# stone/water/air block, it ranks the four nearest sources (selection.hpp,
# already 0.99993 against the server), evaluates barrier/floodedness/spread
# through the SAME real noises via this build's own density::Interpreter
# (not a hand-rolled replica), and scores the two-source-only prediction
# against the three-source Q6.6 prediction on the same blocks.
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
spec="${work}/barrier3way.json"

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

NO_SURFACE = {"type": "minecraft:condition",
              "if_true": {"type": "minecraft:y_above",
                          "anchor": {"absolute": 2000},
                          "surface_depth_multiplier": 0, "add_stone_depth": False},
              "then_run": {"type": "minecraft:block",
                           "result_state": {"Name": "minecraft:stone"}}}

def aquifer(name, density):
    return {"name": name, "min_y": MIN_Y, "height": HEIGHT,
            "raw_final_density": {"type": "minecraft:constant", "argument": density},
            "aquifers_enabled": True, "sea_level": SEA,
            "default_fluid": {"Name": "minecraft:water", "Properties": {"level": "0"}},
            "size_vertical": 2, "surface_rule": NO_SURFACE,
            "router": {"barrier": REAL_BARRIER, "lava": 0.0,
                       "preliminary_surface_level": 96.0,
                       "fluid_level_floodedness": REAL_FLOODEDNESS,
                       "fluid_level_spread": REAL_SPREAD}}

spec = [
    aquifer("d_neg1_0", -1.0),
    aquifer("d_neg0_3", -0.3),
    aquifer("d_neg3_0", -3.0),
]

json.dump(spec, open(sys.argv[1], "w"))
print(len(spec), "dimensions", file=sys.stderr)
PY

exec tools/analysis/density-probe.sh \
    $([[ ${accept_eula} -eq 1 ]] && printf -- --accept-eula) \
    --spec "${spec}" --seed "${seed}"
