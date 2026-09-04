# SPEC — Pipeline Worldgen Engine (successor to ext-vanillagenerator)

Status: v0.1 draft · Owner: Amir (larryTheCoder) · Project name: **Stratum**

This document is the single source of truth for scope and architecture.
Claude Code sessions MUST read this file and `CLAUDE.md` before writing code.

---

## 1. What this is

A C++ world generation engine for PocketMine-MP (Bedrock), exposed as a PHP
extension. It executes worldgen pipelines defined in **vanilla Java Edition's
data-driven worldgen JSON format** (density functions, noise settings, surface
rules, multi-noise biomes), so that existing Java-ecosystem tooling and
datapacks work as content for this engine.

### Goals

- Load worldgen definitions in the vanilla datapack schema (see §3 pin).
- Per-world, per-dimension pipeline configuration, frozen at world creation.
- Bit-exact terrain/biome/surface parity with vanilla Java given identical
  JSON and seed (Tier A, see §7).
- Fast: compiled pipeline execution, cell sampling + interpolation, cache
  nodes, chunk-parallel on PMMP worker threads.
- Standalone CLI for rendering, diffing, and testing without PocketMine.
- Output written directly into pmmp `ext-chunkutils2` structures
  (`PalettedBlockArray`) — the PHP boundary is "a finished chunk".

### Non-goals (v1)

- Hot-swapping pipelines on live worlds. Pipelines are per-world config,
  selected at creation, immutable afterwards.
- Features, structures, carvers (v2 — staged generation with declared
  read/write radii; see §10 milestone M6).
- Embedded scripting (Lua/Wasm custom nodes) — v2 candidate.
- PHP callbacks inside the pipeline. PHP constructs pipelines at world load
  and may hook post-population on the main thread; it never executes inside
  chunk generation (PMMP worker threads do not have plugin code loaded).
- Bit-exact feature placement parity (requires matching vanilla RNG call
  order; explicitly out of scope).
- Serving Java Edition. Target platform is Bedrock via PocketMine-MP.

---

## 2. Prior art & references

Study for design, never transcribe code without license check:

- **Vanilla data-driven worldgen** — the schema itself; documented on
  minecraft.wiki and datapack.wiki; machine-readable in mcdoc (Spyglass).
- **cubiomes** (MIT, C) — reference for noise/biome math.
- **Cuberite** (Apache-2.0, C++) — staged composable pipeline architecture
  (biome → shape → composition → finishers).
- **ext-vanillagenerator / Glowstone lineage** — our own prior port; known
  Java→C++ landmines live in `CLAUDE.md`.
- **Terra / OpenTerrainGenerator** — config schema design ideas only.
- **Misode's generators, Snowcapped, Spyglass** — external authoring tools we
  must remain compatible with.
- **deepslate** (MIT, TypeScript) — the library behind Misode's generators,
  used as a **black-box oracle only**, never read. See the provenance rule in
  `CLAUDE.md`. It earns that place by measurement: running its own
  `final_density` down a column and comparing the highest positive y against
  the `OCEAN_FLOOR` heightmap vanilla itself wrote into the golden regions
  gives **6143 of 6144 columns exact**, across the overworld, the Nether and
  the End and four seeds. It reproduces vanilla's terrain, which is a
  stronger claim than this project can make about any of its other oracles —
  and one made against the goldens, which are the authority (§7).

---

## 3. Schema pin

- **Pinned game version: Java Edition 1.21.11** (released 2025-12-09).
- **Data pack format: 94.1** (`min_format`/`max_format` scheme).
- Rationale: last version of the classic 1.x.y line; recent enough that the
  current datapack ecosystem targets 1.21.x.
- The pin is a number, not a policy. Upgrading the pin is deliberate,
  versioned migration work: schema diff review, conformance goldens
  re-generated against the new vanilla version, capability matrix re-checked.
  We do not chase snapshots.
- Pack loading validates `pack.mcmeta` version ranges and warns/errors on
  packs that do not declare compatibility with format 94.x.

## 4. Architecture

Three layers, strictly separated:

```
lib/      Pure C++ static library. No PHP headers. No zend types.
          Links against ext-chunkutils2's `lib/` chunk components.
cli/      Standalone tools built on lib/: render (PNG heightmap/biome/
          slice), generate (region output), diff (conformance), validate
          (JSON schema check).
ext/      Thin zend binding: registers the generator with PocketMine,
          marshals config in, hands finished chunk storages out.
tools/    Non-C++ helper scripts (fixture fetching, mcdoc sync, CI glue).
```

### 4.1 Pipeline model

- JSON is parsed and validated once at world load, resolved into an immutable
  in-memory graph: reference resolution (inline vs `namespace:id` holders),
  tag expansion, cycle detection with a clear error naming the cycle.
- The resolved graph is compiled into a flat execution program (no
  per-block virtual dispatch through a node tree). Interpretation of the
  graph per block is acceptable only as a Milestone-M2 stepping stone.
- Sampling follows vanilla's cell model: cell dimensions derived from
  `size_horizontal` / `size_vertical` in noise settings, density evaluated at
  cell corners, trilinear interpolation within cells.
- Cache node semantics (`cache_2d`, `flat_cache`, `cache_once`,
  `interpolated`) are implemented exactly as vanilla defines them relative to
  the cell structure — they affect *where* sampling happens and are therefore
  parity-critical, not merely optimizations.
- Pipeline objects are deeply immutable after compile and shared by reference
  across threads.

### 4.2 Threading

- Chunk generation runs on PMMP worker threads; the core library is
  re-entrant and lock-free during generation (immutability + per-task
  scratch arenas).
- All randomness is derived, never shared: every consumer derives its RNG
  from `(worldSeed, position, salt)` per vanilla's derivation rules. No
  shared mutable `Random` objects anywhere.

### 4.3 PHP surface (ext/)

- World load: PHP passes the world's stored pipeline blob (see §6) +
  seed; ext compiles it and registers a generator instance.
- Per chunk: ext calls `generateChunk(cx, cz)` on the compiled pipeline and
  receives populated `PalettedBlockArray` sub-chunk storages + biome arrays.
- Optional main-thread post-population hooks for plugins (decoration in PHP,
  outside the parity contract).

---

## 5. Determinism contract

These are hard requirements; violations are release blockers.

1. Same (pipeline blob, seed, chunk pos) → identical output on every
   supported platform, architecture, compiler, and engine release.
2. All seed/hash arithmetic in `uint64_t` with explicit wrapping; casts to
   signed only at API edges. Java semantics helpers (`floorDiv`, `floorMod`,
   `>>>`) are mandatory — raw `%` / `>>` on possibly-negative values is
   forbidden.
3. RNG implementations: Java LCG (`java.util.Random`-compatible) and
   Xoroshiro128++ with vanilla's seed derivation, including MD5-based
   position-independent salts from resource-location strings. Both verified
   against known-answer test vectors.
4. Floating point: `-ffp-contract=off`, never `-ffast-math` /
   `-funsafe-math-optimizations`; MSVC builds use `/fp:precise`. No FMA
   contraction differences between x86-64 and ARM64 may reach observable
   output. Transcendental functions are **not** covered by these flags:
   vanilla uses `StrictMath` (fdlibm) in places, and a platform libm may
   differ by an ulp. See the open item in §11.
5. CI runs the golden suite on x86-64 **and** ARM64, Linux + Windows +
   macOS. Cross-architecture divergence is a build failure.
6. Engine updates must reproduce stored pipelines byte-identically (§6). Any
   intentional output change bumps the pipeline engine version and is called
   out in release notes.

---

## 6. Per-world pipeline freeze

- At world creation, the fully-resolved pipeline (post-defaults,
  post-includes, tags expanded) is serialized into the world's data folder
  together with: engine version, schema pin, and a content hash.
- Generation always reads from this stored copy — never from the live preset
  registry — so preset edits and engine updates cannot introduce chunk seams
  into existing worlds.
- If the running engine cannot reproduce a stored pipeline version, it
  refuses to generate for that world with a loud, actionable error. Silent
  best-effort generation is forbidden.

---

**Landed (M3).** `stratum::freeze` writes and reads the blob:
`lib/include/stratum/freeze/pipeline.hpp`. It carries the resolved graph, the
noise parameters the graph names, and every noise settings entry — including
the surface rules and spawn targets this build does not yet interpret, which
have to round-trip anyway or a world frozen today could not be generated by
the build that finally understands them.

The format is binary, little-endian with explicit widths, and every double is
its bit pattern rather than a rendering: a printed double is a platform's
opinion and §5.6 needs a fact. Not in it: the world seed, which is world
metadata rather than pipeline — the same frozen pipeline generates different
worlds for different seeds.

Refusals, in the order they are reached: a file that is not a blob, a
container format this build does not write, **a pipeline engine version this
build does not reproduce**, a schema pin that does not match, a payload whose
length disagrees with the file, a content hash that does not match, and then
the structural checks behind all of that — an unknown node type, an index
that names nothing, trailing bytes. The engine-version refusal is the
load-bearing one: a build that generates differently from the one that froze
a world must not open it, because opening it and generating anyway is how a
seam gets into terrain that already exists.

The content hash is MD5. That is an integrity check and not a security
boundary — the question is whether the file changed, not whether somebody
forged it — and it is already in the tree and verified against `md5sum`.

The container format is **2** since M4. A node's `noise` field is a union in
the schema, so the blob writes a tag naming which spelling was used rather
than a present/absent flag. The two encodings agree byte for byte on a blob
with no inline noise in it, which is exactly why the number had to change: a
format-1 reader would take the new tag for the old flag and read an octave
count as the length of an identifier. Format-1 blobs are refused by the
existing container-format check, which is the intended behaviour rather than
a regression — a world frozen by a build that could not represent an inline
noise is not one this build can be sure it reproduces.

---

## 7. Parity tiers & conformance

| Tier | Scope | Bar |
|------|-------|-----|
| A | Density functions, noise, terrain shape, aquifer fill decision (air/default/fluid), multi-noise biome assignment, surface rules | Bit-exact vs vanilla Java, block-for-block, in Java block space |
| B | (v2) carvers, features, structures | Statistical parity: distributions match, exact placement may differ |
| U | Everything outside the capability matrix | Hard error at pack load, never silent skip |

### Conformance harness (built in Milestone M1, not later)

- `tools/fetch-vanilla`: downloads the official 1.21.11 server jar via
  Mojang's piston-meta version manifest, extracts
  `data/minecraft/worldgen/**`, runs the vanilla data generator where needed
  (multi-noise parameter lists), and generates region files for the fixed
  test seed set by running the vanilla server headlessly. Four properties of
  that run are load-bearing, each learned by getting it wrong first:

  1. **The world is frozen (`/tick freeze`) before any chunk is generated.**
     Worldgen is deterministic; the ticking world on top of it is not.
     Fluids keep flowing after a chunk is generated, so how far lava has
     spread when the region is saved depends on wall-clock timing. Measured:
     two runs of the same seed differed in 197 of 100,663,296 blocks — all
     flowing lava, flowing water, and the cobblestone where they met. Frozen,
     the same two runs agree exactly. Aquifer fill is Tier A, so those blocks
     cannot simply be excluded from the comparison; they have to not happen.
  2. **Carvers and features are stripped** by a generated datapack that
     empties every biome's `features` and `carvers`. They are Tier B (below),
     and would otherwise be noise in every Tier-A comparison. Ore *veins*
     stay, because they come from the noise router and are Tier A.
  3. **A margin of chunks is generated around each region** (default 4) so
     that every chunk of the region itself reaches `minecraft:full` status.
     Chunks at the edge of a generated area are otherwise left partway
     through the pipeline and are not comparable.
  4. **Goldens are compared as decoded blocks, never as bytes.** Two runs of
     the same seed produce region files with different SHA-256 hashes but
     identical block content: chunk timestamps, `InhabitedTime`, sector
     ordering and compression all vary. A byte-level harness would report
     failure on every run.
- `cli diff`: parses `.mca` region files and diffs vanilla output against
  engine output block-for-block in Java block space (before Bedrock
  mapping), reporting first divergence with coordinates and pipeline node
  trace.
- Fixed seed set: at least 8 seeds × overworld/nether/end noise settings ×
  a spread of chunk regions including y-extremes and biome borders.
- Golden fixtures are **generated locally / in CI and never committed**
  (they are derived from Mojang data). The repo ships scripts, not
  fixtures.

---

## 8. Capability matrix (v1)

Supported registries: `worldgen/density_function`, `worldgen/noise`,
`worldgen/noise_settings` (noise router + surface rules + spawn targets),
multi-noise biome source, and the minimum of
`dimension` / `world_preset` needed to select settings. Tags and
namespaced references within these.

Unsupported in v1 (hard error at load, listed by name in the error):
`configured_feature`, `placed_feature`, `configured_carver`, `structure`,
`structure_set`, `template_pool`, `processor_list`,
`flat_level_generator_preset`.

