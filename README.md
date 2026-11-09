# Stratum

Stratum is a world generator for Bedrock servers, written in C++20. It gives a
Bedrock server Java Edition's terrain and biomes by running the same worldgen
JSON files Java runs. Given the same JSON and seed, it aims to place the same
blocks and biomes as a Java server, bit for bit.

- **Pinned to:** Java Edition 1.21.11, data pack format 94.1
- **Runs in:** PocketMine-MP and Nukkit ([setup](#on-a-server)), or alone
  through the `stratum` command ([usage](#use-the-command-line))
- **Speed:** about 3.4 ms per overworld chunk, a quarter of what vanilla takes
  for the same stages
- **Generates:** terrain, biomes, aquifers, noise caves, large ore veins and
  surface blocks
- **Not yet:** worlds have no trees, plants, ordinary ores, carved caves or
  structures. Those are planned for v2, and need not land where Java puts them.
  The Nether generates through the library and `stratum generate`; neither
  server plugin offers it as a world yet.

[`SPEC.md`](SPEC.md) is the source of truth for scope and design.
[`PROGRESS.md`](PROGRESS.md) tracks the milestones. [`CLAUDE.md`](CLAUDE.md)
says how changes are made.

## What generates today

Status on 2026-11-09. Every figure is measured against the vanilla 1.21.11
server.

| World | State |
|---|---|
| Overworld | Generates, with no trees or other features. 99.9980% of sampled blocks match. |
| Amplified, large biomes | Generate. On test worlds forced to one biome, terrain, aquifers, ore veins and surface rules match the server, apart from fluid it moved. Not yet measured: where either preset places its biomes, and its surface blocks on its real biomes. |
| The End | Generates, with no towers, chorus plants or End cities. Blocks on y 0 to 127, the only heights its terrain uses, match exactly on every sampled chunk. Every biome stored on y 0 to 255 also matches, in 45 regions. |
| The Nether | Generates, with no features or carved caves. Every block of eight golden regions (8×8 chunks each) matches. |
| `caves`, `floating_islands` noise settings | Compile through the same legacy named-noise construction as the Nether. Not measured against the server. `generate` does not offer them. |

**The overworld sample** is 128 chunks from eight seeds. Of its 12 582 912
blocks, 12 582 474 match. Of the other 438, 415 are surface blocks near biome
borders and 23 are fluids the server moved after generating them.

A whole region scores a little lower. `stratum diff` on seed 42's region
`r.0.0` finds 6 686 of 100 663 296 blocks differ (99.9934% match), and 189 of
1 572 864 biome cells. In the one other region scored in full, every biome
difference (614 cells) was a tie the server broke the other way.

Every server world behind these figures was generated with features and
carvers turned off, so it holds no trees to compare. The carvers are scored
on their own, against the chunks the server stores at its carving stage:
every block matches at two seeds in the overworld and the Nether.

**The Nether's** surface rules read noises seeded by vanilla's older random
source. A clean-room specification settled that seeding on 2026-11-09, and
with the surface rules reading the block-level ("zoomed") biome, every block
of the eight golden Nether regions matches.

**Milestones.** M4 (biomes and surface) carries ties in the biome search
and the presets not yet measured; the 232 overworld surface blocks once
unexplained were the water height, and are exact. The server integration (M5) closed on 2026-10-29.

The large noise caves, ore veins and carvers (carved caves, ravines) do
generate. Features (trees, plants, ordinary ores) and structures do not. A
pack's copies of those are recorded and not run until v2 (M6).

## Speed

Mean ms per chunk on an AMD Ryzen 5 7600, with one worldgen thread, on an
otherwise idle machine (2026-11-01). The vanilla column counts only its biome,
noise and surface stages, the same terrain work Stratum does. Two things favour
vanilla here: Stratum's figure also includes encoding the chunk, and vanilla's
JVM gets help from JIT and GC threads on other cores.

| Dimension | Stratum | Vanilla | Share of vanilla's time |
|---|---|---|---|
| Overworld | 3.39 | 13.38 | 0.25x |
| Amplified | 3.27 | 16.59 | 0.20x |
| Large biomes | 3.45 | 15.24 | 0.23x |
| The End | 0.33 | 2.79 | 0.12x |

The overworld took 155.7 ms before the first performance pass. Each pass kept
the output byte-identical.

Inside PocketMine-MP's generation task the same four take 3.41, 3.29, 3.49 and
0.37 ms. The PHP around the engine adds under 0.05 ms of that. These figures
cover generating a chunk only. Queueing, the copy between threads, a
population's neighbour chunks, lighting, fluid scheduling and sending the chunk
to players have not been measured.

To measure it yourself, use `tools/perf-baseline` and `tools/pmmp-task-cost`.

## On a server

Stratum has run inside a real headless PocketMine-MP 5.44.3 and Nukkit b3caad67.
On 2026-10-27 a Bedrock client visited sixteen places on each, and saw the same
terrain on both. This evidence is from Linux x86-64. To repeat the headless
runs, use `ctest --preset pmmp` or `ctest --preset nukkit` after the setup in
the READMEs below.

- **PocketMine-MP:** see [`ext/README.md`](ext/README.md) to build the
  extension and [`ext/plugin/README.md`](ext/plugin/README.md) to install the
  plugin.
- **Nukkit:** see [`ext-nukkit/README.md`](ext-nukkit/README.md). Nukkit's own
  biome code has two known limits. A section offset in its biome serializers
  makes stored and sent biomes wrong wherever biomes change with height. Its
  registry lacks four overworld biomes, which clients see as ocean.

Both servers generate the overworld, amplified and large biomes presets.
PocketMine-MP also generates the End, and shows it to clients as the overworld.
Nukkit refuses the End by name. Neither offers the Nether as a world yet. Running your own
datapack on a server is not a v1 goal, and a biome with no Bedrock id is refused
by name.

## Build and test

You need CMake 3.24 or newer and a C++20 compiler (GCC 13+, Clang 16+ or
MSVC 2022). Catch2, nlohmann/json and zlib are fetched on first configure if you do not
have them.

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

### Use the command line

The binary is `build/dev/cli/stratum`. Generating needs vanilla's worldgen data
on your machine, which `tools/fetch-vanilla` downloads, verifies and extracts.
That script needs `curl`, `jq`, `unzip` and Java 21 or newer. Without Java it
still exits 0 but skips the biome data, and `generate` then refuses.

```bash
tools/fetch-vanilla
export PATH="$PWD/build/dev/cli:$PATH"
stratum generate .fixtures/1.21.11 --dimension overworld --seed 42 \
    --region 0,0 --out build/out/
stratum render build/out/r.0.0.mca --out build/map.png --mode biome
stratum diff build/out/r.0.0.mca \
    .fixtures/1.21.11/regions/seed-42/overworld/r.0.0.mca   # the server's region, from Run conformance
stratum validate .fixtures/1.21.11
```

Keep region files you generate under `build/`. The lint fails on one loose in
the source tree; `build/` and `.fixtures/`, where the fetch scripts put theirs,
are exempt.

- `generate` writes Anvil region files in the form the server stores them, which
  `diff` and `render` read. Its dimensions are `overworld`, `amplified`,
  `large_biomes`, `nether` and `end`.
- `render` also draws one density function's field, blue below zero and orange
  above:
  `stratum render --pack .fixtures/1.21.11 --function minecraft:overworld/continents --seed 42 --out build/field.png`.
- `diff` compares every block and stored biome, and exits 0 only if both match.
  A chunk it cannot compare is reported, never skipped.
- `validate` opens a pack, resolves every reference and builds every noise,
  without generating. Some refusals come only when a dimension compiles, so a
  clean report does not promise a world will generate. It exits 0 when clean, 1
  on warnings and 4 on errors. `--strict` makes warnings exit 4, so vanilla's
  own data always fails it.

Where a command takes a pack, that is a data pack (`pack.mcmeta` beside `data/`)
or a fetched tree like `.fixtures/1.21.11`.

### Run conformance

Conformance compares our output with a real vanilla server. Its fixtures are
generated locally and never committed. Generating them runs the server, so
you must accept [Mojang's EULA](https://aka.ms/MinecraftEULA) yourself. The
scripts refuse without `--accept-eula`.

```bash
tools/fetch-vanilla --with-structures       # the jar, its data and its structure files
tools/probe-worlds generate --accept-eula   # every world, one server at a time; hours
tools/probe-worlds verify                   # check what is there, without a server
ctest --preset conformance                  # skips loudly while fixtures are absent
```

CI builds and tests on Linux, macOS and Windows, on x86-64 and ARM64. It also
generates these worlds on Linux x86-64, and runs conformance over them on Linux
x86-64 and ARM64. On 2026-11-01, 176 of the 185 cases ran on each, and the other
9 have no committed generator.

### Presets

| Preset | Purpose |
|---|---|
| `dev` | Debug. Warnings show but are not fatal. |
| `release` | RelWithDebInfo, with the same determinism flags as `dev`. No test preset. |
| `ci-debug`, `ci-release` | As above, with warnings as errors. |
| `ci-shipped` | Release, warnings as errors: the code generation the bindings ship. |
| `ci-shipped-conformance` | Test preset: the conformance suite on `ci-shipped`'s build. |
| `conformance` | Test preset: the conformance suite on `dev`'s build. |
| `ci-conformance` | Build and test presets: the same suite four cases at a time, as CI runs it. |
| `sanitize` | Clang UBSan. The first report is fatal. |
| `sanitize-conformance` | Test preset: conformance on `sanitize`'s build. |
| `ext`, `ext-release` | The PocketMine-MP extension. Needs `STRATUM_PHP_CONFIG`; see `ext/README.md`. |
| `pmmp`, `pmmp-task-cost` | Test presets on `ext-release`'s build: its local server tests. |
| `nukkit`, `nukkit-release` | The Nukkit binding. Needs `JAVA_HOME`; see `ext-nukkit/README.md`. |

### Lint the way CI does

```bash
tools/lint/check-determinism.sh   # also runs inside ctest --preset dev
tools/lint/format.sh --fix        # clang-format 18.1.8, pinned
tools/lint/warnings.sh            # the project's warnings as errors
tools/lint/clang.sh               # the same under clang++
tools/lint/tidy.sh                # clang-tidy 18
tools/lint/optimised.sh           # unit tests built optimised
```

## Repository layout

```
lib/         core static library: pure C++, never includes PHP headers
  mapping/   Java to Bedrock block-state and biome mapping
cli/         the stratum command: render, generate, diff, validate
ext/         PocketMine-MP binding and plugin
ext-nukkit/  Nukkit binding and plugin
spec/        clean-room specifications
tools/       lint, fixture fetching, schema sync, measurement, CI scripts
tests/       Catch2 unit suites and conformance cases
```

## Capability matrix (v1)

This table is part of the public contract (SPEC §8). A pack that half-loads
silently is a top-severity bug.

A JSON file the build cannot place in a registry is a **hard error at load,
naming the file**. The loader does not look at other file types. A **Rejected**
registry does not stop a load by default, because vanilla's own data holds
hundreds of such entries. The loader records each one by name and never runs it.
`stratum validate` warns about them, `--strict` makes that fatal, and SPEC §8
keeps open what the default should be for a user's pack.

| Registry / feature | v1 | Notes |
|---|---|---|
| `worldgen/density_function` | Supported | All cache node types included |
| `worldgen/noise` | Supported | Its own entry or written inline. An inline one loads but cannot yet be seeded. |
| `worldgen/noise_settings` | Supported | Noise router and surface rules. Spawn targets load and are kept, not interpreted. |
| Aquifers (`aquifers_enabled` and the router's aquifer entries) | Supported | Under either random source. Refused by name when `default_fluid` is not water, or under the legacy source when the noise router reads a named noise other than the three legacy climate noises. |
| Multi-noise biome source | Supported | Named by its parameter list |
| `the_end` biome source | Supported | Reads the router's `erosion`, as the server does |
| `fixed`, `checkerboard` biome sources | **Refused** | Refused by name when a caller names them. One a pack declares in `dimension` or `world_preset` is not read or reported yet. |
| `worldgen/biome` | Supported | For biome identity, surface rules and carvers. A biome's features do not run. |
| `dimension`, `dimension_type`, `world_preset` | Partial | Load but are not interpreted. The caller names the noise settings and biome source. |
| Namespaced references | Supported | Within the registries above |
| `worldgen/configured_carver` | Supported | `cave`, `nether_cave` and `canyon`, block for block against the server's carving stage. `debug_mode: true` is refused. |
| Tags | **Refused** | A pack with a JSON file under `tags/` is refused by name at load. Carvers read vanilla's block tags from the version root's `tags/block`. |
| `configured_feature`, `placed_feature` | **Rejected** | v2 (M6) |
| `structure`, `structure_set` | **Rejected** | v2 |
| `template_pool`, `processor_list` | **Rejected** | v2 |
| `flat_level_generator_preset` | **Rejected** | Not planned for v1 |

These are also refused by name rather than guessed at:

- **`slide`**, the one density type not implemented. Vanilla does not use it
  at 1.21.11.
- **`interpolated` and `cache_all_in_cell` without a cell lattice.** A cell
  lattice is the grid of cells noise settings divide a chunk into. These two
  types depend on it, so a function evaluated on its own refuses them.
- **Input that would reach undefined behaviour:** values outside their schema
  range, integers their field cannot hold, and nesting too deep for the stack.
  The depth limits are 128 for JSON, 256 for a density function graph and 128
  for surface rules.
- **Far chunks on the overworld presets:** past about 2.8 million blocks from
  the centre on either axis (chunk coordinates 175 770 and up, and -175 771 and
  down). The coordinates of vanilla's `jagged` noise stop fitting in 32 bits
  there, well inside the world border. The server's behaviour past that point
  has not been measured.

The aquifer is the one exception to these refusals. It reads
`preliminary_surface_level`, `fluid_level_spread` and `sea_level` as Java's
`int` would. That is defined and portable, but not yet measured against the
server. SPEC §8 has the full list.

<details>
<summary><strong>Density function types</strong>: all 34, and what each is checked against</summary>

`golden terrain` means the overworld's `final_density` uses the type, so
comparing with heightmaps recorded from vanilla exercises it. That would catch a
type being badly wrong, not subtly wrong. `cubiomes` means known answers
generated from cubiomes. A `datapack probe` is a small test world the vanilla
server generated from a datapack written for that check. `unit vectors` are
known-answer tests.

| Node type | Status | Vanilla uses | Checked against |
|---|---|---|---|
| `abs` | Supported | 12 | golden terrain, cubiomes |
| `add` | Supported | 123 | golden terrain, cubiomes |
| `blend_alpha` | Supported — constant `1.0` | 12 | golden terrain; the no-blending value (SPEC §11) |
| `blend_density` | Supported — passthrough | 7 | golden terrain; the no-blending reading (SPEC §11) |
| `blend_offset` | Supported — constant `0.0` | 3 | golden terrain; the no-blending value (SPEC §11) |
| `cache_2d` | Supported | 24 | golden terrain, cubiomes |
| `cache_all_in_cell` | Needs a cell lattice | 0 | unit vectors; the vanilla server, via datapack aquifer probe — unused at 1.21.11 |
| `cache_once` | Supported | 12 | golden terrain |
| `clamp` | Supported | 14 | golden terrain |
| `constant` | Supported | — | golden terrain; written as a bare number, so pervasive |
| `cube` | Supported | 2 | golden terrain |
| `end_islands` | Supported | 2 | golden End regions plus two far probes: every block exact (SPEC §11) |
| `find_top_surface` | Supported | 3 | the vanilla server, via datapack probe: 1024/1024 columns |
| `flat_cache` | Supported | 16 | golden terrain, cubiomes; its chunk window, via datapack aquifer probe |
| `half_negative` | Supported | 3 | golden terrain |
| `interpolated` | Needs a cell lattice | 20 | golden terrain (the lattice comes from noise settings); the vanilla server's aquifer reads, via datapack probe |
| `invert` | Supported | 3 | golden `preliminary_surface_level`, since `find_top_surface` landed |
| `max` | Supported | 9 | golden terrain |
| `min` | Supported | 13 | golden terrain |
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
| `square` | Supported | 3 | golden terrain |
| `squeeze` | Supported | 7 | golden terrain — sits in `final_density` directly |
| `weird_scaled_sampler` | Supported | 3 | the vanilla server, via datapack probe: both rarity ladders |
| `y_clamped_gradient` | Supported | 29 | golden terrain |

`slide`, `shift` and `cache_all_in_cell` are unused at 1.21.11. That is why
they cost nothing today, not a promise about other versions.

</details>

### Parity tiers

| Tier | Scope | Bar |
|---|---|---|
| A | Density functions, noise, terrain shape, aquifer fill decision, multi-noise biome assignment, surface rules, carvers | Bit-exact vs vanilla Java, block-for-block, in Java block space |
| B (v2) | Features, structures | Statistical parity; exact placement may differ |
| U | Everything outside the matrix | Never executed. A JSON file in a directory this build cannot name is a hard error at pack load. An entry in a **Rejected** registry is recorded by name, and a hard error only under `--strict`. |

## Determinism

The same pipeline, seed and chunk must produce identical bytes on every
supported platform, architecture and compiler (SPEC §5). In practice:

- Seed arithmetic is `uint64_t` with explicit wrapping.
- Java-semantics `floorDiv`, `floorMod` and `>>>` helpers replace raw `%` and
  `>>`.
- The only RNGs are the project's Java LCG and Xoroshiro128++, seeded per
  world seed, position and salt.
- `-ffp-contract=off` or `/fp:precise` is always on, and `-ffast-math` never is.
- Parity-critical code calls `stratum::fdlibm::`, not `<cmath>`, because Java's
  `StrictMath` differs from a host libm. `std::sqrt` is exempt, being correctly
  rounded.

CI treats a difference between architectures as a build failure.
`tools/lint/check-determinism.sh` catches what a compiler cannot see.

## Provenance

This repository ships **no Mojang source code, and no vanilla worldgen JSON,
extracted assets or fixtures** (SPEC §12). The exception is `lib/mapping`'s
tables of block and biome identifiers, committed with the owner's approval.

Vanilla worldgen data and conformance fixtures are produced on your machine, or
on this repository's CI runners, from an official jar downloaded from Mojang.
They are gitignored, never committed, and never published as CI artifacts.

Behaviour comes from minecraft.wiki and datapack.wiki documentation, mcdoc
schemas, licensed references (cubiomes, MIT; Cuberite, Apache-2.0), the
clean-room specifications under `spec/`, and the observed input and output of
the vanilla server and of deepslate, run as a black box. The Bedrock tables come
from GeyserMC's mappings (MIT).

## Licence

Apache License 2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE).

"Minecraft" is a trademark of Mojang AB. This project is not affiliated with,
endorsed by, or associated with Mojang AB or Microsoft.
