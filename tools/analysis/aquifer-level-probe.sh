#!/usr/bin/env bash
# Stratum — the level rule on a constant surface, re-measured block for block.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/aquifer-level-probe.sh --accept-eula [--water] [seed]
#
# WHY THIS EXISTS. SPEC §11 records the level rule's whole-model score as four
# per-seed per-cell figures, 99.9806-99.9902%, over about 1370 probe
# dimensions with a CONSTANT `preliminary_surface_level` (-54 to 141, sea
# level 32 to 200). None of that corpus is reproducible: its specs were never
# committed, its per-cell readout is not in the tree, and it was scored
# against a model that has since changed (the pre-divided bonus constant, the
# clamped ladder, the -54 dry level). So the remainder could never be
# attributed. This generator rebuilds the regime on the same axes, frozen,
# so that `aquifer::computeSubstance` can be replayed against every block the
# server wrote rather than against a per-cell readout of them.
#
# THE INSTRUMENT. Every router entry is a constant: density -1.0 (no
# terrain), so every block above the global lava sea is the aquifer's
# decision and nothing else. `default_fluid` is `minecraft:packed_ice`, the
# fix SPEC §11 records for the old campaign's flow problem: the aquifer places
# it exactly where it places water, and it cannot flow, so a block that
# disagrees is a decision that disagrees. `lava` is 0.0; even so a source
# centred below the lava sea is lava, and at spread 0.9 a ladder in the band
# under -40 reaches -51, so rows -54..-52 can hold lava beside packed ice.
# That is the pair the build has never seen measured with a non-water
# default fluid, and the conformance case keeps those three rows out of
# what it asserts.
#
# levelice_s<seed> (one server run, 85 dimensions):
#   g<i>     the grid: preliminary surface -54, -51, ..., 141 (66 values),
#            one dimension each. Most are on the ocean branch, sea level
#            psl+9 (the strict edge), psl+21 or psl+39, clamped to 32..200;
#            for a surface of 24 or more about a third are off it instead,
#            at sea level psl+8 (the other side of the edge) or
#            max(32, psl-22). Floodedness from {-2.0, -0.5, 0.0, 0.25, 0.4,
#            0.5, 0.6, 0.95} — between them they put the sea gate's and the
#            local gate's crossing depths, and the near-surface edge, inside
#            the rows that are read; spread from {0, 0.3, -0.3, 0.7, -0.7,
#            0.9}; barrier from {+1, -2, +2}. Each seed takes the cycles at a
#            different phase, so the three seeds between them put every
#            surface under three configurations.
#   fr<...>  fractional surfaces, -10.4, -10.6, -20.5 and -20.25, where
#            flooring and truncating part, and 40.5, where they do not.
#   x11 x6   the two reaches at which a pre-divided 11/640 fires the sea gate
#            and the product over the divisor does not: surface 41 and 36,
#            sea 63, floodedness 0.0265625 and -0.059375 exactly, spread
#            -0.7 (a ladder at 11, so a cell centred at 30 is dry under one
#            spelling and the sea under the other), barrier -2.
#   x11c x6c the same a ten-millionth higher, where both spellings fire: the
#            world the pre-divided spelling predicts for x11 and x6.
#   l<psl>   off the ocean branch at its edge (sea psl+8), barrier +2, every
#            source dry under the floodedness gates (or, at floodedness 0.6,
#            at a ladder under the surface): where Q5.3(a) — a source centred
#            more than twenty above its surface takes the sea — is the only
#            thing that can build stone near the sea's level. The first 75
#            dimensions found it on 16 blocks of one of them; these eight,
#            at surfaces the model says the seeds' jitter favours, make it
#            measurable.
#   m<psl>   the same with the sea at psl-22, where a clause keyed on the sea
#            instead of the surface would build that stone and Q5.3(a)
#            cannot.
#
# levelwater_s<seed> (--water; one server run, four dimensions): four of the
# same seed's ice dimensions again with water as the default fluid, under the
# same names — the twin that says packed ice stands where water stands, and
# a world where the instrument's own assumption is not needed. Four, because
# a frozen world keeps every fluid tick it schedules, and water from wall to
# wall exhausts the heap (as aquifer-nsfloor-probe.sh found).
#
# The spec's NAME carries the seed (density-probe.sh names its output after
# the spec), so seeds land beside one another. Read by
# tests/conformance/vanilla_aquifer_level_test.cpp. Nothing this writes is
# committed: worlds are Mojang-derived (SPEC §12).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${repo_root}"

accept_eula=0
water=0
args=()
for arg in "$@"; do
    case "${arg}" in
        --accept-eula) accept_eula=1 ;;
        --water) water=1 ;;
        *) args+=("${arg}") ;;
    esac
done
seed="${args[0]:-42}"
[[ "${seed}" =~ ^-?[0-9]+$ ]] || { echo "error: the seed must be a whole number" >&2; exit 2; }

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
if [[ ${water} -eq 1 ]]; then
    spec="${work}/levelwater_s${seed}.json"
else
    spec="${work}/levelice_s${seed}.json"
fi

python3 - "${spec}" "${seed}" "${water}" <<'PY'
import json, sys

out, seed, water = sys.argv[1], int(sys.argv[2]), sys.argv[3] == '1'
MIN_Y, HEIGHT = -64, 384
# The phase each seed takes the cycles at; any other seed takes 0.
PHASE = {42: 0, 31337: 1, 8675309: 2}.get(seed, 0)
FLOODEDNESS = [-2.0, -0.5, 0.0, 0.25, 0.4, 0.5, 0.6, 0.95]
SPREAD = [0.0, 0.3, -0.3, 0.7, -0.7, 0.9]
BARRIER = [1.0, -2.0, 2.0]

