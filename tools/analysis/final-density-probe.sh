#!/usr/bin/env bash
# Stratum — the server's own final_density, read AT an interior point.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/final-density-probe.sh --accept-eula --targets <targets.json> [seed]
#
# WHY THIS EXISTS. M3's one-block residual is localised: at the cell's own
# corner (y offset 0 in an 8-block cell) this build agrees with the server on
# every one of 94208 blocks; at the other seven offsets it disagrees on 12 to
# 37. Our own interior values are a lerp of the SAME two corner values the
# server agrees with — so either vanilla's `interpolated` is not this
# 8-corner trilinear lerp, or something else enters between corners that this
# build does not read. Settling it needs the server's own density value AT an
# interior point, not just its sign (which the block placement already gives
# for free) or its value at a corner (already known to agree).
#
# THE TRICK. `raw_final_density` is set to vanilla's OWN, UNMODIFIED
# `final_density` tree — copied verbatim out of the extracted fixtures, byte
# for byte, never re-typed by hand — wrapped in ONE `range_choice`:
#
#     range_choice(input: <vanilla's final_density tree>,
#                  min_inclusive: -1000, max_exclusive: THRESHOLD,
#                  when_in_range: -1.0, when_out_of_range: 1.0)
#
# The wrapped function IS the probe's raw_final_density, so solid-vs-not at
# ANY block directly reports whether the server's REAL final_density at that
# exact (x, y, z) is below THRESHOLD or not — an exact-value bisection of a
# continuous quantity, the same technique that pinned the barrier's 1.5 and
# the aquifer's abort threshold, applied here to recover a density value
# rather than a branch. Everything else in the dimension — geometry,
# sea_level, default_block/fluid — is copied from vanilla's own overworld
# settings unmodified; aquifers and ore veins are off, since neither is
# implemented here and both would place blocks this comparison does not
# expect. The biome has no carvers and no features, as every probe in this
# project's history uses.
#
# THE TARGETS FILE names WHICH points to bisect and around what value:
#   [{"x": 16, "y": 18, "z": 60, "thresholds": [-0.08, -0.075, ...]}, ...]
# Each threshold becomes its own dimension; the point itself is read directly
# off the block placed at (x, y, z) in every one of them, in ONE server run.
#
# A CONTROL IS BUILT IN, and it is why one target should always be a CORNER
# (y offset 0) this build already agrees with vanilla on. If the wrapped
# probe's bisected value does not land on this build's own computed value
# there too, the wrapping technique itself is unfaithful — reading through a
# `range_choice` is not the same as reading `final_density` plainly — and
# nothing at the interior points can be trusted until that is understood.
#
# Nothing this writes is committed: worlds are Mojang-derived (SPEC §12).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${repo_root}"

accept_eula=0
targets=""
args=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --accept-eula) accept_eula=1; shift ;;
        --targets)     targets="${2:?--targets needs a file}"; shift 2 ;;
        *) args+=("$1"); shift ;;
    esac
done
seed="${args[0]:-42}"

[[ -n "${targets}" && -f "${targets}" ]] || { echo "error: --targets must name a readable JSON file" >&2; exit 2; }
settings=".fixtures/1.21.11/worldgen/noise_settings/overworld.json"
[[ -f "${settings}" ]] || { echo "error: no ${settings}; run tools/fetch-vanilla first" >&2; exit 2; }

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
spec="${work}/finaldensity.json"

env SETTINGS="${settings}" TARGETS="${targets}" python3 - "${spec}" <<'PY'
import json, os, sys

overworld = json.load(open(os.environ['SETTINGS']))
final_density = overworld['noise_router']['final_density']
sea_level = overworld['sea_level']
default_fluid = overworld['default_fluid']
targets = json.load(open(os.environ['TARGETS']))

def wrapped(threshold):
    return {"type": "minecraft:range_choice", "input": final_density,
            "min_inclusive": -1000.0, "max_exclusive": threshold,
            "when_in_range": -1.0, "when_out_of_range": 1.0}

spec = []
names = set()
for t in targets:
    for i, threshold in enumerate(t['thresholds']):
        name = f"p{t['x']}_{t['y']}_{t['z']}_{i}".replace('-', 'm')
        if name in names:
            raise SystemExit(f'duplicate probe name: {name}')
        names.add(name)
        spec.append({
            "name": name,
            "raw_final_density": wrapped(threshold),
            "aquifers_enabled": False,
            "sea_level": sea_level,
            "default_fluid": default_fluid,
            "size_horizontal": overworld['noise']['size_horizontal'],
            "size_vertical": overworld['noise']['size_vertical'],
            "min_y": overworld['noise']['min_y'],
            "height": overworld['noise']['height'],
        })

json.dump(spec, open(sys.argv[1], 'w'))
print(len(spec), "dimensions across", len(targets), "target points", file=sys.stderr)
PY

exec tools/analysis/density-probe.sh \
    $([[ ${accept_eula} -eq 1 ]] && printf -- --accept-eula) \
    --spec "${spec}" --seed "${seed}"