The matrix is published in the README and is part of the public contract.
A pack that half-loads silently is a bug of the highest severity class.

**Open question, raised by the loader (M2).** "Hard error at load" has two
readings, and the difference matters: taken literally it makes vanilla's own
data unloadable, since 1.21.11 ships 258 `placed_feature` and 224
`configured_feature` entries that v1 never executes — and the Tier-A goldens
are deliberately generated with features stripped because they are out of
scope. The loader therefore *classifies and reports* rather than baking in a
reading: every file is either loaded or recorded by name, nothing is dropped,
and `PackLoadOptions::rejectUnsupported` turns the presence of an
unexecutable registry into a hard failure for callers that need it. Which
should be the default for a user-supplied datapack is still to be decided;
loading vanilla's own data cannot be.

`stratum validate --strict` puts the same question to whoever is holding the
pack, which is a stopgap rather than an answer: the findings are reported
either way and the flag only decides whether they are fatal. The library
default stays permissive, and the question stays open.

---

## 9. Bedrock mapping layer

Its own component (`lib/mapping/`), its own tests:

- Java block state → Bedrock runtime state, built from maintained mapping
  data (GeyserMC mappings / pmmp upgrade schemas as reference inputs).
  Unmappable states resolve through an explicit, configurable fallback
  table — never a crash, never a silent stone substitution without a log.
- Biome mapping: custom/datapack biomes fall back to the nearest vanilla
  Bedrock biome (configurable) for client-side fog/color/music; the engine's
  internal biome identity is preserved for generation purposes.
- Mapping happens after conformance diffing (§7), never before.

---

## 10. Milestones

- **M0** — Repo scaffolding: CMake presets, CI matrix (x86-64+ARM64 ×
  Linux/Windows/macOS), lint/format config, this SPEC + CLAUDE.md.
- **M1** — Core primitives + conformance harness: Java-semantics helpers,
  LCG + Xoroshiro128++ with test vectors, perlin/simplex/normal noise,
  `tools/fetch-vanilla`, `.mca` parser, `cli diff`, `cli render`.
- **M2** — 2D pipeline: JSON load/validate/resolve for density functions +
  noise, interpreted evaluation, heightmap-style rendering, first golden
  comparisons on 2D-derivable values.

  Landed: the pack loader, the resolved density graph, the mcdoc-derived
  schema, the noise registry, the interpreter, heightmap-style rendering of
  a density function's field through `stratum render --pack`, and
  `stratum validate`. Vanilla's
  overworld climate chain — `shift_x`/`shift_z`, `continents`, `erosion`,
  `ridges`, `ridges_folded` and `offset` — is evaluated from vanilla's own
  JSON and matches cubiomes bit-for-bit across six world seeds.

  Remaining: the golden comparisons themselves. "2D-derivable values" turned
  out to be a smaller set than the milestone assumed — a chunk's stored
  `Heightmaps` are a product of terrain, not of the 2D chain, so there is
  nothing in a golden region a 2D pipeline alone can be diffed against. They
  move to M3, where generated terrain makes them meaningful. The cubiomes
  comparison is what stands in their place until then, and it is a weaker
  claim: an independent reimplementation agreeing with us, rather than
  Mojang's own output.
- **M3** — 3D density: full noise router, cell sampling + trilinear
  interpolation, all cache node types, aquifer fill decision, compiled flat
  execution program, Tier-A goldens passing for terrain shape.

  Landed: noise settings — geometry, flags, block states and the fifteen-entry
  noise router, whose inline density functions resolve into the *same* graph
  as the pack's named ones — and the cell sampler over the lattice they
  describe, which gives `interpolated` and `cache_all_in_cell` a meaning.
  All seven of vanilla's dimensions load; 94 of their 105 router entries are
  evaluable, and the eleven that are not are exactly the ones waiting on the
  node types §11 accounts for.

  The router's field list is generated from mcdoc, and that is not ceremony:
  at 1.21.9 `initial_density_without_jaggedness` became
  `preliminary_surface_level`. A hand-written list would have carried the
  older name and refused every vanilla noise settings file, for a reason the
  error would not have made obvious.
- **M4** — Biomes + surface: multi-noise biome source, surface rules,
  Tier-A goldens passing end-to-end in Java block space.
- **M5** — Integration: Bedrock mapping layer, zend binding, chunkutils2
  output, per-world freeze storage, PocketMine world-load path, perf pass.
- **M6 (v2)** — Staged features/structures with declared read/write radii;
  scripting escape hatch evaluation.

Each milestone closes only when its tests run in CI on all targets.

---

## 11. Decisions

Defaults chosen; flip only with a written note in this section:

- **C++ standard:** C++20. Toolchains: GCC/Clang per php-build-scripts;
  MSVC for Windows PHP builds (`/fp:precise`, see §5).
- **Build:** CMake ≥ 3.24 with presets; core lib usable without PHP.
- **JSON:** nlohmann/json (parse happens once at world load; ergonomics
  over throughput). Revisit only if load-time profiling demands it.
- **Tests:** Catch2 v3 for unit tests; golden/conformance via `cli diff`
  driven by CTest.
- **Schema validation:** derived from mcdoc definitions (vendored snapshot
  matching the 1.21.11 pin) rather than hand-written; hand-written checks
  only where mcdoc is insufficient. "Derived" means the type expressions are
  *expanded*: an alias matched by name instead is a hand-written narrowing
  wearing the generator's clothes, and one of those cost a legal input — see
  the noise-field bullet below.

- **Project name:** **Stratum**. Namespace prefix `stratum::`
  (`stratum::javamath`, `stratum::mapping`, ...). CMake targets
  `stratum_core` / `stratum_cli` (aliases `stratum::core` / `stratum::cli`),
  CLI binary `stratum`, PHP extension `ext-stratum`. Chosen in M0; renaming
  after M0 is a breaking change to the extension name and stored world
  metadata. Deliberately avoids "Mine"/"Craft" in the name, per Mojang's
  brand guidelines for third-party projects.
- **License:** **Apache-2.0**, decided in M0. Compatible with vendoring from
  MIT cubiomes and with adapting Cuberite under its own Apache-2.0 terms.
  `LICENSE` holds the canonical text; `NOTICE` holds the copyright line and
  third-party attribution, and every adaptation must be recorded there and
  in a per-file header before it merges.

- **StrictMath parity for transcendental functions:** vendor from
  **netlib fdlibm**, decided in M1. Java's `StrictMath` is fdlibm and is
  identical on every JVM; a host libm is not. Measured on x86-64/glibc,
  `std::log` disagrees with `StrictMath.log` on **30 of 1057** known-answer
  vectors, and that was enough to put `nextGaussian` +1 ulp out on 5 of 96
  values — a Tier-A parity failure (§7), not a rounding curiosity.

  `lib/src/fdlibm_log.cpp` is adapted from fdlibm's `e_log.c` under the
  SunPro notice (freely distributable, notice preserved), recorded in
  `NOTICE` and enforced by `tools/lint/check-determinism.sh`. It was taken
  from netlib, **not** from OpenJDK's `StrictMath`, which is GPL+CE.
  `std::sqrt` needs no such treatment: IEEE 754 requires it to be
  correctly rounded, so it already matches.

  `exp`, `pow`, `sin`, `cos` and `atan2` follow the same route as nodes
  need them: vendor from netlib, verify against JVM vectors, then use
  `stratum::fdlibm::` — never `<cmath>` — in parity-critical code.

Open:

- **Salted and positional RNG derivation has no oracle yet.** §5.3 requires
  vanilla's seed derivation "including MD5-based position-independent salts
  from resource-location strings", verified against known-answer vectors.
  The generator itself and `mixStafford13` are verified against the JDK's
  own implementations, and the 64→128-bit seed upgrade against an
  independent implementation of the documented formula — and, since the
  noise work, against cubiomes, whose `xSetSeed` derives the same state
  constant for constant.

  `nextInt(bound)` and the MD5 salt derivation are verified too: the bounded
  draw against cubiomes, and the salts against `md5sum`, which turned out to
  be a genuinely independent oracle for them — vanilla's octave salts are
  simply `md5("octave_<n>")` taken as two big-endian halves. That is what
  unblocked the noise.

  The **positional random factory** — a world seed forked into a 128-bit
  base by two draws, then salted per name with that name's MD5 — is verified
  as of M2 against cubiomes' `setBiomeSeed`, which derives the climate
  noises identically. Every noise vanilla's overworld uses now matches
  cubiomes bit-for-bit from a world seed.

  Still deliberately **not implemented**, for want of an oracle: Xoroshiro
  Gaussians, the general-purpose `fork()` that derives a child generator for
  a sub-task, and positional seeding at a block position. A caller that
  needs one fails to compile rather than silently seeding a world wrongly.
  They land with M3, when generated terrain can be diffed against the
  goldens and right can be told from plausible.

- **Which density function types the interpreter evaluates (M2).** The
  resolver builds all 34 types the schema declares; the interpreter refuses
  eight of them by name, in two groups.

  *Not defined by a point alone* — `interpolated`, `cache_all_in_cell`,
  `slide`, `find_top_surface`. Their value depends on the cell the point
  sits in, or on noise settings the pipeline does not yet carry. They arrive
  with the cell sampler in M3.

- **`weird_scaled_sampler` is settled, and the wiki could not have settled it
  (M3).** Vanilla's own removal changelog for 26.2 gives the formula outright:

      abs(rarity * noise(x/rarity, y/rarity, z/rarity))

  The coordinates are divided by the rarity, the sampled value is multiplied
  by it, and the modulus is what makes this a cave function — the tunnels are
  where the result is near zero, which is *both* sides of the noise's own zero
  crossing rather than one. That the output is non-negative is what the probe
  noticed first, before any formula was consulted: a field running +0.007 to
  +1.317 where the same noise sampled plainly ran -1.02 to +0.84.

  The rarity ladders, both measured off the vanilla server:

  | `type_1` | | `type_2` | |
  |---|---|---|---|
  | input < -0.5 | 0.75 | input < -0.75 | 0.5 |
  | input < 0.0 | 1.0 | input < -0.5 | 0.75 |
  | input < 0.5 | 1.5 | input < 0.5 | 1.0 |
  | otherwise | 2.0 | input < 0.75 | 2.0 |
  | | | otherwise | 3.0 |

  Compared with a strict `<`, so a value sitting exactly on a threshold takes
  the rarity above it. Every threshold was probed at the value itself and at
  plus and minus 1e-7, which is what pins that rather than assuming it.

  **The names could not be taken from documentation.** minecraft.wiki carries
  both ladders, and its two pages assign them to `type_1` and `type_2` in
  *opposite* order — the Density function page one way, the 26.2 changelog the
  other. A build that trusted the wiki had an even chance of generating every
  cave in the world from the wrong ladder, and nothing in the wiki resolves
  it. The server does, and the assignment above is the server's. The unit
  tests assert the one thing a swap would break — that `type_2` reaches 0.5
  and 3.0 where `type_1` never does, and `type_1` reaches 1.5 where `type_2`
  never does — because nothing else in the file would notice.

  **How it was measured.** `tools/analysis/density-probe.sh` generalises the
  datapack probe: a spec file lists functions to test, each becomes its own
  *dimension* in one world, and one server start therefore measures a whole
  sweep instead of one point. 51 dimensions mapped the ladders coarsely; 35
  more pinned the thresholds. The fit is seeded from the field's spread —
  `sd(r * |noise(p/r)|) = r * sd(|noise|)`, so the spread gives the rarity
  outright — because a blind search cannot find it: at x = 127 a step of
  0.001 in the rarity moves the sampled point by 0.13 in noise space, which
  decorrelates a noise whose features are 8 wide.

