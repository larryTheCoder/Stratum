#!/usr/bin/env bash
# Stratum — where the aquifer stops being consulted: y_skip, against the server.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/aquifer-yskip-probe.sh --accept-eula --group step|step2|rect [seed]
#
# WHY THIS EXISTS. Above a per-chunk height `y_skip` the local aquifer is not
# consulted and the global picker decides (spec Q2.3/Q2.5): water up to
# `sea_level`, lava below lambda. The build computes it as
# `12 * (floorDiv(S_max + 20, 12) + 1) + 10`, where S_max is the highest
# floored `preliminary_surface_level`, read at y = 0, over the chunk's lattice
# rectangle at a stride of four (lattice.hpp, `ySkip` / `ySkipRectangle`;
# sampling.hpp, `chunkYSkip`). Only the arithmetic had been pinned. On every
# vanilla surface the cutoff is invisible — above it the lattice would answer
# what the global picker does — and it stops being invisible below a surface
# of -80 (aquifer_substance_test). These worlds confirmed the closed form and
# refuted the rectangle (SPEC §11): the server reads offsets -16..+24, where
# the build read -16..+16.
#
# THE INSTRUMENT. Every psl value written here is at or below -80, so every
# scan aborts and every source's status is fixed by its centre alone: a centre
# at or above -54 takes the sea (63, water) — it sits more than twenty blocks
# above the scan's minimum — and a centre below it reads lambda (-54), dry
# above the lava sea. Which blocks are POCKETS (the nearest source dry) is
# therefore the same in every dimension of a seed, whatever the psl; only
# y_skip moves. At or below y_skip a pocket is air or barrier stone; above it
# the global picker makes it water. Pockets are common from -54 to -46, rare
# at -45 and -44 and absent above, so the rows that can show a cutoff are
# -54..-44.
#
#   --group step   -> probes/yskip_s<seed>:   psl -93, -92, -81, -80
#                     (the two step edges the closed form puts at -92/-93 and
#                     -80/-81, one psl unit either side of each)
#   --group step2  -> probes/yskip2_s<seed>:  psl -200, -92.5, -85, and ysy0
#                     (-92.5 floors to -93 and truncates to -92; ysy0 is -85
#                     at y = 0 and -80 at every other y, so only the read
#                     height separates them)
#   --group rect   -> probes/yskiprect_s<seed>: a two-valued psl, -85 or -80
#                     over stratum:probe_noise, at two horizontal scales, each
#                     with a readout dimension (as aquifer-nsfloor-probe.sh's)
#                     that records the field in terrain height. Which chunks
#                     reach -80 depends on WHICH columns S_max reads, so this
#                     is the rectangle's and the stride's measurement.
#
# Every aquifer dimension: constant density -1 (no terrain), sea 63, water,
# barrier -2.0, lava 0.0, floodedness 0.5, spread 0.0, size_vertical 2, no
# surface rule; erosion and depth stay at density-probe.sh's 0, so Q5.9 never
# fires. A frozen world keeps every fluid tick, and these are water from -54
# to 62: four water dimensions per server run at a 10 GB heap, as nsfloor.
#
# The spec's NAME carries the seed (density-probe.sh names its output after
# the spec), so a second seed lands beside the first instead of on it.
#
# Read by tests/conformance/vanilla_aquifer_yskip_test.cpp. Nothing this
# writes is committed: worlds are Mojang-derived (SPEC §12).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${repo_root}"

accept_eula=0
group=""
args=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --accept-eula) accept_eula=1; shift ;;
        --group) group="${2:?--group needs step, step2 or rect}"; shift 2 ;;
        *) args+=("$1"); shift ;;
    esac
done
seed="${args[0]:-42}"
case "${group}" in
    step) spec_name="yskip_s${seed}" ;;
    step2) spec_name="yskip2_s${seed}" ;;
    rect) spec_name="yskiprect_s${seed}" ;;
    *) echo "error: --group must be step, step2 or rect" >&2; exit 2 ;;
esac

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
spec="${work}/${spec_name}.json"

