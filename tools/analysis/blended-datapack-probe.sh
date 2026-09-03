#!/usr/bin/env bash
# Stratum — measure old_blended_noise from the vanilla server itself.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/blended-datapack-probe.sh --accept-eula [seed]
#
# The seeding was settled against deepslate, which is an emulator. This settles
# it against Mojang's own binary, which SPEC §7 makes the authority, and it does
# so without asking the server for a number it has no way to report.
#
# THE TRICK. A datapack defines a dimension whose entire `final_density` is
#
#     K * flat_cache(old_blended_noise) + y_clamped_gradient(+1 .. -1)
#
# A block is placed where that is positive. `flat_cache` pins the noise to
# y = 0 for the whole column, so the only thing varying with height is the
# gradient, and the surface therefore sits exactly where
# K*N + g(y) = 0. Invert g and every column's terrain height *is* a reading of
# N(x, 0, z), taken from the server's own output. Aquifers, ore veins, carvers
# and features are all off, and the surface rule is one block, so nothing else
# can move a surface.
#
# Nothing this writes is committed: it is a world, and worlds are Mojang-derived
# (SPEC §12).
set -euo pipefail

readonly MINECRAFT_VERSION="1.21.11"
readonly K="1.5"          # scales N so it spans most of the y range
readonly MIN_Y=-64
readonly HEIGHT=384

accept_eula=0
args=()
for arg in "$@"; do
    case "${arg}" in
        --accept-eula) accept_eula=1 ;;
        *) args+=("${arg}") ;;
    esac
done
seed="${args[0]:-42}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${repo_root}"

log() { printf '==> %s\n' "$*" >&2; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

[[ ${accept_eula} -eq 1 ]] || die "this runs the Minecraft server, so it needs --accept-eula"
command -v java >/dev/null 2>&1 || die "java is needed"

jar="$(find "${repo_root}/.fixtures/${MINECRAFT_VERSION}" -maxdepth 2 -name 'server*.jar' | head -1)"
[[ -n "${jar}" ]] || die "no server jar under .fixtures/${MINECRAFT_VERSION}; run tools/fetch-vanilla first"
jar="$(cd "$(dirname "${jar}")" && pwd)/$(basename "${jar}")"

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
server="${work}/server"
pack="${server}/world/datapacks/stratum-probe/data/stratum"
mkdir -p "${pack}/dimension" "${pack}/worldgen/noise_settings" "${pack}/worldgen/biome"

cat > "${server}/world/datapacks/stratum-probe/pack.mcmeta" <<'META'
{"pack": {"pack_format": 94, "description": "stratum old_blended_noise probe"}}
META

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

# A biome with no carvers and no features. Without this the probe reads the
# wrong thing: minecraft:plains carves caves through the terrain and puts lakes
# and springs on top of it, and the first run of this probe duly found its
# outlier columns topped with water. fetch-vanilla strips both from the goldens
# for the same reason.
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

# The overworld's own base_3d_noise parameters, written out rather than
# referenced, so the probe does not depend on vanilla's file staying put.
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
    "barrier": 0,
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
        "type": "minecraft:mul",
        "argument1": ${K},
        "argument2": {
          "type": "minecraft:flat_cache",
          "argument": {
            "type": "minecraft:old_blended_noise",
            "xz_scale": 0.25, "y_scale": 0.125,
            "xz_factor": 80.0, "y_factor": 160.0,
            "smear_scale_multiplier": 8.0
          }
        }
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
view-distance=10
simulation-distance=10
generate-structures=false
spawn-monsters=false
spawn-npcs=false
spawn-animals=false
sync-chunk-writes=true
max-tick-time=-1
motd=stratum blended noise probe
PROPERTIES

pipe="${work}/console"
mkfifo "${pipe}"
log "starting the server (seed ${seed})"
( cd "${server}" && java -Xmx3G -jar "${jar}" --nogui < "${pipe}" > "${work}/server.log" 2>&1 ) &
server_pid=$!
exec 3> "${pipe}"

region="${server}/world/dimensions/stratum/probe/region/r.0.0.mca"
waited=0
while (( waited < 120 )); do
    grep -q 'Done (' "${work}/server.log" 2>/dev/null && break
    kill -0 "${server_pid}" 2>/dev/null || { tail -20 "${work}/server.log" >&2; die "server died at startup"; }
    sleep 5; waited=$((waited + 5))
done
grep -q 'Done (' "${work}/server.log" || { tail -20 "${work}/server.log" >&2; die "server did not start"; }

log "forceloading r.0.0 of stratum:probe"
# /forceload refuses more than 256 chunks at a time, so this goes in strips.
for cz in $(seq 0 4 28); do
    printf 'execute in stratum:probe run forceload add 0 %d 511 %d\n' \
        "$((cz * 16))" "$(((cz + 4) * 16 - 1))" >&3
done

waited=0; stable=0; previous=""
while (( waited < 600 )); do
    kill -0 "${server_pid}" 2>/dev/null || { tail -20 "${work}/server.log" >&2; die "server died"; }
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
(( stable >= 2 )) || { printf 'stop\n' >&3; die "the region did not settle in 600s"; }

printf 'stop\n' >&3
exec 3>&-
wait "${server_pid}" 2>/dev/null || true
log "generated $(wc -c < "${region}") bytes"

# The probe's output is a fixture like any other: Mojang-derived, never
# committed (SPEC §12), and read by the conformance suite when it is present.
# Generating it takes minutes and reading it takes none, so it is written where
# `ctest --preset conformance` will find it rather than thrown away.
out="${PROBE_OUT:-${repo_root}/.fixtures/${MINECRAFT_VERSION}/probes/blended-noise/seed-${seed}/r.0.0.mca}"
mkdir -p "$(dirname "${out}")"
cp "${region}" "${out}"
log "wrote ${out}"
log "now run: ctest --preset conformance -R 'blended noise the server itself'"
