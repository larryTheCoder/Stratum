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
  schema, the noise registry and the interpreter. Vanilla's overworld
  climate chain — `shift_x`/`shift_z`, `continents`, `erosion`, `ridges`,
  `ridges_folded` and `offset` — is evaluated from vanilla's own JSON and
  matches cubiomes bit-for-bit across six world seeds. Remaining:
  heightmap-style rendering and the golden comparisons themselves, which
  need terrain and so wait on M3.
- **M3** — 3D density: full noise router, cell sampling + trilinear
  interpolation, all cache node types, aquifer fill decision, compiled flat
  execution program, Tier-A goldens passing for terrain shape.
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
  only where mcdoc is insufficient.

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

  *No documentation and no oracle here* — `old_blended_noise`, `end_islands`,
  `weird_scaled_sampler`, `blend_density`. minecraft.wiki documents neither
  `weird_scaled_sampler`'s rarity mapping nor what `blend_density` returns
  outside blending, and cubiomes models neither terrain density nor the End
  islands. Implementing them from memory would produce a world that
  generates and is quietly wrong, which §8 treats as the most severe class
  of bug, so they are refused until M3's goldens can tell right from
  plausible.

  Two types *are* implemented on documentation alone, and are flagged here
  because nothing else stands behind them: `squeeze` and `invert` (the
  latter documented only through its later rename to `reciprocal`). Vanilla
  1.21.11 uses neither, so no golden will ever reach them; only the unit
  vectors do.

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
