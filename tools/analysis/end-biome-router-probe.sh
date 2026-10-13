#!/usr/bin/env bash
# Stratum — does the End's biome source read the noise router?
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/end-biome-router-probe.sh --accept-eula [--seed 42] \
#       [--region -2,-2]
#
# THE QUESTION. The End's dimension declares `"biome_source": {"type":
# "minecraft:the_end"}`, which takes no parameters (world_preset/normal.json),
# and the End's noise settings route `cache_2d(end_islands)` into the router's
# `erosion` and the constant 0 into every other climate entry. Whether that
# biome source reads the router — so a datapack that changes the End's
# `erosion` changes its biome map — or carries an island field of its own
# that no datapack can reach decides what a compiled End takes its biomes
# from. Nothing documents which. This asks the server.
#
# THE EXPERIMENT. One world, one seed, one server start. Every dimension below
# has the_end's biome source and a copy of the pinned End noise settings
# (worldgen/noise_settings/end.json) with ONE change to its noise router:
#
#   ctl            none: the control, a verbatim copy
#   erosion_<v>    erosion the constant v (m = minus, p = plus, _ = the
#                  point), swept from -0.5 to 0.5. Three of the values are
#                  the exact binary fractions -0.21875, -0.0625 and 0.25,
#                  each with neighbours 1e-5 and 1e-10 away on one side, so
#                  wherever the map changes at one of them the sweep says
#                  which side that value falls on, and whether a difference
#                  a float cannot hold still counts
#   erosion_ygrad  erosion y_clamped_gradient(y 0 -> -1, y 128 -> +1) with no
#                  cache_2d: a value that depends on the y it is read at, so
#                  the map says whether the source reads it per quart or at
#                  one height, and at which
#   erosion_ystep_<y>  the same over one block, y -> y + 1 (m1 = -1): -1 at
#                  y and below, +1 from y + 1. Steps at -1, 0, 3 and 4 put
#                  the y the bottom two quarts are read at to the block
#   climate        temperature, vegetation, continents, depth and ridges
#                  non-zero constants; erosion untouched
#   terrain        final_density the constant -1, so no block at all;
#                  erosion untouched
#
# and the world's own minecraft:the_end, forceloaded over the same region in
# the same world, collected as `vanilla`: a second control the copy has to
# equal block for block and biome for biome.
#
# Every carver and feature is emptied from every biome, as in
# end-islands-probe.sh; ticks are frozen before any chunk generates (SPEC §7).
# The region is whole: forceloaded edge to edge, and collected only once
# every one of its 1024 chunks is at full status. The default, r.-2.-2,
# straddles the 1024-block ring where both coordinates are negative: chunk
# (-33,-33) is inside it, (-64,-33) outside.
#
# WHAT IT FOUND (1.21.11, seed 42, r.-2.-2; two runs identical in every
# block and quart cell of all 23 regions). The source reads the router's
# `erosion`, and nothing else in it:
#   * Each erosion constant turns all 725 chunks outside the ring into ONE
#     biome; the 299 inside it (chunkX^2 + chunkZ^2 <= 4096) stay the_end
#     whatever erosion says. `climate` and `terrain` change 0 of 524288
#     quart cells, and no erosion variant changes a block. `ctl` and
#     `vanilla` agree in every block and cell.
#   * Edges, held to 1e-10 and so read in double, not float: above 0.25
#     end_highlands (0.25 itself is midlands), from -0.0625 end_midlands,
#     from -0.21875 end_barrens, below it small_end_islands.
#   * Erosion is read per quart, at block y = 4 * quartY: with a y-dependent
#     erosion the biome varies with y, the steps put quart 0 at y 0 and
#     quart 1 at y 4, and the gradient's 13/2/6/11 quarts per bin are what
#     y = 4 * quartY gives against those edges.
#
# WHAT IT WRITES. probes/end-biome-router/seed-<seed>/<name>/r.X.Z.mca for
# each dimension, a manifest.json (seed, region, ticks_frozen) and
# variants.json (each variant's router change, as written into its settings),
# under STRATUM_FIXTURES_DIR when that is set, else .fixtures —
# tools/probe-worlds clears it for every generator it runs. An existing
# seed-<seed> corpus is refused, never replaced. STRATUM_PROBE_XMX sets the
# server's heap (default 3G). Nothing it writes is committed: worlds are
# Mojang-derived (SPEC §12).
set -euo pipefail

