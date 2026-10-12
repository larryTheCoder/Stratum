#!/usr/bin/env bash
# Stratum — cache markers inside the aquifer's router entries.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/aquifer-markers-probe.sh --accept-eula [seed]
#
# WHY THIS EXISTS. The aquifer reads its router entries at five kinds of
# point, and only one of them is the block being generated:
#
#   barrier                         the block itself;
#   fluid_level_floodedness         a source's jittered centre, often in a
#   (and erosion/depth, for Q5.9)   neighbouring chunk;
#   fluid_level_spread              contracted indices (cell.x, band, cell.z);
#   lava                            contracted indices on a 64-block pitch;
#   preliminary_surface_level       4-aligned anchors at y = 0.
#
# A datapack may wrap any of them in a cache marker, and what a marker means
# at a point that is not the block being generated is not documented
# anywhere. `interpolated` could blend over the cell holding the read point,
# be transparent there, or hand back the generating block's own value;
# `cache_all_in_cell` likewise; `flat_cache` relocates inside the chunk's own
# grid, and how far that grid reaches was only ever taken from the goldens,
# where 16, 20 and 24 columns tie. No vanilla preset wraps an aquifer entry in
# anything but `flat_cache` (erosion and depth), so this is datapack-only.
#
# One dimension per (entry, marker), each built so the readings part on whole
# bands rather than on noise luck:
#
#   b*  barrier = W(1.2 * fast noise), over slow floodedness and a spread that
#       vary the levels so barriers form;
#   f*  floodedness = W(0.6 + 0.5 * fast noise), straddling both of the level
#       rule's gates;
#   s*  spread = W(Ys(y) + 0.45 * noise(xz)), Ys a y-step that is nonzero only
#       at y = -8, -1, 0, 1, 8 — so an exact read, a lerp across the cell, the
#       cell's lower corner and a read at y = 0 predict different levels in
#       the visible bands -1, 0 and 1;
#   l*  lava = W(Lk(y)), three y-step profiles whose (lava, water) signatures
#       differ for every reading; floodedness keeps only band -1 wet;
#   p*  psl = W(two-armed noise field, 96 / -20), floodedness 0.6 so the
#       surface decides between sea and capped ladder — a 4-aligned anchor is
#       a cell corner at size_horizontal 1 (a null control) and a midpoint
#       at 2;
#   e*  erosion = W(-0.225 + 0.3 * fast noise), depth 1.0: Q5.9's twin of f*.
#
# Every dimension is barrier3way's geometry (min_y -48, so the lava sea's
# lambda of -54 is below the floor and Q6.3 never fires), and its default
# fluid is `minecraft:packed_ice`: the aquifer places it exactly where it
# places water and it never flows (SPEC §11), so a frozen world keeps no fluid
# tick heap and a block count needs no flow allowance outside the lava arms.
# Five corpora per seed, one server start each, so a server that cannot build
# one marker cannot cost the others: markers_bare_s<seed> (controls),
# markers_int_s<seed> (interpolated), markers_cell_s<seed>
# (cache_all_in_cell), markers_flat_s<seed> (flat_cache), markers_memo_s<seed>
# (cache_once, cache_2d). Names carry the seed, so seeds never overwrite each
# other.
#
# Read by tests/conformance/vanilla_aquifer_markers_test.cpp. Nothing this
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

python3 - "${work}" "${seed}" <<'PY'
import json, pathlib, sys

out = pathlib.Path(sys.argv[1])
seed = sys.argv[2]

SEA = 63
MIN_Y = -48
HEIGHT = 320

NO_SURFACE = {"type": "minecraft:condition",
              "if_true": {"type": "minecraft:y_above",
                          "anchor": {"absolute": 2000},
                          "surface_depth_multiplier": 0, "add_stone_depth": False},
              "then_run": {"type": "minecraft:block",
                           "result_state": {"Name": "minecraft:stone"}}}

def noise(xz, y):
    return {"type": "minecraft:noise", "noise": "stratum:probe_noise",
            "xz_scale": xz, "y_scale": y}

def add(a, b):
    return {"type": "minecraft:add", "argument1": a, "argument2": b}

def mul(a, b):
    return {"type": "minecraft:mul", "argument1": a, "argument2": b}

# y itself, exactly, at every integer y the probe can reach: the gradient's
# span is a power of two, so (y + 2048) / 4096 * 4096 - 2048 rounds nowhere.
Y = {"type": "minecraft:y_clamped_gradient", "from_y": -2048, "to_y": 2048,
     "from_value": -2048.0, "to_value": 2048.0}

def ystep(table, otherwise=0.0):
    """A function of y alone: table[y] at those integers, `otherwise` elsewhere.
    Thresholds sit at half-integers, so no integer y is on one."""
    node = otherwise
    for y in sorted(table):
        node = {"type": "minecraft:range_choice", "input": Y,
                "min_inclusive": y - 0.5, "max_exclusive": y + 0.5,
                "when_in_range": table[y], "when_out_of_range": node}
    return node

def wrap(marker, argument):
    if marker is None:
        return argument
    return {"type": "minecraft:" + marker, "argument": argument}

