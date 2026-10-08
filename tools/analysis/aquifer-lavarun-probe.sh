#!/usr/bin/env bash
# Stratum — how the surface pass counts aquifer lava: the stone-depth runs
# and the water height, read off marker ladders under flat lava pools.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/aquifer-lavarun-probe.sh --accept-eula [seed]
#   tools/analysis/aquifer-lavarun-probe.sh --spec-only <out.json> [seed]
#
# WHY THIS EXISTS. In a dimension whose `default_fluid` is water the aquifer
# writes literal `minecraft:lava`, and the surface pass reads its two
# stone-depth runs (`stone_depth` floor counts top down, ceiling bottom up)
# and its water height (`water`) off the blocks the first pass left. Water's
# part is documented (surface::Context: neither counts toward a run nor
# breaks it; latches the height). Lava's is not, and no existing corpus can
# say: every aquifer probe runs a surface rule that never fires, and in the
# shipped presets every run-reading condition sits under
# `above_preliminary_surface`, far above the y -59..-15 where the goldens'
# lava is. So this world asks directly. Three readings per run — lava counts
# like stone, lava is a fluid like water (neither counts nor breaks), lava
# breaks the run like air — and two for the height — lava latches it or not.
#
# THE AQUIFER, the fluid-type probe's measured arm P (`p_psl_m12`): barrier
# -2.0 so no barrier is ever placed, floodedness 0.5 (the ladder), spread 6.0
# so every rung sits above the cap, `preliminary_surface_level` -12 and
# `sea_level` -16 so neither the ocean branch nor the near-surface path runs
# (-12 >= -24) and nothing aborts (-12 >= lambda - 8). Every source's level
# is then -12 and, with `lava` 0.5 (past 0.3, level <= -10), lava. Open space
# at y <= -13 is lava, at y >= -12 air, and below lambda = -54 the global lava
# sea (Q2.4) whatever the lattice says. `lava` 0.0 gives water instead: the
# water twins, which re-measure the documented water behaviour in the same
# run as the positive control. Floodedness -1.0 dries the lattice, which
# leaves only the sea: the `sea_*` dimensions.
#
# THE TERRAIN depends on y alone: a constant plus one `y_clamped_gradient`
# per 8-block cell (`size_vertical` 2), so it is linear inside every cell and
# cell interpolation and per-block evaluation agree. Every solid/open boundary
# sits at a half-integer y with at most one per cell, the corner values are
# chosen so each cell's line crosses zero exactly there, and every value is a
# dyadic rational so both sides compute it exactly. This script evaluates its
# own function at every integer y and refuses to write a spec whose layout is
# not the one it records, per dimension, as `expected_layout` (top down,
# [top, bottom, solid|air|lava|water]).
#
# THE READOUT. No surface noise is read: three ladders, each a `sequence` of
# sixteen rungs, the k-th placing the k-th wool colour.
#   floor   — stone_depth{floor, offset k, no surface depth, range 0}: a stone
#             block shows its 0-based depth in its top-down run, 0..15, and
#             stays stone deeper than that;
#   ceiling — the same with surface_type ceiling, the bottom-up run;
#   water   — water{offset -k, multiplier 0, no stone depth}: wool k where
#             waterHeight - y = k (k >= 1), wool 0 at and above the height
#             or wherever the column has none, stone deeper than 15.
# A marker replaces only the default block (measured, SPEC §11), so the
# fluids and the air read back exactly as the first pass left them.
#
# THE DIMENSIONS. G1, top down: stone 319..4; cave A open 3..-24 (air to -12,
# then 12 blocks of lattice fluid); stone -25..-32; cave B -33..-40, enclosed
# lattice fluid; stone -41..-58; the lava sea -59..-64.
#   lava_floor / lava_ceiling / lava_water        G1, lava, one ladder each
#   water_floor / water_ceiling / water_water     G1, water (the controls)
# G2, the ceiling run's lattice question, in a world whose floor is -48:
# stone -48..-37, an enclosed pocket -36..-29, stone -28..319. Bottom up,
# twelve blocks of stone reach the pocket, so the run above it shows whether
# the pocket counted, held or reset — which G1 cannot, because its run
# reaches cave B too long to show.
#   lava_pocket_ceiling / water_pocket_ceiling
# The floor is lifted rather than the pocket raised, and that is measured:
# the first version put the pocket at -52..-45 over a -64 floor, and its water
# twin held lava at -52/-51 in 5 445 blocks — a source centred below lambda
# is lava whatever `lava` says (fluid_type.hpp), and cells centred at
# -72..-55 are candidates down there. At -36..-29 every candidate cell is
# centred at -48 or above.
# The lava sea on its own, lattice dry:
#   sea_open    stone >= -49, air -50..-54, sea -55..-60, stone -61..-64; floor
#   sea_roof    stone >= -54 straight onto the sea -55..-60, stone -61..-64; floor
#   sea_ceiling sea_roof's terrain, ceiling ladder
#   sea_water   sea_open's terrain, water ladder (does the sea latch the height?)
# Every reading of every factor is separated by some dimension, on whole
# columns: tests/conformance/vanilla_aquifer_lava_run_test.cpp scores them.
#
# FLOW. No lava touches air below its own surface, no water touches lava,
# and every source shares one level and type, so the aquifer flags no fluid
# update (Q8) and nothing is expected to move — and on seed 42 nothing did,
# 0 blocks in all twelve. The test bounds flow rather than pinning it.
#
# --spec-only writes the spec and stops, without the server — for checking
# a change to this script against the shipped filler before spending a run.
#
# Nothing this writes is committed: worlds are Mojang-derived (SPEC §12).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${repo_root}"