- **The terrain chain runs end to end (M3).** With
  the blended noise and this settled, the overworld's `final_density`
  evaluates for the first time. Against vanilla's own recorded OCEAN_FLOOR
  heightmaps, over 4096 columns a seed:

  | seed | exact | within one block | worst |
  |---|---|---|---|
  | 42 | 99.561% | 100% | 1 block |
  | 0 | 99.902% | 99.951% | 2 blocks |
  | -1 | 96.167% | 97.949% | **50 blocks** |

  **What that residual turned out to be, and it was not the density (M3).**
  The table above is dominated by a step this build does not implement rather
  than by an error in the chain. Vanilla's overworld runs aquifers, and an
  aquifer places a stone BARRIER between two bodies of water at different
  levels — solid blocks no density function produced. OCEAN_FLOOR is the
  highest block that is neither air nor fluid, so it reports the barrier, and
  a comparison against `final_density` reads that as terrain being wrong.

  Measured, not argued. Regenerating seed -1 from vanilla's own overworld
  settings with one field changed — `aquifers_enabled: false`, everything
  else byte for byte — moves the same 4096 columns:

  | seed -1, 4096 columns | with aquifers | without |
  |---|---|---|
  | exact | 96.167% | **98.267%** |
  | within one block | 97.949% | **100.000%** |
  | worst | **50 blocks** | **1 block** |

  The worst column, (104, 112), is the mechanism in miniature: gravel over
  stone at y = 27-28 floating in water with aquifers on, water all the way
  down with them off, and OCEAN_FLOOR moving 28 to 9 — which is where this
  build's density does turn positive. Two branches were eliminated on the
  way: `noodle` is +64 in every disagreeing column and never the minimum, and
  `squeeze` cannot be responsible because near the zero crossing its input is
  small enough that the cubic term is worth about 1e-11.

  So the aquifer fill decision, which §7 already places in Tier A, is what
  stands between this and faithful terrain — not something unfound in the
  arithmetic. `tools/analysis/aquifer-free-probe.sh` produces the reference,
  and `golden_terrain_no_aquifer_test.cpp` pins the comparison against it.

  **`find_top_surface` is settled (M3).** minecraft.wiki gives the semantics —
  "scans through a column of an input density and returns the topmost y-level
  that is above 0. If no such position exists within the bounds, the
  lower_bound is returned" — and leaves four things open. All four were
  measured off the server with a density whose zero crossing is placed by
  hand, so each answer is analytic rather than inferred:

  * **The scan lattice is absolute multiples of `cell_height`**, anchored to
    neither bound. Probed with an `upper_bound` of 317 and a `lower_bound` of
    -60, both off a lattice of eight, and vanilla still answered in multiples
    of eight. A loop written straight from the wiki would anchor to one bound
    or the other and be wrong by up to seven blocks.
  * **`upper_bound` is floored onto that lattice, and is inclusive when it
    lands on it.** 319.9 scans from 312; 320.0 scans from 320. It floors
    rather than rounds, which matters because vanilla's own `upper_bound` is
    a `clamp` and fractional in general.
  * **The test is a strict `> 0`.** A density of exactly zero at a lattice
    point is not a surface.
  * **Nothing found returns `lower_bound` itself**, as the wiki says.

  Checked end to end: vanilla's own `preliminary_surface_level`, read back
  through the datapack probe, agrees with this build on **1024 of 1024
  columns**, every one inside the probe's own resolution.

  That comparison also closes something else. `invert` was the only density
  function type vanilla uses that nothing here had ever compared against
  anything — all three of its uses sit inside `preliminary_surface_level`,
  which was refused for want of `find_top_surface`. It is now reached and
  checked. The old refusal's stated reason was wrong too: it said the node
  "needs the cell sampler", and it needs no lattice at all.

  With this, **all 45 of vanilla's 45 noise router entries evaluate**, across
  every dimension this build can seed.

  **The chunk filler (M3).** The first code in this project that produces a
  block rather than a number. For every position: `default_block` where
  `final_density` is positive, `default_fluid` below `sea_level` where it is
  not, air above that.

  Two things were learned by building it, both from comparing against blocks
  the server actually wrote:

  * **`sea_level` is EXCLUSIVE.** With vanilla's 63 the water stops at 62 and
    63 is the first air. An inclusive comparison put one extra water block on
    top of every column in the world — 256 a chunk, and the only category
    disagreement in four chunks. Nothing short of a golden comparison would
    have caught a mistake that uniform.
  * **`interpolated` is where the time goes.** It is defined over a cell, so
    evaluating it at a point costs eight evaluations of its argument, and a
    filler asking for every block of a 4x8x4 cell paid that 128 times for
    eight values that never change. Computing them once per cell is
    **86.8 times faster** — 39.7 seconds a chunk down to 0.46 — and bit-for-bit
    identical, which is asserted over 17000 points rather than assumed. The
    cache belongs to the calling task, not to the interpreter, because a
    compiled pipeline is immutable and shared between threads (§4.1).

  Against the aquifer-free reference, over four chunks and 393216 blocks:
  **every block is in the right category** — solid, fluid or air — and 82.013%
  are the exact right block. All of the remaining 17.987% is surface rules
  this build does not run: deepslate and bedrock, which are vertical gradients
  reaching the whole column rather than a skin at the top, and gravel, dirt
  and grass at the surface.

  **What it refuses.** A dimension with `aquifers_enabled` or
  `ore_veins_enabled` is refused by name at compile, not filled approximately.
  Vanilla's overworld sets both, so this filler cannot generate it today —
  which is the honest position: with aquifers the block is not a function of
  the density, and filling as though it were floods every cave in the world.
  §8 puts a world that generates and is quietly wrong in the most severe class
  there is, and this is exactly that case.

  Surface rules are NOT refused, because they only ever replace blocks the
  filler already placed. A column without them is bare stone where grass and
  dirt belong — visibly incomplete rather than wrong.

  **Surface rules: `vertical_gradient`, half settled (M4).** Two of the
  overworld's three top-level surface rules are vertical gradients — bedrock
  at the world floor and deepslate — and between them they account for 4368
  of the 4496 blocks the filler currently gets wrong. They are also the only
  undocumented rule types the aquifer-free probe world can exercise, because
  the other four sit under a `biome` condition and that world has one biome.

  **The probability is settled.** Counting vanilla's own blocks by height:

  | deepslate, y | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
  |---|---|---|---|---|---|---|---|
  | measured | 0.868 | 0.750 | 0.620 | 0.498 | 0.372 | 0.252 | 0.125 |
  | (8 - y) / 8 | 0.875 | 0.750 | 0.625 | 0.500 | 0.375 | 0.250 | 0.125 |

  and bedrock, over a different band with different anchors, matches
  `(-59 - y) / 5` just as closely. So the probability at height y is
  `(false_at_and_above - y) / (false_at_and_above - true_at_and_below)`,
  clamped, on two independent gradients.

  **The draw is per block, not per column.** A single draw per column would
  make every column deepslate up to a height and stone above it; 47.8% of
  columns are, where 100% would be.

  **What the random source is remains open, and the shape of the answer is
  now constrained.** The outcome correlates between vertically adjacent
  blocks by +0.089 over what independent draws with the same marginals would
  give — and by +0.003 horizontally, +0.001 in z, and +0.008 two blocks
  apart in y. That is a fingerprint: correlation only between y and y+1, in
  one axis, gone by y+2.

  A well-mixed hash of (x, y, z) cannot produce it, which is why eight
  candidate derivations — Xoroshiro and the Java LCG, three position mixes,
  seeded from the world seed, from the MD5 of `random_name`, and from both —
  all scored at the marginal, 62%, against 96535 labelled positions. They are
  recorded here so the next attempt starts past them rather than at them.

  **Isolated, the draw gives up three more of its properties (M4).**
  `tools/analysis/density-probe.sh` now carries a `surface_rule`, a
  `raw_final_density` and the two cell sizes per dimension, so a probe can
  vary the lattice under a fixed rule, and can be one construct alone
  over solid ground with a marker block for its output — no caves, no
  aquifers, no competing rules, and the answer read straight from block
  identities rather than from a terrain height.

  * **The formula holds away from vanilla's own data.** A band of 0..4 gives
    0.755, 0.503, 0.248 against `(4 - y)/4`; a band of 0..8 reproduces the
    golden numbers.
  * **The draw depends only on `random_name` and the position.** Three bands
    sharing one name were checked for nesting — a smaller threshold
    succeeding must imply a larger one does, if and only if the band enters
    nowhere but the threshold. 100% nested, **zero violations**.
  * **The draw is uniform.** Choosing bands so that `p(y=1) = k/32` for
    k = 1..31 brackets the value at every position to a thirty-second; over
    16384 positions the histogram is flat and nothing is non-monotone.

  That last one is an instrument, not just a result: a candidate derivation
  can now be scored against *bracketed values* rather than against a coin
  flip, where a wrong answer lands in the right bucket 3.1% of the time
  instead of 62%. **Nineteen candidates have been refuted** — eight against
  binary outcomes and eleven at the sharper discrimination, spanning both
  RNGs, four position mixes and four seedings. All landed at chance. They are
  written down so the next attempt starts past them.

  **The draw is not an independent per-position value at all (M4).** With the
  bracketing instrument the correlation can be measured as a correlation
  rather than as an agreement rate, and it has a shape:

  | vertical lag | 1 | 2 | 3 | 4 |
  |---|---|---|---|---|
  | rho | **+0.273** | +0.133 | +0.055 | -0.003 |

  against +0.010 and +0.018 one and two blocks sideways. So the value behaves
  like a field with a vertical correlation length of about three blocks and no
  horizontal correlation whatever — which is not what a WELL-MIXED hash of
  (x, y, z) produces, and is why nineteen of them scored at chance. The
  measurement below sharpens both halves of this: the horizontal figures are
  an artefact, and the vertical one is a bit effect rather than a distance.

  **It is not a cell, and it is not a distance — it is the bits of y (M4).**
  Splitting the lag-1 pairs by whether they share a vertical cell gave +0.314
  within a cell against +0.104 across a boundary, which looked like the
  answer. It was confounded: over a y range of six, "across a boundary" is one
  particular pair of heights. Two experiments took the confound out, and
  between them they refute the cell reading and replace it.

  *Widening the sweep.* Fifty-five bands, band k with
  `true_at_and_below: k - 31` and `false_at_and_above: k + 1`, put a full
  thirty-second ladder at every height from 0 to 23 — six whole cells instead
  of one boundary. The split survives (+0.182 within, +0.043 across at lag 1),
  but the lag profile that comes with it does not decay: it combs, near zero
  at lag 4 and lag 8 and positive at 5, 6 and 7.

  *Varying the cell.* The same rule over the same solid ground at
  `size_vertical` 1, 2 and 4 — cells of 4, 8 and 16 blocks — is **block for
  block identical**, 0 of 786432. So the draw never consults the density
  lattice at all, and a four-block cell cannot be what the split was seeing.
  What the split was actually seeing is a fixed phase in the absolute height:
  lag-1 pairs at `y ≡ 0` and `y ≡ 2 (mod 4)` couple at +0.153, at `y ≡ 1` at
  +0.105, and at `y ≡ 3` at +0.035, repeating exactly at mod 8.

  Neither separation nor phase explains that alone, so the two were crossed —
  separation down, the absolute height's residue mod 4 across, over 786432
  labelled blocks:

  | lag | y=0 | y=1 | y=2 | y=3 | mean |
  |---|---|---|---|---|---|
  | 1 | +0.1530 | +0.1048 | +0.1531 | +0.0350 | +0.1115 |
  | 2 | +0.0796 | +0.0832 | +0.0275 | +0.0284 | +0.0547 |
  | 3 | +0.1046 | +0.0135 | +0.0333 | +0.0171 | +0.0421 |
  | **4** | **+0.0019** | **+0.0019** | **+0.0020** | **+0.0017** | **+0.0019** |
  | 5 | +0.0162 | +0.0350 | +0.0156 | -0.0011 | +0.0164 |
  | 6 | +0.0269 | +0.0269 | -0.0008 | -0.0017 | +0.0128 |
  | 7 | +0.0349 | +0.0035 | -0.0023 | +0.0019 | +0.0095 |
  | **8** | **+0.0021** | **+0.0004** | **+0.0039** | **+0.0064** | **+0.0032** |

  Lags 4 and 8 are flat at every phase; every other lag has structure. **Two
  blocks whose heights agree mod 4 are independent, and no others are.** The
  bracketing instrument says the same thing without ever estimating a
  threshold — its lag-4 correlation is -0.003 against +0.273 at lag 1 — so
  this is a property of the draw and not of the baseline.

  The horizontal axes stay flat under the same treatment, to within 0.003 at
  every separation and phase. This corrects the earlier figures: the +0.010
  and +0.018 quoted sideways were an artefact of estimating the baseline
  across heights of differing probability.

  Read down the table rather than across and the structure names itself. The
  six pairs that sit inside one group of four absolute heights — (0,1), (0,2),
  (0,3), (1,2), (1,3), (2,3) — carry +0.153, +0.080, +0.105, +0.105, +0.083
  and +0.153. Every pair that crosses a group boundary carries +0.035 or less.
  So the draw is grouped into FOUR-BLOCK RUNS OF ABSOLUTE HEIGHT, which is
  what the first experiment saw; what it got wrong was calling that group the
  density cell, and the second experiment is what rules the density cell out.

  **What this changes is the search.** The grouping is four blocks whatever
  the cell height is, so something other than the noise lattice fixes it. That
  does not put the target outside the class of position functions — a value
  sliced out of one word per (x, z, floor(y/4)) would look exactly like this,
  independent across groups and correlated within one — but it does say a
  twentieth flat position mix is the wrong next move, because every one of the
  nineteen was a function of y that treats all heights alike. The table itself
  is the acceptance test, and a far sharper one than a hit rate: a candidate
  must put zeroes on the lag-4 and lag-8 rows and reproduce the phase
  structure on the rest.

  One thing this cannot yet say is where the group boundary sits. Both probes
  run with `min_y: -64`, and 64 is a multiple of four, so "aligned to absolute
  y" and "aligned to the bottom of the world" predict the same table. Vanilla
  requires `min_y` to be a multiple of sixteen, so no setting of it separates
  them; whatever tries next has to distinguish them some other way.

  **What the vertical correlation looks like, in the values themselves.** The
  bands used for bracketing put a threshold at every height, not only at
  y = 1 — band k has `p(y) = (1 + k - y)/32` — so the same run brackets the
  draw at two adjacent heights for the same position, and the two can be
  crossed. Over 14944 positions the shift between them is a symmetric peak on
  zero:

  | shift in 32nds | -3 | -2 | -1 | 0 | +1 | +2 | +3 |
  |---|---|---|---|---|---|---|---|
  | share | 3.9% | 5.1% | 6.5% | **8.1%** | 6.6% | 5.0% | 3.6% |

  against 3.1% everywhere if the two were independent. So the draw at y+1 is
  *near* the draw at y far more often than chance and yet frequently
  unrelated — not a shared draw, not a fixed offset, and not independence.
  Whatever seeds the two positions is close for adjacent heights and the
  values it produces are correlated rather than equal, which is a strong
  constraint on the derivation and the reason a well-mixed hash cannot be it.

  The vertical-only correlation survives isolation, which settles that it was
  the random source and not the world: +0.067 for `minecraft:deepslate` and
  +0.097 for another name, against +0.003 and below horizontally. That it
  varies with the NAME by nineteen standard errors says it is a property of
  particular seeds rather than of the construction — which is what a
  weakly-mixed position seed looks like, and why a well-mixed hash keeps
  failing.

