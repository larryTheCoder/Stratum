#!/usr/bin/env bash
# Stratum — the surface scan's carried readings, parted on a varying surface.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/aquifer-ties-probe.sh --accept-eula [seed]
#   tools/analysis/aquifer-ties-probe.sh --spec-only=FILE [seed]
#
# WHY THIS EXISTS. Pipeline engine v12 decided a source's status from four
# values of its surface scan, and three readings of them were carried rather
# than measured, because every probe that reached them held a surface whose
# values could not part them:
#
#   (a) the near-surface exemption of an aborting scan (`centreY > cap + 20`)
#       read the whole window's minimum; the rival was the aborting sample
#       itself. They are one number unless the window holds two DIFFERENT
#       values below the abort threshold, the later one lower, and no probe
#       field had two.
#   (b) Q5.3(a) for a scan that did NOT abort, off the ocean branch, read the
#       same `cap`; the clean-room spec's comparand is the anchor (a0). A
#       constant surface makes them one number (aquifer-level-probe.sh).
#   (c) the clean-room spec's Q5.3(b) lets the FIRST submerged sample in scan
#       order decide the status, where v12 read the abort flag and the whole
#       window's minimum. They part on a scan holding a submerged sample that
#       does not abort BEFORE one that does (and, by the same argument, on an
#       aborted scan whose prefix is land, and on one whose centre sits four
#       or more below every sample).
#
# The server took the clean-room spec's side on every one, and the library
# now reads Q5.3 as written (SPEC §11, "The surface scan, sample by sample").
#
# Every aquifer dimension holds density -1 (no terrain), `lava` 0 and spread
# 0, with `minecraft:packed_ice` as the default fluid, as aquifer-level-
# probe.sh's do: the aquifer places it wherever it places water and it cannot
# flow, so a block that disagrees is a decision that disagrees, and many
# dimensions fit one server start. The surface is `stratum:probe_noise`
# (xz_scale 0.25: features about thirty blocks across, inside the scan's 64
# by 32 window) cut into arms by `range_choice` at 0, or at -0.25 and 0.25 for
# three arms; the lid dimensions cut it at -0.5 at twice the frequency, so
# that most anchors read the high arm and most windows meet the low one.
# Barrier -2 holds sheets to where two levels part widely; +8 and +20 raise
# them at every level boundary, which is the only way a source centred above
# the sea can show its status; "real" is vanilla's own `aquifer_barrier`.
#
# sea 63 (lambda -54, abort below -62), min_y -64:
#   ta     arms -100 / -63, floodedness -2, barrier -2. Every scan aborts at
#          its anchor; an anchor at -63 with -100 in its window, centred at
#          -54..-43, is the sea under v12 (cap -100) and A_lava under the
#          aborting sample and the spec (a0 -63).                       (a)
#   tar    the same, barrier real.
#   tb     arms -64 / -60, floodedness -2, barrier -2. An anchor at -60 with
#          -64 in its window, centred at -63..-44: A_lava under v12 (cap -64),
#          the sea under the spec (-60 fires first).                    (c)
#   tbr    the same, barrier real.
#   tab    arms -100 / -64 / -60, floodedness -2, barrier -2: the same scans
#          with a lower sample after the aborting one, where the aborting
#          sample's exemption parts from v12 and from the spec.         (a)
#   tl35, tl43   land arms W / W + 12 at sea W + 8 (the ocean gate's land
#          edge), floodedness -2, barrier +8 / +20. An anchor at W + 12 with
#          W in its window, centred at W + 21..W + 32: the sea under v12
#          (cap W), dry under the anchor; seen through the sea's lid.   (b)
#   tp35, tp43   arms -70 / W at sea W + 8, the same otherwise. An anchor at
#          W with -70 in its window, centred above W + 20: A_lava under v12,
#          the sea under the spec's Q5.3(a); seen through the lid.      (c)
#          W 35 and 43 are the lattice phases that put the most such sources
#          within reach of the lid rows on both seeds (the analyzer's
#          --model, scanned over W 30-45).
#   td     psl -63 everywhere, water, floodedness -2, barrier +20. A source
#          four or more below -63 fires no sample: the level rule under the
#          spec, A_lava under v12. Only Π just above lambda can show it
#          (on row lambda alone, in the model's scans), and -63 is the one
#          surface of -63..-75 where any block parts at all.             (c)
# sea -70 (lambda -70, abort below -78), min_y -128, height 256:
#   tc     arms -90 / -78, floodedness 0, barrier -2. -78 is land here. An
#          anchor at -78 with -90 in its window, centred above -58: A_lava
#          (lava to y = -55) under v12, the sea (dry above -70) under the
#          spec.                                                         (c)
#   tcr    the same, barrier real.
# and the readouts:
#   tr     the 0.25-scale field as a four-step indicator (-1, -1/3, 1/3, 1 on
#          the four ranges the cuts make), so terrain height names the arm at
#          every column the probe pins.
#   trs    the lid dimensions' field (-1 / 1).
#
# --spec-only=FILE writes the spec and starts nothing: the analyzer's --model
# reads it to check a design before a server runs.
#
# The spec's NAME carries the seed (density-probe.sh names its output after
# the spec), so seeds land beside one another. Read by
# tests/conformance/vanilla_aquifer_ties_test.cpp and
# tools/analysis/aquifer-ties-analyze.cpp. Nothing this writes is committed:
# worlds are Mojang-derived (SPEC §12).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${repo_root}"

accept_eula=0
spec_only=""
args=()
for arg in "$@"; do
    case "${arg}" in
        --accept-eula) accept_eula=1 ;;
        --spec-only=*) spec_only="${arg#--spec-only=}" ;;
        *) args+=("${arg}") ;;
    esac
