#!/usr/bin/env bash
# Stratum — the End where its golden regions cannot reach.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/end-islands-probe.sh --accept-eula [--seeds 0,42] \
#       [--regions "-1,-1 64,0"]
#
# THE QUESTION. `minecraft:end_islands` is now implemented and is exact
# against every golden End region on disk (golden_end_test.cpp, eight seeds).
# But those regions are all r.0.0 — blocks 0..511 on both axes — and that
# leaves two parts of the field with no oracle behind them at all:
#
#   * THE OUTER ISLANDS AND THEIR SEEDING. The outer term is gated on
#     `cellX^2 + cellZ^2 > 4096`, with the cell coordinate being
#     `blockX / 16`. Even the far corner of the +/-12 cell neighbourhood a
#     column can be reached from only gets to 43^2 * 2 = 3698 inside r.0.0.
#     The term cannot fire there once — so the simplex gate, the `9..21`
#     steepness hash and above all the SEEDING had no oracle at all. The
#     seeding was carried over from `BlendedNoise::legacyFromWorldSeed` by
#     analogy; the region this script generates is what showed the analogy
#     was WRONG (SPEC §11), and end-islands-field-probe.sh is what replaced
#     it with a measurement.
#   * NEGATIVE COORDINATES. `blockX / 8` and the `/2` and `%2` under it are
#     floorDiv/floorMod in this build, per CLAUDE.md's determinism rule.
#     Java's own `/` and `%` truncate toward zero, and the two disagree for
#     every negative coordinate. r.0.0 is entirely non-negative, so it cannot
#     tell the readings apart.
#
# Region 64,0 covers blocks 32768..33279 — well past the 1024-block gate, so
# the outer term fires throughout it; region -1,-1 covers blocks -512..-1, and
# its HIGH corner (chunks -8..-1) is where the central island reaches, which
# is what exercises the negative-coordinate readings. Its low corner is 512
# blocks out and holds nothing at all. Both regions together are what closes
# `end_islands`.
#
# WHAT IT WRITES, AND WHAT IT WILL NOT TOUCH. New files only, under
# .fixtures/<version>/probes/end-islands/seed-<seed>/r.X.Z.mca. It never
# extracts, never removes and never rewrites anything else under .fixtures —
# the worldgen tree it reads is read-only here, unlike tools/fetch-vanilla,
# which re-extracts it. A region that already exists is left alone and
# skipped, so a second run costs nothing and cannot clobber a first.
#
# Its world is generated the same way the goldens are: vanilla's own End, the
# server's own defaults, structures off, and a generated datapack emptying
# every biome's carvers and features so nothing but terrain and surface rules
# reaches the blocks. Read back by tests/conformance/golden_end_test.cpp,
# which skips when the probe has not been run.
#
# Nothing it writes is committed: worlds are Mojang-derived (SPEC §12).
set -euo pipefail

readonly MINECRAFT_VERSION="1.21.11"
readonly PACK_FORMAT_MAJOR=94
readonly PACK_FORMAT_MINOR=1
readonly MARGIN=4

accept_eula=0
seeds="0,42"
regions="-1,-1 64,0"
generation_timeout=2400
while [[ $# -gt 0 ]]; do
    case "$1" in
        --accept-eula) accept_eula=1; shift ;;
        --seeds)       seeds="${2:?--seeds needs a list}"; shift 2 ;;
        --regions)     regions="${2:?--regions needs a list}"; shift 2 ;;
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
command -v jq   >/dev/null 2>&1 || die "jq is needed to build the terrain-only datapack"

fixtures="${repo_root}/.fixtures/${MINECRAFT_VERSION}"
worldgen_dir="${fixtures}/worldgen"
[[ -d "${worldgen_dir}/biome" ]] || die "no worldgen tree at ${worldgen_dir}; run tools/fetch-vanilla"
jar="$(find "${fixtures}" -maxdepth 2 -name 'server*.jar' | head -1)"
[[ -n "${jar}" ]] || die "no server jar under ${fixtures}; run tools/fetch-vanilla first"
jar="$(cd "$(dirname "${jar}")" && pwd)/$(basename "${jar}")"

for region in ${regions}; do
    [[ "${region}" =~ ^-?[0-9]+,-?[0-9]+$ ]] \
        || die "malformed region coordinate '${region}': expected X,Z"
done