- **Surface rules load whole, and refuse by name (M4).**
  `stratum::surface::RuleGraph` resolves the tree for every one of vanilla's
  seven dimensions: the overworld's is 287 rules over 141 conditions naming 7
  noises, an order of magnitude larger than anything else here. All fifteen
  types are read; anything the schema does not define is an error naming the
  type, never a skip.

  Resolving is separated from RUNNING on purpose. The structure of every type
  is documented and can be loaded today; the semantics of ten of them are not
  settled, and §8 would rather refuse a rule by name than approximate it. So
  the graph reports what it cannot execute and why — and the reasons are the
  measurements above rather than "unimplemented": `vertical_gradient` says its
  probability is settled and its random source is not, `hole` says it fires on
  0.04% of terrain and its comparison is undocumented, `steep` says its
  predicate needs neighbouring columns the filler cannot reach.

  Five constructs are already runnable — the three structural rules, `not`,
  and `above_preliminary_surface`, the last only because `find_top_surface`
  landed. That makes `minecraft:end`, whose whole surface rule is one `block`,
  the one dimension this build could decorate the moment there is an executor.

  The schema is written out rather than generated, and that is a debt (§11).
  Five of the fifteen are absent from mcdoc entirely, so they would be
  hand-written whatever happens — the precedent is `tools/mcdoc/schema.py`,
  which already hand-writes `blend_alpha` and `end_islands`. The other ten
  could be generated and are not: the generator cannot parse either
  surface-rule mcdoc file, and the types they refer to live in a third file it
  cannot parse either.

