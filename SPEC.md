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

  So `old_blended_noise` stays refused, with a refusal that names those three
  rather than the whole function.

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
