# Stratum

A C++20 world generation engine for [PocketMine-MP](https://pmmp.io) that
executes **vanilla Java Edition's data-driven worldgen JSON** — density
functions, noise settings, multi-noise biome sources and surface rules — so
that Java-ecosystem datapacks and authoring tools (Misode's generators,
Snowcapped, Spyglass) work as content for a Bedrock server.

- **Schema pin:** Java Edition **1.21.11**, data pack format **94.1**
- **Parity goal:** bit-exact terrain, biome and surface output versus vanilla
  Java for the same JSON and seed (Tier A, see below)
- **Status:** Milestone **M1** — core primitives and the conformance
  harness. The engine does not generate terrain yet, but it can already read
  what vanilla produced: `stratum diff` compares two region files
  block-for-block and `stratum render` draws one. Subcommands that are not
  implemented fail loudly with the milestone that owns them.

`SPEC.md` is the single source of truth for scope and architecture;
`CLAUDE.md` defines how changes are made. Read both before contributing.

## Build and test

```bash
cmake --preset dev            # configure (Debug; no generator is pinned)
cmake --build --preset dev    # build lib/, cli/ and the test suites
ctest --preset dev            # unit + repo-policy lint suites
```

Conformance runs separately, because it needs fixtures that are generated
locally and never committed:

```bash
tools/fetch-vanilla --dry-run             # show the plan without touching the network
tools/fetch-vanilla --with-structures     # fetch + verify + extract vanilla data
ctest --preset conformance                # skips loudly while fixtures are absent
```

`fetch-vanilla` resolves the pinned version through Mojang's piston-meta
manifest and verifies every hop of the chain (manifest → version metadata
SHA-1 → published jar SHA-1 → nested server jar SHA-256) before extracting
anything. Pass `--jar <path>` to use a jar you already downloaded. It needs
`curl`, `jq`, `unzip` and — for generation — a JRE; its own tests also need
`zip`.

Golden region files come from running the vanilla server headlessly:

```bash
tools/fetch-vanilla --generate-regions --accept-eula
```

That runs the server, so it requires accepting
[Mojang's EULA](https://aka.ms/MinecraftEULA) yourself — the script refuses
without `--accept-eula` and will not accept it on your behalf. The defaults
generate SPEC §7's fixed seed set across all three dimensions; `--seeds`,
`--dimensions` and `--regions` narrow it. The world is frozen before
generation and carvers and features are stripped, so the goldens isolate
Tier A and two runs of the same seed agree exactly — see SPEC §7 for why
both matter.

| Preset | Purpose |
|--------|---------|
| `dev` | Debug, warnings visible but not fatal |
| `release` | RelWithDebInfo; identical determinism flags to `dev` |
| `ci-debug` / `ci-release` | As above with `-Werror` / `/WX` |

Requirements: CMake ≥ 3.24, a C++20 compiler (GCC 13+, Clang 16+, MSVC 2022),
zlib, and network access on first configure so Catch2 v3 can be fetched (or
install Catch2 ≥ 3 yourself and it will be used instead). zlib is taken from
the system when present and fetched and built otherwise, so no manual setup
is needed on platforms that ship without it.

Read what vanilla produced:

```bash
stratum diff <left.mca> <right.mca> [--max <n>]   # 0 identical, 1 differing
stratum render <region.mca> --out map.png --mode heightmap|biome|blocks
```

`diff` compares block-for-block in Java block space, before any Bedrock
mapping (SPEC §7), and reports the first differences with their coordinates.
Anything it cannot compare — a chunk present on one side, one that fails to
decode, one whose stored coordinates disagree — is reported as a finding
rather than skipped. Slice rendering is still to come.

Look at a density function before there is any terrain to look at:

```bash
stratum render --pack <dir> --function minecraft:overworld/continents \
    --seed 42 --out continents.png \
    [--origin X,Z] [--y N] [--step N] [--size WxH] [--ramp signed|grey]
```

`<dir>` is either a data pack (`pack.mcmeta` beside `data/`) or an extracted
worldgen tree, told apart by what is on disk. The default ramp draws values
below zero blue and above zero orange, which is the distinction most of
vanilla's 2D functions are shaped around — continentalness below zero is
ocean. The value range is printed alongside, because a shade cannot be read
back into a number.

This is **not** terrain height: a real heightmap needs the cell sampler and
block placement that follow it (M3). A function this build cannot evaluate
is refused by name rather than drawn as a guess.

Check a pack before generating from it:

```bash
stratum validate <pack-dir> [--seed N] [--strict]   # 0 clean, 1 warnings, 4 errors
```

It does everything world load does short of sampling a point: opens the
pack, resolves every reference, builds every noise, and walks each density
function for node types this build cannot execute. It keeps two kinds of
"no" apart — a registry v1 does not execute (your pack is fine; this engine
will not run that part of it) and a function this *build* cannot yet
evaluate (your pack is fine; the code is not there yet) — and reports the
registries that load but which nothing interprets yet, so that a clean
report is not read as more approval than it is.

`--strict` makes warnings fatal. It is where SPEC §8's open question is
handed to you rather than answered: whether an unexecutable registry should
stop a load depends on whose pack it is, and vanilla's own data could not
load if this build decided it.

Lint locally the way CI does:

```bash
tools/lint/check-determinism.sh   # also runs as the ctest `lint.determinism` case
tools/lint/format.sh --fix        # clang-format 18.1.8, pinned
```

## Repository layout

```
lib/       core static library — pure C++, never includes PHP/zend headers
  mapping/ Java -> Bedrock block-state and biome mapping (M5, own tests)
cli/       render / generate / diff / validate, built on lib/
ext/       zend binding for PocketMine-MP (M5; marshaling only)
tools/     lint, fixture fetching, schema sync, CI glue
tests/     Catch2 v3 unit suites; conformance driven via `cli diff` + CTest
```

## Capability matrix (v1)

This matrix is part of the public contract (SPEC §8). A pack that
half-loads silently is a bug of the highest severity class: anything outside
the supported column is a **hard error at load, naming the registry or node
that was rejected**.

| Registry / feature | v1 | Notes |
|---|---|---|
| `worldgen/density_function` | Supported | Including all cache node types |
| `worldgen/noise` | Supported | Either as its own entry or written inline in a density function; an inline one loads but cannot yet be seeded (SPEC §11) |
| `worldgen/noise_settings` | Supported | Noise router, surface rules, spawn targets |
| Multi-noise biome source | Supported | |
| `worldgen/biome` | Supported | Loaded for biome identity and surface rules; features and carvers within a biome are not executed |
| `dimension`, `dimension_type`, `world_preset` | Partial | Only what is needed to select noise settings |
| Tags and namespaced references | Supported | Within the registries above |
| `configured_feature`, `placed_feature` | **Rejected** | v2 (SPEC §10, M6) |
| `configured_carver` | **Rejected** | v2 |
| `structure`, `structure_set` | **Rejected** | v2 |
| `template_pool`, `processor_list` | **Rejected** | v2 |
| `flat_level_generator_preset` | **Rejected** | Not planned for v1 |

Rejected entries are never silently dropped: the loader records each one by
identifier and registry, so a caller can report exactly what it will not
execute. See the open question in SPEC §8 about when their presence should be
fatal.

This matrix states the v1 contract, not what is finished. The build in
progress refuses three density function types outright, and two more only
when it has no cell lattice to evaluate them over — by name and with a
reason, rather than approximating them. It refuses a noise written inline
for a narrower reason: a noise is seeded from the MD5 of its identifier, and
one written in place has none. SPEC §11 lists why each is where it is.

### Density function types

All 34 types vanilla defines at 1.21.11. The last column is the one that
matters for a parity project: what a type has actually been *checked*
against, rather than whether it has code.

`golden terrain` means the type is reached by the overworld's `final_density`
and is therefore exercised by the end-to-end comparison against the
heightmaps vanilla itself recorded (`golden_terrain_test`). That comparison
is close but not yet exact, so it is coverage rather than a clean bill of
health — it would catch a type being badly wrong, not subtly.

| Node type | Status | Vanilla uses | Checked against |
|---|---|---|---|
| `abs` | Supported | 12 | golden terrain, cubiomes |
| `add` | Supported | 123 | golden terrain, cubiomes |
| `blend_alpha` | Supported — constant `1.0` | 12 | golden terrain; the no-blending value (SPEC §11) |
| `blend_density` | Supported — passthrough | 7 | golden terrain; the no-blending reading (SPEC §11) |
| `blend_offset` | Supported — constant `0.0` | 3 | golden terrain; the no-blending value (SPEC §11) |
| `cache_2d` | Supported | 24 | golden terrain, cubiomes |
| `cache_all_in_cell` | Needs a cell lattice | 0 | unit vectors only — unused at 1.21.11 |
| `cache_once` | Supported | 12 | golden terrain |
| `clamp` | Supported | 14 | golden terrain, cubiomes |
| `constant` | Supported | — | golden terrain; written as a bare number, so pervasive |
| `cube` | Supported | 2 | golden terrain, cubiomes |
| `end_islands` | **Not implemented** | 2 | — the End's terrain (SPEC §10, M3) |
| `find_top_surface` | **Not implemented** | 3 | — blocks `preliminary_surface_level` in three settings |
| `flat_cache` | Supported | 16 | golden terrain, cubiomes |
| `half_negative` | Supported | 3 | golden terrain |
| `interpolated` | Needs a cell lattice | 20 | golden terrain (the lattice comes from noise settings) |
| `invert` | Supported | 3 | **nothing** — its only uses sit behind `find_top_surface` |
| `max` | Supported | 9 | golden terrain, cubiomes |
| `min` | Supported | 13 | golden terrain, cubiomes |
| `mul` | Supported | 88 | golden terrain, cubiomes |
| `noise` | Supported | 49 | golden terrain, cubiomes |
| `old_blended_noise` | Supported | 3 | the vanilla server, via datapack probe: 16384/16384 columns |
| `quarter_negative` | Supported | 6 | golden terrain |
| `range_choice` | Supported | 20 | golden terrain |
| `shift` | Supported | 0 | unit vectors only — unused at 1.21.11 |
| `shift_a` | Supported | 1 | golden terrain, cubiomes |
| `shift_b` | Supported | 1 | golden terrain, cubiomes |
| `shifted_noise` | Supported | 17 | golden terrain, cubiomes |
| `slide` | **Not implemented** | 0 | — unused at 1.21.11, so no parity cost yet |
| `spline` | Supported | 9 | golden terrain, cubiomes |
| `square` | Supported | 3 | golden terrain, cubiomes |
| `squeeze` | Supported | 7 | golden terrain — sits in `final_density` directly |
| `weird_scaled_sampler` | Supported | 3 | the vanilla server, via datapack probe: both rarity ladders |
| `y_clamped_gradient` | Supported | 29 | golden terrain, cubiomes |

Two of these deserve reading twice. **`invert`** is the only type vanilla
uses that nothing checks: all three of its uses are inside
`preliminary_surface_level`, which `find_top_surface` refuses, so no golden
reaches it and it rests on documentation alone. **`squeeze`** rests on
documentation too, but it sits directly in the overworld's `final_density`,
so the terrain comparison does reach it.

`slide`, `shift` and `cache_all_in_cell` are unused at 1.21.11. That is not
a promise about other versions — it is why they cost nothing today.

### Parity tiers

| Tier | Scope | Bar |
|---|---|---|
| A | Density functions, noise, terrain shape, aquifer fill decision, multi-noise biome assignment, surface rules | Bit-exact vs vanilla Java, block-for-block, in Java block space |
| B (v2) | Carvers, features, structures | Statistical parity; exact placement may differ |
| U | Everything outside the matrix | Hard error at pack load, never a silent skip |

## Determinism

Same (pipeline, seed, chunk) must produce identical bytes on every supported
platform, architecture and compiler (SPEC §5). In practice that means: all
seed arithmetic in `uint64_t` with explicit wrapping, Java-semantics
`floorDiv`/`floorMod`/`>>>` helpers instead of raw `%` and `>>`, only the
project's Java LCG and Xoroshiro128++ seeded per `(worldSeed, position,
salt)`, `-ffp-contract=off` / `/fp:precise`, and never `-ffast-math`.

Transcendentals are a special case: Java uses `StrictMath` (fdlibm), and a
host libm is within an ulp of it rather than equal to it. Parity-critical
code therefore calls `stratum::fdlibm::` — never `<cmath>` — for `log` and
friends. `std::sqrt` is exempt, being IEEE-754 correctly rounded.

CI enforces this on x86-64 **and** ARM64 across Linux, Windows and macOS; a
cross-architecture divergence is a build failure, not noise.
`tools/lint/check-determinism.sh` catches the parts a compiler cannot see,
including that vendored licence notices are still intact.

## Provenance

This repository ships **no Mojang data and no Mojang source code**, in any
form (SPEC §12). Vanilla worldgen definitions and conformance fixtures are
produced on your machine by `tools/fetch-vanilla` from an official jar you
download yourself; they are gitignored and never redistributed here.
Behaviour is implemented from minecraft.wiki / datapack.wiki documentation,
mcdoc schemas, licensed references (cubiomes, MIT; Cuberite, Apache-2.0) and
the observed input/output of the vanilla server.

## Licence

Apache License 2.0 — see [LICENSE](LICENSE) and [NOTICE](NOTICE).

"Minecraft" is a trademark of Mojang AB. This project is not affiliated with,
endorsed by, or associated with Mojang AB or Microsoft.