- **The other four undocumented surface constructs, measured (M4).** In
  vanilla's data these all sit under a `biome` condition, so no probe of
  vanilla's own overworld can reach them. Written directly into a probe's own
  rule tree they are reachable, and four of the five gave something up.

  * **`temperature` is a threshold at 0.30, strict.** Sweeping a fixed
    biome's temperature: true at -0.5, 0.0, 0.14 through 0.29 inclusive, and
    false from 0.30 up. One position of 98304 at exactly 0.30 came out true,
    which is evidence of a position-dependent adjustment near the top of the
    world; separating "0.30 flat" from "a lower threshold plus an adjustment"
    needs a sweep of terrain HEIGHT, not of temperature, and has not been
    done.
  * **`bandlands` paints terracotta banding**, and the band table is readable
    straight off: plain terracotta, orange, red, white and light grey, in
    that order of frequency, over a fixed column.
  * **`steep` fires on 16.9% of columns** of a gently varying terrain — a
    workable signal, but deriving the predicate needs neighbouring columns'
    heights, which the filler's per-column API cannot currently reach.
  * **`hole` fires on 0.04%**, six columns in sixteen thousand. Too rare on
    ordinary terrain to derive from; it needs terrain built so that the
    surface depth is zero over a known area.
  * **`above_preliminary_surface` was true everywhere** on the probe's
    terrain and so said nothing. It needs terrain where the preliminary
    surface and the real one differ, which the probe did not arrange.

  **The aquifer fill decision: what it is, and what it will cost (M3).**
  Scoped, not started. Three things are now known about it.

  *It is undocumented.* minecraft.wiki describes what the four router entries
  influence but gives no algorithm: no cell grid, no fluid-level formula, no
  barrier rule. The single numeric constant documented anywhere permitted is
  the lava threshold, 0.3. So this is a derivation like `old_blended_noise`
  was, not an implementation — and a larger one, because the unknowns are
  structural (a cell lattice, randomised centres, a nearest-cells search)
  rather than a formula with a few constants.

  *Its effect is measured.* Generating seed -1 twice from vanilla's own
  settings, once with `aquifers_enabled` and once without, 70426 of 6291456
  blocks differ — 1.12%. By transition:

  | from the pure density field | to vanilla | share |
  |---|---|---|
  | water | **air** | 81.3% |
  | water | deepslate / stone / gravel (**barriers**) | 14.8% |
  | water | **lava** | 3.2% |

  So the aquifer's main job is *draining*: without it every underground void
  below sea level fills with `default_fluid`, which is wrong for playability
  and not only for parity. The barriers that broke the terrain comparison are
  a sixth of its work, not the bulk of it.

  *It is observable, with a catch.* Every block of a generated chunk is a
  labelled sample, and the inputs can be controlled through a datapack, so
  the geometry is reachable by measurement. The catch, found immediately: the
  top of a column's water body is `min(aquifer fluid level, cavity ceiling)`,
  not the fluid level, so naive run-length estimates of the cell spacing are
  contaminated by terrain shape — a bimodal histogram of runs, one mode from
  cell boundaries and one from cave roofs. Any estimator has to take the
  level from where a cavity is known to reach above it.

  deepslate implements aquifers and reproduced vanilla's OCEAN_FLOOR on 6143
  of 6144 golden columns, so it is available as the fast black-box oracle
  here, with the server as the authority — the same arrangement that settled
  the blended noise.

  **An open world is a better instrument than a cave (M3).** The catch above
  is that a fluid top is `min(fluid level, cavity ceiling)`, so terrain
  contaminates every estimate. The way past it is to delete the terrain: a
  dimension whose `raw_final_density` is the constant -1 has no solid block
  anywhere, so nothing can clip a fluid top and every boundary in the chunk
  belongs to the aquifer. `tools/analysis/density-probe.sh` carries
  `aquifers_enabled`, `sea_level`, `default_fluid`, per-entry `min_y` and
  `height`, and a `router` map that overrides individual entries, which is
  what lets a probe hand the aquifer vanilla's own four noises while holding
  everything else at zero.

  A cross-section of that world shows the whole mechanism at once: the sea
  aquifer's water from y = 62 down to about -12, a transition zone of stone
  barriers and drained air pockets, and a global lava floor below.

  *The global lava level is -54.* Lava is 100% of blocks at y <= -56, 89.2%
  at -55, and **0.00%** at -54 — a hard edge. That is the same exclusive
  convention the sea uses: `sea_level` 63 puts the water top at 62, and a lava
  level of -54 puts the lava top at -55. The one documented constant, the 0.3
  lava threshold, now has a measured level to go with it.

  *The vertical cell is twelve blocks, anchored to absolute y.* Where a column
  stops belonging to the sea aquifer and starts belonging to a deeper cell is
  a cell edge, and those heights concentrate hard: by residue mod 12 they run
  14.8% at 0 down to 2.1% at 7, while mod 6 is flat to a chi-square per degree
  of freedom of 2.5 — which is exactly what folding a period-12 distribution
  in half produces. Running the identical probe at three world floors settles
  the anchor:

  | | peak residue of `y mod 12` | peak residue of `(y - min_y) mod 12` |
  |---|---|---|
  | `min_y` -64 | 8 (14.8%) | 0 |
  | `min_y` -80 | 8 (14.8%) | 4 |
  | `min_y` -48 | 8 (15.1%) | 8 |

  The `y mod 12` histogram is IDENTICAL at all three floors, shape included;
  the `(y - min_y)` one moves with the floor.

  **That experiment was void as first run, and is recorded here because the
  failure mode is easy to repeat.** The probe wrote `minecraft:overworld` as
  the dimension TYPE, and a dimension's type — not its noise settings — fixes
  the world's height. Every arm therefore generated at `min_y` -64: all three
  worlds stored `yPos = -4`, two of them were the same world twice, and the
  `(y - min_y) mod 12` shift that looked like a result was pure arithmetic.
  `density-probe.sh` now emits a dimension type of its own whenever the floor
  moves, and the re-run is a real test — the worlds genuinely differ, lava
  reaching y = -78 at `min_y` -80 and stopping at -64 otherwise — and the
  conclusion survives unchanged: the aquifer geometry above the floor does not
  move with the floor.

  The same fix settles a question that had been withdrawn as untestable: the
  lava level is ABSOLUTE at -54, not `min_y + 10`. At `min_y` -80 lava still
  tops out at y = -55 rather than -71, and at `min_y` -48 there is no lava at
  all, the floor being above it.

  *Horizontally it is a Voronoi, and the spacing is not yet pinned.* The level
  field is large irregular flat regions rather than aligned blocks, which is
  what jittered centres inside grid cells produce, so no modulus lines up with
  the boundaries — every candidate from 4 to 24 sits within 1.3x of the
  background step rate. What the field does say is scale: two-point
  disagreement rises to a plateau by a separation of about twelve to sixteen
  columns. Consistent with a sixteen-wide grid, not established by it, and the
  estimator to build next is one that finds the CENTRES rather than the edges.

  **The fluid level, as a formula (M3).** The probe's `router` map overrides
  single entries, so the aquifer's own inputs can be held at CONSTANTS while
  vanilla's machinery runs around them. With `lava` pinned to -1 no fluid is
  lava, so every water-to-air boundary is a fluid level and can be read
  straight off. Sweeping `fluid_level_spread` at nine values from -1 to 1,
  with `fluid_level_floodedness` at 0, gives -32, -29, -26, -23, -20, -20,
  -17, -14, -11: steps of exactly three, with a doubled step at zero, which is
  a floor rather than a round.

  Two forms fit those nine. Ten more values chosen to separate them settle it:

  | spread | 0.29 | 0.31 | 0.58 | 0.62 | 0.88 | -0.29 | -0.31 | -0.58 | -0.62 |
  |---|---|---|---|---|---|---|---|---|---|
  | level | -20 | -17 | -17 | -14 | -14 | -23 | -26 | -26 | -29 |

  `3 * floor(spread * 3.5)` predicts five of these wrongly; the transitions sit
  at spread = +-0.3, +-0.6, +-0.9, so

  ```
  fluid level = base + 3 * floorDiv(floor(spread * 10), 3)
  ```

  which is `base + 3 * floor(spread * 10 / 3)` — the same function, and the
  bracketing puts the multiplier in (9.68, 10.34), consistent with exactly ten.
  Note the two floors: this is a place where a C++ `/` on a negative value
  would silently disagree with vanilla for every cell below the offset, which
  is what §5's `floorDiv` rule exists for.

  *The offset is -20 and does not follow the sea.* At `sea_level` 63 and 32 the
  spread-controlled level is -20 in both, while the sea plateau itself moves
  from 62 to 31 as expected. At `sea_level` 0 and -32 the world comes out with
  no fluid at all, so the aquifer's fluid region is not simply "below sea
  level" and the offset is not measured from it.

  *Floodedness is a gate with at least three regimes, and the name is a trap.*
  Held constant with the spread at 0: at -1 to -0.25 essentially every column
  takes the sea level; at 0 the spread controls, though only 3% of columns; at
  0.75 and 1.0, 27-29% of columns sit at a fixed deep level of -54 that the
  spread does NOT move. So higher floodedness means LESS flooded here, and the
  deep branch is not the spread branch.

  *The aquifer carries randomness of its own.* With all four of its inputs
  pinned to constants, 27% of columns still differ from the other 73%. Nothing
  in the router can explain that, so the flooded decision is not a pure
  function of the four noises — which is the jittered-centre randomness the
  horizontal geometry already implied, showing up in a second place.

  **The horizontal pitch is sixteen (M3).** Bounding it was easy and pinning
  it was not, for a structural reason: every configuration tried at first
  either flooded almost every cell to the sea, or gave the non-flooded ones a
  single shared level, so neighbouring cells merged and counting regions
  undercounted cells. Three things together fixed that.

  * `preliminary_surface_level`, which the probe had been holding at zero all
    along, turns out to be an input to the flooding decision — at 320 the
    aquifer produces no fluid at all, and raising it to 64 stops the sea
    flooding everything, taking 83% of columns off the sea plateau.
  * `fluid_level_floodedness` at 0.5 keeps cells in the spread branch rather
    than the deep one that parks them all at -54.
  * The spread wrapped in `flat_cache`, so it is one value per column and
    cells stacked in y cannot disagree — without this the level field
    fragments into thousands of tendrils, because a per-column scan finds
    whichever body is deepest.

  Then the test that settled the vertical lattice works sideways. Level
  changes by position within a candidate pitch, over 16384 columns:

  | pitch | 8 | 12 | **16** | 20 | 24 |
  |---|---|---|---|---|---|
  | spread of the step rate | 0.058 | 0.039 | **0.327** | 0.099 | 0.188 |

  Sixteen wins by a factor of three over anything else, and the two that beat
  the rest are its own aliases — pitch 8 is the pattern folded in half and
  pitch 24 is it beaten against a period of 48. The profile within the pitch
  runs from 0.075 at offset 4 to 0.402 at offset 13: smooth rather than
  spiked, which is the smearing that jittered centres produce.

  It survives every control. Repeating with the spread noise at `xz_scale` 4,
  8 and 16 gives pitch-16 spreads of 0.187, 0.327 and 0.398 with the SAME
  phase, so the sixteen belongs to the aquifer and not to the input; and x and
  z separately agree, peaking at offset 12 and 13 against a trough at 4 to 6.
  The peak is not at offset 0, so it is not an artefact of the 16-wide chunk
  either.

  **So the aquifer cell is 16 x 12 x 16**, with the vertical grid line at
  `y = 8 (mod 12)` and the horizontal boundaries concentrating around
  `x = 12 (mod 16)`, centres near `x = 5`. Every number there is measured
  against the server rather than assumed from the shape the wiki describes.

  *One contamination to know about.* Where the probe's water meets its lava
  the server's fluid physics makes obsidian and cobblestone — 2821 blocks,
  0.045% — after generation rather than during it. Small enough to ignore for
  geometry, not small enough to ignore when this becomes a bit-exact
  comparison.

  **What the base is NOT, and why the obvious readout was wrong (M3).**
  Sweeping `preliminary_surface_level` with the spread pinned at zero should
  read the base straight off, and it does not behave like a formula.

  *It does not follow the sea.* At psl 0 the whole non-sea structure is
  byte-identical at `sea_level` 32, 63 and 96; at psl 96 it is identical again
  at 32, 96 and 128. Five sea levels, two regimes, no movement. Whatever the
  base is measured from, it is not the sea.

  *The surface is a gate, not a slope.* Between psl 52 and psl 60 the world
  flips: at and below 52 the sea plateau covers 95-99% of columns, and at and
  above 60 every column reads 19. Then psl 56, 64, 72, 80, 88, 96, 112, 128
  and 160 give IDENTICAL output. A quantity that stops responding over a
  three-fold change in its input is not a term in a sum.

  *And floodedness is non-monotone in the same place.* Holding psl at 96:
  floodedness 0 and 0.25 produce no fluid at all, 0.5 and 0.75 produce the
  uniform 19, and 1.0 produces the sea plateau with 27% of columns at -54. So
  the gate is joint in the two inputs and cannot be read one axis at a time.

  *The readout itself was the mistake.* A cross-section of the psl-96 world
  shows water from y = 19 down to about y = -3 and air below it, with barriers
  along the lower edge — the fluid body has a BOTTOM. If every cell shared the
  level 19, every cell below 19 would be full; the air says the cells beneath
  hold a much lower level. So "the level" is per CELL and varies with the
  cell's own height, and a per-world histogram reads whichever body happens to
  be topmost. That is why the low-psl sweep looked non-monotone: at 1-3%
  coverage the modal value jumps between populations rather than tracking one.

  What this changes is the instrument, not the target. The base needs a
  per-cell readout — every fluid body in a column, tagged with the cell it sits
  in — where every measurement so far has taken one number per world. Building
  that is the next step, and until it exists no formula for the base should be
  written down.

  **The per-cell readout, and what it shows (M3).**
  `tools/analysis/aquifer-cells.cpp` enumerates every fluid body in every
  column and tags each by the lattice cell its top sits in, which is the
  instrument the base needed. The first thing it prints settles why the earlier
  numbers misbehaved: a column holds **three to eight separate fluid bodies**,
  four most often. Every measurement before this took one number per world, so
  it was reading whichever body happened to be topmost.

  Grouped by cell layer, the levels resolve into ladders three apart — the
  spread quantisation, now visibly operating PER CELL:

  | cell y | span | the ladder | cell ceiling `12(cy+1)` |
  |---|---|---|---|
  | -5 | [-60,-49] | -54, at 100% of 16382 bodies | -48 |
  | -3 | [-36,-25] | -29, -26, (-24) | -24 |
  | -2 | [-24,-13] | -23, -20, -17 | -12 |
  | 0 | [0,11] | ..., 9, 10, 11, 12 | 12 |
  | 1 | [12,23] | 14, 17, 20, 23 | 24 |
  | 3 | [36,47] | 42, 45, 48 | 48 |
  | 4 | [48,59] | 51, 54, 57, 60 | 60 |

  Two things follow. **The base is the cell's own ceiling, not a world
  constant**: for layers -3, 0, 3 and 4 the ladder's top rung is exactly
  `12(cy + 1)`, and the spread only ever lowers it — which is what makes a
  cell dry, since the offset reaches -12 and can drop a level below the cell's
  own floor. That also explains the air beneath the psl-96 water body, and it
  retires the idea that the base saturates in the preliminary surface: what
  saturated was which cell the old readout was looking at.

  **And something beyond the four inputs is still moving it.** The ladders'
  residues mod 3 differ by layer — 1 for cell -2, 2 for cell 1, 0 for cell 4 —
  and `base = 12(cy + 1)` cannot produce that, because 12 is divisible by 3.
  So a further per-cell term sits inside the base, which is consistent with
  the randomness already seen when all four router inputs were held constant
  and 27% of columns still differed. Measuring THAT is the next step, and the
  readout that will do it now exists.

  One number came out of this cleanly enough to record on its own: the lava
  floor's cell, `cy = -5` spanning `[-60, -49]`, reports level -54 unanimously
  across 16382 bodies, agreeing exactly with the global lava level measured
  from the block census.

  **The base is a lattice of its own, and nothing random is in it (M3).**
  Pinning `fluid_level_spread` to the constant zero makes the offset exactly
  zero, so the level read out of a chunk IS the base. Done per cell, each cell
  layer then reports exactly ONE level, unanimous across some 16400 bodies —
  which retires the per-cell random term the ladders seemed to need.

  That apparent randomness has an exact explanation. The bases are 40 apart
  and 40 is not divisible by 3, so consecutive bases cycle through all three
  residues mod 3:

  | base | -54 | -20 | 20 | 60 | 100 | 140 |
  |---|---|---|---|---|---|---|
  | mod 3 | 0 | 1 | 2 | 0 | 1 | 2 |

  The ladders measured in cells -2, 1 and 4 had residues 1, 2 and 0, and the
  bases in those cells are -20, 20 and 60 — residues 1, 2 and 0. The spread
  never changes a residue, because its offset is always a multiple of three.
  So the earlier reading, that a further per-cell term sat inside the base, was
  wrong: the residues differ because the BASES differ, on a lattice.

  *What the lattice is.* Sweeping the preliminary surface and reading per cell:

  | psl | 56 | 80 | 96 | 128 | 160 |
  |---|---|---|---|---|---|
  | top slab | 56 | 80 | 96 | 128 | 160 |
  | beneath it | -54, -20, 20 | -54, -20, 20, 60 | -54, -20, 20, 60 | -54, -20, 20, 60, 100 | -54, -20, 20, 60, 100, 140 |

  **The topmost fluid level is `preliminary_surface_level` exactly**, on all
  five values, and beneath it sits a fixed ladder of period **40** anchored at
  `y = 20 (mod 40)` which psl does not move at all. The lava floor's -54 sits
  outside that progression, as its own thing.

  *What the lattice is not.* It does not follow the sea: at `sea_level` 32 and
  96 the ladder is identical. Raising the sea ABOVE part of it does not shift
  it either — at `sea_level` 128 the 60 and 96 slabs stop reporting tops
  because they are swallowed into one sea body, which is a merge rather than a
  move. And floodedness 1.0 collapses everything to the sea alone.

  This is also the last correction the old readout forced: what looked like the
  base saturating in the preliminary surface was the deepest-body scan sitting
  on the fixed ladder while the psl-tracking slab moved above it, unseen.

  **The base rule, and what the residual is (M3).** Classifying every cell as
  wet, dry or split — rather than scanning for body tops, which a fully
  submerged cell does not have — shows bodies running twenty blocks wet then
  twenty dry, and the switch inside cell 3 happening at y = 40, which is not a
  cell boundary. So the base is a function of HEIGHT on the 40-lattice rather
  than of the cell index:

  ```
  base(y) = min(40 * floorDiv(y, 40) + 20, preliminary_surface_level)
  ```

  Predicting every block of a spread-pinned world from it — fluid iff
  `y < base(y)` — gives 96.1% over 6.1 million blocks, and the shape of the
  4% is the interesting part. The error is zero away from the lattice and
  concentrated entirely around it: 78% wrong at y = -40, 0, 40 and 80, falling
  to under 2% by ten blocks either side. A sharp step predicted where the
  server has a SMEARED one.

  That smear is the vertical centre jitter. A cell takes the lattice point
  nearest its own centre, and centres are jittered, so the transition between
  two lattice points is spread over roughly the cell height rather than being a
  clean cut. It is the same jitter the horizontal centre measurement found,
  showing up on the axis that measurement could not reach — so the residual
  here and the open jitter question are one thing, not two.

  `stratum::aquifer::baseLevel` carries the rule with that approximation stated
  in its own comment, and its tests pin the ladder measured at psl 96 together
  with the sub-zero cases where a truncating division would fold two bands into
  one.

  **The grid origin, from centres instead of boundaries (M3).** Boundaries
  could not settle the anchor because a boundary's position is the grid origin
  convolved with the centre jitter. A RUN's midpoint estimates the centre
  instead: for centres `c_k = 16k + j_k`, a run is bounded by the midpoints to
  each neighbour, so its own midpoint is `16k + (j_-1 + 2 j_0 + j_1) / 4` —
  an unbiased estimate of the mean jitter, and a smoothed one, which is why
  the histogram is a bell rather than a box.

  | centre phase mod 16 | seed 42 | seeds 7 + 12345 | pooled |
  |---|---|---|---|
  | along x | 4.05 | 4.64 | **4.44** |
  | along z | 4.24 | 4.78 | **4.60** |

  Concentration is 0.61 to 0.64, where zero would be uniform. That alone
  excludes a centre jittered uniformly across its whole cell: that would put
  the mean at 7.5 and the concentration at zero. Re-running with the spread
  noise at `xz_scale` 4, 8 and 16 gives x phases of 4.06, 3.35 and 4.05 and z
  phases of 5.00, 4.67 and 4.24, so the figure belongs to the aquifer rather
  than to the probe's input, exactly as the pitch did.

  So **the cell centre sits at `16k + j` with `E[j]` about 4.4 — the lower
  half of its own cell — which places the grid on multiples of sixteen**, and
  the boundary measurement agrees: a centre at 4.4 puts the far boundary at
  12.4, and the observed boundary peak is 12 to 13.

  Vertically the first reading of this was WRONG, in a way worth recording.
  Cell edges were taken from the highest AIR block and came out at
  `y = 8 (mod 12)`, giving a centre at `12k + 2`. But an aquifer rests on a
  stone floor about 2.4 blocks thick, so the highest air block sits several
  blocks below the interface it was taken for. The two clean estimators — the
  floor's own bottom and the fluid's bottom — bracket the interface at 9.68 and
  11.61 (mod 12) over 262119 interfaces on four seeds, putting the vertical
  centre near `12k + 4.6`. That is the SAME offset the horizontal axes give,
  so one jitter law covers all three axes, and the correction makes the picture
  more consistent rather than less. `cellOf` is unaffected: the cell is
  `[12k, 12k + 12)` under either figure.

  What is still open is the jitter's DISTRIBUTION and the random source behind
  it. Only its mean is measured, so `cellOf` is the grid, not the centre.

  **What has landed in code, and what has not (M3).**
  `stratum::aquifer` carries the two things the probe actually settled: the
  measured cell pitch as named constants, and
  `fluidLevel(base, spread) = base + 3 * floorDiv(floor(spread * 10), 3)`,
  with the ten discriminating values as known-answer vectors and the
  negative-operand cases spelled out separately, because a `/ 3` truncating
  toward zero agrees on every non-negative spread and is one step high on
  every negative one.

  `cellOf` now goes with them, since the origin is measured: it floors on
  every axis, and its tests pin the cases a truncating division gets wrong —
  below y = 0, and west or north of the origin, where truncation folds two
  cells into one and shifts every cell after them. The filler still refuses
  `aquifers_enabled` by name, and will until the base, the floodedness gate,
  the barrier rule and the per-cell randomness are measured too.

  **The jitter, the gate and the barrier, measured together (M3).** Twenty-three
  probe dimensions at four seeds — 92 terrain-free worlds — were analysed by ten
  agents, each question attacked from two independent angles and then
  adversarially verified. The verification earned its place: it overturned
  twenty-one of the fifty-five claims put to it, including several of this
  project's own.

  *The jitter is one law on all three axes.* A cell's centre is
  `(16cx + jx, 12cy + jy, 16cz + jz)` with `j` drawn **per 3D cell**, and block
  assignment is nearest-centre. The width is an ABSOLUTE range rather than a
  fraction of each pitch: uniform on `[0, w)` with w = 9.15 +- 0.20 vertically
  and 9.6 +- 0.2 horizontally, from position-free routes (per-cell mixture
  counts) that do not depend on the block-sampling convention. Uniform `[0,12)`
  is out at 7 sigma, symmetric triangular by 18-60 log-likelihood, and
  pitch-proportional scaling predicts horizontal walls in residues 2-5 at 6.9%
  against 0.4% observed.

  That `j` is per 3D cell rather than per column is settled by an instrument
  neither angle set out to build: the interface inside one cell is a TILTED
  plane, not a flat height, and a plane fitted within a cell has RMS 0.42-0.65
  blocks against 1.27-1.40 for a constant fit on the same window. Two stacked
  cells sharing a horizontal centre would give an exactly horizontal plane.

  *The floodedness gate is two exact constants and no randomness.*

  ```
  level = floodedness > 0.8 ? sea_level
        : floodedness > 0.4 ? max(-54, min(40 * floorDiv(centreY, 40) + 20, psl))
        :                     -54
  ```

  Both thresholds are bracketed to a ten-thousandth: at psl 96, floodedness
  0.4000 gives only the lava floor and 0.4001 the full ladder; 0.8000 is
  block-identical to 0.4001 and 0.8001 is sea everywhere. Reproduced on three
  seeds. The gate is DETERMINISTIC — every cell in a world flips across that
  ten-thousandth, which excludes any per-cell threshold spread wider than 1e-4
  and retires the "per-cell randomness" this SPEC inferred twice.
  Below `sea_level - 8` an ocean branch adds a depth-dependent bonus of roughly
  `max(0, 1 - (psl - centreY)/58)`; the 58 is bracketed only to 56.6-58.4 and
  is NOT settled.

  *The barrier is geometry, not a threshold.* A stone sheet is written at every
  cell interface where the two cells place different blocks, unconditionally —
  0 bare interfaces in 1.33 million, and no direct water-to-lava contact
  anywhere. Even at `barrier` = -2 all 261277 vertical interfaces still carry
  stone, so the documented input range never suppresses one. Sheets are about
  2.4 blocks thick, and **99.5% of barrier volume is the FLOOR of one aquifer
  resting on air** rather than a partition between two touching fluids. The
  `aquifer_barrier` input controls only SAME-block interfaces: from -2 to +2 the
  mandatory sheets move by 0.03 blocks while water-stone-water grows 501 to 5536
  and lava-stone-lava 171 to 1942.

  That floor is also what corrupted this project's earlier vertical numbers. The
  "highest air block" estimator sits below a 2.4-block floor, and the fluid
  bottom above it; the two teams measured one interface with two estimators
  separated by exactly that thickness and agreed to within 0.09 blocks once it
  was accounted for.

  **What is genuinely left is small.** With aquifers out of the way, 1.7% of
  columns are still off, every one of them by exactly one block, with the
  density in dispute of order 1e-3 — real, not a tie resolved differently,
  but two orders of magnitude below what the aquifer gap contributed. It does
  not correlate strongly with the cell lattice: by cell y-offset the
  disagreement rate runs 5.4% at offset 0 down to 0.3% at offset 2, and
  columns on a cell corner in both x and z do only slightly better than
  columns on neither (1.37% against 1.95%). An interpolation error would
  vanish at the corners, and this does not. Block heights cannot resolve it
  further — they saturate at half a block — so the next instrument is the
  datapack probe run against a y-independent slice of the chain, where an
  amplified window can read a difference of 1e-4 directly.

  `tests/conformance/golden_terrain_test.cpp` pins a cheap sample so the
  number has to move deliberately; it samples every eighth column across 8x8
  chunks rather than filling one chunk, because a single chunk of seed 42
  comes out 256 of 256 and would have hidden the residual entirely.

  *No documentation and no oracle here* — `end_islands` and, as it was,
  `old_blended_noise`, `weird_scaled_sampler` and `blend_density`; the last
  three are settled above, leaving `end_islands` and `find_top_surface`. minecraft.wiki documents neither
  `weird_scaled_sampler`'s rarity mapping nor what `blend_density` returns
  outside blending, and cubiomes models neither terrain density nor the End
  islands. Implementing them from memory would produce a world that
  generates and is quietly wrong, which §8 treats as the most severe class
  of bug, so they are refused until M3's goldens can tell right from
  plausible.

  Two types *are* implemented on documentation alone: `squeeze` and `invert`
  (the latter documented only through its later rename to `reciprocal`).

  This entry used to say vanilla 1.21.11 uses neither, so no golden would
  ever reach them. **That was wrong, and wrong in both directions.**
  `squeeze` is used seven times and sits *directly* in the overworld's
  `final_density/argument1`, so the end-to-end terrain comparison reaches it
  — it has golden coverage, and had it while this said otherwise. `invert`
  is used three times, but every use is inside `preliminary_surface_level`,
  which `find_top_surface` refuses, so nothing reaches it after all. The
  conclusion happened to hold for `invert` and the reason given was still
  false.

  So `invert` is the one type vanilla uses that nothing checks. It is the
  place to look first if `preliminary_surface_level` comes out wrong once
  `find_top_surface` lands, because it will be the only thing on that path
  that has never been compared to anything.

  `blend_alpha` and `blend_offset` are implemented as the constants 1.0 and
  0.0. This engine generates every chunk itself and never blends against
  terrain another generator wrote, which is the state those values describe
  — and it is what makes vanilla's `overworld/offset` reduce to its spline.