def dimension(name, router, sh=1):
    base = {"barrier": 0.0, "fluid_level_floodedness": 0.6, "fluid_level_spread": 0.0,
            "lava": 0.0, "preliminary_surface_level": 96.0, "erosion": 0.0, "depth": 0.0}
    base.update(router)
    return {"name": name, "min_y": MIN_Y, "height": HEIGHT,
            "raw_final_density": {"type": "minecraft:constant", "argument": -1.0},
            "aquifers_enabled": True, "sea_level": SEA,
            "default_fluid": {"Name": "minecraft:packed_ice"},
            "size_vertical": 2, "size_horizontal": sh, "surface_rule": NO_SURFACE,
            "router": base}

# The six families. Each takes the marker (None for the bare control) and
# returns the router overrides.
def fam_b(m):
    return {"barrier": wrap(m, mul(1.2, noise(1.7, 1.7))),
            # Slow, like vanilla's own: sea, ladder and dry cells side by side.
            "fluid_level_floodedness": add(0.6, mul(0.6, noise(0.0625, 0.042))),
            "fluid_level_spread": noise(2.0, 2.0)}

def fam_f(m, y_scale=1.7):
    return {"fluid_level_floodedness": wrap(m, add(0.6, mul(0.5, noise(1.7, y_scale))))}

YS = {-8: -0.9, -1: 0.6, 0: 0.3, 1: -0.6, 8: 0.9}

def fam_s(m):
    return {"fluid_level_spread": wrap(m, add(ystep(YS), mul(0.45, noise(4.0, 0.0))))}

# Only band -1 is wet: floodedness is read at the centre, so every centre
# below y = 0 takes the ladder (-20 at spread 0) and every one above is dry.
L_FLOOD = {"type": "minecraft:range_choice", "input": Y,
           "min_inclusive": -2048.0, "max_exclusive": -0.5,
           "when_in_range": 0.6, "when_out_of_range": -1.0}
L_PROFILES = {"l1": {-1: 1.0}, "l2": {0: 0.4}, "l3": {-8: -2.0, 0: 0.4}}

def fam_l(m, profile):
    return {"fluid_level_floodedness": L_FLOOD,
            "lava": wrap(m, ystep(L_PROFILES[profile]))}

PSL_FIELD = {"type": "minecraft:range_choice", "input": noise(1.0, 0.0),
             "min_inclusive": -1000.0, "max_exclusive": 0.0,
             "when_in_range": -20.0, "when_out_of_range": 96.0}

def fam_p(m):
    # Floodedness between the level rule's gates, so the surface decides:
    # under a low (-20) anchor a source near it takes the sea, under a high
    # one the ladder capped by the scan's minimum. At 0.9 every source takes
    # the sea whatever the surface reads, and the arm is blind (measured: the
    # first run of this probe had it so).
    return {"preliminary_surface_level": wrap(m, PSL_FIELD),
            "fluid_level_floodedness": 0.6,
            "barrier": noise(1.0, 0.5)}

def fam_e(m):
    return {"erosion": wrap(m, add(-0.225, mul(0.3, noise(1.7, 1.7)))),
            "depth": 1.0, "fluid_level_floodedness": 0.9}

def cell_group(m):
    return [dimension("b1", fam_b(m), 1), dimension("b2", fam_b(m), 2),
            dimension("f1", fam_f(m), 1), dimension("f2", fam_f(m), 2),
            dimension("s1", fam_s(m), 1), dimension("s2", fam_s(m), 2),
            dimension("l1", fam_l(m, "l1")), dimension("l2", fam_l(m, "l2")),
            dimension("l3", fam_l(m, "l3")),
            dimension("p1", fam_p(m), 1), dimension("p2", fam_p(m), 2),
            dimension("e1", fam_e(m), 1)]

groups = {
    "bare": [dimension("b", fam_b(None)), dimension("f", fam_f(None)),
             dimension("s", fam_s(None)), dimension("l1", fam_l(None, "l1")),
             dimension("p", fam_p(None))],
    "int": cell_group("interpolated"),
    "cell": cell_group("cache_all_in_cell"),
    # psl under flat_cache is left out: moving an aligned y = 0 read to its
    # aligned y = 0 corner is the identity.
    "flat": [dimension("b", fam_b("flat_cache")), dimension("f", fam_f("flat_cache")),
             dimension("s", fam_s("flat_cache")), dimension("l1", fam_l("flat_cache", "l1")),
             dimension("e", fam_e("flat_cache"))],
    # cache_2d over a y-varying argument is refused by this build, so its arm
    # is column-invariant.
    "memo": [dimension("f_once", fam_f("cache_once")), dimension("s_once", fam_s("cache_once")),
             dimension("f_2d", fam_f("cache_2d", 0.0))],
}

for group, dims in groups.items():
    path = out / f"markers_{group}_s{seed}.json"
    json.dump(dims, open(path, "w"))
    print(path.name, len(dims), "dimensions", file=sys.stderr)
PY

# packed_ice never flows, so these worlds schedule no water ticks; only the
# lava arms keep a few. density-probe.sh's default heap is enough.
for group in bare int cell flat memo; do
    tools/analysis/density-probe.sh \
        $([[ ${accept_eula} -eq 1 ]] && printf -- --accept-eula) \
        --spec "${work}/markers_${group}_s${seed}.json" --seed "${seed}"
done