readonly MINECRAFT_VERSION="1.21.11"
readonly PACK_FORMAT_MAJOR=94
readonly PACK_FORMAT_MINOR=1

accept_eula=0
seed=42
region="-2,-2"
generation_timeout=2400
while [[ $# -gt 0 ]]; do
    case "$1" in
        --accept-eula) accept_eula=1; shift ;;
        --seed)        seed="${2:?--seed needs a number}"; shift 2 ;;
        --region)      region="${2:?--region needs X,Z}"; shift 2 ;;
        --timeout)     generation_timeout="${2:?--timeout needs seconds}"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${repo_root}"
log() { printf '==> %s\n' "$*" >&2; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

[[ ${accept_eula} -eq 1 ]] || die "this runs the Minecraft server, so it needs --accept-eula"
command -v java >/dev/null 2>&1 || die "java is needed"
command -v python3 >/dev/null 2>&1 || die "python3 is needed to write the datapack"
[[ "${seed}" =~ ^-?[0-9]+$ ]] || die "--seed takes a whole number"
[[ "${region}" =~ ^-?[0-9]+,-?[0-9]+$ ]] || die "malformed --region '${region}': expected X,Z"
region_x="${region%%,*}"
region_z="${region##*,}"
region_file="r.${region_x}.${region_z}.mca"

fixtures="${STRATUM_FIXTURES_DIR:-${repo_root}/.fixtures}/${MINECRAFT_VERSION}"
worldgen_dir="${fixtures}/worldgen"
[[ -f "${worldgen_dir}/noise_settings/end.json" && -d "${worldgen_dir}/biome" ]] \
    || die "no worldgen tree at ${worldgen_dir}; run tools/fetch-vanilla"
# `|| true`: under pipefail a find over a directory that is not there fails
# the assignment, and `set -e` would exit here without the message below.
jar="$(find "${fixtures}" -maxdepth 2 -name 'server*.jar' 2>/dev/null | head -1 || true)"
[[ -n "${jar}" ]] || die "no server jar under ${fixtures}; run tools/fetch-vanilla first"
jar="$(cd "$(dirname "${jar}")" && pwd)/$(basename "${jar}")"

out_dir="${fixtures}/probes/end-biome-router/seed-${seed}"
[[ ! -e "${out_dir}" ]] || die "${out_dir} already exists; this never replaces a corpus"
probe_xmx="${STRATUM_PROBE_XMX:-3G}"

work="$(mktemp -d)"
server_pid=""
staging=""
cleanup() {
    if [[ -n "${staging}" ]]; then rm -rf "${staging}"; fi
    if [[ -n "${server_pid}" ]] && kill -0 "${server_pid}" 2>/dev/null; then
        kill "${server_pid}" 2>/dev/null || true
        for _ in $(seq 1 15); do
            kill -0 "${server_pid}" 2>/dev/null || break
            sleep 1
        done
        kill -9 "${server_pid}" 2>/dev/null || true
    fi
    rm -rf "${work}"
}
trap cleanup EXIT INT TERM

server="${work}/server"
world="${server}/world"
pack="${world}/datapacks/stratum-end-router"
mkdir -p "${pack}"
cat > "${pack}/pack.mcmeta" <<PACK
{
  "pack": {
    "description": "Stratum End biome-source router probe",
    "min_format": [${PACK_FORMAT_MAJOR}, ${PACK_FORMAT_MINOR}],
    "max_format": ${PACK_FORMAT_MAJOR}
  }
}
PACK

# The variants, the datapack, and variants.json, in one place. The names are
# printed one per line for the forceload and collection below.
mapfile -t names < <(env WORLDGEN="${worldgen_dir}" PACK="${pack}" python3 - <<'PY'
import copy, json, os, pathlib

worldgen = pathlib.Path(os.environ['WORLDGEN'])
pack = pathlib.Path(os.environ['PACK'])

# Every vanilla biome with its carvers and features emptied.
biome_dir = pack / 'data' / 'minecraft' / 'worldgen' / 'biome'
biome_dir.mkdir(parents=True)
for path in sorted((worldgen / 'biome').glob('*.json')):
    body = json.loads(path.read_text())
    body['features'] = [[] for _ in body.get('features', [])]
    body['carvers'] = []
    (biome_dir / path.name).write_text(json.dumps(body))

end = json.loads((worldgen / 'noise_settings' / 'end.json').read_text())
variants = {'ctl': {}}
# Written as decimals and parsed, so each settings file carries exactly the
# number its name spells (json.dumps gives a double's shortest round trip).
for text in ('-0.5', '-0.21876', '-0.2187500001', '-0.21875', '-0.1', '-0.06251',
             '-0.0625000001', '-0.0625', '0', '0.1', '0.25', '0.2500000001', '0.25001',
             '0.5'):
    sign = 'm' if text.startswith('-') else 'p' if text != '0' else ''
    variants['erosion_' + sign + text.lstrip('-').replace('.', '_')] = {'erosion': float(text)}
gradient = {'type': 'minecraft:y_clamped_gradient', 'from_value': -1.0, 'to_value': 1.0}
variants['erosion_ygrad'] = {'erosion': dict(gradient, from_y=0, to_y=128)}
for low in (-1, 0, 3, 4):
    variants[f'erosion_ystep_{"m" if low < 0 else ""}{abs(low)}'] = {
        'erosion': dict(gradient, from_y=low, to_y=low + 1)}
variants['climate'] = {'temperature': 0.7, 'vegetation': -0.7, 'continents': 0.7,
                       'depth': 0.7, 'ridges': -0.7}
variants['terrain'] = {'final_density': -1.0}

settings_dir = pack / 'data' / 'stratum' / 'worldgen' / 'noise_settings'
dimension_dir = pack / 'data' / 'stratum' / 'dimension'
settings_dir.mkdir(parents=True)
dimension_dir.mkdir(parents=True)
for name, change in variants.items():
    unknown = set(change) - set(end['noise_router'])
    if unknown:
        raise SystemExit(f'{name}: the End router has no {sorted(unknown)}')
    settings = copy.deepcopy(end)
    settings['noise_router'].update(change)
    (settings_dir / f'{name}.json').write_text(json.dumps(settings))
    (dimension_dir / f'{name}.json').write_text(json.dumps({
        'type': 'minecraft:the_end',
        'generator': {'type': 'minecraft:noise',
                      'biome_source': {'type': 'minecraft:the_end'},
                      'settings': f'stratum:{name}'},
    }))
(pack.parent.parent / 'variants.json').write_text(json.dumps(variants, indent=1) + '\n')
for name in variants:
    print(name)
PY
)
(( ${#names[@]} > 0 )) || die "no variants were written"
log "${#names[@]} variant dimension(s) plus the world's own End, seed ${seed}, ${region_file}"

printf 'eula=true\n' > "${server}/eula.txt"
cat > "${server}/server.properties" <<PROPERTIES
level-seed=${seed}
level-name=world
online-mode=false
server-port=0
max-players=1
spawn-protection=0
view-distance=10
simulation-distance=10
generate-structures=false
spawn-monsters=false
spawn-npcs=false
spawn-animals=false
sync-chunk-writes=true
enable-command-block=false
max-tick-time=-1
motd=stratum end biome-source router probe
PROPERTIES

pipe="${work}/console"
server_log="${server}/server.log"
mkfifo "${pipe}"
log "starting the server"
# `exec` so the subshell BECOMES java and the trap's kill reaches the JVM.
( cd "${server}" && exec java "-Xmx${probe_xmx}" -jar "${jar}" --nogui \
    < "${pipe}" > "${server_log}" 2>&1 ) &
server_pid=$!
exec 3> "${pipe}"

waited=0
until grep -q 'Done (' "${server_log}" 2>/dev/null; do
    sleep 2
    waited=$((waited + 2))
    kill -0 "${server_pid}" 2>/dev/null || { tail -30 "${server_log}" >&2; die "the server died at startup"; }
    (( waited < 600 )) || die "the server never started"
done

log "freezing the world before generating"
printf 'tick freeze\n' >&3
sleep 2
grep -q -i 'frozen' "${server_log}" || die "'tick freeze' was not acknowledged"

# How many of a region file's 1024 chunks are absent, unreadable or short of
# full status (end-islands-probe.sh says why the file's size cannot).
unfinished_chunks() {
    python3 -c '
import re, sys, zlib
data = open(sys.argv[1], "rb").read()
status = re.compile(rb"\x08\x00\x06Status(..)", re.S)
left = 0
for index in range(1024):
    location = int.from_bytes(data[index * 4:index * 4 + 4], "big")
    start = (location >> 8) * 4096
    if location == 0 or start + 5 > len(data):
        left += 1
        continue
    length = int.from_bytes(data[start:start + 4], "big")
    kind, raw = data[start + 4], data[start + 5:start + 4 + length]
    try:
        payload = zlib.decompress(raw, 47) if kind == 1 else zlib.decompress(raw) if kind == 2 else raw
    except zlib.error:
        left += 1
        continue
    found = status.search(payload)
    size = int.from_bytes(found.group(1), "big") if found else 0
    if not found or payload[found.end():found.end() + size] != b"minecraft:full":
        left += 1
print(left)
' "$1"
}

# /forceload refuses more than 256 chunks at a time: 16x16-chunk commands.
forceload_region() {
    local dimension="$1" cx cz
    for (( cz = region_z * 32; cz < region_z * 32 + 32; cz += 16 )); do
        for (( cx = region_x * 32; cx < region_x * 32 + 32; cx += 16 )); do
            printf 'execute in %s run forceload add %d %d %d %d\n' "${dimension}" \
                $(( cx * 16 )) $(( cz * 16 )) $(( cx * 16 + 255 )) $(( cz * 16 + 255 ))
        done
    done
}

# Where the server writes each collected dimension's region.
source_of() {
    if [[ "$1" == vanilla ]]; then
        printf '%s\n' "${world}/DIM1/region/${region_file}"
    else
        printf '%s\n' "${world}/dimensions/stratum/$1/region/${region_file}"
    fi
}
for name in "${names[@]}"; do
    forceload_region "stratum:${name}" >&3
done
forceload_region "minecraft:the_end" >&3
collected=("${names[@]}" vanilla)

log "generating (timeout ${generation_timeout}s)"
stable=0
previous=""
waited=0
while (( waited < generation_timeout )); do
    kill -0 "${server_pid}" 2>/dev/null || { tail -30 "${server_log}" >&2; die "the server died during generation"; }
    printf 'save-all flush\n' >&3 || true
    sleep 10
    waited=$((waited + 10))
    sizes=""
    present=0
    for name in "${collected[@]}"; do
        file="$(source_of "${name}")"
        if [[ -f "${file}" ]]; then
            sizes+="$(wc -c < "${file}") "
            present=$((present + 1))
        else
            sizes+="- "
        fi
    done
    if (( present == ${#collected[@]} )) && [[ "${sizes}" == "${previous}" ]]; then
        stable=$((stable + 1))
        if (( stable >= 2 )); then
            left=0
            for name in "${collected[@]}"; do
                left=$(( left + $(unfinished_chunks "$(source_of "${name}")") ))
            done
            (( left == 0 )) && break
            log "  sizes steady but ${left} chunk(s) not yet at full status"
            stable=0
        fi
    else
        stable=0
    fi
    previous="${sizes}"
    log "  ${present}/${#collected[@]} region(s) present after ${waited}s"
done
(( stable >= 2 )) || die "the regions never settled within ${generation_timeout}s"
log "generation settled after ${waited}s"

printf 'stop\n' >&3
exec 3>&-
stopping=0
while kill -0 "${server_pid}" 2>/dev/null; do
    sleep 2
    stopping=$((stopping + 2))
    (( stopping < 120 )) || { kill "${server_pid}" 2>/dev/null || true; break; }
done
wait "${server_pid}" 2>/dev/null || true
server_pid=""

# Written beside the final directory and renamed into place, so a run that
# dies here leaves nothing a later run would have to refuse (the trap removes
# the staging directory until the rename).
staging="${out_dir}.partial.$$"
mkdir -p "${staging}"
for name in "${collected[@]}"; do
    mkdir -p "${staging}/${name}"
    cp "$(source_of "${name}")" "${staging}/${name}/${region_file}"
done
cp "${world}/variants.json" "${staging}/variants.json"
env SEED="${seed}" REGION="${region_file}" VERSION="${MINECRAFT_VERSION}" python3 -c '
import json, os, sys
names = sys.argv[2:]
manifest = {"seed": int(os.environ["SEED"]), "version": os.environ["VERSION"],
            "region": os.environ["REGION"], "margin": 0, "ticks_frozen": True,
            "dimensions": names}
with open(sys.argv[1], "w") as out:
    json.dump(manifest, out, indent=1)
    out.write("\n")
' "${staging}/manifest.json" "${collected[@]}"
[[ ! -e "${out_dir}" ]] || die "${out_dir} appeared while generating; leaving it alone"
mv "${staging}" "${out_dir}"
staging=""
log "wrote ${#collected[@]} region(s) under ${out_dir} — do not commit them."
