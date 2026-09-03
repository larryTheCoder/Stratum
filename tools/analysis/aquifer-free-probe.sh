#!/usr/bin/env bash
# Stratum — vanilla's own overworld, generated with aquifers turned off.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/aquifer-free-probe.sh --accept-eula [seed]
#
# WHY THIS EXISTS. A chunk's OCEAN_FLOOR heightmap is the highest block that is
# neither air nor fluid, and comparing it against `final_density` only measures
# the density chain if nothing ELSE places blocks. Vanilla's overworld runs
# aquifers, and an aquifer puts a stone barrier between two bodies of water at
# different levels — solid blocks no density function produced. Against those
# columns the comparison reads the barrier as terrain and blames the density.
#
# So this generates the same world from vanilla's own overworld noise settings
# with one field changed, `aquifers_enabled: false`, and the biome swapped for
# one with no carvers and no features. Everything else — every density
# function, every noise, the cell lattice — is vanilla's, byte for byte, read
# out of the extracted fixtures. What comes back is terrain whose OCEAN_FLOOR
# IS the density chain, and comparing against it measures this build's
# arithmetic rather than the aquifer step it has not implemented.
#
# Measured on seed -1 over 4096 columns: with aquifers, 96.167% exact and a
# worst error of 50 blocks; without, 98.267% exact, every column within one
# block, worst error 1.
#
# Nothing it writes is committed: it is a world, and worlds are Mojang-derived
# (SPEC §12).
set -euo pipefail

readonly MINECRAFT_VERSION="1.21.11"
readonly CHUNKS=8

accept_eula=0
args=()
for arg in "$@"; do
    case "${arg}" in
        --accept-eula) accept_eula=1 ;;
        *) args+=("${arg}") ;;
    esac
done
seed="${args[0]:--1}"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${repo_root}"
log() { printf '==> %s\n' "$*" >&2; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

[[ ${accept_eula} -eq 1 ]] || die "this runs the Minecraft server, so it needs --accept-eula"
command -v java >/dev/null 2>&1 || die "java is needed"
settings=".fixtures/${MINECRAFT_VERSION}/worldgen/noise_settings/overworld.json"
[[ -f "${settings}" ]] || die "no ${settings}; run tools/fetch-vanilla first"
jar="$(find ".fixtures/${MINECRAFT_VERSION}" -maxdepth 2 -name 'server*.jar' | head -1)"
[[ -n "${jar}" ]] || die "no server jar; run tools/fetch-vanilla first"
jar="$(cd "$(dirname "${jar}")" && pwd)/$(basename "${jar}")"

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
server="${work}/server"
pack="${server}/world/datapacks/stratum-no-aquifer/data/stratum"
mkdir -p "${pack}/dimension" "${pack}/worldgen/noise_settings" "${pack}/worldgen/biome"

cat > "${server}/world/datapacks/stratum-no-aquifer/pack.mcmeta" <<'META'
{"pack": {"pack_format": 94, "description": "stratum aquifer-free overworld"}}
META

cat > "${pack}/worldgen/biome/probe.json" <<'BIOME'
{
  "temperature": 0.8, "downfall": 0.4, "has_precipitation": false,
  "effects": {"sky_color": 7907327, "fog_color": 12638463,
              "water_color": 4159204, "water_fog_color": 329011},
  "spawners": {}, "spawn_costs": {}, "carvers": [], "features": []
}
BIOME

log "copying vanilla's overworld settings with aquifers off"
env SETTINGS="${settings}" PACK="${pack}" python3 - <<'PY'
import json, os, pathlib
pack = pathlib.Path(os.environ['PACK'])
s = json.load(open(os.environ['SETTINGS']))
# The one field under test. Ore veins go too: they only ever replace stone
# with stone, so they cannot move OCEAN_FLOOR, but leaving them on would make
# that a thing to argue about rather than a thing that is off.
s['aquifers_enabled'] = False
s['ore_veins_enabled'] = False
(pack / 'worldgen' / 'noise_settings' / 'no_aquifer.json').write_text(json.dumps(s))
(pack / 'dimension' / 'no_aquifer.json').write_text(json.dumps({
    'type': 'minecraft:overworld',
    'generator': {'type': 'minecraft:noise', 'settings': 'stratum:no_aquifer',
                  'biome_source': {'type': 'minecraft:fixed', 'biome': 'stratum:probe'}},
}))
PY

printf 'eula=true\n' > "${server}/eula.txt"
cat > "${server}/server.properties" <<PROPERTIES
level-seed=${seed}
level-name=world
online-mode=false
server-port=0
max-players=1
generate-structures=false
spawn-monsters=false
spawn-npcs=false
spawn-animals=false
sync-chunk-writes=true
max-tick-time=-1
motd=stratum aquifer-free probe
PROPERTIES

pipe="${work}/console"
mkfifo "${pipe}"
log "starting the server (seed ${seed})"
( cd "${server}" && java -Xmx4G -jar "${jar}" --nogui < "${pipe}" > "${work}/server.log" 2>&1 ) &
server_pid=$!
exec 3> "${pipe}"

waited=0
while (( waited < 180 )); do
    grep -q 'Done (' "${work}/server.log" 2>/dev/null && break
    kill -0 "${server_pid}" 2>/dev/null || { tail -30 "${work}/server.log" >&2; die "server died at startup"; }
    sleep 5; waited=$((waited + 5))
done
grep -q 'Done (' "${work}/server.log" || { tail -30 "${work}/server.log" >&2; die "server did not start"; }

log "forceloading ${CHUNKS}x${CHUNKS} chunks"
printf 'execute in stratum:no_aquifer run forceload add 0 0 %d %d\n' \
    "$(( CHUNKS * 16 - 1 ))" "$(( CHUNKS * 16 - 1 ))" >&3

region="${server}/world/dimensions/stratum/no_aquifer/region/r.0.0.mca"
waited=0; stable=0; previous=""
while (( waited < 900 )); do
    kill -0 "${server_pid}" 2>/dev/null || { tail -30 "${work}/server.log" >&2; die "server died"; }
    printf 'save-all flush\n' >&3 || true
    sleep 10; waited=$((waited + 10))
    size="$( [[ -f "${region}" ]] && wc -c < "${region}" || echo - )"
    if [[ "${size}" != "-" && "${size}" == "${previous}" ]]; then
        stable=$((stable + 1)); (( stable >= 2 )) && break
    else
        stable=0
    fi
    previous="${size}"
done
(( stable >= 2 )) || { printf 'stop\n' >&3; die "the region did not settle"; }

printf 'stop\n' >&3
exec 3>&-
wait "${server_pid}" 2>/dev/null || true

out="${repo_root}/.fixtures/${MINECRAFT_VERSION}/probes/no-aquifer/seed-${seed}"
mkdir -p "${out}"
cp "${region}" "${out}/r.0.0.mca"
log "wrote ${out}/r.0.0.mca"
log "now run: ctest --preset conformance -R 'without aquifers'"
