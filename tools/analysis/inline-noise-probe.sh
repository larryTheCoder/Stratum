#!/usr/bin/env bash
# Stratum — what the vanilla server does with a noise written inline.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/inline-noise-probe.sh --accept-eula [--variants a,b,c] [seed]
#
# mcdoc declares every density-function `noise` field as
# `#[id="worldgen/noise"] string | NoiseParameters`, so the parameters may be
# written in place of the identifier. A named noise is seeded from the MD5 of
# its identifier; an inline one has no identifier, and what seeds it was the
# open question in SPEC §11. This asks the only authority there is.
#
# THE ANSWER IS THAT THERE IS NO SEEDING. The first thing to establish was
# whether the server takes the pack at all, and it does not: it loads the JSON
# without complaint and then dies building the world, with
#
#     java.util.NoSuchElementException: No value present
#             at java.base/java.util.Optional.orElseThrow
#
# thrown out of the pass that turns a dimension's noise router into its random
# state. So the plan of reading terrain back out of a generated region — the
# trick tools/analysis/blended-datapack-probe.sh uses — never applies: there is
# no terrain, and no world. This script is what is left of that plan: a matrix
# of one-variable-at-a-time packs, each started once, each verdict recorded.
#
# The variants, all identical but for how one noise is spelled:
#
#   named_noise      noise: "minecraft:ridge"                      (control)
#   named_own        noise: "stratum:ridge_copy", same parameters  (control)
#   unreferenced     named dimension, plus a worldgen/density_function file
#                    holding an inline noise that no router reaches
#   inline_noise     inline under `noise`'s own `noise` field
#   inline_shifted_noise, inline_shift, inline_shift_a, inline_shift_b,
#   inline_weird_scaled_sampler
#                    the other five fields SPEC §11 lists as taking the union
#   inline_unused    inline in `barrier`, which this dimension never samples
#                    because it has aquifers off
#   inline_other_parameters
#                    different parameters, in case the first set was the
#                    problem rather than the spelling
#
# The verdicts, and the exact density function that produced each, are written
# to .fixtures/<version>/probes/inline-noise/verdicts.json, which
# tests/conformance/vanilla_inline_noise_test.cpp replays through this build.
# Like every fixture it is Mojang-derived and is never committed (SPEC §12).
set -euo pipefail

readonly MINECRAFT_VERSION="1.21.11"
readonly PACK_FORMAT_MAJOR=94
readonly PACK_FORMAT_MINOR=1
readonly MIN_Y=-64
readonly HEIGHT=384
readonly STARTUP_TIMEOUT=180

# minecraft:ridge and minecraft:offset, written out rather than referenced so
# the probe does not depend on vanilla's files staying put. They are kept
# beside the names that point at them: the controls only mean anything if the
# copy really is identical.
# The recording carries this under `minecraft:ridge` as well as under
# `stratum:ridge_copy`, so that a replay of it needs no vanilla data: what is
# under test is how the noise is *spelled*, and a reference resolves or does
# not resolve regardless of the numbers on the other end of it.
readonly RIDGE='{"firstOctave": -7, "amplitudes": [1.0, 2.0, 1.0, 0.0, 0.0, 0.0]}'
readonly OFFSET='{"firstOctave": -3, "amplitudes": [1.0, 1.0, 1.0, 0.0]}'

readonly ALL_VARIANTS="named_noise,named_own,unreferenced,inline_noise,inline_shifted_noise,inline_shift,inline_shift_a,inline_shift_b,inline_weird_scaled_sampler,inline_unused,inline_other_parameters"

accept_eula=0
variants_arg=""
args=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --accept-eula) accept_eula=1; shift ;;
        --variants)    variants_arg="${2:?--variants needs a list}"; shift 2 ;;
        *)             args+=("$1"); shift ;;
    esac
done
seed="${args[0]:-42}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${repo_root}"

