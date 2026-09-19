#!/usr/bin/env bash
# Stratum — where above_preliminary_surface's boundary actually is.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/aps-boundary-probe.sh --accept-eula [seed] [second-seed]
#
# WHY THIS EXISTS. This project carried `above_preliminary_surface` for two
# milestones as "the documented reading, `y >= preliminary_surface_level`,
# strictness unmeasured", on the strength of one probe that was recorded as
# having found the condition true everywhere. It had not. Re-read at the
# BOTTOM edge of the band it paints rather than at the terrain's top block —
# where a condition of this shape is true by construction — that same probe
# (`probes/surf`, entry `aps`) shows a clean stone/marker step in all 36864 of
# its columns, two to eight blocks BELOW the level the condition is named
# after. The question was never the strictness. It was the comparand.
#
# THE READOUT. Each dimension's `raw_final_density` is the constant 1, so the
# column is solid from the world floor to its roof: no terrain height, no air,
# no fluid, no aquifer, no carvers, no features (the probe biome has none).
# The entire surface rule is
#
#     { condition: above_preliminary_surface, then_run: diamond_block }
#
# so the marker band's LOWER EDGE is the condition's own boundary, at
# single-block resolution, in every column. The control that this is the
# condition turning false rather than the surface pass stopping is already on
# disk: the `bandlands` and `steep` entries of `probes/surf`, same density and
# same world, paint down to y = -64. That control is no longer prose — it is
# asserted, entry by entry, in
# tests/conformance/vanilla_above_preliminary_surface_test.cpp ("the band's
# lower edge is the condition going false, not the surface pass stopping"):
# `bandlands` 36864 of 36864 columns to the floor, `steep` 6217 of 6217, `aps`
# 0 of 36864 and its lower edges confined to -8..-2.
#
# THE LEVER. `density-probe.sh` pins every router entry to the constant 0
# unless an entry overrides it by name, which is why the earlier probe
# measured the boundary at exactly one `preliminary_surface_level` and could
# not have told a psl dependence from a fixed offset. The machinery for
# driving it is not new — `probes/kpsl` and `probes/pslvar` already drove this
# same entry as a constant and as a spatially varying `range_choice`, for the
# AQUIFER work. What had never been done is pointing a surface rule at it.
#
# THREE FAMILIES, one per question, and the third is the one that keeps the
# answer from being an accident of this probe's own shape:
#
#   apsb   (first seed)  19 constants from -60 to +200, on and off every
#                        lattice the world has, so a quantisation of psl onto
#                        cells would read as a staircase rather than a line;
#                        7 fractional levels, which separate FLOOR from
#                        truncation toward zero (only the negative ones can);
#                        3 over real terrain instead of a solid column, so
#                        terrain-independence is a controlled statement rather
#                        than a correlation; and one spatially VARYING psl.
#   apsb2  (second seed) a subset of the same, against the standing rule that
#                        a per-column field fitted at one seed is exactly the
#                        kind of thing that matches by luck.
#   apsb3  (first seed)  12 geometry variants — cell heights 4/8/16, cell
#                        widths 4/8/16, three floors, three heights, three sea
#                        levels — because a constant of 8 in a boundary is
#                        most likely to be cell geometry in disguise, and this
#                        is what rules that out.
#
# WHAT IT MEASURED (SPEC §11, and scored in
# tests/conformance/vanilla_above_preliminary_surface_test.cpp):
#
#     above_preliminary_surface(x, y, z)  ==  y >= psl + surfaceDepth(x, z) - 8
#
# with psl FLOORED, and the 8 a literal invariant under every geometry above.
# 52 dimensions in all: 19 constants + 7 fractions + 3 over terrain + 1 varying
# in apsb, 10 in apsb2, 12 in apsb3.
#
# WHAT IT DOES NOT MEASURE, and cannot: whether the depth carries a bottom
# clamp, i.e. `8 - surfaceDepth` against `8 - max(0, surfaceDepth)`. Those
# differ only where the RETURNED depth is negative, and no column of any of
# these 52 dimensions is — nor is any column of the golden regions: over every
# column of all eight (2097152) the returned depth's minimum is 0 and the raw
# value's is -0.449658, where the cast truncates to 0 either way.
#
# That is a statement about those columns and NOT about vanilla. The columns
# exist — `aps-boundary-analyze sweep` over the same eight seeds and
# x, z in [-16384, 16384), 8589934592 columns, finds 98 of them, 1 in
# 87652393 — they are simply nowhere near the world origin this probe
# forceloads. So the question is answered by a probe pointed at where they
# ARE: tools/analysis/aps-clamp-probe.sh, which is this same machinery with
# `density-probe.sh --origin-chunk` and three clusters at three seeds. The
# clamp AT 0 is REFUTED: 49 separating columns, lower edge
# `psl + surfaceDepth - 8` on every one. At 0 and no lower — all 49 are at
# depth exactly -1, and the sweep's lowest raw anywhere is -1.134416806, so a
# clamp at -1 or below predicts the same edge as none. Scored in the
# conformance file's "the surface depth carries no bottom clamp".
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
second_seed="${args[1]:-31337}"

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

python3 - "${work}" <<'PY'
import json, sys, os