- **Cubic splines are evaluated in float, not double (M2).** Vanilla's knot
  locations, derivatives and values are floats, and the coordinate is
  narrowed to float before use. Widening the arithmetic would change the
  last bits of every terrain offset in the world.

  This is not a theoretical distinction. cubiomes' `getSpline` routes its
  two interpolations through a `double` lerp helper shared with the rest of
  that library, so it rounds to float once at the end where vanilla rounds
  at every step; the two readings disagree on **30 of 90** sampled
  coordinates. `tools/vectors/climate_vectors.c` therefore emits both, and
  the conformance suite holds the interpreter to the float-throughout
  reading while asserting the difference is still non-zero — if it ever
  reached zero, the handling that makes them agree would be untested.
  Which reading matches Mojang is settled by the M3 goldens.

- **The order the cell sampler blends its axes in is not yet verified (M3).**
  `interpolated` is trilinear over the eight corners of the cell a point sits
  in, and this build blends y first, then x, then z. Trilinear interpolation
  is order-independent in exact arithmetic and is not in floating point, so
  the order is part of the answer, not a detail of the loop.

  Nothing available here samples vanilla's terrain density — cubiomes models
  climate and biomes, not this — so the order is the documented reading and
  stands unverified. The unit suite pins it: it evaluates every block of one
  cell against an explicitly composed y-then-x-then-z blend, and asserts that
  two other orders genuinely disagree, so the choice cannot be changed by
  accident and cannot quietly become a test of nothing. What settles it is
  the goldens, which need terrain.

  Note the trap this fell into first: a single sampled point let a
  y-then-z-then-x mutation through, because two orders agree bitwise far more
  often than intuition suggests.

