#!/usr/bin/env bash
# Stratum — old_blended_noise's floating-point form, bisected on the server.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/blended-fpform-probe.sh --accept-eula
#
# WHY THIS EXISTS. IMPLEMENTER-RULING-01 (blended noise P2) asks for the
# floating-point forms spec/blended-noise-spec.md's amendments state, and its
# §2 admits a claim to lib/ only once a golden confirms it. The forms differ
# from cubiomes' order in the last bit, which no terrain height can show. So
# this reads the server's own value of minecraft:overworld/base_3d_noise at
# 24 points to the bit, as tools/analysis/final-density-probe.sh does: each
# threshold T is one dimension whose final density is
#
#     range_choice(base_3d_noise, [-1000, T) -> -1, else 1)
#
# with vanilla's own base_3d_noise read from the fixtures at run time, never
# re-typed. The block AT the point is then open (air, or water below the
# overworld's sea level 63) exactly when the server's value is below T. The
# points and thresholds are tests/conformance/blended_fpform_targets.inc,
# parsed here, so the script and the case that reads the corpus cannot drift.
# 88 dimensions, one server start, at seed 42 (the candidates are seed 42's).
#
# Read by tests/conformance/vanilla_blended_fpform_test.cpp. Nothing this
# writes is committed: worlds are Mojang-derived (SPEC §12).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${repo_root}"

accept_eula=0
for arg in "$@"; do
    case "${arg}" in
        --accept-eula) accept_eula=1 ;;
        *) echo "unknown argument: ${arg}" >&2; exit 2 ;;
    esac
done
readonly seed=42

fixtures="${STRATUM_FIXTURES_DIR:-${repo_root}/.fixtures}"
settings="${fixtures}/1.21.11/worldgen/noise_settings/overworld.json"
function_file="${fixtures}/1.21.11/worldgen/density_function/overworld/base_3d_noise.json"
for f in "${settings}" "${function_file}"; do
    [[ -f "${f}" ]] || { echo "error: no ${f}; run tools/fetch-vanilla first" >&2; exit 2; }
done

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
spec="${work}/blendedfp_s${seed}.json"

python3 - "${spec}" "${settings}" "${function_file}" \
    tests/conformance/blended_fpform_targets.inc <<'PY'
import json, re, sys

out, settings_path, function_path, table = sys.argv[1:]
overworld = json.load(open(settings_path))
noise = overworld['noise']

NUMBER = r'-?[0-9.eE+-]+'
LINE = re.compile(r'^\s*\{\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*\{([^}]*)\},\s*\{([^}]*)\}\},?\s*$')
targets = []
for line in open(table):
    if line.lstrip().startswith('//') or not line.strip():
        continue
    m = LINE.match(line)
    if not m:
        raise SystemExit(f'{table}: unparsed line: {line.rstrip()}')
    x, y, z = (int(m.group(i)) for i in (1, 2, 3))
    candidates = [float(v) for v in m.group(4).split(',')]
    thresholds = [float(v) for v in m.group(5).split(',')]
    assert len(candidates) == 8, line
    # repr round-trips a double exactly, so the threshold the server parses
    # is the one the case compares against.
    for v in thresholds:
        assert float(repr(v)) == v
    targets.append((x, y, z, thresholds))
assert len(targets) == 24, len(targets)

spec = []
for i, (x, y, z, thresholds) in enumerate(targets):
    for j, threshold in enumerate(thresholds):
        name = f"p{i}_{x}_{y}_{z}_{j}".replace('-', 'm')
        tree = json.load(open(function_path))  # fresh per dimension
        spec.append({
            "name": name,
            "raw_final_density": {"type": "minecraft:range_choice", "input": tree,
                                  "min_inclusive": -1000.0, "max_exclusive": threshold,
                                  "when_in_range": -1.0, "when_out_of_range": 1.0},
            "aquifers_enabled": False,
            "sea_level": overworld['sea_level'],
            "default_fluid": overworld['default_fluid'],
            "size_horizontal": noise['size_horizontal'],
            "size_vertical": noise['size_vertical'],
            "min_y": noise['min_y'],
            "height": noise['height'],
        })
json.dump(spec, open(out, 'w'))
print(len(spec), "dimensions across", len(targets), "points", file=sys.stderr)
PY

exec tools/analysis/density-probe.sh \
    $([[ ${accept_eula} -eq 1 ]] && printf -- --accept-eula) \
    --spec "${spec}" --seed "${seed}"
