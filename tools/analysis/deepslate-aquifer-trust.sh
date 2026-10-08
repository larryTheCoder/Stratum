#!/usr/bin/env bash
# Stratum — is deepslate an oracle for the 1.21.11 aquifer? Measured, not assumed.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/deepslate-aquifer-trust.sh [chunks]
#
# Installs the pinned deepslate and scores it against probe corpora the server
# already wrote, so it starts no server:
#
#   0. Whether either entry point runs an aquifer at all: one chunk of c96,
#      re-run with the floodedness, the spread, the lava and the surface level
#      each moved, compared byte for byte with the unmoved run.
#   1. The position mix. Whether PositionalRandom.at is the documented mix
#      (SPEC §11) or the same formula left unwrapped, on fixed cells.
#   2. Blocks. The five open-void worlds (pslvar/c96 at seed 42, and
#      comb_<seed>/jv at 42, 7, 12345 and 999), over chunks [0,n)x[0,n),
#      through NoiseChunkGenerator.fill and through NoiseAquifer directly.
#      c96 is deepslate's world exactly. jv's barrier is vanilla's noise and
#      deepslate's is held at c96's constant -2, keeping the driver free of
#      Mojang data; at seed 42 that is inert (the server's c96 and jv hold the
#      same 155 630 stone blocks over chunks [0,8)x[0,8)), at the other seeds
#      unmeasured. Every disagreement is counted by category; the server's
#      own flowing-fluid blocks are the most that flow could explain.
#   3. The jitter readout of vanilla_aquifer_jitter_test.cpp, over chunks
#      [0,8)x[0,8) of the four comb worlds, for deepslate's at() and for the
#      documented mix fed to deepslate's own generator.
#
# Its figures are the ones tools/vectors/deepslate_aquifer_vectors.mjs and
# SPEC §11 quote. Reads Mojang-derived corpora under .fixtures (never
# committed, SPEC §12); writes nothing outside a temporary directory.
set -euo pipefail

readonly DEEPSLATE_VERSION="0.26.2"
chunks="${1:-4}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
probes="${STRATUM_FIXTURES_DIR:-${repo_root}/.fixtures}/1.21.11/probes"

for tool in node npm python3; do
    command -v "${tool}" >/dev/null 2>&1 || { echo "error: ${tool} is needed" >&2; exit 2; }
done
python3 -c 'import numpy' 2>/dev/null || { echo "error: numpy is needed" >&2; exit 2; }
for corpus in pslvar/c96 comb_42/jv comb_7/jv comb_12345/jv comb_999/jv; do
    [[ -f "${probes}/${corpus}/r.0.0.mca" ]] || {
        echo "error: no ${probes}/${corpus}/r.0.0.mca; generate it with" \
             "tools/analysis/aquifer-psl-probe.sh or aquifer-comb-probe.sh" >&2
        exit 2
    }
done

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

echo "==> installing deepslate@${DEEPSLATE_VERSION}" >&2
(cd "${work}" && npm init -y >/dev/null 2>&1 &&
    npm install --no-audit --no-fund "deepslate@${DEEPSLATE_VERSION}" >/dev/null 2>&1)
installed="$(node -e "console.log(require('${work}/node_modules/deepslate/package.json').version)")"
[[ "${installed}" == "${DEEPSLATE_VERSION}" ]] ||
    { echo "error: got deepslate ${installed}, wanted ${DEEPSLATE_VERSION}" >&2; exit 1; }
cp "${repo_root}/tools/analysis/deepslate-aquifer-trust.mjs" "${work}/trust.mjs"

# world name, seed, lava, corpus
worlds=(
    "c96 42 0.0 pslvar/c96"
    "jv 42 -1.0 comb_42/jv"
    "jv 7 -1.0 comb_7/jv"
    "jv 12345 -1.0 comb_12345/jv"
    "jv 999 -1.0 comb_999/jv"
)
for world in "${worlds[@]}"; do
    read -r name seed lava corpus <<<"${world}"
    for entry in fill aquifer; do
        echo "==> deepslate ${entry}: ${name} at seed ${seed}" >&2
        (cd "${work}" && node trust.mjs blocks "${seed}" "${lava}" "${entry}" "${chunks}" \
            "${work}/${name}_${seed}_${entry}.bin")
    done
done
for seed in 42 7 12345 999; do
    (cd "${work}" && node trust.mjs jitter "${seed}" 8 -4) > "${work}/jitter_${seed}.txt"
done

echo; echo "=== 0. does the entry point respond to the aquifer's inputs? ==="
for entry in fill aquifer; do
    (cd "${work}" && node trust.mjs blocks 42 0.0 "${entry}" 1 "${work}/base_${entry}.bin")
    moved=0
    for override in '{"fluid_level_floodedness": -1.0}' '{"fluid_level_spread": 0.5}' \
                    '{"lava": -1.0}' '{"preliminary_surface_level": 0.0}'; do
        (cd "${work}" && node trust.mjs blocks 42 0.0 "${entry}" 1 "${work}/moved.bin" "${override}")
        cmp -s "${work}/base_${entry}.bin" "${work}/moved.bin" || moved=$((moved + 1))
    done
    echo "${entry}: ${moved} of 4 moved inputs change the chunk"
done

echo; echo "=== 1. the position mix ==="
(cd "${work}" && node trust.mjs mix 400)

WORK="${work}" PROBES="${probes}" CHUNKS="${chunks}" REPO="${repo_root}" python3 - "${worlds[@]}" <<'PY'
import importlib.util
import os
import struct
import sys
import zlib

import numpy as np

