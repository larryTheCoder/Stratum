#!/usr/bin/env bash
# Stratum — measure any density function from the vanilla server itself.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/density-probe.sh --accept-eula --spec <spec.json> [--seed N]
#
# THE TRICK. A datapack gives a dimension the entire `final_density`
#
#     K * flat_cache(<the function under test>) + y_clamped_gradient(+1 .. -1)
#
# and nothing else that can place a block. `flat_cache` pins the function to
# y = 0 for the whole column, so the only thing varying with height is the
# gradient, and the surface sits exactly where K*F + g(y) = 0. Invert g and
# every column's terrain height *is* a reading of F(x, 0, z), taken from
# Mojang's own binary. That is how old_blended_noise was confirmed, and it
# generalises to every node with no documentation and no oracle.
#
# WHY A SPEC FILE. One server start costs minutes and one dimension costs
# almost nothing, so a sweep goes in ONE world as many dimensions rather than
# as many worlds. The spec is a JSON array of {"name": ..., "function": ...};
# each entry becomes a dimension, and each dimension's region lands under
# .fixtures/<version>/probes/<spec name>/<entry name>/r.0.0.mca.
#
# Two things the first version of this got wrong, both of which read as a
# disagreement rather than as a mistake:
#   * Only CELL CORNERS carry the function's own value. final_density is
#     evaluated on the cell lattice and interpolated across it, and flat_cache
#     pins its argument to the 4x4 column corner as well.
#   * The biome must have NO carvers and NO features. minecraft:plains carves
#     caves through the terrain and puts lakes on top of it.
#
# Nothing this writes is committed: worlds and their regions are Mojang-derived
# (SPEC §12).
set -euo pipefail

readonly MINECRAFT_VERSION="1.21.11"
readonly K="0.35"          # scales the function into the gradient's range
readonly MIN_Y=-64
readonly HEIGHT=384
readonly CHUNKS=8          # per dimension, squared: 8 -> 128x128 blocks

accept_eula=0
spec=""
seed=42
while [[ $# -gt 0 ]]; do
    case "$1" in
        --accept-eula) accept_eula=1; shift ;;
        --spec)        spec="${2:?--spec needs a file}"; shift 2 ;;
        --seed)        seed="${2:?--seed needs a number}"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${repo_root}"
log() { printf '==> %s\n' "$*" >&2; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

[[ ${accept_eula} -eq 1 ]] || die "this runs the Minecraft server, so it needs --accept-eula"
[[ -n "${spec}" && -f "${spec}" ]] || die "--spec must name a readable JSON file"
command -v java >/dev/null 2>&1 || die "java is needed"
command -v python3 >/dev/null 2>&1 || die "python3 is needed"

spec_name="$(basename "${spec}" .json)"
jar="$(find "${repo_root}/.fixtures/${MINECRAFT_VERSION}" -maxdepth 2 -name 'server*.jar' | head -1)"
[[ -n "${jar}" ]] || die "no server jar under .fixtures/${MINECRAFT_VERSION}; run tools/fetch-vanilla first"
jar="$(cd "$(dirname "${jar}")" && pwd)/$(basename "${jar}")"

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
server="${work}/server"
pack="${server}/world/datapacks/stratum-probe/data/stratum"
mkdir -p "${pack}/dimension" "${pack}/worldgen/noise_settings" "${pack}/worldgen/biome" \
         "${pack}/worldgen/noise"

cat > "${server}/world/datapacks/stratum-probe/pack.mcmeta" <<'META'
{"pack": {"pack_format": 94, "description": "stratum density probe"}}
META

# No carvers, no features. Without this the probe reads the wrong thing.
cat > "${pack}/worldgen/biome/probe.json" <<'BIOME'
{
  "temperature": 0.8, "downfall": 0.4, "has_precipitation": false,
  "effects": {"sky_color": 7907327, "fog_color": 12638463,
              "water_color": 4159204, "water_fog_color": 329011},
  "spawners": {}, "spawn_costs": {}, "carvers": [], "features": []
}
BIOME

# A noise a probe can name when it needs one it can also compute here. One
# octave, so nothing about it is in doubt.
cat > "${pack}/worldgen/noise/probe_noise.json" <<'NOISE'
{"firstOctave": -3, "amplitudes": [1.0]}
NOISE

log "writing dimensions from ${spec}"
count="$(env SPEC="${spec}" PACK="${pack}" PROBE_K="${K}" PROBE_MIN_Y="${MIN_Y}" \
    PROBE_HEIGHT="${HEIGHT}" python3 - <<'PY'
import json, os, pathlib
spec = json.load(open(os.environ['SPEC']))
pack = pathlib.Path(os.environ['PACK'])
k = float(os.environ['PROBE_K'])
min_y = int(os.environ['PROBE_MIN_Y'])
height = int(os.environ['PROBE_HEIGHT'])
names = set()
for entry in spec:
    name = entry['name']
    if name in names:
        raise SystemExit(f'duplicate probe name: {name}')
    names.add(name)
    (pack / 'dimension' / f'{name}.json').write_text(json.dumps({
        'type': 'minecraft:overworld',
        'generator': {
            'type': 'minecraft:noise',
            'settings': f'stratum:{name}',
            'biome_source': {'type': 'minecraft:fixed', 'biome': 'stratum:probe'},
        },
    }))
    (pack / 'worldgen' / 'noise_settings' / f'{name}.json').write_text(json.dumps({
        'sea_level': min_y,
        'disable_mob_generation': True,
        'aquifers_enabled': False,
        'ore_veins_enabled': False,
        'legacy_random_source': False,
        'default_block': {'Name': 'minecraft:stone'},
        'default_fluid': {'Name': 'minecraft:air'},
        'noise': {'min_y': min_y, 'height': height,
                  'size_horizontal': 1, 'size_vertical': 1},
        'spawn_target': [],
        'surface_rule': {'type': 'minecraft:block',
                         'result_state': {'Name': 'minecraft:stone'}},
        'noise_router': {
            'barrier': 0, 'fluid_level_floodedness': 0, 'fluid_level_spread': 0,
            'lava': 0, 'temperature': 0, 'vegetation': 0, 'continents': 0,
            'erosion': 0, 'depth': 0, 'ridges': 0, 'preliminary_surface_level': 0,
            'vein_toggle': 0, 'vein_ridged': 0, 'vein_gap': 0,
            'final_density': {
                'type': 'minecraft:add',
                'argument1': {'type': 'minecraft:mul', 'argument1': k,
                              'argument2': {'type': 'minecraft:flat_cache',
                                            'argument': entry['function']}},
                'argument2': {'type': 'minecraft:y_clamped_gradient',
                              'from_y': min_y, 'to_y': min_y + height,
                              'from_value': 1.0, 'to_value': -1.0},
            },
        },
    }))
print(len(spec))
PY
)"
log "${count} dimension(s)"