work = sys.argv[1]

SOLID = {"type": "minecraft:constant", "argument": 1.0}
# The one entry that keeps a real terrain under the same rule, so that
# "the boundary does not read the terrain's own height" is measured rather
# than inferred from a correlation. Same shape density-probe.sh builds for a
# density probe, written out here because these entries take `raw_final_density`.
TERRAIN = {"type": "minecraft:add",
           "argument1": {"type": "minecraft:y_clamped_gradient",
                         "from_y": 40, "to_y": 72,
                         "from_value": 1.0, "to_value": -1.0},
           "argument2": {"type": "minecraft:mul",
                         "argument1": 0.8,
                         "argument2": {"type": "minecraft:noise",
                                       "noise": "stratum:probe_noise",
                                       "xz_scale": 1.0, "y_scale": 0.0}}}

MARK = {"type": "minecraft:condition",
        "if_true": {"type": "minecraft:above_preliminary_surface"},
        "then_run": {"type": "minecraft:block",
                     "result_state": {"Name": "minecraft:diamond_block"}}}


def entry(name, psl, density=None, **extra):
    probe = {"name": name,
             "raw_final_density": density if density is not None else SOLID,
             "surface_rule": MARK,
             "router": {"preliminary_surface_level": psl
                        if isinstance(psl, dict)
                        else {"type": "minecraft:constant", "argument": float(psl)}}}
    probe.update(extra)
    return probe


def tag(value):
    """k_m8, r_p3_9999 — the entry names the fixtures and the tests use."""
    return ("m" if value < 0 else "p") + str(abs(value)).replace(".", "_")


# A three-valued psl, the same nested range_choice probes/pslvar drives the
# aquifer with. Here it answers a different question: whether a psl that VARIES
# reaches the condition per column at all.
def varying(scale=1.0):
    noise = {"type": "minecraft:noise", "noise": "stratum:probe_noise",
             "xz_scale": scale, "y_scale": 0.0}
    return {"type": "minecraft:range_choice", "input": noise,
            "min_inclusive": -1000.0, "max_exclusive": -0.25, "when_in_range": -40.0,
            "when_out_of_range": {"type": "minecraft:range_choice", "input": noise,
                                  "min_inclusive": -1000.0, "max_exclusive": 0.25,
                                  "when_in_range": 0.0, "when_out_of_range": 60.0}}


# -60 puts the boundary through the world floor for most columns, which is the
# control that the band really is clipped by the world rather than by the rule.
CONSTANTS = [-60, -40, -33, -20, -9, -8, -7, -4, -1, 0, 1, 3, 4, 7, 16, 33, 64, 100, 200]
# Only the NEGATIVE fractions separate floor from truncation toward zero; the
# positive ones are controls, and a harness that scored them alone would report
# a clean pass for either.
FRACTIONS = [3.5, -3.5, 0.5, -0.5, 3.9999, -3.9999, -0.0001]

apsb = [entry("k_" + tag(k), k) for k in CONSTANTS]
apsb += [entry("r_" + tag(k), k) for k in FRACTIONS]
apsb += [entry("t_" + tag(k), k, density=TERRAIN) for k in (-20, 0, 40)]
apsb += [entry("v_psl", varying())]

apsb2 = [entry("k_" + tag(k), k) for k in (-40, -8, 0, 1, 7, 64)]
apsb2 += [entry("r_" + tag(k), k) for k in (0.5, -0.5)]
apsb2 += [entry("t_p0", 0, density=TERRAIN), entry("v_psl", varying())]

# A constant of 8 in a boundary is most likely to be cell geometry wearing a
# disguise, so every knob that could produce one is moved. min_y and height
# move together because density-probe.sh ships a dimension_type per entry only
# when they leave the default.
apsb3 = [entry("g_base", 100),
         entry("g_sv2", 100, size_vertical=2),
         entry("g_sv4", 100, size_vertical=4),
         entry("g_sh2", 100, size_horizontal=2),
         entry("g_sh4", 100, size_horizontal=4),
         entry("g_miny_m32", 100, min_y=-32, height=256),
         entry("g_miny_p0", 100, min_y=0, height=256),
         entry("g_h128", 100, min_y=-64, height=192),
         entry("g_sea63", 100, sea_level=63),
         entry("g_sea200", 100, sea_level=200),
         entry("g_sv2_sh2", 100, size_vertical=2, size_horizontal=2),
         entry("g_sv4b", 200, size_vertical=4)]

for name, spec in (("apsb", apsb), ("apsb2", apsb2), ("apsb3", apsb3)):
    with open(os.path.join(work, name + ".json"), "w") as out:
        json.dump(spec, out)
    print(name, len(spec), "dimensions", file=sys.stderr)
PY

eula=()
[[ ${accept_eula} -eq 1 ]] && eula+=(--accept-eula)

tools/analysis/density-probe.sh "${eula[@]}" --spec "${work}/apsb.json" --seed "${seed}"
tools/analysis/density-probe.sh "${eula[@]}" --spec "${work}/apsb2.json" --seed "${second_seed}"
tools/analysis/density-probe.sh "${eula[@]}" --spec "${work}/apsb3.json" --seed "${seed}"