accept_eula=0
spec_only=""
args=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --accept-eula) accept_eula=1; shift ;;
        --spec-only) spec_only="${2:?--spec-only needs an output path}"; shift 2 ;;
        *) args+=("$1"); shift ;;
    esac
done
seed="${args[0]:-42}"

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
# density-probe.sh names the corpus after this basename: one per seed.
spec="${work}/lavarun_s${seed}.json"

python3 - "${spec}" <<'PY'
import json, math, sys
from fractions import Fraction

MIN_Y = -64                       # every dimension's floor but G2's
TOP = 319                         # every dimension's top
CELL = 8                          # size_vertical 2
SEA = -16
PSL = -12                         # every source's level, on arm P
LAMBDA = min(-54, SEA)
Y_SKIP = 22                       # lattice.hpp ySkip(-12): the lattice's ceiling

WOOL = ["white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
        "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black"]


def wool(k):
    return {"type": "minecraft:block", "result_state": {"Name": f"minecraft:{WOOL[k]}_wool"}}


def ladder(kind):
    rungs = []
    for k in range(16):
        if kind == "water":
            condition = {"type": "minecraft:water", "offset": -k,
                         "surface_depth_multiplier": 0, "add_stone_depth": False}
        else:
            condition = {"type": "minecraft:stone_depth", "offset": k,
                         "add_surface_depth": False, "secondary_depth_range": 0,
                         "surface_type": kind}
        rungs.append({"type": "minecraft:condition", "if_true": condition, "then_run": wool(k)})
    return {"type": "minecraft:sequence", "sequence": rungs}


def solid_at(solid, y, min_y):
    y = min(max(y, min_y), TOP)
    return any(bottom <= y <= top for top, bottom in solid)


