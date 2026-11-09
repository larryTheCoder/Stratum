#!/usr/bin/env bash
# Stratum — carving-stage goldens: vanilla's own chunks as its carvers left
# them, before features and before promotion.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/carver-golden-probe.sh --accept-eula [seed]
#
# WHY THIS EXISTS. IMPLEMENTER-RULING-01 (carvers P1): carver goldens compare
# at the CARVING STAGE, not against full chunks, whose blocks are the carver's
# output plus a promotion step that runs marked fluid ticks; and they compare
# STORED block arrays (spec/carvers-a-spec.md Q12.7: a section holding only
# air-like blocks reads back as plain air through the game's own read).
#
# HOW. The server saves the chunks it generated around a forceloaded block in
# rings of decreasing status. Measured on this project's own probe worlds: a
# full core, then `initialize_light`, then `minecraft:carvers` three chunks
# out, then `biomes` and `structure_starts`. A chunk stored at status
# `minecraft:carvers` has run its carvers and nothing after them — no
# features, no promotion — so it IS the carving stage, read straight off disk
# (tests/conformance/vanilla_carver_golden_test.cpp selects by status).
#
# EXCEPT WHAT A NEIGHBOUR'S FEATURES WRITE INTO IT. A chunk's features may
# place blocks in the chunks around it, so the carvers ring, which borders
# chunks that ran their features, held ore blobs, seagrass and blackstone
# that no carver made (measured on the first run). So a generated datapack
# empties every biome's features and keeps its carvers, vanilla's own
# configured carvers untouched; nothing then writes into the ring but the
# stages before and including the carvers.
#
# One server start; in each of the overworld and the Nether, two 8x8 blocks:
#
#   pos  origin chunk (8, 8):     its carvers ring in r.0.0
#   neg  origin chunk (-24, -24): its carvers ring in r.-1.-1, where every
#                                 coordinate is negative (floorDiv, the reach
#                                 test's |dx|, the table sine's truncation)
#
# The world is frozen (`/tick freeze`) before anything generates, as every
# probe here is. Nothing this writes is committed: worlds are Mojang-derived
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
seed="${args[0]:-42}"
[[ "${seed}" =~ ^-?[0-9]+$ ]] || { echo "seed must be a whole number: ${seed}" >&2; exit 2; }

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${repo_root}"
log() { printf '==> %s\n' "$*" >&2; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

[[ ${accept_eula} -eq 1 ]] || die "this runs the Minecraft server, so it needs --accept-eula"
command -v java >/dev/null 2>&1 || die "java is needed"

fixtures="${STRATUM_FIXTURES_DIR:-${repo_root}/.fixtures}"
jar="$(find "${fixtures}/${MINECRAFT_VERSION}" -maxdepth 2 -name 'server*.jar' 2>/dev/null | head -1 || true)"
[[ -n "${jar}" ]] || die "no server jar under ${fixtures}/${MINECRAFT_VERSION}; run tools/fetch-vanilla first"
jar="$(cd "$(dirname "${jar}")" && pwd)/$(basename "${jar}")"

corpus="carvers_s${seed}"
out_root="${fixtures}/${MINECRAFT_VERSION}/probes/${corpus}"

work="$(mktemp -d)"
server_pid=""
cleanup() {
    if [[ -n "${server_pid}" ]] && kill -0 "${server_pid}" 2>/dev/null; then
        kill "${server_pid}" 2>/dev/null || true
        for _ in 1 2 3 4 5 6 7 8 9 10; do
            kill -0 "${server_pid}" 2>/dev/null || break
            sleep 1
        done
        kill -9 "${server_pid}" 2>/dev/null || true
    fi
    rm -rf "${work}"
}
trap cleanup EXIT INT TERM

server="${work}/server"
mkdir -p "${server}"
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
motd=stratum carving-stage goldens
PROPERTIES

worldgen="${fixtures}/${MINECRAFT_VERSION}/worldgen"
[[ -d "${worldgen}/biome" ]] || die "no biomes under ${worldgen}; run tools/fetch-vanilla first"
pack="${server}/world/datapacks/stratum-no-features"
mkdir -p "${pack}/data/minecraft/worldgen/biome"
cat > "${pack}/pack.mcmeta" <<META
{"pack": {"description": "stratum carving-stage goldens: features removed, carvers kept",
          "min_format": [94, 1], "max_format": 94}}
META
command -v jq >/dev/null 2>&1 || die "jq is needed"
for biome in "${worldgen}"/biome/*.json; do
    jq '.features = [range(.features | length) | []]' "${biome}" \
        > "${pack}/data/minecraft/worldgen/biome/$(basename "${biome}")"
done
log "datapack: $(ls "${pack}/data/minecraft/worldgen/biome" | wc -l) biome(s), features removed"

pipe="${work}/console"
mkfifo "${pipe}"
log "starting the server (seed ${seed})"
( cd "${server}" && exec java "-Xmx${STRATUM_PROBE_XMX:-6G}" -jar "${jar}" --nogui \
    < "${pipe}" > "${work}/server.log" 2>&1 ) &
server_pid=$!
exec 3> "${pipe}"

waited=0
while (( waited < 300 )); do
    grep -q 'Done (' "${work}/server.log" 2>/dev/null && break
    kill -0 "${server_pid}" 2>/dev/null || { tail -30 "${work}/server.log" >&2; die "server died at startup"; }
    sleep 5; waited=$((waited + 5))
done
grep -q 'Done (' "${work}/server.log" || { tail -30 "${work}/server.log" >&2; die "server did not start"; }

log "freezing the world before generating"
printf 'tick freeze\n' >&3
sleep 2
grep -q -i 'frozen' "${work}/server.log" || { printf 'stop\n' >&3; die "'tick freeze' was not acknowledged"; }

# placement : origin chunk x : origin chunk z : region x : region z
placements=("pos:8:8:0:0" "neg:-24:-24:-1:-1")
for dimension in overworld the_nether; do
    for placement in "${placements[@]}"; do
        IFS=':' read -r _ cx cz _ _ <<< "${placement}"
        printf 'execute in minecraft:%s run forceload add %d %d %d %d\n' "${dimension}" \
            $(( cx * 16 )) $(( cz * 16 )) $(( (cx + CHUNKS - 1) * 16 + 15 )) \
            $(( (cz + CHUNKS - 1) * 16 + 15 )) >&3
    done
done

region_path() {
    local dimension="$1" rx="$2" rz="$3"
    if [[ "${dimension}" == "overworld" ]]; then
        echo "${server}/world/region/r.${rx}.${rz}.mca"
    else
        echo "${server}/world/DIM-1/region/r.${rx}.${rz}.mca"
    fi
}

expected=$(( 2 * ${#placements[@]} ))
waited=0; stable=0; previous=""
while (( waited < 3600 )); do
    kill -0 "${server_pid}" 2>/dev/null || { tail -30 "${work}/server.log" >&2; die "server died during generation"; }
    printf 'save-all flush\n' >&3 || true
    sleep 15; waited=$((waited + 15))
    sizes="$(for dimension in overworld the_nether; do
        for placement in "${placements[@]}"; do
            IFS=':' read -r _ _ _ rx rz <<< "${placement}"
            f="$(region_path "${dimension}" "${rx}" "${rz}")"
            [[ -f "${f}" ]] && wc -c < "${f}" || echo -
        done
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
server_pid=""

rm -rf "${out_root}"
mkdir -p "${out_root}/overworld" "${out_root}/nether"
for placement in "${placements[@]}"; do
    IFS=':' read -r _ _ _ rx rz <<< "${placement}"
    cp "$(region_path overworld "${rx}" "${rz}")" "${out_root}/overworld/"
    cp "$(region_path the_nether "${rx}" "${rz}")" "${out_root}/nether/"
done
cat > "${out_root}/manifest.json" <<MANIFEST
{"seed": ${seed}, "version": "${MINECRAFT_VERSION}", "chunks": ${CHUNKS},
 "placements": [{"name": "pos", "origin_chunk": [8, 8], "region": [0, 0]},
                {"name": "neg", "origin_chunk": [-24, -24], "region": [-1, -1]}],
 "dimensions": ["overworld", "nether"], "ticks_frozen": true,
 "stage": "chunks stored at status minecraft:carvers",
 "datapack": "every biome's features emptied, carvers kept"}
MANIFEST
log "wrote ${expected} region(s) under ${out_root}"