done
seed="${args[0]:-42}"
[[ "${seed}" =~ ^-?[0-9]+$ ]] || { echo "error: the seed must be a whole number" >&2; exit 2; }

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
spec="${work}/ties_s${seed}.json"

python3 - "${spec}" <<'PY'
import json, sys

REAL_BARRIER = {"type": "minecraft:noise", "noise": "minecraft:aquifer_barrier",
                "xz_scale": 1.0, "y_scale": 0.5}

NO_SURFACE = {"type": "minecraft:condition",
              "if_true": {"type": "minecraft:y_above",
                          "anchor": {"absolute": 2000},
                          "surface_depth_multiplier": 0, "add_stone_depth": False},
              "then_run": {"type": "minecraft:block",
                           "result_state": {"Name": "minecraft:stone"}}}

ICE = {"Name": "minecraft:packed_ice"}
WATER = {"Name": "minecraft:water", "Properties": {"level": "0"}}


def noise(scale):
    return {"type": "minecraft:noise", "noise": "stratum:probe_noise",
            "xz_scale": scale, "y_scale": 0.0}


def cut(below, at_or_above, threshold, scale=0.25):
    return {"type": "minecraft:range_choice", "input": noise(scale),
            "min_inclusive": -1000.0, "max_exclusive": threshold,
            "when_in_range": below, "when_out_of_range": at_or_above}


def two(low, high):
    """`low` where the noise is under 0, `high` elsewhere."""
    return cut(low, high, 0.0)


def three(low, mid, high):
    """`low` under -0.25, `mid` in [-0.25, 0.25), `high` from 0.25."""
    return cut(low, cut(mid, high, 0.25), -0.25)


def stripes(low, high):
    """`low` in narrow stripes (the noise under -0.5 at twice the frequency),
    `high` elsewhere: most anchors read `high` and most windows meet `low`."""
    return cut(low, high, -0.5, 0.5)


def aquifer(name, sea, psl, floodedness, barrier, low_sea=False, fluid=ICE):
    return {"name": name,
            "min_y": -128 if low_sea else -64, "height": 256 if low_sea else 384,
            "raw_final_density": {"type": "minecraft:constant", "argument": -1.0},
            "aquifers_enabled": True, "sea_level": sea, "default_fluid": fluid,
            "size_vertical": 2, "surface_rule": NO_SURFACE,
            "router": {"barrier": barrier, "lava": 0.0,
                       "preliminary_surface_level": psl,
                       "fluid_level_floodedness": floodedness,
                       "fluid_level_spread": 0.0}}


spec = [
    aquifer("ta", 63, two(-100.0, -63.0), -2.0, -2.0),
    aquifer("tar", 63, two(-100.0, -63.0), -2.0, REAL_BARRIER),
    aquifer("tb", 63, two(-64.0, -60.0), -2.0, -2.0),
    aquifer("tbr", 63, two(-64.0, -60.0), -2.0, REAL_BARRIER),
    aquifer("tab", 63, three(-100.0, -64.0, -60.0), -2.0, -2.0),
    # The lids. The surface W sits at the ocean gate's land edge (sea W + 8),
    # so a source parts the readings from W + 21 up; at W 35 and 43 the
    # lattice puts the most such sources within a barrier's reach of the
    # sea's lid rows on both seeds (scanned with the analyzer's --model over
    # W 30-45). A barrier of +8 or +20 widens the lid to every pair whose
    # squared distances differ by less than about 24, where +2 needs a
    # closer pair: five to nine times the blocks of +2 at these phases.
    aquifer("tl35", 43, stripes(35.0, 47.0), -2.0, 8.0),
    aquifer("tl43", 51, stripes(43.0, 55.0), -2.0, 20.0),
    aquifer("tp35", 43, stripes(-70.0, 35.0), -2.0, 8.0),
    aquifer("tp43", 51, stripes(-70.0, 43.0), -2.0, 20.0),
    aquifer("tc", -70, two(-90.0, -78.0), 0.0, -2.0, low_sea=True),
    aquifer("tcr", -70, two(-90.0, -78.0), 0.0, REAL_BARRIER, low_sea=True),
    # A source four or more below every sample of an aborting scan: no
    # sample fires, so the spec runs the level rule where v12 read A_lava.
    # Both are below lambda, so only Π just above it can tell them apart (on
    # row lambda alone, where Q6.3 is measured for water only): water, and
    # the shallowest abort, the one surface of -63..-75 where any block parts.
    aquifer("td", 63, -63.0, -2.0, 20.0, fluid=WATER),
    {"name": "tr", "function": three(-1.0, cut(-1.0 / 3.0, 1.0 / 3.0, 0.0), 1.0)},
    {"name": "trs", "function": stripes(-1.0, 1.0)},
]

json.dump(spec, open(sys.argv[1], "w"))
print(len(spec), "dimensions", file=sys.stderr)
PY

if [[ -n "${spec_only}" ]]; then
    cp "${spec}" "${spec_only}"
    exit 0
fi

# Packed ice schedules no fluid tick, so the ice dimensions are light; the
# lava the build or the spec places between -70 and -55 at sea -70 is frozen
# with the rest. `td` is water from wall to wall above its y_skip, and a
# frozen world keeps every tick that schedules: the 10 GB the water probes
# use. Overridable, as in density-probe.sh.
export STRATUM_PROBE_XMX="${STRATUM_PROBE_XMX:-10G}"
exec tools/analysis/density-probe.sh \
    $([[ ${accept_eula} -eq 1 ]] && printf -- --accept-eula) \
    --spec "${spec}" --seed "${seed}"