NO_SURFACE = {"type": "minecraft:condition",
              "if_true": {"type": "minecraft:y_above",
                          "anchor": {"absolute": 2000},
                          "surface_depth_multiplier": 0, "add_stone_depth": False},
              "then_run": {"type": "minecraft:block",
                           "result_state": {"Name": "minecraft:stone"}}}
ICE = {"Name": "minecraft:packed_ice"}
WATER = {"Name": "minecraft:water", "Properties": {"level": "0"}}


def dim(name, psl, sea, flood, spread, barrier, fluid):
    return {"name": name, "min_y": MIN_Y, "height": HEIGHT,
            "raw_final_density": {"type": "minecraft:constant", "argument": -1.0},
            "aquifers_enabled": True, "sea_level": sea,
            "default_fluid": fluid,
            "size_vertical": 2, "surface_rule": NO_SURFACE,
            "router": {"barrier": barrier, "lava": 0.0,
                       "preliminary_surface_level": psl,
                       "fluid_level_floodedness": flood,
                       "fluid_level_spread": spread}}


def ice_spec():
    spec = []
    for i, psl in enumerate(range(-54, 142, 3)):
        k = i + PHASE
        # Off the ocean branch only where the old corpus could be: a sea
        # level of at least 32 at or under psl+8 needs a surface of 24.
        if psl >= 24 and k % 3 == 0:
            sea = psl + 8 if (k // 3) % 2 == 0 else max(32, psl - 22)
        else:
            sea = min(200, max(32, psl + 9 + (0, 12, 30)[k % 3]))
        spec.append(dim(f"g{i:02d}", float(psl), sea,
                        FLOODEDNESS[(i + i // 4 + PHASE) % 8],
                        SPREAD[(i + i // 6 + 2 * PHASE) % 6],
                        BARRIER[(i // 2 + PHASE) % 3], ICE))
    for name, psl, sea, flood, spread, barrier in (
            ("fr104", -10.4, 32, 0.0, 0.0, 1.0),
            ("fr106", -10.6, 32, 0.0, 0.3, -2.0),
            ("fr205", -20.5, 32, 0.25, 0.0, 2.0),
            ("fr2025", -20.25, 32, 0.0, -0.3, 1.0),
            ("fr405", 40.5, 63, 0.5, 0.9, 2.0)):
        spec.append(dim(name, psl, sea, flood, spread, barrier, ICE))
    for name, psl, flood in (("x11", 41.0, 0.0265625), ("x11c", 41.0, 0.0265626),
                             ("x6", 36.0, -0.059375), ("x6c", 36.0, -0.0593749)):
        spec.append(dim(name, psl, 63, flood, -0.7, -2.0, ICE))
    # Off the ocean branch at its edge, the barrier at +2. Under the
    # floodedness gates alone every source here is dry (two at the ladder,
    # which sits under the surface), so no stone forms near the sea's level;
    # under Q5.3(a) a source centred more than twenty above the surface takes
    # the sea, and walls itself off just above it. Surfaces chosen, by the
    # model alone, where the three seeds' jitter puts such sources nearest.
    for psl in (34, 35, 58, 59, 71, 94, 106, 107):
        spec.append(dim(f"l{psl}", float(psl), psl + 8, 0.6 if psl in (58, 94) else -2.0,
                        0.0, 2.0, ICE))
    # The same with the sea 30 lower, where a clause keyed on the sea rather
    # than on the surface would build those walls and Q5.3(a) cannot.
    for psl in (71, 107):
        spec.append(dim(f"m{psl}", float(psl), psl - 22, -2.0, 0.0, 2.0, ICE))
    return spec


def water_spec():
    """Four of the ice dimensions, by property, under their own names."""
    ice = ice_spec()

    def first(predicate, why):
        for entry in ice:
            r, sea = entry["router"], entry["sea_level"]
            if entry["name"].startswith("g") and predicate(r["preliminary_surface_level"], sea,
                                                            r["fluid_level_floodedness"],
                                                            r["fluid_level_spread"],
                                                            r["barrier"]):
                return entry
        raise SystemExit(f"no ice dimension is {why}")

    picks = [
        # The sea gate, the ladder and the dry outcome together, barrier on.
        first(lambda p, s, f, sp, b: p < s - 8 and f == 0.25 and b > 0,
              "on the ocean branch at floodedness 0.25 with the barrier on"),
        # Ladders in the band under -40 reaching -51 beside water: lava.
        first(lambda p, s, f, sp, b: p < s - 8 and sp == 0.9 and 0.4 <= f <= 0.6,
              "on the ocean branch at spread 0.9 between the gates"),
        # Off the ocean branch, between the gates.
        first(lambda p, s, f, sp, b: p >= s - 8 and 0.4 < f <= 0.8,
              "off the ocean branch between the gates"),
    ]
    picks.append(next(e for e in ice if e["name"] == "x11"))
    for entry in picks:
        entry["default_fluid"] = WATER
    return picks


spec = water_spec() if water else ice_spec()
json.dump(spec, open(out, "w"))
print(len(spec), "dimensions", file=sys.stderr)
PY

# Packed ice schedules no fluid tick, so an ice world is light; a frozen
# water world keeps every tick it schedules, and four water dimensions need
# the 10 GB the other water probes use. Overridable, as in density-probe.sh.
if [[ ${water} -eq 1 ]]; then
    export STRATUM_PROBE_XMX="${STRATUM_PROBE_XMX:-10G}"
fi
exec tools/analysis/density-probe.sh \
    $([[ ${accept_eula} -eq 1 ]] && printf -- --accept-eula) \
    --spec "${spec}" --seed "${seed}"
