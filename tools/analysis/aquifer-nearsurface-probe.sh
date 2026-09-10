#!/usr/bin/env bash
# Stratum — an aquifer probe isolating cellFluidLevel's two remaining
# unverified corrections (SPEC §10 MA blocker 2).
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/aquifer-nearsurface-probe.sh --accept-eula [seed]
#
# WHY THIS EXISTS. The `lambda` correction to the aborting near-surface
# floor's comparand and return value already LANDED and is confirmed by the
# `sea_level < -54` world (aquifer-lowsea-probe.sh). But that world holds
# `preliminary_surface_level` CONSTANT, which makes `PslRead::gate` and
# `PslRead::cap` the same number by construction (readPreliminarySurface:
# `cap` only differs from `gate` when a sample AFTER the one that armed the
# abort keeps lowering `whole` while `prefix` sits frozen) — so it could not
# tell apart the floor branch's own comparand, `cell.surface.cap +
# kNearSurfaceFloorOffset`, from the same expression written with `gate`
# instead. Separately, the depth path's and the ocean branch's own sea
# outcomes both read `!cell.surface.aborted` as a precondition
# (aquifer_lattice.cpp) and neither has been tested with `aborted` actually
# true while everything else says "flood to the sea".
#
# ONE FIELD ANSWERS BOTH. `preliminary_surface_level` is driven by the same
# three-armed `range_choice` over noise that aquifer-psl-probe.sh already
# validated (HIGH above the ocean gate, MID below it but above the abort
# threshold, LOW below the abort threshold) — the shape that project's own
# report already found leaves "a prefix minimum differing from the
# whole-window minimum on most" aborting cells, which is exactly gate != cap.
# `fluid_level_floodedness` is held at a CONSTANT 0.9, comfortably past both
# of the level rule's gates (0.4, 0.8), so the only thing left to vary a
# cell's outcome is whichever `aborted`-gated branch it falls into.
#
# WHAT EACH SUBSET TESTS, all read by
# tools/analysis/aquifer-nearsurface-analyze.cpp off the SAME world:
#
#   (a) Near-surface floor, cap vs gate. Cells that enter the near-surface
#       early return (`gate < oceanGate && gate - centreY < 4`) with
#       `aborted` true and `gate != cap`. The analyzer calls the real
#       `cellFluidLevel` twice — once as measured, once with `surface.cap`
#       overridden to `surface.gate` — and checks which prediction the
#       server's own terrain matches.
#
#   (b) The ocean branch's sea check (line resembling `!aborted &&
#       floodedness > 0.8`, off the depth path). Cells NOT on the
#       near-surface path, with `anchor >= oceanGate` (so the depth path is
#       not taken either) and `aborted` true, restricted to `centreY >=
#       lambda` so the trailing lava-level guard cannot make the two
#       hypotheses agree by coincidence. `cellFluidLevel` as measured versus
#       the same call with `surface.aborted` forced false.
#
#   (c) The depth path's own sea check, same comparison, restricted instead
#       to `anchor < oceanGate` (so the depth path IS taken) with the same
#       `aborted` and `centreY` conditions.
#
# `lava` is pinned at 0.0 so every fluid reads as water — a clean level
# readout, not a level and a type. `barrier` is held at -2.0 so no barrier
# ever wins and every cell takes the ladder/sea/lambda decision on its own
# merits (the same trick every other aquifer-*-probe.sh in this project
# uses).
#
# TWO FEATURE SCALES, matching aquifer-psl-probe.sh's own naming (`8` and
# `16`): the window reaches 64 blocks, so a smaller feature size gives more
# arm transitions inside one cell's own scan, and the redundancy is a check
# against either scale being a coincidence rather than a load-bearing part of
# the design.
#
# THE READOUT IS PER BLOCK, NOT PER FLUID BODY, and a first version of the
# analyzer got this wrong the way this project's own convention is to keep on
# record. Every earlier aquifer probe grouped a column's contiguous fluid run
# into one "body" and compared only its top boundary — safe under those
# probes' own gentler configurations, but not here: this field's own extreme
# psl swings put water and lava in the same column constantly, and water
# touching lava turns the contact block to obsidian a tick after worldgen —
# neither `minecraft:water` nor `minecraft:lava`, so it silently split one
# continuous column into two fake "bodies" with a fabricated air gap between
# them. The body-boundary reading scored 0.51, indistinguishable from a coin
# flip; reading fluid/air at every y directly, skipping only the handful of
# blocks that are neither, scores 0.9976 on the same worlds — matching this
# project's usual baseline and confirming the field/selection replica itself
# was never the problem.
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
spec="${work}/nearsurface.json"

python3 - "${spec}" <<'PY'
import json, sys

HIGH, MID, LOW = 96.0, -20.0, -70.0
SEA = 63
T_LOW, T_HIGH = -0.25, 0.25
MIN_Y = -64
HEIGHT = 384

def field(scale, low, mid, high):
    noise = {"type": "minecraft:noise", "noise": "stratum:probe_noise",
             "xz_scale": scale, "y_scale": 0.0}
    return {"type": "minecraft:range_choice", "input": noise,
            "min_inclusive": -1000.0, "max_exclusive": T_LOW,
            "when_in_range": low,
            "when_out_of_range": {
                "type": "minecraft:range_choice", "input": noise,
                "min_inclusive": -1000.0, "max_exclusive": T_HIGH,
                "when_in_range": mid, "when_out_of_range": high}}

NO_SURFACE = {"type": "minecraft:condition",
              "if_true": {"type": "minecraft:y_above",
                          "anchor": {"absolute": 2000},
                          "surface_depth_multiplier": 0, "add_stone_depth": False},
              "then_run": {"type": "minecraft:block",
                           "result_state": {"Name": "minecraft:stone"}}}

def aquifer(name, scale):
    return {"name": name, "min_y": MIN_Y, "height": HEIGHT,
            "raw_final_density": {"type": "minecraft:constant", "argument": -1.0},
            "aquifers_enabled": True, "sea_level": SEA,
            "default_fluid": {"Name": "minecraft:water", "Properties": {"level": "0"}},
            "size_vertical": 2, "surface_rule": NO_SURFACE,
            "router": {"barrier": -2.0, "lava": 0.0,
                       "preliminary_surface_level": field(scale, LOW, MID, HIGH),
                       "fluid_level_floodedness": 0.9,
                       "fluid_level_spread": 0.0}}

spec = [aquifer("nsf_8", 1.0), aquifer("nsf_16", 0.5)]

json.dump(spec, open(sys.argv[1], "w"))
print(len(spec), "dimensions", file=sys.stderr)
PY

exec tools/analysis/density-probe.sh \
    $([[ ${accept_eula} -eq 1 ]] && printf -- --accept-eula) \
    --spec "${spec}" --seed "${seed}"