log() { printf '==> %s\n' "$*" >&2; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

[[ ${accept_eula} -eq 1 ]] || die "this runs the Minecraft server, so it needs --accept-eula"
command -v java >/dev/null 2>&1 || die "java is needed"
command -v jq >/dev/null 2>&1 || die "jq is needed"

# STRATUM_FIXTURES_DIR is the same knob the CMake cache carries, so a
# checkout that keeps its fixtures elsewhere — a git worktree sharing one
# 300MB download, say — does not need a second copy of the server jar.
fixtures="${STRATUM_FIXTURES_DIR:-${repo_root}/.fixtures}"
jar="$(find "${fixtures}/${MINECRAFT_VERSION}" -maxdepth 2 -name 'server*.jar' 2>/dev/null | head -1)"
[[ -n "${jar}" ]] || die "no server jar under ${fixtures}/${MINECRAFT_VERSION}; run tools/fetch-vanilla first"
jar="$(cd "$(dirname "${jar}")" && pwd)/$(basename "${jar}")"

work="$(mktemp -d)"
# PROBE_KEEP=1 keeps every server directory, which is where the full log and
# the crash report are; the summary below quotes only the first lines of them.
[[ -n "${PROBE_KEEP:-}" ]] || trap 'rm -rf "${work}"' EXIT
log "servers under ${work}"

# --- the one density function each variant differs in ----------------------
#
# Each is what goes under the dimension's `flat_cache`, except inline_unused,
# which puts it in `barrier` and leaves a named noise under the flat_cache.
sampled_for() {
    case "$1" in
        named_noise)   printf '{"type": "minecraft:noise", "noise": "minecraft:ridge", "xz_scale": 1.0, "y_scale": 0.0}' ;;
        named_own|unreferenced)
                       printf '{"type": "minecraft:noise", "noise": "stratum:ridge_copy", "xz_scale": 1.0, "y_scale": 0.0}' ;;
        inline_noise)  printf '{"type": "minecraft:noise", "noise": %s, "xz_scale": 1.0, "y_scale": 0.0}' "${RIDGE}" ;;
        inline_other_parameters)
                       printf '{"type": "minecraft:noise", "noise": %s, "xz_scale": 1.0, "y_scale": 0.0}' "${OFFSET}" ;;
        inline_shifted_noise)
                       printf '{"type": "minecraft:shifted_noise", "noise": %s, "shift_x": 0.0, "shift_y": 0.0, "shift_z": 0.0, "xz_scale": 1.0, "y_scale": 0.0}' "${RIDGE}" ;;
        inline_shift)  printf '{"type": "minecraft:shift", "argument": %s}' "${RIDGE}" ;;
        inline_shift_a) printf '{"type": "minecraft:shift_a", "argument": %s}' "${RIDGE}" ;;
        inline_shift_b) printf '{"type": "minecraft:shift_b", "argument": %s}' "${RIDGE}" ;;
        inline_weird_scaled_sampler)
                       printf '{"type": "minecraft:weird_scaled_sampler", "input": 0.0, "noise": %s, "rarity_value_mapper": "type_1"}' "${RIDGE}" ;;
        inline_unused) printf '{"type": "minecraft:noise", "noise": "minecraft:ridge", "xz_scale": 1.0, "y_scale": 0.0}' ;;
        *) die "unknown variant '$1'" ;;
    esac
}

# Only inline_unused puts anything but a constant in `barrier`.
barrier_for() {
    if [[ "$1" == "inline_unused" ]]; then
        printf '{"type": "minecraft:noise", "noise": %s, "xz_scale": 1.0, "y_scale": 0.0}' "${RIDGE}"
    else
        printf '0'
    fi
}