- **Terrain needs two more decisions, not one (M3).** With the cell sampler
  in, vanilla's `final_density` is still refused — and the walk meets
  `blend_density` before it reaches `old_blended_noise`, so *both* have to be
  settled before a single block can be placed. Neither is a cell problem, and
  "the cell sampler landed" should not be read as terrain being next.

  On `old_blended_noise` specifically, the claim above that it "has no
  independent oracle here" was too strong. The noise is now **implemented and
  checked** against cubiomes' `sampleSurfaceNoise`: the 16/16/8 octave
  stacks, the per-octave scaling, the `clampedLerp(0.5 + 0.05*blend,
  min/512, max/512)` blend, and the legacy seeding — three stacks drawn in
  order from one Java LCG. See `lib/src/blended.cpp` and NOTICE.

  **Correction, from trying to use it (M3).** "Implemented and checked" was
  too generous, and the commit that introduced it says so too. Matching
  cubiomes bit-exactly does not make this function usable, because cubiomes'
  `sampleSurfaceNoise` is not the same quantity:

  * It is handed *cell* coordinates — its caller in generator.c passes a `py`
    running 0..32 — where `old_blended_noise` is evaluated at block
    coordinates. That part reconciles: `xz_scale: 0.25` and
    `y_scale: 0.125` are exactly the block-to-cell conversion.
  * Its output is ±128 by construction: sixteen octaves whose amplitudes
    *double* to 32768, then divided by 512. The legacy generator worked in
    that density space. Modern `old_blended_noise` feeds
    `sloped_cheese = 4 * quarter_negative(...) + base_3d_noise`, whose other
    term is order one. Measured with the overworld's own parameters, this
    implementation returns −300.35 to +578.79 where it needs order one.

  So there is a **third** unknown — how the modern function is normalised —
  and it is the largest of the three: a wrong smear shifts values, a wrong
  normalisation makes the function unusable. It was found by running the
  experiment below, not by reading, which is the argument for running
  experiments early.

  The three things that oracle cannot reach, each of which changes every
  block:

  * **How the modern function is normalised.** See above. Nothing available
    here says.


  * **Where `smear_scale_multiplier` enters.** cubiomes models the pre-1.18
    noise, which had no such parameter, so agreement pins only the
    multiplier-of-one case. Vanilla's data uses 8.0 in the overworld and
    Nether and 4.0 in the End. This build multiplies the per-octave y slab
    width by it, which is a guess. Measured, not assumed: the multiplier is
    inert at exactly y = 0 and changes the value at every other height, so a
    wrong placement is not confined to some corner of the world.
  * **How a dimension that does not declare `legacy_random_source` seeds the
    three stacks.** Nothing here answers this, and the overworld is such a
    dimension.

  **What measuring the field settled (M3).** Two of the three are now
  answered, by statistics rather than by a candidate. The seeding is unknown,
  so the two fields cannot be compared point by point — they are different
  realisations. But a field's *spectrum*, its *distribution shape* and its
  *spread* do not depend on the seed, and those pin everything except the
  seeding. Reproduce with `tools/analysis/run-blended-probe.sh`.

  * **The octave schedule is right.** Power in every dyadic wavelength band
    from 2 to 8192 blocks agrees with deepslate's, and — the part that makes
    it a result rather than an impression — every difference is smaller than
    the spread of each spectrum across seeds. An earlier read of the same
    data found a "systematic tilt" at 256–512 blocks; with the seed-to-seed
    control it is 1.3x the noise. This also retires the amplitude-schedule
    question for good.
  * **The normalisation is a further division by 128**, making the divisor
    512 x 128 = 65536 = 2^16 — which is exactly the sum of the sixteen
    doubling amplitudes, so it is the constant that takes the stack to +/-1
    rather than a fitted number. Measured at y = 0, the one height where this
    build's smear provably does nothing, so it is normalisation alone:
    sd ratio **128.17 +/- 1.11** over 41 seeds and 5.4M samples a side
    (0.16 sigma from 128), quantile-implied scale 127.7, kurtosis agreeing to
    0.5% (2.773 against 2.786) and skew zero on both.
  * **`smear_scale_multiplier` is in the wrong place, and that is new.**
    Vanilla's field barely notices it: deepslate's spread at y = -64 moves
    1%, from 0.3112 at a multiplier of 1 to 0.3082 at 8. This build's moves
    **6.6x**, from 31.5 to 206.8, because it scales the per-octave y slab
    width. Whatever the multiplier does in vanilla is close to
    variance-preserving, which the current reading is not. The discrepancy
    survives at a multiplier of 1 (a factor of 1.9 at y = -32), so this
    build's "no smearing" is not vanilla's either.

  **Sweeping the multiplier (M3).** It is a free parameter in the oracle, so
  it can simply be swept, and the response is the strongest constraint
  available on where it can possibly enter. Vanilla's, over 0 to 256:

  | multiplier | 0 | 0.25 | 1 | 4 | 16 | 64 | 256 |
  |---|---|---|---|---|---|---|---|
  | correlation length along y | 51 | 33 | 33 | 33 | 33 | 38 | 44 |
  | spread | 0.151 | 0.209 | 0.209 | 0.208 | 0.204 | 0.192 | 0.176 |

  Three things follow. Smearing is **on** in vanilla — turning it off, at a
  multiplier of 0, changes the field a great deal (r = 0.62 against a
  multiplier of 1) and lengthens the correlation along y from 33 blocks to
  51. Its effect **saturates**: from 0.25 to 16, a factor of sixty-four,
  nothing moves. And it only weakens again past 64, drifting back toward the
  unsmeared field. That is the signature of a quantisation whose step is
  below one for nearly every octave until the multiplier grows large enough
  to lift the early octaves out of it.

  **What the sweep found, and the correction that followed.** This build
  passes the Perlin sampler a y cap of `y * step`. The sampler computes
  `min(cap, localY)` and then folds; below y = 0 the cap is large and
  negative, so `floor(cap / step)` displaces `localY` by a multiple of the
  step proportional to both y *and* the multiplier. That produces the two
  symptoms measured above: amplitude scaling linearly with the multiplier,
  and the blow-up at negative y.

  Passing no cap at all — fold `localY`, never clamp it — agreed with vanilla
  on three summary statistics of four, and was recorded here as "the cap is
  established wrong, the step right". **That was wrong, and the way it was
  wrong is the lesson.** Summary statistics rank candidates; they do not
  reject them. Sampling *between* the integers does.

  **The fingerprint.** Vanilla's field is smooth in y at a sixty-fourth of a
  block, except at integer y, where it jumps by twenty to sixty times its
  median step — and those jumps vanish entirely at a multiplier of 0. So the
  smear is a quantisation keyed to the **block** coordinate, not to the
  per-octave scaled one. Measured at multiplier 1, as multiples of the
  median step at y = 1..8:

  | | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
  |---|---|---|---|---|---|---|---|---|
  | vanilla | 59 | 21 | 3 | 20 | 19 | 38 | 46 | 50 |
  | vanilla, multiplier 0 | 2 | 1 | 2 | 1 | 1 | 1 | 0 | 0 |
  | this build's cap | 39 | 37 | 1 | 33 | 45 | 1 | 56 | 62 |
  | fold, never cap | 1 | 1 | 1 | 0 | 1 | 0 | 1 | 1 |

  This build's cap reproduces the effect almost exactly; the candidate that
  scored well on the summary statistics has none of it. So the cap's *form*
  is right and only the way the multiplier enters it is wrong. The candidate
  is withdrawn.

  **What is settled.** Four structural questions closed, each by a
  measurement that would have shown the opposite had it been the other way.
  The fade is taken from the *unfolded* y — folding first misses the spread
  by 13 sigma and the multiplier-sensitivity by 11. All three octave stacks
  are smeared — limits only misses the sensitivity by 17 sigma, blend only
  misses the spread by 12. And the cap, which is the whole of the rest:

  * **It is built from the slab width taken before the multiplier.** The slab
    widens with the multiplier; the cap does not. That is what leaves the
    field almost unmoved across a factor of sixteen in the multiplier, which
    vanilla's is.
  * **Below y = 0 it does not bind at all**, so the fold runs at full effect.
    This was the last piece and the y-profile is what gave it away: vanilla's
    smear boost below zero is *flat* at 1.67, not varying with y at all,
    while above zero it climbs steadily — 1.01 at y in [0,32) to 1.67 at
    [224,256). Flat at the value the rising curve only reaches near y = 240
    is saturation, not a mirrored cap, and mirroring the cap (which was tried)
    leaves the negative side at 1.07 and misses the spread by 4.5 sigma.

  Against vanilla over sixteen seeds, on the dimension's own y range:

  | | vanilla | measured reading | |
  |---|---|---|---|
  | correlation length along y | 27.00 +/- 0.86 | 28.50 +/- 0.65 | 1.4 sigma |
  | spread raised over no smearing | 1.4333 +/- 0.027 | 1.4384 +/- 0.018 | 0.2 sigma |
  | r(multiplier 1, multiplier 16) | 0.9875 +/- 0.001 | 0.9881 +/- 0.000 | 0.8 sigma |
  | integer-y jumps | present | present | — |

  and it tracks the whole multiplier response, including its non-monotonic
  tail: correlation length 43/29/29/28/28/31/39 at multipliers
  0/0.25/1/4/16/64/256 against vanilla's 40/27/27/27/28/31/38.

  **A range check that mattered.** The first version of this comparison
  sampled y from -256, and the dimension is y in [-64, 320). Everything below
  -64 is outside the world and vanilla never evaluates there; including it
  made the fit look worse and pointed at the wrong things. The probes now
  restrict to the real range.

  **`legacy()` keeps the old reading, and the two are related exactly.** At a
  multiplier of one they are bit-identical for y >= 0 and differ only below
  it, which is why the cubiomes vectors — 150 of whose 270 rows sit below
  y = 0 — still pin the pre-1.18 function unchanged. `withMeasuredSmear()` is
  the new one. High enough up the two agree again at any multiplier, once the
  cap exceeds the local offset under either reading and both saturate.

  **The seeding, and the whole node (M3).** One generator, taken from the
  world seed's positional factory under the name `minecraft:terrain`, with the
  three stacks drawn from it in order: sixteen minimum, sixteen maximum, eight
  blend. Every part of that is load-bearing — a different salt, the blend
  stack first, or the world seed put straight into Xoroshiro all miss by order
  one rather than narrowly, and `terrain` without the namespace misses too,
  because the name is MD5'd into the seed.

  It was found by search rather than by probing vanilla again, and the reason
  that worked *now* and not before is worth recording: a seeding hypothesis
  can only be tested against exact values if everything else about the
  function is already right. With the normalisation and the smear settled, a
  correct seeding had to reproduce deepslate's 240 values outright, and one
  did. With either still open, no hypothesis could have matched and the search
  would have found nothing — which is what an earlier attempt, scoring
  candidates by correlation, actually found.

  **Together: 240 of 240 to within a part in a billion, 230 of them
  bit-for-bit.** The ten that are not differ by at most 6.3e-14 and sit at the
  largest coordinates in the set, five of them at (1000, -60, -1000). That is
  accumulated rounding across sixteen octaves whose amplitudes reach 32768,
  not a difference of construction — and which side's last bits are right is
  not something deepslate can settle, because it is an emulator and the golden
  regions are the authority (§7).

  **Confirmed against the server, not the emulator.** deepslate settled the
  seeding, and deepslate is an emulator; §7 makes the golden regions the
  authority. `tools/analysis/blended-datapack-probe.sh` closes that gap
  without asking the server for a number it has no way to report. A datapack
  gives a dimension the entire `final_density`

      K * flat_cache(old_blended_noise) + y_clamped_gradient(+1 .. -1)

  and nothing else that can place a block. `flat_cache` pins the noise to
  y = 0 for the whole column, so the only thing varying with height is the
  gradient, the surface sits exactly where `K*N + g(y) = 0`, and inverting g
  turns every column's terrain height into a reading of `N(x, 0, z)` written
  by Mojang's own binary.

  Over a full region at seed 42: **all 16384 cell corners agree to within half
  a block**, which is the floor of what the measurement can resolve — the
  surface is a block, and its centre is the best estimate of where the density
  crossed zero. Correlation 0.99997595, means +0.054288 against +0.054286,
  spreads 0.143683 against 0.143685.

  Two things had to be right for that number, and both were wrong first:

  * **Only the cell corners can be read.** `final_density` is evaluated on the
    cell lattice and interpolated across it, and `flat_cache` pins its
    argument to the 4x4 column corner as well. Comparing every column against
    the noise at that column compares two different quantities, and cost 0.04
    of correlation until it was restricted.
  * **The biome must have no carvers and no features.** `minecraft:plains`
    carves caves through the terrain and puts lakes and springs on top of it;
    the probe's outlier columns turned out to be topped with *water*. The pack
    now defines its own biome with both lists empty, which is what
    `tools/fetch-vanilla` does to the goldens for the same reason.

  The fixture is Mojang-derived and never committed (§12). The conformance
  case skips without it, and a wrong salt or a reordered draw fails it.

  **What it unblocked.** `old_blended_noise` is no longer refused, and with it
  vanilla's evaluable named density functions go from 25 of 35 to **31**,
  including `overworld/base_3d_noise` and `overworld/sloped_cheese` — the
  terrain shape function itself. `final_density` still refuses, but one step
  further down: vanilla's caves reach terrain through `min`s nested deep
  inside it, so `weird_scaled_sampler` is now the first thing met on that
  path. Four named functions remain unevaluable, all of them cave or End
  functions.

  **A methodological correction worth keeping.** The first estimate of the
  normalisation was 130.86 +/- 0.21, reported as 13 sigma from 128 — and the
  error bar was wrong by an order of magnitude. It came from 1/sqrt(2n) on
  1.4M points, which assumes independence; a noise field is spatially
  correlated, so the effective count is the number of independent patches,
  and the true seed-to-seed spread is ~6%. Forty-one seeds give
  128.17 +/- 1.11 and the disagreement evaporates. The comparison script
  refuses to be used this way twice: it computes its error bar across seeds,
  and its header says so.

  So `old_blended_noise` stays refused. The refusal now names one open
  question rather than three: how a dimension that does not declare
  `legacy_random_source` seeds the three stacks. The normalisation is
  settled, and the smear is not settled but is at least *localised* — it is
  known to be wrong and known by how much, which the next experiment can aim
  at.

  It is also **not** the single remaining thing between the pipeline and a
  block of overworld terrain, which is another thing the experiment
  corrected: vanilla's caves reach terrain through `min`s nested sixteen
  levels inside `final_density`, so `weird_scaled_sampler` is on that path
  too.

- **How the unsettled types get settled, and how they do not leak (M3).**
  `density::UnsettledSubstitutions` lets a caller put a candidate reading of
  `old_blended_noise`, or a constant for `weird_scaled_sampler`, in front of
  the pipeline. It is absent by default and that default is the whole
  point: without it those types stay refused, so no ordinary caller can
  generate a world from a reading nothing has verified.

  It exists because the only thing that can settle them is a comparison
  against the goldens, and that comparison needs a way to run a candidate.
  The experiment shape, established by measurement:

  * Evaluate the whole `final_density` with `weird_scaled_sampler` forced to
    a large positive, which loses every carving `min` and isolates terrain
    without claiming to know the rarity mapping.
  * Scan each column for the highest y where it is positive, and compare
    against vanilla's own `OCEAN_FLOOR` heightmap.
  * Check the premise as well as the answer: caves only remove material, so
    a prediction made this way must come out at or above vanilla's surface
    in every column. Columns below it mean the isolation is wrong, not the
    candidate.

  It discriminates: with the blended noise forced to zero, 1.6% of columns
  match exactly. A candidate that is right should be near the other end of
  that scale, and nothing yet is.

  **What a time-boxed search established (M3).** Using the deepslate vectors
  as the oracle and Pearson correlation as the filter — chosen because a
  correct *seeding* should track the answer even while the normalisation and
  smear are still wrong, since those change scale and detail rather than
  shape — three parts of the derivation now have real evidence behind them:

  * the salt is **`"minecraft:terrain"`**, through the positional factory:
    `XoroshiroPositionalFactory(worldSeed).fromHashOf("minecraft:terrain")`;
  * the octaves are drawn **sequentially from that one generator**, not
    per-octave salted the way NormalNoise's are;
  * in the order **min (16), max (16), main (8)**.

  Each is held up by its own control rather than by looking plausible.
  `"minecraft:terrain"` correlates at **0.809**; two nonsense salts give
  0.115 and −0.059. Reversing the octave order with the correct salt
  collapses it to −0.108, which is the useful one: if the permutations were
  wrong, their order could not matter that much. All five other draw orders
  fall to 0.29 or below.

  It is evidence, not proof, and the function is still not reproduced. 0.809
  is not 1.0, and the gap is somewhere in the sampling formula: sweeping five
  smear placements, twenty-four frequency and amplitude schedules, and three
  normalisation divisors moved it not at all. `old_blended_noise` stays
  refused.

  **What the isolation experiment ruled out.** `xz_factor` and `y_factor`
  only reach the *main* stack's sampling coordinates, so a point whose output
  does not move when they change is one where the blend is not deciding
  anything and the value is a single stack's sum. 190 of 4000 probed points
  behave that way. That turns the amplitude schedule from something to guess
  into a linear system: sixteen unknown weights, one equation per point.

  It does not fit. With the seeding above and sixteen *free* amplitudes, the
  best R² is 0.38, against 0.16 for the same fit with a nonsense salt — which
  is about what sixteen free parameters over 190 points will reach by
  overfitting alone. A correct frequency schedule with free weights should
  have fitted almost exactly.

  So the discrepancy is **not in the amplitude schedule**: the fit was handed
  every weight it could have wanted and still could not reach the answer.
  What is left is the coordinates or the per-octave structure. Two caveats on
  the strength of that: the selected points may be invariant because the two
  limit stacks nearly coincide there rather than because the blend clamps,
  which would make them an unrepresentative subset; and the right salt
  scoring 0.38 against a control's 0.16 is consistent with the seeding
  finding without adding much to it.

  The golden round trip is no longer the fastest way to test a candidate,
  though. `tools/vectors/generate-deepslate-vectors.sh` records deepslate's
  own `old_blended_noise` at 240 points across five seeds and four parameter
  shapes, which answers in microseconds and bit-exactly. What it does not do
  is supply the algorithm: twenty-four candidate derivations of the modern
  seeding — sequential and per-stack draws, six salt strings, amplitudes
  doubling and halving — produce no constant ratio against those vectors, so
  the space is larger than guessing covers. The function stays refused; what
  changed is that any future attempt is now checkable in one step instead of
  none.

  A third, smaller gap worth writing down: cubiomes' `maintainPrecision` is
  a no-op — the real line is commented out in its header as "useless in
  practice". This build implements the wrap (fold into ±2^25, rounding half
  up as Java does), and the vectors deliberately stay inside the band where
  the two agree. Where the wrap actually bites, nothing checks it.

