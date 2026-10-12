#!/usr/bin/env bash
# Stratum — an ore-vein probe: a fully solid column, ore veins on, everything
# else off (SPEC §10, M3/M4 "ore veins" — untouched until this).
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
#   tools/analysis/ore-vein-probe.sh --accept-eula [seed]
#
# WHY THIS EXISTS. No clean-room spec covers ore veins — unlike the aquifer,
# there is no spec/ore-vein-spec.md, so this starts from the permitted
# public references (minecraft.wiki's "Ore vein" and "Noise router" pages)
# rather than a researched brief, and treats every number those pages give
# as a HYPOTHESIS to confirm against the real server, not a fact to encode.
#
# SETTLED — this probe's worlds are what confirmed the derivation, per block,
# on 79790 candidates across both probe sets (SPEC's M3 section). The
# hypothesis below is kept verbatim as what was believed going in, with one
# correction worth carrying: `vein_ridged < 0` does NOT give "a 30% chance" of
# becoming filler or ore. It gives a 70% chance — the roll is
# `nextFloat() < 0.7`, and reading that number backwards cost a long search.
# See tools/analysis/ore-vein-rng-test.cpp's header for how.
#
# THE HYPOTHESIS, as documented publicly:
#   * `vein_toggle` decides TYPE and existence: `y` in [0, 50] and
#     `vein_toggle > 0` is a candidate COPPER vein; `y` in [-60, -8] and
#     `vein_toggle <= 0` is a candidate IRON vein. On top of that, veins
#     need `|vein_toggle|` to clear a threshold that is 0.6 AT either y
#     limit and falls linearly to 0.4 at 20 blocks inside it — richer
#     (lower threshold) toward the middle of each range.
#   * `vein_ridged` decides per-block membership: `>= 0` means the block is
#     never touched; `< 0` gives a 30% chance of becoming filler or ore.
#   * `vein_gap` decides ore vs filler among touched blocks: `<= -0.3`
#     always yields filler; otherwise the ore chance is `|vein_toggle|`
#     mapped from [0.4, 0.6] to [0.1, 0.3] (clamped), with a further 2%
#     chance of a raw-metal block instead of a normal ore block.
#
# `vein_toggle` and `vein_ridged` are wrapped in `minecraft:interpolated` in
# vanilla's own router (read directly from the fetched overworld settings,
# not transcribed from memory); `vein_gap` is not. All three are copied
# verbatim from `tools/fetch-vanilla`'s own output — read, never committed
# (SPEC §12) — rather than reconstructed by hand, removing an entire class
# of transcription risk from the noise math itself.
#
# THE COLUMN IS FULLY SOLID (`raw_final_density` a positive constant) so
# every block below the dimension's own ceiling is stone before the vein
# system touches it, and NO surface rule runs — isolating ore veins from
# terrain shape and everything else. `min_y`/`height` match vanilla's own
# overworld geometry exactly (-64/384), well past both vein ranges on every
# side, so the y=-60/-8/0/50 boundaries this probe exists to pin are never
# an artefact of a probe-specific dimension shape.
#
# `AQUIFERS_ENABLED IS ALSO TRUE`, and that is itself a finding rather than
# an oversight. A first attempt at this probe left it false — the honest
# reading of "isolate ore veins from aquifers" — and got 6291456 of 6291456
# blocks back as plain stone: not one ore, filler or raw block, despite the
# vein router inputs themselves clearing their documented gates on a
# measurable fraction of positions (`vein_ridged < 0` on 4.2% of a coarse
# sample). Flipping `aquifers_enabled` to true, with density still a
# constant positive — so the aquifer's OWN fluid decision can never apply to
# any block (Q2.2: `D > 0` is unconditionally solid) — immediately produced
# real vein output (`minecraft:copper_ore`, `minecraft:granite`,
# `minecraft:raw_copper_block`, matching the wiki's own copper composition
# exactly). So the two flags are NOT independent in the real server: ore
# veins are gated on the SAME enable path aquifers use, even where the fluid
# logic itself never fires. The likely shape — an "Aquifer" sampler object
# vanilla only constructs when aquifers are enabled, which ore veins also
# read from — is inferred from this observation, not read from source; it
# is recorded as a hypothesis for the coupling, not a claim about the
# implementation.
#
# RETRACTED SINCE: A SEED CONFOUND, NOT A COUPLING (SPEC §11, "Ore veins under
# the legacy source"). The paragraph above, kept as written, leaves out what
# else moved. The first world ran at this script's default seed, 42, with
# aquifers off and default_fluid air; the contrast that "produced real vein
# output" set aquifers on, default_fluid to water AND the seed to 43 in one
# step (the first run's session record, not in the repository). Nor did the
# inputs clear "their documented gates": of the deterministic gate, the 4.2%
# above counted `vein_ridged < 0` alone (beside `vein_gap > -0.3`, which is no
# part of the gate and only picks ore over filler), and the type/sign and
# richness gate, never counted then, admits no position of seed 42's window
# under vanilla's vein router (the library: none of 1818624), so the library
# predicts that world stone with aquifers on too; at 43 the gate admits 468
# positions and the library's draws place exactly the 339 vein blocks that
# world held (48 copper_ore, 290 granite, 1 raw_copper_block), at the same
# positions: the library agrees with it on all 1818624 positions of the
# window. With aquifers off veins ARE placed: at 13579, whose window has
# veins, the recipe pair below (`ov`, and `ova` with aquifers off) was
# generated under the scratch names `mra2` and `mr2`, and those are block for
# block the same (0 of 6291456 differing, 14373 vein blocks in each); so are
# legacy-orevein-probe.sh's aquifers-off arms at four seeds under both random
# sources. `ova` itself has never been generated under that name. 13579 was
# never the stone world's seed: it is the one probes/orevein has held since
# the held-out loop.
#
# So run with no seed (42), `ova` is the first run's stone world, the same
# recipe then named `ov`, and the library predicts `ov` all stone too (no
# vein position in that window; no server world holds `ov`'s recipe at 42),
# so the pair shows nothing either way. Where probes/orevein already holds
# another seed (13579 on the machine these worlds were made on),
# density-probe.sh's seed guard refuses that run unless
# STRATUM_PROBE_OVERWRITE=1 is set. Pass 13579 to reproduce the reversal; the
# guard lets it replace probes/orevein, which already holds 13579. The
# default stays 42, the seed this script has always meant with no argument.
# Engine v14 places veins on ore_veins_enabled alone; `ov` keeps aquifers
# on, which changes nothing.
#
# CONFIRMED, across five seeds and 25608 real non-stone blocks
# (`tools/analysis/ore-vein-analyze.cpp`), with ZERO exceptions on every
# deterministic gate: the y-range, the type/sign correspondence, the
# richness threshold (0.6 at the limits falling to 0.4 at 20 blocks in),
# `vein_ridged < 0` as a necessary condition for any vein block, and
# `vein_gap > -0.3` as a necessary condition for ore over filler. The
# mapped-probability formula tracks closely too — the largest bin (10584
# blocks, |vein_toggle| in [0.59, 0.60)) reads 29.44% ore against a
# predicted 29%. The raw-ore rate reads 1.87% (98/5235), close to the
# documented 2% but not yet pinned to it.
#
# STILL OPEN (SINCE CLOSED, both; see the end of this paragraph): the exact
# RNG derivation behind the three random draws (the 30% membership roll, the
# mapped-probability ore/filler roll, the 2% raw roll) — this probe confirms
# the SHAPE and the aggregate rate of each, not the per-block algorithm that
# would let a reimplementation match the same seed's exact blocks rather than
# merely the same statistics. Also open: pinning "30%"/"2%"/"20 blocks" to
# more precision than this sample size gives — a bisection-style probe, the
# way this project pinned other constants, once the RNG derivation makes
# single-block prediction possible at all. SINCE CLOSED, both: the derivation
# is settled per block (79790 of 79790 candidates), the membership roll is
# `nextFloat() < 0.7`, not 30%; the membership and raw-roll thresholds are
# bracketed rather than assumed, and the deterministic gate, 20-block ramp
# included, holds with zero exceptions — SPEC §10's M3 entry ("Ore veins,
# SETTLED") and lib/include/stratum/ore/vein.hpp ("THE DERIVATION",
# "THRESHOLDS BRACKETED"). What is left open about the draws is SPEC §11's
# "Ore veins under the legacy source", Still open.
#
# Nothing this writes is committed: worlds are Mojang-derived (SPEC §12).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${repo_root}"