# --- one pack, one dimension, one difference -------------------------------
build_pack() {
    local variant="$1" server="$2"
    local pack="${server}/world/datapacks/stratum-probe/data/stratum"
    mkdir -p "${pack}/dimension" "${pack}/worldgen/noise_settings" "${pack}/worldgen/biome" \
             "${pack}/worldgen/noise" "${pack}/worldgen/density_function"

    # min_format/max_format, not pack_format: at this pack format the server
    # answers pack_format with "missing mandatory fields min_format and
    # max_format" and marks the pack incompatible, which is a second variable
    # this experiment does not want.
    cat > "${server}/world/datapacks/stratum-probe/pack.mcmeta" <<META
{
  "pack": {
    "description": "stratum inline noise probe",
    "min_format": [${PACK_FORMAT_MAJOR}, ${PACK_FORMAT_MINOR}],
    "max_format": ${PACK_FORMAT_MAJOR}
  }
}
META

    # A biome with no carvers and no features: nothing here reads terrain, but
    # a biome that generates caves and lakes would add reasons for a chunk to
    # fail that have nothing to do with the question.
    cat > "${pack}/worldgen/biome/probe.json" <<'BIOME'
{
  "temperature": 0.8,
  "downfall": 0.4,
  "has_precipitation": false,
  "effects": {
    "sky_color": 7907327,
    "fog_color": 12638463,
    "water_color": 4159204,
    "water_fog_color": 329011
  },
  "spawners": {},
  "spawn_costs": {},
  "carvers": [],
  "features": []
}
BIOME

    printf '%s\n' "${RIDGE}" > "${pack}/worldgen/noise/ridge_copy.json"

    if [[ "${variant}" == "unreferenced" ]]; then
        sampled_for inline_noise > "${pack}/worldgen/density_function/unreferenced.json"
    fi

    cat > "${pack}/dimension/probe.json" <<'DIM'
{
  "type": "minecraft:overworld",
  "generator": {
    "type": "minecraft:noise",
    "settings": "stratum:probe",
    "biome_source": {"type": "minecraft:fixed", "biome": "stratum:probe"}
  }
}
DIM

    cat > "${pack}/worldgen/noise_settings/probe.json" <<SETTINGS
{
  "sea_level": ${MIN_Y},
  "disable_mob_generation": true,
  "aquifers_enabled": false,
  "ore_veins_enabled": false,
  "legacy_random_source": false,
  "default_block": {"Name": "minecraft:stone"},
  "default_fluid": {"Name": "minecraft:air"},
  "noise": {"min_y": ${MIN_Y}, "height": ${HEIGHT}, "size_horizontal": 1, "size_vertical": 1},
  "spawn_target": [],
  "surface_rule": {"type": "minecraft:block", "result_state": {"Name": "minecraft:stone"}},
  "noise_router": {
    "barrier": $(barrier_for "${variant}"),
    "fluid_level_floodedness": 0,
    "fluid_level_spread": 0,
    "lava": 0,
    "temperature": 0,
    "vegetation": 0,
    "continents": 0,
    "erosion": 0,
    "depth": 0,
    "ridges": 0,
    "preliminary_surface_level": 0,
    "vein_toggle": 0,
    "vein_ridged": 0,
    "vein_gap": 0,
    "final_density": {
      "type": "minecraft:add",
      "argument1": {
        "type": "minecraft:flat_cache",
        "argument": $(sampled_for "${variant}")
      },
      "argument2": {
        "type": "minecraft:y_clamped_gradient",
        "from_y": ${MIN_Y}, "to_y": $((MIN_Y + HEIGHT)),
        "from_value": 1.0, "to_value": -1.0
      }
    }
  }
}
SETTINGS

    printf 'eula=true\n' > "${server}/eula.txt"
    cat > "${server}/server.properties" <<PROPERTIES
level-seed=${seed}
level-name=world
online-mode=false
server-port=0
max-players=1
view-distance=2
simulation-distance=2
generate-structures=false
spawn-monsters=false
spawn-npcs=false
spawn-animals=false
max-tick-time=-1
motd=stratum inline noise probe
PROPERTIES
}