def density(solid, min_y):
    """Constant + one y_clamped_gradient per cell, zero exactly on each boundary."""
    corners = list(range(min_y, TOP + 2, CELL))
    sign = {c: (1 if solid_at(solid, c, min_y) else -1) for c in corners}
    boundary = {}
    for a in corners[:-1]:
        cuts = [y + Fraction(1, 2) for y in range(a, a + CELL)
                if solid_at(solid, y, min_y) != solid_at(solid, y + 1, min_y)]
        assert len(cuts) <= 1, ("two boundaries in one cell", a, cuts)
        if cuts:
            boundary[a] = cuts[0]
    # Slope per boundary cell. Two adjacent boundary cells share a corner, so
    # their slopes are tied; a chain of them is solved together, then scaled
    # to whole numbers so every corner value is exact in binary.
    slope = {}
    chain = []
    chains = []
    for a in corners[:-1]:
        if a in boundary:
            if chain and chain[-1] + CELL == a:
                prev = chain[-1]
                shared = (prev + CELL) - boundary[prev]          # |corner - b_prev|
                slope[a] = slope[prev] * shared / (boundary[a] - a)
            else:
                if chain:
                    chains.append(chain)
                chain = []
                slope[a] = Fraction(1)
            chain.append(a)
    if chain:
        chains.append(chain)
    for members in chains:
        scale = 1
        for a in members:
            den = slope[a].denominator
            scale = scale * den // math.gcd(scale, den)
        for a in members:
            slope[a] *= scale
    value = {c: Fraction(sign[c] * CELL) for c in corners}
    for a, b in boundary.items():
        value[a] = sign[a] * (b - a) * slope[a]
        value[a + CELL] = sign[a + CELL] * (a + CELL - b) * slope[a]
    for c, v in value.items():
        assert v != 0 and v.denominator & (v.denominator - 1) == 0, (c, v)

    def at(y):
        a = min_y + ((y - min_y) // CELL) * CELL
        if a == corners[-1]:
            return value[a]
        return value[a] + (value[a + CELL] - value[a]) * Fraction(y - a, CELL)

    for y in range(min_y, TOP + 1):
        d = at(y)
        assert (d > 0) == solid_at(solid, y, min_y), ("layout", y, d)
        assert abs(d) >= Fraction(1, 2), ("too close to zero", y, d)

    node = {"type": "minecraft:constant", "argument": float(value[corners[0]])}
    for a in corners[:-1]:
        step = value[a + CELL] - value[a]
        if step == 0:
            continue
        gradient = {"type": "minecraft:y_clamped_gradient", "from_y": a, "to_y": a + CELL,
                    "from_value": 0.0, "to_value": float(step)}
        node = {"type": "minecraft:add", "argument1": node, "argument2": gradient}
    return node


def layout(solid, lattice, min_y):
    """Top down, run-length: what the first pass leaves, before any marker."""
    rows = []
    for y in range(TOP, min_y - 1, -1):
        if solid_at(solid, y, min_y):
            kind = "solid"
        elif y < LAMBDA:
            kind = "lava"                 # Q2.4, whatever the lattice says
        else:
            assert y <= Y_SKIP, ("open space above the lattice's ceiling", y)
            kind = lattice if (lattice != "air" and y < PSL) else "air"
        if rows and rows[-1][2] == kind:
            rows[-1][1] = y
        else:
            rows.append([y, y, kind])
    return rows


def dim(name, solid, kind, fluid, min_y=MIN_Y):
    """fluid: 'lava' or 'water' fills the lattice below -12; 'air' dries it."""
    # The server refuses a dimension type off the 16-block grid.
    assert min_y % 16 == 0 and (TOP + 1 - min_y) % 16 == 0, (name, min_y)
    return {"name": name, "min_y": min_y, "height": TOP + 1 - min_y,
            "raw_final_density": density(solid, min_y),
            "aquifers_enabled": True, "sea_level": SEA,
            "default_fluid": {"Name": "minecraft:water", "Properties": {"level": "0"}},
            "size_vertical": 2, "surface_rule": ladder(kind),
            "router": {"barrier": -2.0,
                       "lava": 0.0 if fluid == "water" else 0.5,
                       "preliminary_surface_level": float(PSL),
                       "fluid_level_floodedness": -1.0 if fluid == "air" else 0.5,
                       "fluid_level_spread": 6.0},
            "ladder": kind, "expected_layout": layout(solid, fluid, min_y)}


G1 = [(TOP, 4), (-25, -32), (-41, -58)]
G2_MIN_Y = -48
G2 = [(TOP, -28), (-37, G2_MIN_Y)]
SEA_OPEN = [(TOP, -49), (-61, MIN_Y)]
SEA_ROOF = [(TOP, -54), (-61, MIN_Y)]

spec = [
    dim("lava_floor", G1, "floor", "lava"),
    dim("lava_ceiling", G1, "ceiling", "lava"),
    dim("lava_water", G1, "water", "lava"),
    dim("lava_pocket_ceiling", G2, "ceiling", "lava", G2_MIN_Y),
    dim("water_floor", G1, "floor", "water"),
    dim("water_ceiling", G1, "ceiling", "water"),
    dim("water_water", G1, "water", "water"),
    dim("water_pocket_ceiling", G2, "ceiling", "water", G2_MIN_Y),
    dim("sea_open", SEA_OPEN, "floor", "air"),
    dim("sea_roof", SEA_ROOF, "floor", "air"),
    dim("sea_ceiling", SEA_ROOF, "ceiling", "air"),
    dim("sea_water", SEA_OPEN, "water", "air"),
]

json.dump(spec, open(sys.argv[1], "w"), indent=1)
print(len(spec), "dimensions", file=sys.stderr)
PY

if [[ -n "${spec_only}" ]]; then
    cp "${spec}" "${spec_only}"
    printf '==> wrote %s\n' "${spec_only}" >&2
    exit 0
fi

exec tools/analysis/density-probe.sh \
    $([[ ${accept_eula} -eq 1 ]] && printf -- --accept-eula) \
    --spec "${spec}" --seed "${seed}"
