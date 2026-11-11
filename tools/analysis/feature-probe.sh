#!/usr/bin/env bash
# Stratum — feature probes: vanilla's own terrain, every biome's features
# replaced by a variant's own, so what the decoration stage does can be read
# off the blocks it leaves.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/feature-probe.sh --accept-eula --variant <name> [seed]
#
# The variants, and what each isolates, are in feature-probe-pack.py, which
# writes the pack; the corpus is `feat<variant>_s<seed>`. A variant's
# redefinitions are kept beside its regions, under `overrides/` in the layout
# of the worldgen tree, so the test builds the same pack.
#
# WHY. M6's parity bar (the owner, 2026-11-11): the decoration framework —
# its seeds, the order features run in, the placement modifiers — exact, each
# feature type exact where a clean-room spec exists and statistical where
# none does. Nothing documents the framework to the bit, so it is measured:
# a marker feature writes one block behind the modifiers under test, and
# where the block lands is what they drew.
#
# One server start; in the overworld, two 8x8 blocks of forceloaded chunks:
#
#   pos  origin chunk (8, 8), in r.0.0
#   neg  origin chunk (-24, -24), in r.-1.-1, every coordinate negative
#
# A test reads the chunks stored at status `minecraft:full`: those ran their
# own features and had every neighbour's written into them. The world is
# frozen (`/tick freeze`) before anything generates, as every probe here is.
# Nothing this writes is committed: worlds are Mojang-derived (SPEC §12).
set -euo pipefail

readonly MINECRAFT_VERSION="1.21.11"
readonly CHUNKS=8

accept_eula=0
variant=""
args=()
while (( $# > 0 )); do
    case "$1" in
        --accept-eula) accept_eula=1 ;;
        --variant)
            [[ $# -ge 2 ]] || { echo "--variant needs a name" >&2; exit 2; }
            variant="$2"; shift ;;
        *) args+=("$1") ;;
    esac
    shift
done
[[ "${variant}" =~ ^[a-z0-9]+$ ]] || { echo "--variant <name> is needed (see feature-probe-pack.py)" >&2; exit 2; }
corpus_prefix="feat${variant}"
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

corpus="${corpus_prefix}_s${seed}"
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
motd=stratum feature probe
PROPERTIES

worldgen="${fixtures}/${MINECRAFT_VERSION}/worldgen"
[[ -d "${worldgen}/biome" ]] || die "no biomes under ${worldgen}; run tools/fetch-vanilla first"
pack="${server}/world/datapacks/stratum-feature-probe"
mkdir -p "${pack}/data/minecraft/worldgen"
cat > "${pack}/pack.mcmeta" <<META
{"pack": {"description": "stratum feature probe: ${variant}",
          "min_format": [94, 1], "max_format": 94}}
META
overrides="${work}/overrides"
log "datapack: $(python3 "${repo_root}/tools/analysis/feature-probe-pack.py" "${variant}" "${worldgen}" "${overrides}")"
cp -r "${overrides}/." "${pack}/data/minecraft/worldgen/"

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

# A pack the server could not load is dropped, and the world generates from
# the shipped data alone: that would be a golden of the wrong thing.
if grep -E -i -q 'failed to load datapacks|safe mode' "${work}/server.log"; then
    grep -E -i 'datapack|safe mode|error' "${work}/server.log" | head -40 >&2
    printf 'stop\n' >&3
    die "the server did not load this probe's datapack (variant ${variant})"
fi
printf 'datapack list enabled\n' >&3
sleep 2
grep -q 'file/stratum-feature-probe' "${work}/server.log" \
    || { printf 'stop\n' >&3; die "the probe's datapack is not among the enabled ones"; }

log "freezing the world before generating"
printf 'tick freeze\n' >&3
sleep 2
grep -q -i 'frozen' "${work}/server.log" || { printf 'stop\n' >&3; die "'tick freeze' was not acknowledged"; }

# placement : origin chunk x : origin chunk z : region x : region z
placements=("pos:8:8:0:0" "neg:-24:-24:-1:-1")
for dimension in overworld; do
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

expected=${#placements[@]}
waited=0; stable=0; previous=""
while (( waited < 3600 )); do
    kill -0 "${server_pid}" 2>/dev/null || { tail -30 "${work}/server.log" >&2; die "server died during generation"; }
    printf 'save-all flush\n' >&3 || true
    sleep 15; waited=$((waited + 15))
    sizes="$(for dimension in overworld; do
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
mkdir -p "${out_root}/overworld"
for placement in "${placements[@]}"; do
    IFS=':' read -r _ _ _ rx rz <<< "${placement}"
    cp "$(region_path overworld "${rx}" "${rz}")" "${out_root}/overworld/"
done
cp -r "${overrides}" "${out_root}/overrides"
cat > "${out_root}/manifest.json" <<MANIFEST
{"seed": ${seed}, "version": "${MINECRAFT_VERSION}", "chunks": ${CHUNKS},
 "variant": "${variant}",
 "placements": [{"name": "pos", "origin_chunk": [8, 8], "region": [0, 0]},
                {"name": "neg", "origin_chunk": [-24, -24], "region": [-1, -1]}],
 "dimensions": ["overworld"], "ticks_frozen": true,
 "stage": "chunks stored at status minecraft:full",
 "datapack": "every biome's features replaced by the variant's (overrides/)"}
MANIFEST
log "wrote ${expected} region(s) under ${out_root}"