# Starts a server on the pack and echoes "accepted" or "refused". Nothing is
# generated: the question is whether the world can be built at all, and both
# answers are reached within seconds of startup.
run_variant() {
    local variant="$1"
    local server="${work}/${variant}"
    rm -rf "${server}"
    mkdir -p "${server}/world"
    build_pack "${variant}" "${server}"

    local pipe="${server}/console"
    mkfifo "${pipe}"
    ( cd "${server}" && java -Xmx2G -jar "${jar}" --nogui < "${pipe}" > "${server}/server.log" 2>&1 ) &
    local pid=$!
    exec 3> "${pipe}"

    local started=0 waited=0
    while (( waited < STARTUP_TIMEOUT )); do
        if grep -q 'Done (' "${server}/server.log" 2>/dev/null; then started=1; break; fi
        kill -0 "${pid}" 2>/dev/null || break
        sleep 2; waited=$((waited + 2))
    done

    if [[ ${started} -eq 1 ]]; then
        printf 'stop\n' >&3
    fi
    exec 3>&-
    wait "${pid}" 2>/dev/null || true

    [[ ${started} -eq 1 ]] && echo accepted || echo refused
}

exception_of() {
    grep -m1 -A2 'Encountered an unexpected exception' "${work}/$1/server.log" 2>/dev/null |
        tail -n +2 | head -1 | sed 's/^[[:space:]]*//' || true
}

frame_of() {
    # The first frame below the exception that is not the JDK's own. Every
    # variant failing in the same one is the evidence that this is one place
    # in the server that cannot proceed, not a handful of separate refusals.
    grep -A6 'Encountered an unexpected exception' "${work}/$1/server.log" 2>/dev/null |
        grep -E '^[[:space:]]+at ' | grep -v 'java\.base/' | head -1 |
        sed 's/^[[:space:]]*at //' || true
}

IFS=',' read -r -a variants <<< "${variants_arg:-${ALL_VARIANTS}}"

entries="[]"
for variant in "${variants[@]}"; do
    log "${variant}: starting the server"
    verdict="$(run_variant "${variant}")"
    exception=""
    frame=""
    if [[ "${verdict}" == "refused" ]]; then
        exception="$(exception_of "${variant}")"
        frame="$(frame_of "${variant}")"
        log "${variant}: REFUSED — ${exception:-(no exception logged; see the log)}"
    else
        log "${variant}: accepted"
    fi

    entries="$(jq -n \
        --argjson so_far "${entries}" \
        --arg name "${variant}" \
        --arg verdict "${verdict}" \
        --arg exception "${exception}" \
        --arg frame "${frame}" \
        --argjson density_function "$(sampled_for "${variant}")" \
        --argjson barrier "$(barrier_for "${variant}")" \
        --argjson referenced "$([[ "${variant}" == "unreferenced" ]] && echo false || echo true)" \
        --argjson unreferenced_function "$([[ "${variant}" == "unreferenced" ]] && sampled_for inline_noise || echo null)" \
        --argjson noises "{\"minecraft:ridge\": ${RIDGE}, \"stratum:ridge_copy\": ${RIDGE}}" \
        '$so_far + [{name: $name, verdict: $verdict, exception: $exception, frame: $frame,
                     density_function: $density_function, barrier: $barrier,
                     referenced: $referenced, unreferenced_function: $unreferenced_function,
                     noises: $noises}]')"
done

out="${PROBE_OUT:-${fixtures}/${MINECRAFT_VERSION}/probes/inline-noise}"
mkdir -p "${out}"
jq -n --arg version "${MINECRAFT_VERSION}" --argjson seed "${seed}" --argjson variants "${entries}" \
    '{version: $version, seed: $seed, note: "Mojang-derived; never commit (SPEC 12)", variants: $variants}' \
    > "${out}/verdicts.json"
log "wrote ${out}/verdicts.json"
jq -r '.variants[] | "  \(.name): \(.verdict)  \(.exception)"' "${out}/verdicts.json" >&2
log "now run: ctest --preset conformance -R inline_noise"