work, probes, n = os.environ["WORK"], os.environ["PROBES"], int(os.environ["CHUNKS"])
spec = importlib.util.spec_from_file_location(
    "region_palette", os.path.join(os.environ["REPO"], "tools/analysis/region-palette.py"))
region_palette = importlib.util.module_from_spec(spec)
spec.loader.exec_module(region_palette)

CATEGORY = {"minecraft:air": 0, "minecraft:cave_air": 0, "minecraft:void_air": 0,
            "minecraft:water": 1, "minecraft:lava": 2, "minecraft:stone": 3}
NAMES = ["air", "water", "lava", "stone", "other"]
FLOWING = 8


def server_blocks(path, chunks):
    """[y + 64][z][x] categories, FLOWING set on a fluid whose level is not 0."""
    data = open(path, "rb").read()
    out = np.full((384, chunks * 16, chunks * 16), 255, dtype=np.uint8)
    for cz in range(chunks):
        for cx in range(chunks):
            index = cx + cz * 32
            offset = int.from_bytes(data[index * 4:index * 4 + 3], "big")
            if offset == 0:
                raise SystemExit(f"{path}: chunk {cx} {cz} missing")
            start = offset * 4096
            (length,) = struct.unpack_from(">i", data, start)
            raw = zlib.decompress(data[start + 5:start + 4 + length])
            chunk, _ = region_palette.read_nbt(raw, 1, raw[0])
            for section in chunk.get("sections", []):
                base = section["Y"] * 16
                states = section.get("block_states") or {}
                palette = []
                for entry in states.get("palette", []):
                    category = CATEGORY.get(entry.get("Name"), 4)
                    level = (entry.get("Properties") or {}).get("level", "0")
                    palette.append(category | (FLOWING if category in (1, 2) and level != "0" else 0))
                if not palette or base + 16 <= -64 or base >= 320:
                    continue
                if len(palette) == 1:
                    values = np.full(4096, palette[0], dtype=np.uint8)
                else:
                    bits = max(4, (len(palette) - 1).bit_length())
                    per, mask = 64 // bits, (1 << bits) - 1
                    indices = [((word & 0xFFFFFFFFFFFFFFFF) >> (k * bits)) & mask
                               for word in states["data"] for k in range(per)][:4096]
                    values = np.array([palette[i] for i in indices], dtype=np.uint8)
                out[base + 64:base + 80, cz * 16:cz * 16 + 16, cx * 16:cx * 16 + 16] = \
                    values.reshape(16, 16, 16)
    if (out == 255).any():
        raise SystemExit(f"{path}: blocks missing from the window")
    return out


print("\n=== 2. blocks, chunks [0,%d)x[0,%d) ===" % (n, n))
for world in sys.argv[1:]:
    name, seed, _, corpus = world.split()
    server = server_blocks(os.path.join(probes, corpus, "r.0.0.mca"), n)
    category = server & 7
    for entry in ("fill", "aquifer"):
        ours = np.fromfile(os.path.join(work, f"{name}_{seed}_{entry}.bin"),
                           dtype=np.uint8).reshape(category.shape)
        wrong = category != ours
        pairs = []
        for a in range(5):
            for b in range(5):
                count = int((wrong & (category == a) & (ours == b)).sum())
                if count:
                    pairs.append(f"server {NAMES[a]} / deepslate {NAMES[b]} {count}")
        print(f"{name:3} {seed:>5} {entry:7} disagree {int(wrong.sum())} of {wrong.size} "
              f"(server flowing: {int(((server & FLOWING) != 0).sum())}); "
              f"deepslate stone {int((ours == 3).sum())}, lava above -55 {int((ours[10:] == 2).sum())}")
        for pair in pairs:
            print("      " + pair)

print("\n=== 3. the jitter readout (y = -42, layer -4), chunks [0,8)x[0,8) ===")
totals = {0: [0, 0], 1: [0, 0]}
for seed in (42, 7, 12345, 999):
    server = server_blocks(os.path.join(probes, f"comb_{seed}", "jv", "r.0.0.mca"), 8)
    layer = server[-42 + 64] & 7
    draws = {}
    for line in open(os.path.join(work, f"jitter_{seed}.txt")):
        cx, cz, *d = map(int, line.split())
        draws[(cx, cz)] = (tuple(d[:3]), tuple(d[3:]))
    for which, label in ((0, "deepslate at()"), (1, "documented mix")):
        tally = {}
        for z in range(128):
            for x in range(128):
                best, owner = 1 << 60, None
                for qx in range(x // 16 - 1, x // 16 + 2):
                    for qz in range(z // 16 - 1, z // 16 + 2):
                        if (qx, qz) not in draws:
                            continue
                        jx, _, jz = draws[(qx, qz)][which]
                        distance = (x - 16 * qx - jx) ** 2 + (z - 16 * qz - jz) ** 2
                        if distance < best:
                            best, owner = distance, (qx, qz)
                wet_total = tally.setdefault(owner, [0, 0])
                wet_total[0] += int(layer[z, x] in (1, 2))
                wet_total[1] += 1
        cells = agree = 0
        for cell, (wet, total) in tally.items():
            if total < 120:
                continue
            cells += 1
            agree += int((wet * 2 > total) == (draws[cell][which][1] == 8))
        totals[which][0] += agree
        totals[which][1] += cells
        print(f"seed {seed:>5} {label:15} {agree} of {cells} cells")
print(f"all four      deepslate at()  {totals[0][0]} of {totals[0][1]} cells")
print(f"all four      documented mix  {totals[1][0]} of {totals[1][1]} cells")
PY