- **`legacy_random_source` is honoured by refusing, not by guessing (M3).**
  A noise settings entry picks one generator for all of a dimension's
  noises. Four of vanilla's seven — Nether, End, caves, floating islands —
  pick the Java LCG; the overworld and its two variants pick Xoroshiro.

  This mattered more than it looked. `minecraft:temperature` is referenced
  by the overworld's router *and* by the Nether's, so one pack needs the same
  noise seeded two different ways, and the registry was built once per pack
  and always with Xoroshiro. Three dimensions were right and four were
  silently wrong — invisible only because nothing generates terrain yet, and
  baked into the shape of the API rather than a slip in one function.

  Registries are now per dimension, and `RandomSource` is a required
  argument with no default, because a default is how this happened.
  `RandomSource::Legacy` throws: how a noise's *name* becomes an LCG seed is
  not settled here. cubiomes seeds named noises only through Xoroshiro; it
  models the LCG for the blended noise alone, which is a different
  construction with no name hashing. So the four legacy dimensions cannot be
  built, and `stratum validate` reports each one as a warning and leaves its
  router **unchecked** — deliberately not counted as fifteen failures, since
  "we did not look" and "we looked and it does not work" are different
  claims. That is why the headline count reads 39 of 45 across 3 of 7
  dimensions rather than 94 of 105: fewer entries, and all of them ones we
  can actually speak to.

  One assumption, stated because it has not been verified: that the flag
  selects the generator for *all* of a dimension's noises rather than some
  subset. That matches a single RandomState being constructed once, but
  nothing here checks it, and if it is wrong this refusal is too broad.

- **A `noise` field is a union, and narrowing it refused legal input (M4).**
  Upstream mcdoc declares
  `type NoiseParametersRef = (#[id="worldgen/noise"] string | NoiseParameters)`,
  so every `noise` field — `noise`, `shifted_noise`, `shift`, `shift_a`,
  `shift_b`, `weird_scaled_sampler` — accepts either an identifier or the
  parameters written out in place. The generator matched the alias by *name*
  and recorded only the identifier, so a datapack that inlines its noise
  parameters was refused by a table that claimed to be derived from the
  schema. Not a parity hole — a refusal is not a wrong world (§8) — but a
  false rejection, and the generator now expands the alias like any other and
  emits a union-capable field kind. `tests/tools/mcdoc_generator_test.py`
  covers it: CI already checks the generated tables are *reproducible*, which
  catches a hand-edit and not a reader that has quietly narrowed something,
  because regenerating from the same wrong reader diffs clean.

  **Settled (M4): vanilla does not seed a nameless noise, it refuses to build
  the world.** The question this entry used to carry was how the seed is
  derived when there is no identifier to derive it from. It has no answer.
  Asked, the 1.21.11 server takes the pack, loads every file in it without a
  word of complaint, and then dies while the world is being built:

  ```
  java.util.NoSuchElementException: No value present
      at java.base/java.util.Optional.orElseThrow(Optional.java:377)
      at eve$a.a(SourceFile:71)
      ... the dimension's noise router, function by function ...
      at eve.<init> / eve.a
  ```

  The plan recorded here — generate a region and compare its terrain against
  candidate derivations, the way `old_blended_noise` was settled in M3 —
  therefore never applied: there is no region, and no world.
  `tools/analysis/inline-noise-probe.sh` is what became of it. It runs eleven
  datapacks past the server, each differing from the others in exactly one
  thing, and records what happened to each:

  | the one difference | the server |
  |---|---|
  | `noise` names `minecraft:ridge` | starts |
  | `noise` names a copy of it the pack defines under its own namespace | starts |
  | a `worldgen/density_function` file holding an inline noise, named by no dimension's router | starts |
  | inline under `noise` | dies |
  | inline under `shifted_noise`, `shift`, `shift_a`, `shift_b`, `weird_scaled_sampler` | dies, all five |
  | inline in `barrier`, in a dimension with aquifers off, so nothing ever samples it | dies |
  | inline with a different entry's parameters | dies |

  Every death is the same exception in the same frame, so this is one place
  in the server that cannot proceed rather than a scatter of separate
  refusals, and between them the eleven fix its edges. It is not the field:
  six take the union and all six are fatal. It is not the parameters, which
  two sets settle. It is not the *sampling*, because an entry the dimension
  never reads is fatal too. And it is not loading, because a density function
  file no router names is carried perfectly happily. What visits the node is
  the pass that builds a dimension's random state, and what it wants there is
  the name.

  The verdicts, and the exact density function that produced each, are
  recorded in `.fixtures/<version>/probes/inline-noise/verdicts.json` —
  Mojang-derived, never committed (§12), regenerated by re-running the probe.
  `tests/conformance/vanilla_inline_noise_test.cpp` replays every one of them
  through this build and requires it to reach the same verdict.

  Three things follow, and they are the whole of the change:

  * The interpreter still refuses such a node, but the refusal now says the
    true thing. It was "how vanilla seeds it is not settled here, so it will
    not be sampled from a guess"; it is now that a noise is seeded from the
    MD5 of its identifier, this one has none, and the vanilla server does not
    work around that either. Nothing is missing at this end.
  * That distinction gets a type: `density::UnbuildableError`, deriving from
    `EvalError` so every caller asking "can this be sampled?" still gets one
    answer, and separate so that callers who care *why* can tell a pack that
    works nowhere from an engine that is not finished.
  * `stratum validate` reports it accordingly, and its header comment now
    describes three kinds of no rather than two. A router entry that reaches
    an inline noise is an **error** — that pack does not generate anywhere,
    and calling it a warning would tell whoever holds it that the problem is
    at this end. A `worldgen/density_function` file carrying one, with no
    dimension reaching it, stays a **warning**: this build will not sample
    it, and the server it came from starts and generates regardless. Same
    node, two severities, split exactly where the server splits it.

  What is *not* established. The pin is 1.21.11 (§3) and the evidence is one
  version's server; that a future one keeps refusing is an assumption, and
  the conformance case above is written so that it fails rather than passes
  if that changes — it takes each verdict from the recording rather than from
  anything written into the test, so a regenerated recording that disagrees
  turns it red instead of quietly agreeing with itself. The
  obfuscated frame names are recorded as evidence and are meaningless across
  versions, so nothing is asserted about them. And this says nothing about
  why the server cannot proceed beyond what it does: no Mojang source was
  read, here or anywhere (CLAUDE.md).

- **Doc comments in mcdoc carry semantics, and were being dropped (M4).**
  The reader treated `///` as trivia and the enum reader kept only the string
  literals, so anything mcdoc documented rather than declared could not reach
  the generated tables — `DistanceMetric`'s four per-value formulas are
  written that way and nowhere else. Doc comments are now parsed and emitted
  as comments beside the entry they belong to. At the pinned version nothing
  live carries one (the types that do are all `#[since="26.3"]`), so today
  this changes no generated byte; it is in place for the version bump that
  makes it matter, and tested directly rather than through the pinned file.
  The same pass made enum values gate-filtered: a value from another version
  was previously offered as one this version accepts.

- **The biome parameter table is not data, and vanilla will dump it (M4).**
  At 1.21.11 the pack ships
  `worldgen/multi_noise_biome_source_parameter_list/overworld.json` as
  nothing but `{"preset": "minecraft:overworld"}`; the table is compiled into
  the jar. That looked like a blocker and is not: the server's own data
  generator writes it out on request, and CLAUDE.md permits the observed
  output of the vanilla server where it forbids its code.
  `tools/fetch-vanilla` now asks for it — 7593 rows over 54 biomes for the
  overworld, five for the Nether — and keeps it under `.fixtures/`, never
  committed (§12).

  With that, **the biome source is the first thing in this project checked
  against vanilla's own output rather than against another reimplementation
  of it.** The golden regions store a biome per 4x4x4 cell; the climate chain
  and the table between them choose one; the two are compared directly. All
  four seeds checked match on every one of 24576 cells — 98304 in all.

- **The biome search runs on quantised integers, and ties go to the later
  row (M4).** This is settled now, but it was found the hard way and the way
  it was found is the point.

  The first implementation compared fitnesses as doubles and took the earlier
  entry on a tie. It got 98228 of 98304 cells. The residual was not scattered:
  75 of the 76 misses were a **single column**, `(x=36, z=12)` of seed 42,
  wrong at every height from -64 to 316 — one (x,z) where something that does
  not vary with y flips the answer. That shape ruled out the climate chain
  before anything was measured, and measuring confirmed it: deepslate agrees
  with this build's six climate values at that column to every digit printed.
  The climate was right and the *search* was wrong.

  At that column `beach` scores 0.000537869 and `dark_forest` 0.000537878 —
  apart by 9e-9, which is not a difference so much as an artefact of summing
  six squares in floating point. Vanilla does not have the artefact because
  vanilla does not compare in the reals: every coordinate is first mapped to a
  fixed-point integer, ten thousand steps per unit, truncated through
  `float` (cubiomes carries the same `int64` climate coordinates, which is the
  independent corroboration). Quantised, the two entries are not 9e-9 apart —
  they are **exactly equal, both 53824**. The near-miss was never a near-miss.

  Which leaves the tie, and the tie is the part that is measured rather than
  derived. `beach` is row 3609 and `dark_forest` is row 3611, so "earlier
  wins" still answers `beach`; vanilla answers `dark_forest`. Vanilla does not
  scan the list at all — it searches a tree built from it, and which leaf a
  tie resolves to is a property of that tree's shape, which is not documented
  and is not derivable from the dumped table. **Later row wins** reproduces
  every tie observed across 98304 cells and four seeds, and it is adopted on
  that evidence, flagged here as an empirical match rather than a derivation.

  Neither change is sufficient alone, which is what makes this more than
  fitting seed 42: seed -4172144997902289642 is exact under the old
  double-precision search, and quantising *without* the tie-break breaks one
  of its cells. Only both together leave all four seeds whole. A future
  counterexample would be a finding about the shape of vanilla's tree, not a
  bug in the arithmetic.

- **The three blending types take their no-blending values (M2, extended M3).**
  This engine generates every chunk itself and never blends against terrain
  another generator wrote. `blend_alpha` is 1.0, `blend_offset` is 0.0, and
  `blend_density` returns its argument unchanged.

  None of the three is documented — minecraft.wiki records that they exist
  and says "[more information needed]" — so all three rest on one structural
  reading: they are the same interface, and vanilla's own
  `overworld/offset` is a lerp of the shape `blended*(1-alpha) + own*alpha`,
  which at alpha 1 takes the function's own value and is hard to read any
  other way. Leaving a density alone is that same statement. The goldens
  settle it; until then it is a reading, not a fact.

- **`cache_2d` over a column-varying function is refused (M2).** Vanilla
  caches it on (x, z) alone, so the whole column would take the value of
  whichever y was asked for first — an order-dependent answer. Everything
  vanilla wraps in `cache_2d` is column-invariant, which the interpreter
  checks statically rather than assumes, so treating it as transparent is
  exact for vanilla's data and a loud error for anything else. `flat_cache`
  is a different matter and is implemented literally: it relocates the
  sample to the corner of the 4x4 column at y = 0, which changes the value
  at every block that is not on a corner.

---

## 12. Content, provenance & distribution policy

- The engine ships **no Mojang data**: no vanilla worldgen JSON, no
  extracted assets, no golden fixtures derived from them. Users obtain
  vanilla presets by pointing `tools/fetch-vanilla` (or the in-server
  equivalent) at the official jar they download from Mojang.
- No decompiled or unobfuscated Mojang source code is read, pasted,
  transcribed, or paraphrased into this repository — in any language.
  Behavior is implemented from: minecraft.wiki / datapack.wiki
  documentation, mcdoc schemas, licensed references (§2), and observed
  input/output of the vanilla server via the conformance harness.
- Third-party datapacks are user-supplied content; the engine loads them,
  the repo does not redistribute them.