out_root="${fixtures}/probes/end-islands"
work="$(mktemp -d)"
server_pid=""
cleanup() {
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

# /forceload refuses more than 256 chunks at a time, so a region goes in as a
# grid of 16x16-chunk commands.
emit_forceload() {
    local from_x="$1" from_z="$2" to_x="$3" to_z="$4"
    local cx cz
    for (( cz = from_z; cz <= to_z; cz += 16 )); do
        for (( cx = from_x; cx <= to_x; cx += 16 )); do
            local end_x=$(( cx + 15 < to_x ? cx + 15 : to_x ))
            local end_z=$(( cz + 15 < to_z ? cz + 15 : to_z ))
            printf 'execute in minecraft:the_end run forceload add %d %d %d %d\n' \
                $(( cx * 16 )) $(( cz * 16 )) $(( end_x * 16 + 15 )) $(( end_z * 16 + 15 ))
        done
    done
}

generate_for_seed() {
    local seed="$1"
    local wanted=()
    local region
    for region in ${regions}; do
        local target="${out_root}/seed-${seed}/r.${region%%,*}.${region##*,}.mca"
        if [[ -f "${target}" ]]; then
            log "seed ${seed}: r.${region%%,*}.${region##*,} already present, leaving it alone"
            continue
        fi
        wanted+=("${region}")
    done
    if [[ ${#wanted[@]} -eq 0 ]]; then
        return 0
    fi

    local server="${work}/server-${seed}"
    local world="${server}/world"
    mkdir -p "${world}/datapacks/stratum-terrain-only/data/minecraft/worldgen/biome"
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
motd=stratum end-islands probe
PROPERTIES

    local pack="${world}/datapacks/stratum-terrain-only"
    cat > "${pack}/pack.mcmeta" <<PACK
{
  "pack": {
    "description": "Stratum end-islands probe: carvers and features removed",
    "min_format": [${PACK_FORMAT_MAJOR}, ${PACK_FORMAT_MINOR}],
    "max_format": ${PACK_FORMAT_MAJOR}
  }
}
PACK
    local biome
    for biome in "${worldgen_dir}"/biome/*.json; do
        jq '.features = [range(.features | length) | []] | .carvers = []' \
            "${biome}" > "${pack}/data/minecraft/worldgen/biome/$(basename "${biome}")"
    done

    local pipe="${work}/console-${seed}"
    local server_log="${server}/server.log"
    rm -f "${pipe}"
    mkfifo "${pipe}"
    log "seed ${seed}: starting the server"
    ( cd "${server}" && java -Xmx3G -jar "${jar}" --nogui \
        < "${pipe}" > "${server_log}" 2>&1 ) &
    server_pid=$!
    exec 3> "${pipe}"

    local waited=0
    until grep -q 'Done (' "${server_log}" 2>/dev/null; do
        sleep 2
        waited=$((waited + 2))
        if ! kill -0 "${server_pid}" 2>/dev/null; then
            exec 3>&-
            tail -20 "${server_log}" >&2
            die "seed ${seed}: the server died before it finished starting"
        fi
        (( waited < 600 )) || { exec 3>&-; die "seed ${seed}: the server never started"; }
    done

    # The world ticks while chunks generate, and a ticking End has nothing to
    # flow but the goldens froze ticks anyway; the same is done here so the
    # probe and the goldens are produced under identical conditions.
    printf 'gamerule randomTickSpeed 0\n' >&3
    printf 'gamerule doFireTick false\n' >&3

    local expected=()
    for region in "${wanted[@]}"; do
        local rx="${region%%,*}" rz="${region##*,}"
        log "seed ${seed}: forceloading the End r.${rx}.${rz} (margin ${MARGIN})"
        emit_forceload $(( rx * 32 - MARGIN )) $(( rz * 32 - MARGIN )) \
                       $(( rx * 32 + 31 + MARGIN )) $(( rz * 32 + 31 + MARGIN )) >&3
        expected+=("${world}/DIM1/region/r.${rx}.${rz}.mca")
    done

    log "seed ${seed}: generating (timeout ${generation_timeout}s)"
    local stable=0 previous="" sizes="" all_present
    waited=0
    while (( waited < generation_timeout )); do
        kill -0 "${server_pid}" 2>/dev/null || { exec 3>&-; tail -20 "${server_log}" >&2;
            die "seed ${seed}: the server died during generation"; }
        printf 'save-all flush\n' >&3 || true
        sleep 10
        waited=$((waited + 10))
        all_present=1
        sizes=""
        local file
        for file in "${expected[@]}"; do
            if [[ -f "${file}" ]]; then sizes+="$(wc -c < "${file}") "; else all_present=0; sizes+="- "; fi
        done
        if [[ ${all_present} -eq 1 && "${sizes}" == "${previous}" ]]; then
            stable=$((stable + 1))
            (( stable < 2 )) || break
        else
            stable=0
        fi
        previous="${sizes}"
    done
    (( stable >= 2 )) || { printf 'stop\n' >&3; exec 3>&-; die "seed ${seed}: regions never settled"; }
    log "seed ${seed}: generation settled after ${waited}s"

    printf 'stop\n' >&3
    exec 3>&-
    local stopping=0
    while kill -0 "${server_pid}" 2>/dev/null; do
        sleep 2
        stopping=$((stopping + 2))
        (( stopping < 120 )) || { kill "${server_pid}" 2>/dev/null || true; break; }
    done
    wait "${server_pid}" 2>/dev/null || true
    server_pid=""

    mkdir -p "${out_root}/seed-${seed}"
    for region in "${wanted[@]}"; do
        local rx="${region%%,*}" rz="${region##*,}"
        local source="${world}/DIM1/region/r.${rx}.${rz}.mca"
        [[ -f "${source}" ]] || die "seed ${seed}: ${source} was never written"
        # Never overwrite: a concurrent run may have produced it meanwhile.
        if [[ -f "${out_root}/seed-${seed}/r.${rx}.${rz}.mca" ]]; then
            log "seed ${seed}: r.${rx}.${rz} appeared while generating; keeping the existing one"
            continue
        fi
        cp "${source}" "${out_root}/seed-${seed}/r.${rx}.${rz}.mca"
        log "seed ${seed}: collected r.${rx}.${rz}.mca ($(wc -c < "${source}") bytes)"
    done
    rm -rf "${server}"
}

log "probing the End at regions ${regions} for seeds ${seeds}"
for seed in ${seeds//,/ }; do
    generate_for_seed "${seed}"
done
log "done. Probe regions under ${out_root} — do not commit them."
