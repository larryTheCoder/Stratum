#!/usr/bin/env bash
# Stratum — an aquifer probe isolating the near-surface floor's comparand and
# the abort's refusal of the sea (SPEC §10 MA blocker 2).
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/aquifer-nearsurface-probe.sh --accept-eula [seed]
#
# Writes probes/nearsurface_s<seed>. tools/probe-worlds runs it, so CI
# generates it, for tests/conformance/vanilla_aquifer_nearsurface_test.cpp,
# which scores every reading below on it through the library's own substance
# decision. tools/analysis/aquifer-nearsurface-analyze.cpp is the original
# readout (bare `y < level` on the nearest source), kept because the case
# compares its figures with the decision's.
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
# instead. Separately, an aborted scan off the near-surface path is refused
# the sea — it takes A_lava (-54, lava) before the level rule since pipeline
# engine v11; before that, each of the level rule's two sea checks carried a
# `!aborted` guard — and no world had tested that with `aborted` actually
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
# WHAT EACH SUBSET TESTS, off the SAME world, each by the build's level for
# the deciding source against one rival reading of it:
#
#   (a) Near-surface floor, cap vs gate. Cells that enter the near-surface
#       early return (`gate < oceanGate && gate - centreY < 4`) with
#       `aborted` true and `gate != cap`; the rival writes `gate` where the
#       floor's comparand reads `cap`.
#
#   (b) The abort's refusal of the sea with the anchor at or above
#       `oceanGate` (the depth path not taken). Cells NOT on the
#       near-surface path, with `aborted` true, restricted to `centreY >=
#       lambda` so the trailing lava-level guard cannot make the two
#       hypotheses agree by coincidence; the rival ignores the abort, which
#       at floodedness 0.9 is the sea.
#
#   (c) The same, with the anchor below `oceanGate` (the depth path).
#
# `lava` is pinned at 0.0 so every fluid a source holds is water — a clean
# level readout, not a level and a type. `barrier` is held at -2.0, which
# takes the barrier NOISE out of every pressure term that weighs it — but a
# term whose `|u|` passes 2 does not weigh it (aquifer_barrier.cpp), so a sea
# source beside an A_lava one still walls itself off between them. The case
# scores that stone as stone.
#
# TWO FEATURE SCALES, matching aquifer-psl-probe.sh's own naming (`8` and
# `16`): the window reaches 64 blocks, so a smaller feature size gives more
# arm transitions inside one cell's own scan, and the redundancy is a check
# against either scale being a coincidence rather than a load-bearing part of
# the design. Each scale ships a readout dimension (`nsr_8`, `nsr_16`), the
# same noise and thresholds as an indicator instead of a surface: its terrain
# height names the arm per column from the server's own arithmetic, and the
# case checks its rebuild of the field against it.
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
# was never the problem (0.9997 on the frozen worlds).
#
# density-probe.sh freezes the world before any chunk generates and records
# the seed and `ticks_frozen` in the manifest; tools/probe-worlds verify and
# the case check both. Nothing this writes is committed: worlds are
# Mojang-derived (SPEC §12).
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
# The seed is in the name: density-probe.sh writes probes/<spec name>, so a
# second seed is a corpus of its own rather than a replacement.
spec="${work}/nearsurface_s${seed}.json"

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
# The readouts: same noise, same thresholds, an indicator instead of a
# surface, so terrain height names the arm per column (aquifer-psl-probe.sh).
for tag, scale in (("8", 1.0), ("16", 0.5)):
    spec.append({"name": "nsr_" + tag, "function": field(scale, -1.0, 0.0, 1.0)})

json.dump(spec, open(sys.argv[1], "w"))
print(len(spec), "dimensions", file=sys.stderr)
PY

# Frozen, a world keeps every fluid tick it schedules, and these two are water
# from wall to wall wherever a cell floods: aquifer-nsfloor-probe.sh, the same
# field, needs 10 GB to settle. Overridable, as there.
export STRATUM_PROBE_XMX="${STRATUM_PROBE_XMX:-10G}"
exec tools/analysis/density-probe.sh \
    $([[ ${accept_eula} -eq 1 ]] && printf -- --accept-eula) \
    --spec "${spec}" --seed "${seed}"