python3 - "${spec}" "${group}" <<'PY'
import json, sys

SEA = 63
MIN_Y = -64
HEIGHT = 384

NO_SURFACE = {"type": "minecraft:condition",
              "if_true": {"type": "minecraft:y_above",
                          "anchor": {"absolute": 2000},
                          "surface_depth_multiplier": 0, "add_stone_depth": False},
              "then_run": {"type": "minecraft:block",
                           "result_state": {"Name": "minecraft:stone"}}}

def aquifer(name, psl):
    return {"name": name, "min_y": MIN_Y, "height": HEIGHT,
            "raw_final_density": {"type": "minecraft:constant", "argument": -1.0},
            "aquifers_enabled": True, "sea_level": SEA,
            "default_fluid": {"Name": "minecraft:water", "Properties": {"level": "0"}},
            "size_vertical": 2, "surface_rule": NO_SURFACE,
            "router": {"barrier": -2.0, "lava": 0.0,
                       "preliminary_surface_level": psl,
                       "fluid_level_floodedness": 0.5,
                       "fluid_level_spread": 0.0}}

# The rectangle's field: -80 where the probe noise reaches T, else -85, as
# (tag, xz_scale, T). Chosen offline from the rebuilt noise: at T 0.7 about
# one column in a hundred is -80, in blobs a few blocks across, so whether a
# chunk reaches -80 turns on exactly which columns its rectangle reads. Over
# seeds 42 and 31337 every rival rectangle and stride the case scores is
# refuted on at least twelve chunks (the conformance case prints them).
RECT = (("a", 1.0, 0.7), ("b", 2.0, 0.7))

def two_valued(scale, threshold, low, high):
    noise = {"type": "minecraft:noise", "noise": "stratum:probe_noise",
             "xz_scale": scale, "y_scale": 0.0}
    return {"type": "minecraft:range_choice", "input": noise,
            "min_inclusive": -1000.0, "max_exclusive": threshold,
            "when_in_range": low, "when_out_of_range": high}

group = sys.argv[2]
spec = []
if group == "step":
    for name, psl in (("ys93", -93.0), ("ys92", -92.0), ("ys81", -81.0), ("ys80", -80.0)):
        spec.append(aquifer(name, psl))
elif group == "step2":
    for name, psl in (("ys200", -200.0), ("ys925", -92.5), ("ys85", -85.0)):
        spec.append(aquifer(name, psl))
    # -85 at y = 0 and -80 everywhere else: the scan reads y = 0 (measured,
    # sampling.hpp), so the statuses are those of a flat -85; a cutoff read
    # at any other height sees -80.
    at_zero = {"type": "minecraft:y_clamped_gradient", "from_y": -1, "to_y": 1,
               "from_value": -1.0, "to_value": 1.0}
    spec.append(aquifer("ysy0", {"type": "minecraft:range_choice", "input": at_zero,
                                 "min_inclusive": -0.5, "max_exclusive": 0.5,
                                 "when_in_range": -85.0, "when_out_of_range": -80.0}))
else:
    for tag, scale, threshold in RECT:
        spec.append(aquifer("ysf_" + tag, two_valued(scale, threshold, -85.0, -80.0)))
        # The readout: the same noise and threshold as a +-1 indicator, so
        # terrain height names the arm per column.
        spec.append({"name": "ysr_" + tag,
                     "function": two_valued(scale, threshold, -1.0, 1.0)})

json.dump(spec, open(sys.argv[1], "w"))
print(len(spec), "dimensions", file=sys.stderr)
PY

# A frozen world keeps every fluid tick it schedules, and these worlds are
# water from -54 to 62: at density-probe.sh's 6 GB default the server runs
# out of heap before the regions settle. Overridable, as there.
export STRATUM_PROBE_XMX="${STRATUM_PROBE_XMX:-10G}"
exec tools/analysis/density-probe.sh \
    $([[ ${accept_eula} -eq 1 ]] && printf -- --accept-eula) \
    --spec "${spec}" --seed "${seed}"