printf 'eula=true\n' > "${server}/eula.txt"
cat > "${server}/server.properties" <<PROPERTIES
level-seed=${seed}
level-name=world
online-mode=false
server-port=0
max-players=1
view-distance=10
simulation-distance=10
generate-structures=false
spawn-monsters=false
spawn-npcs=false
spawn-animals=false
sync-chunk-writes=true
max-tick-time=-1
motd=stratum density probe
PROPERTIES

pipe="${work}/console"
mkfifo "${pipe}"
log "starting the server (seed ${seed})"
( cd "${server}" && java -Xmx6G -jar "${jar}" --nogui < "${pipe}" > "${work}/server.log" 2>&1 ) &
server_pid=$!
exec 3> "${pipe}"

waited=0
while (( waited < 180 )); do
    grep -q 'Done (' "${work}/server.log" 2>/dev/null && break
    kill -0 "${server_pid}" 2>/dev/null || { tail -30 "${work}/server.log" >&2; die "server died at startup"; }
    sleep 5; waited=$((waited + 5))
done
grep -q 'Done (' "${work}/server.log" || { tail -30 "${work}/server.log" >&2; die "server did not start"; }

mapfile -t names < <(python3 -c "
import json,sys
for e in json.load(open('${spec}')): print(e['name'])
")

log "forceloading ${CHUNKS}x${CHUNKS} chunks in each of ${#names[@]} dimension(s)"
last=$(( CHUNKS * 16 - 1 ))
for name in "${names[@]}"; do
    printf 'execute in stratum:%s run forceload add 0 0 %d %d\n' "${name}" "${last}" "${last}" >&3
done

expected="${#names[@]}"
waited=0; stable=0; previous=""
while (( waited < 3600 )); do
    kill -0 "${server_pid}" 2>/dev/null || { tail -30 "${work}/server.log" >&2; die "server died during generation"; }
    printf 'save-all flush\n' >&3 || true
    sleep 15; waited=$((waited + 15))
    sizes="$(for name in "${names[@]}"; do
        f="${server}/world/dimensions/stratum/${name}/region/r.0.0.mca"
        [[ -f "${f}" ]] && wc -c < "${f}" || echo -
    done | tr '\n' ' ')"
    present="$(tr ' ' '\n' <<< "${sizes}" | grep -c '^[0-9]' || true)"
    if [[ "${present}" -eq "${expected}" && "${sizes}" == "${previous}" ]]; then
        stable=$((stable + 1)); (( stable >= 2 )) && break
    else
        stable=0
    fi
    previous="${sizes}"
    log "  ${present}/${expected} region(s) present after ${waited}s"
done
(( stable >= 2 )) || { printf 'stop\n' >&3; die "regions did not settle within 3600s"; }

printf 'stop\n' >&3
exec 3>&-
wait "${server_pid}" 2>/dev/null || true

out_root="${repo_root}/.fixtures/${MINECRAFT_VERSION}/probes/${spec_name}"
mkdir -p "${out_root}"
for name in "${names[@]}"; do
    mkdir -p "${out_root}/${name}"
    cp "${server}/world/dimensions/stratum/${name}/region/r.0.0.mca" "${out_root}/${name}/r.0.0.mca"
done
cat > "${out_root}/manifest.json" <<MANIFEST
{"seed": ${seed}, "k": ${K}, "min_y": ${MIN_Y}, "height": ${HEIGHT},
 "chunks": ${CHUNKS}, "version": "${MINECRAFT_VERSION}",
 "probe_noise": {"id": "stratum:probe_noise", "first_octave": -3, "amplitudes": [1.0]}}
MANIFEST
cp "${spec}" "${out_root}/spec.json"
log "wrote ${expected} region(s) under ${out_root}"