accept_eula=0
args=()
for arg in "$@"; do
    case "${arg}" in
        --accept-eula) accept_eula=1 ;;
        *) args+=("${arg}") ;;
    esac
done
seed="${args[0]:-42}"

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
spec="${work}/orevein.json"

settings=".fixtures/1.21.11/worldgen/noise_settings/overworld.json"
[[ -f "${settings}" ]] || { echo "error: no ${settings}; run tools/fetch-vanilla first" >&2; exit 1; }

env SETTINGS="${settings}" python3 - "${spec}" <<'PY'
import json, os, sys

SEA = 63
MIN_Y = -64
HEIGHT = 384

router = json.load(open(os.environ['SETTINGS']))['noise_router']
real_vein_toggle = router['vein_toggle']
real_vein_ridged = router['vein_ridged']
real_vein_gap = router['vein_gap']

# Never fires: the vein system's own blocks are the only ones of interest,
# and a surface rule would just repaint the topmost few uselessly here
# anyway (there is no "surface" in a fully solid column).
NO_SURFACE = {"type": "minecraft:condition",
              "if_true": {"type": "minecraft:y_above",
                          "anchor": {"absolute": 2000},
                          "surface_depth_multiplier": 0, "add_stone_depth": False},
              "then_run": {"type": "minecraft:block",
                           "result_state": {"Name": "minecraft:stone"}}}

def aquifer(name, aquifers=True):
    return {"name": name, "min_y": MIN_Y, "height": HEIGHT,
            "raw_final_density": {"type": "minecraft:constant", "argument": 1.0},
            "aquifers_enabled": aquifers, "ore_veins_enabled": True,
            "sea_level": SEA,
            "default_fluid": {"Name": "minecraft:air"},  # irrelevant: D>0 everywhere, so the aquifer's own fluid decision never applies.
            "size_horizontal": 1, "size_vertical": 2, "surface_rule": NO_SURFACE,
            "router": {"vein_toggle": real_vein_toggle, "vein_ridged": real_vein_ridged,
                       "vein_gap": real_vein_gap}}

# `ova` is `ov` with aquifers off: the recipe of the world the retraction
# above rests on, generated at 13579 as scratch `mr2` (never as `ova`).
spec = [aquifer("ov"), aquifer("ova", aquifers=False)]

json.dump(spec, open(sys.argv[1], "w"))
print(len(spec), "dimensions", file=sys.stderr)
PY

exec tools/analysis/density-probe.sh \
    $([[ ${accept_eula} -eq 1 ]] && printf -- --accept-eula) \
    --spec "${spec}" --seed "${seed}"
