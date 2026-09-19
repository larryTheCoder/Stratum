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
- PHP callbacks inside the pipeline, and per-block PHP anywhere. A
  `Generator` IS plugin PHP running on a PocketMine-MP worker thread (its
  classes are autoloadable there, and it is constructed with only a seed and
  an options string), but its whole per-chunk job is handing over
  already-packed sub-chunks and translating a handful of palette entries.
  Nothing in the pipeline itself calls into PHP.
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

- World creation: `Stratum\freezePipeline($versionRoot, $blobPath)` writes
  the resolved pipeline into the world (§6).
- World load: `Stratum\Dimension::open($blobPath, $noiseSettings,
  $biomeParameterList, $seed)` compiles it — once per process, shared by
  every worker thread that opens the same blob, settings, list and seed.
- Per chunk: `$dimension->encodeChunk($cx, $cz)` returns every sub-chunk,
  keyed by PocketMine-MP sub-chunk index, as `PalettedBlockArray::fromData`
  argument lists: block palettes hold Java block state ids, biome palettes
  Bedrock biome ids, and an all-air sub-chunk has no block layer. PHP
  translates each sub-chunk's few Java ids to PMMP's own internal state ids
  through `Stratum\bedrockBlockState()` and PMMP's own deserializer (§9),
  then builds the `SubChunk`s — never anything Bedrock-protocol-specific,
  and no per-block PHP.
- Every failure is a `Stratum\GenerationException` naming what failed.
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

The container format is **3** since M5. A format-2 blob could not actually
generate a world on its own: M4's biome source and surface rules read the
multi-noise biome parameter lists (which vanilla compiles in; only its data
generator dumps them) and every biome's declared temperature, and surface
rules sample noises the density graph never names (`minecraft:surface`,
`surface_secondary`, `clay_bands_offset`, and the rule trees' own). Format 3
appends the parameter lists — entry order preserved, because ties in the
search go to the later entry — and the temperature table as float32 bit
patterns, and `freeze::resolve(pack, biomeParametersDir)` is now the single
place a pipeline is assembled: it freezes every noise the pack defines, not
only the graph's. `NoiseRegistry::create` builds from the stored parameters
through the same derivation the pack path uses, and a unit test checks the
two sample bit-identically. The pipeline engine version does not change:
nothing about the terrain a given pipeline generates moved, only what the
blob carries.

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

Generation never needs to know which Bedrock protocol version a client
will connect with. Every target platform stores and generates chunks in its
own protocol-independent block representation and translates to a
connecting client's protocol downstream, in its own networking layer (§11's
"returns to `lib/mapping/`" entry has the source citations). So each
mapping has two halves, split at a platform-neutral midpoint:

- **`lib/mapping/` owns Java → Bedrock blockstate data** (its own CMake
  target, `stratum_mapping`, downstream of the conformance boundary —
  `stratum_core` never links it). One compiled-in table per Minecraft
  version this build is pinned to, regenerated when the pin moves:
  - *Biomes:* a Java biome id resolves to Bedrock's numeric id (GeyserMC
    mappings as reference input), with custom/datapack biomes falling back
    to the nearest vanilla Bedrock biome (configurable) for client-side
    fog/color/music; the engine's internal biome identity is preserved for
    generation either way.
  - *Block states (landed):* a Java block state resolves to a Bedrock
    blockstate triple — `{name, states, version}`, the named, typed shape
    both PocketMine-MP (`BlockStateData`) and Nukkit (`BlockStateMapping`'s
    `NbtMap` key) already consume. Omitted Java properties take the block's
    default; an unknown block, property or value is a named
    `BlockMappingError`. It never resolves to a numeric network runtime id.
    That id is a hash-sorted index that shifts between Bedrock protocol
    versions, and it is derived later, per connection, inside each
    platform's own networking code. The table (`tools/mapping-sync`) is
    GeyserMC/mappings' `blocks.nbt` at the commit Geyser shipped for Java
    1.21.11, keyed by vanilla's own block report and checked state by state
    against Geyser's Bedrock 1.21.130 block palette: all 29,671 Java states
    resolve, at blockstate version 1.21.60.33.
- **Each native binding owns the last step:** Bedrock blockstate → the
  platform's own internal block id, using the platform's own resolver
  rather than a second table. `ext/` feeds the triple through PMMP's
  `BlockStateUpgrader` then `BlockStateToObjectDeserializer::deserialize()`,
  which returns the internal state id `Chunk::setBlockStateId()` takes.
  `ext-nukkit/` feeds it through Nukkit's `BlockStateMapping` for a legacy
  id:meta. Both resolve once per distinct state, when the generator starts
  up, not once per block.
- **Version coupling is per state, not a version gate** (measured, §11).
  Both platforms' upgraders only move a blockstate forward, but a version
  number alone says nothing about whether a given state changed. The table
  is at 1.21.60.33 — exactly PMMP `stable`'s own `BlockStateData::
  CURRENT_VERSION`, so its upgrader passes states through untouched.
  Nukkit's palette is 1.21.30.7, older, yet every state vanilla's noise
  settings can emit is identical there by `{name, states}`. A binding logs
  any version difference at world load, matches on name and states
  (stamping its platform's own version where the platform's lookup key
  includes it, as Nukkit's does), and routes each state that still does not
  resolve to the fallback table below.
- Unmappable states resolve through an explicit, configurable fallback
  table — never a crash, never a silent stone substitution without a log.
  Both platforms have their own silent fallback to `info_update`; bindings
  must detect the miss themselves rather than inherit it. This is not
  hypothetical: PMMP `stable` has no `minecraft:powder_snow` block at all,
  and the overworld's surface rules emit it, so `ext/` needs a fallback
  entry for it from its first working chunk.
- Mapping happens after conformance diffing (§7), never before. `lib/`'s
  generation engine itself emits nothing beyond Java block state.

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
  interpolation, all cache node types, compiled flat execution program, Tier-A
  goldens passing for terrain shape.

  The aquifer fill decision was part of this milestone and is now **MA**, a
  parallel track. It was moved because it had stopped being a step on the way
  to terrain shape and become an open research programme in its own right —
  four campaigns deep, with the last one's remaining blocker being a router
  entry nobody has measured at all — while everything behind it in the plan
  (surface rules, biomes, the bindings this project exists to feed) needed
  none of it. Sequencing only: nothing shipped with aquifers approximated
  while MA was open, and the filler refused `aquifers_enabled` by name until
  it measured enough to call — see MA's own section for what it calls now
  and what two narrow pieces still are not.

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

  **One item remains; the other closed.**

  1. **RESOLVED. The one-block residual.** §11 has the full chase: a claim
     of "zero at the cell's y boundary" that turned out to be a sparse-
     sampling artifact, a corrected reading that found this build's own
     cell-corner computation wrong (not merely the interpolation between
     corners), and — once the clean-room provision (§12) supplied
     `spec/blended-noise-spec.md` — the actual mechanism: one missing
     epsilon in `PerlinNoise::sample`'s fold, inside `old_blended_noise`'s
     Modern reading. Fixed and confirmed independently against the server:
     an exhaustive block-level rescan that found 322 disagreements now
     finds 0. `golden_terrain_no_aquifer_test.cpp` is 256 of 256 exact
     columns; Tier A's bit-exactness criterion holds for the density chain.
  2. **RESOLVED for the overworld.** `weird_scaled_sampler` — the first of
     the four named functions this bullet used to name, and the one sitting
     on the overworld's own caves path — is settled (§11: both rarity
     ladders measured off the real server, the formula itself pulled from
     Mojang's own 26.2 removal changelog, not the wiki, which has the two
     ladders swapped). With it, the overworld's `final_density` evaluates
     end to end for the first time — this is what actually carves the
     tunnels, overhangs and floating terrain `generate-world.cpp` produces
     (SPEC §11, "the terrain chain runs end to end"). Whatever remains of
     the other three named functions is End-dimension-specific and does not
     block overworld generation; it has not been re-surveyed since.

  **Ore veins, SETTLED — the derivation confirmed per block and wired.**
  Unlike the aquifer, no clean-room spec covers this: there is no
  `spec/ore-vein-spec.md`, so the starting hypothesis came from the
  permitted public references (minecraft.wiki's "Ore vein" and "Noise
  router" pages) rather than a researched brief, and every number those
  pages give was treated as a hypothesis to confirm, not a fact to encode.
  `tools/analysis/ore-vein-probe.sh` builds a fully solid column (density a
  positive constant, so every block starts as stone) with vanilla's own
  real `vein_toggle`/`vein_ridged`/`vein_gap` router entries, and
  `tools/analysis/ore-vein-analyze.cpp` checks the documented algorithm
  against what comes back.

  *A real coupling, found rather than assumed.* A first attempt left
  `aquifers_enabled` false — the honest reading of "isolate ore veins from
  aquifers" — and got back 6291456 of 6291456 blocks as plain stone: not
  one ore, filler or raw block, despite the vein inputs themselves clearing
  their documented gates on a measurable fraction of positions. Setting
  `aquifers_enabled` true as well — with density still a constant positive,
  so the aquifer's own fluid decision can never apply to any block (Q2.2)
  — immediately produced real vein output. The two flags are not
  independent in the real server: ore veins are gated on the same enable
  path aquifers use, even where the fluid logic itself never fires.
  `ore::veinsPlaceBlocks` is that observation and nothing more; it is a
  measured coupling, not a claim about why.

  *The deterministic gate, confirmed with ZERO exceptions.* The y-range
  (`y` in `[-60, -8]` for iron, `[0, 50]` for copper — the dead zone
  `[-8, 0)` between them produces neither), the type/sign correspondence
  (`vein_toggle > 0` for copper, `<= 0` for iron), the richness threshold
  (`|vein_toggle|` must clear 0.6 at either y limit, falling linearly to
  0.4 at 20 blocks inside), and `vein_ridged < 0` as necessary for any vein
  block at all. Block identities likewise: granite/copper_ore/
  raw_copper_block for copper, tuff/deepslate_iron_ore/raw_iron_block for
  iron — iron's whole range sits below y=0, so it is never anything but the
  deepslate variant.

  *The RNG, confirmed per block.* All three draws come from ONE generator,
  `rng::positionalSourceFor(worldSeed, "minecraft:ore").at(x, y, z)`, in
  this order:

  1. `nextFloat() < 0.7` — the block becomes a vein block, or is left alone.
  2. `nextFloat() < clampedMap(|vein_toggle|, 0.4, 0.6 -> 0.1, 0.3)` AND
     `vein_gap > -0.3` gives ore; anything else gives filler.
  3. only after an ore: `nextFloat() < 0.02` upgrades it to the raw-metal
     block.

  **79790 of 79790 candidate positions, 100.000%, across 11 CONTRIBUTING
  seeds**, on every seed alone and on copper and iron alone. That is 36725 on
  the discovery worlds (`probes/orevein-multi`) and 43065 on nine worlds
  generated afterwards and never used to fit anything
  (`probes/orevein-heldout`). Fourteen seeds were run; 200, 24680 and 99991
  produce no candidate row at all and are not counted. The sign-varying probe
  below agrees on a further 11455 SOLID candidates (25509 of its positions are
  air), but those are seed-100 positions already inside the 79790 re-measured
  at a different density — evidence about PLACEMENT, not additional
  independent confirmation of the derivation — so they are reported separately
  rather than summed into it. `tests/conformance/
  vanilla_ore_vein_test.cpp` holds the discovery/held-out split; the
  held-out half is what makes this a law rather than a fit.

  Each threshold is bracketed rather than rounded to. Over the combined
  set the largest draw on a touched block and the smallest on an untouched
  one pin membership into `(0.6999681, 0.7000110]`; 0.6973 reads 99.752%
  and 0.71 reads 99.022%, so only 0.7 is exact. The raw roll brackets to
  `(0.0199925, 0.0200130]` the same way.

  *Why the earlier search missed it, which is the part worth keeping.* The
  membership roll was described throughout as a "30% membership roll", and
  `ore-vein-rng-test.cpp` hard-coded a threshold of 0.3 — while the
  measured marginal touch rate, recorded in this very section, was 69.729%.
  At the CORRECT salt a 0.3 threshold still reads 60.294%: an unremarkable
  near-miss, indistinguishable from the ~58% uncorrelated baseline by eye
  and nowhere near the 85% per-seed bar the instrument used to flag a hit.
  So roughly 1226 salt candidates — four noise names, three router field
  names, the confirmed `"minecraft:aquifer"` salt, ~25 natural-language
  guesses, a systematic 1196-entry word list, and draws 2 through 5 of each
  — were swept and refuted against a threshold that could not have passed
  for any of them. `"minecraft:ore"` was among the names tried. The lesson
  is not about salts: an instrument that can only be wrong in one direction
  will refute the right answer as confidently as the wrong ones, and the
  number that would have caught it (a 69.7% rate against a 30% hypothesis)
  was in the record the whole time.

  *Placement: solid ground only, measured separately.* The solid probe is
  solid everywhere by design, which is what makes it a clean read on the
  RNG and what makes it blind to whether a vein would replace air.
  `tools/analysis/ore-vein-placement-probe.sh` answers it: the same dimension
  with `raw_final_density` a `y_clamped_gradient` crossing zero INSIDE a
  vein range, so the identical candidate set (the vein router does not read
  the density) appears both over solid ground and over air. At seed 100,
  of the candidates the server left as air the confirmed chain would have
  placed **17813 vein blocks and the server placed zero**, over 25509 air
  positions; of the candidates it left solid, all **11455 came back exact**
  — including the ones the aquifer's own barrier turned solid against a
  negative density, which the fully solid probe cannot produce at all. So
  veins replace solid ground only, and "solid" here means whatever the filler
  ended up placing, not merely `final_density > 0` — with one exception the
  probe could not see, recorded below: every one of those 11455 was the
  default block, so the probe says nothing about a vein meeting a FLUID.

  *And nothing repaints them afterwards.* The same script's third dimension
  is solid throughout but runs an UNCONDITIONAL surface rule painting
  `minecraft:diamond_block`, so every candidate answers plainly. All **12934
  positions the chain calls a vein block came back as that vein block, and
  all 5548 it calls stone came back as diamond_block**: surface rules repaint
  the default block and not a vein block. This one is load-bearing rather
  than academic. The overworld's own `deepslate` rule is unconditionally true
  below y = -8 and iron's whole range is `[-60, -8]`, so a filler that let
  surface rules win would erase every iron vein in the world — and every
  existing golden test, all of which run with ore veins off, would have
  stayed green while it happened. `ChunkFiller::applySurfaceRules` skips a
  position holding a vein block; it is keyed on the six vein blocks rather
  than on "anything that is not the default block", because `categorize`
  counts lava as solid too and shielding that would have moved the aquifer
  results §11 already has exact.

  *Two ambiguities carried rather than guessed.* Neither is observable on
  any probe run so far, and both are recorded in `ore/vein.hpp` beside the
  code:

  * `nextFloat()` vs `nextDouble()`. One `nextLong()` backs both, and no
    row of the 79790 lands between the two precisions, so both read
    100.000%. Separating them needs a probe that takes a FOURTH draw.
  * whether draw 2 is consumed when `vein_gap <= -0.3`. Nothing downstream
    reads this position's stream — every position gets its own
    `.at(x, y, z)` generator, so there is no ordering between blocks at all
    — and both variants read 100.000%. The implementation takes the draw
    unconditionally, as the simpler reading.
  * **whether a vein may replace a FLUID — and this one is different in kind.**
    `ChunkFiller` gates the vein call on `block == &settings_->defaultBlock`,
    so a vein never replaces water or lava the aquifer placed. Nothing measured
    that. The placement probe contained zero fluid candidates, provable from
    its own pass, so the guard was CHOSEN rather than observed. The two above
    are unobservable in principle on these probes; this one is merely
    unobserved, and it can change real blocks — a veins-on overworld puts
    aquifer fluid inside the vein ranges routinely. Settling it needs a
    placement probe whose aquifer produces water or lava inside y in
    [-60, 50]. (`categorize()` counts lava as solid, so the probe's "solid"
    and this guard's "replaceable" are not the same predicate.)

  *A spatial fingerprint, measured and deliberately NOT used as evidence.*
  The membership outcome is vertically clustered: agreement with the
  neighbour at `dy=1` is 67.2% against a 57.8% per-block baseline, decaying
  with the trailing 1-bits of `y`, with no horizontal structure at all
  (`+x` 57.0%). That looks like grounds to reject the whole per-block
  `.at(x, y, z)` family — and it is not: the same dyadic fingerprint
  appears for EVERY salt, including junk ones, because it is a property of
  the positional source itself rather than of the derivation. Recorded here
  because reasoning from it would have retired the correct hypothesis class.

- **M4** — Biomes + surface: multi-noise biome source, surface rules,
  Tier-A goldens passing end-to-end in Java block space.
- **M5** — Integration: Bedrock mapping layer, zend binding, chunkutils2
  output, per-world freeze storage, PocketMine world-load path, perf pass.
- **M6 (v2)** — Staged features/structures with declared read/write radii;
  scripting escape hatch evaluation.
- **MA** — Aquifers. Lettered rather than numbered because it does **not** gate
  M4, M5 or M6: it is a parallel track, and the numeric milestones proceed past
  it. It gates only the capability matrix's aquifer row (§8) and any golden
  whose dimension sets `aquifers_enabled`.

  Landed and under test (§11): the cell lattice, the centre jitter and its RNG
  derivation, the ladder, the floodedness gate, the ocean branch with both its
  slopes, the barrier predicate, and where all three router inputs are read —
  including the surface's thirteen-position aborting-minimum scan, re-derived
  at twenty feature scales.

  Four things stood between MA and closing, and all four are now CLOSED —
  the barrier's third source (3) including Q6.3's water-over-lava
  exception, measured on the server and smaller than it read (see (d)),
  Q6.4's mixed-type Π, measured on the same worlds and landed (see (e)),
  fluid TYPE (4), whose level ceiling is pinned at -10 and shown to be an
  ABSOLUTE constant rather than a sea-relative one (§11), and — found by the
  barrier measurement rather than listed at the outset — the LEVEL a source
  carries below lambda, the dry sentinel and the ladder clamp, now measured
  through the barrier's Π rather than through a block readout, which cannot
  see below lambda at all (§11, item 6).

  Two things are deliberately NOT claimed by that. Q5.8 has two further
  conjuncts — a source already reading lava being exempt, and `L != never` —
  which are carried on the specification's word: both are provably
  unobservable in every world measured here, so "the rule is implemented as
  written" is the claim, not "the rule is measured". The 64-chunk
  golden-fill residual below, briefly unattributed, is now attributed for
  425 of its 617 blocks (§11, item 7); the 192 left are a fluid-extent
  question rather than a barrier one.

  **`ChunkFiller` now
  calls all of it** (`aquifer::computeSubstance`, wired into `fill()`):
  measured against a real, aquifer-on overworld region, the wiring's own
  category decision is EXACT on 393216 of 393216 blocks over the four chunks
  `golden_fill_aquifer_test.cpp` pins, and 6291264 of 6291456 (99.997%) over
  a wider 64-chunk sweep. That residual was previously explained away as the
  level pieces above; it is not — the sentinel and clamp change leaves it
  block-for-block IDENTICAL at 6290839 — and the real cause was the
  barrier's own agree-guard (§11, item 7), whose removal fixes 425 of the
  617 and introduces none. WITH the real 287-rule surface tree also running,
  `golden_fill_aquifer_test.cpp` is now 393216 of 393216 too (§11: the
  deepslate surface-rule gap `golden_fill_test.cpp` named is CLOSED, not
  carried). What remains is that 192-block fluid-extent residual, Q5.8's two
  unobservable conjuncts, and ore veins (M3, untouched) — none of which is
  unique to aquifers, which is why this still reads as a track.

  1. **CLOSED. `PslRead::anchor` as the depth path's gate, confirmed by a
     second instrument.** The asymmetry — the near surface gates on the
     window's minimum while the depth path gates on the anchor — was exactly
     the shape that had been wrong twice here, so it stayed open on one
     instrument's word alone. A second, independent probe (§11) built the
     named separating configuration — a field where the two differ by 60
     blocks, eight times the required margin, combed across nine
     floodedness values — and reads each cell strictly inside its own
     ~12-block territory rather than across a wide band that mostly belongs
     to other cells, which is what sank the first attempt at this probe.
     202 of 210 genuinely discriminating cells confirm anchor-gating; the
     eight exceptions are two specific cells whose ladder level exactly
     equals their own centreY, a boundary tie rather than a rival pattern.
     No code changed — current code was already right.

  2. **CLOSED. One correction to `cellFluidLevel` is LANDED; the other two
     needed no code change and are now confirmed against the server.** All
     three were invisible to every other corpus, which is why they went
     unseen — and with all three the original instrument scored 1.00000 on
     410842 cells against 0.9958 without them. One instrument found all
     three, and this project's own history says that alone is a hypothesis;
     it no longer stands alone.

     *Landed.* The `centreY >= -54` conjunct in the aborting near-surface
     return was wrong, and is now `centreY >= lambda` — both the comparand
     and the return value, which was the bare literal `kLavaLevel` before.
     A second, independent instrument (the `sea_level < -54` world below)
     confirms it directly: 251229 blocks the old reading called wet up to
     y=-55 are observed dry at every one, 0/251229, over 1966080 blocks.
     The comparand itself is a PERMANENT TIE, not a further measurement —
     `lambda` equals `sea_level` on every world that can even ask the
     question, so no corpus can separate "the comparand" from "the return
     value" once both are lambda-based; using `lambda` in both places is a
     no-op at every already-verified sea_level, not an isolated finding.

     *Now confirmed, both without a code change.* Both were open because
     every probe that could reach them held `preliminary_surface_level`
     CONSTANT, which makes the scan's `gate` and `cap` the same number by
     construction (`readPreliminarySurface`, §11) — trivially true of the
     `sea_level < -54` world above too, so landing the correction there
     could not touch either. `tools/analysis/aquifer-nearsurface-probe.sh`
     drives a genuinely varying psl instead, held at floodedness 0.9 (past
     both level-rule gates, so only the abort state is left to vary an
     outcome), and reads every block rather than by fluid body — a
     body-boundary reading is corrupted here by water/lava contact turning
     to obsidian mid-column, which is why the analyzer's own first version
     scored 0.51 before that fix. The near-surface floor's comparand reads
     `cap`, not `gate`: a perfect 1.0000 against 0.9266-0.9358 on 7.8M+
     discriminating blocks across two seeds. The `aborted` guard on the sea
     outcome is correct as written on both its copies: 0.9911-0.9941 against
     0.0564-0.0924 on the depth path, 0.9812-0.9829 against 0.6612-0.6872 on
     the ocean branch, on 469575-913229 discriminating blocks each.

  3. **CLOSED for (a)-(c). Which sources compete, and how hard.** About
     13-16% of the server's real barriers come from a third source the old
     two-source-only `placesBarrier` could not see at all. Four things stood
     between the clean-room spec's Q6.6 and code: (a) **DONE** —
     `selection.hpp` ranks four sources per block (window, integer metric,
     later-wins tie-break), with golden coverage on 637252 of the server's
     own barrier blocks and 0.99993 on the substance it decides.
     (b) **DONE, mostly by arithmetic rather than a probe.** At `D = -1` —
     every Stratum aquifer probe's own density constant so far — Q6.4's
     pressure function, with its stated divisors (1.5/2.5 near-fluid, 3
     near-air), reproduces the OLD, already-confirmed two-source rule EXACTLY
     over an exhaustive sweep of 39150 synthetic configurations: 0
     mismatches. The old rule's own 251M-block server validation transitively
     confirms those divisors; the fourth ("/10", `3+t<=0`) stays unmeasured —
     not wrong, just never reached: 0 of 866 real-block mismatches touched it
     across the barrier probe below, and it was never used at all, on either
     seed. (c) **DONE.**
     `tools/analysis/aquifer-barrier-probe.sh` drives `barrier`,
     `fluid_level_floodedness` and `fluid_level_spread` with vanilla's own
     REAL noises (a synthetic field built for MA blocker 2 had no reason to
     produce genuine three-way junctions) at three density constants and two
     seeds. The three-source formula rescues 83-98% of the two-source rule's
     errors on real blocks, cutting the real-barrier miss rate from
     13.19%/16.48% to 1.01%/1.02% overall, seed-to-seed — matching the ~13%
     figure this project had already measured by a different route. `BarrierAt`
     now carries `D` and a third ranked source (`lib/include/stratum/aquifer/
     barrier.hpp`). (d) **DONE. Q6.3's water-over-lava exception, measured
     and landed** — in `computeSubstance`, in front of the predicate rather
     than inside it. It reduces to Q1.2's picker, a function of `y` alone,
     so it can fire on exactly ONE row, `y = lambda = min(-54, sea_level)`:
     Q2.4 has already answered everything below. `tools/analysis/
     aquifer-waterlava-probe.sh` (the barrier probe's configuration with
     the sea inside the world, plus a `sea_level` -70 arm; three seeds)
     found it real where the row has water sources — 278 / 1102 / 1940
     blocks at sea -70, the bare logic writing stone on 14 / 50 / 70 of
     them and the server on 0 of 3320, the two decisions identical one row
     up, and a nearest source reading AIR on the same row still getting
     the server's barriers (93 / 239 / 244) — and EMPTY on the shipped sea:
     no source reads water at y = -54 on 3 seeds x 3 densities x 16384
     columns, 0 stone on that row from either side. The same probe found
     Q2.4 itself unimplemented (below the sea the decision read the
     nearest source's type, water on 7682 / 5330 / 5736 of 16384 blocks
     per density, invisible to category-only goldens); it is landed too,
     16384 / 16384. (e) **DONE. Q6.4's mixed-type Π, measured on the same
     worlds and landed** — in `placesBarrier`, with every ranked source
     now typed (`aquifer::StatusCache`). The sea -70 arm is the first
     world where lava-typed sources compete with water-typed ones on rows
     the lattice owns, and it settled which of three readings "`Π = 2.0`
     if one reads lava and the other water" is: what each source READS at
     `y`. A lava body meeting a water body — both fluid — takes the
     constant; a pair that disagrees at `y` keeps the level formula
     whatever its types. Pooled over three seeds, the server's real
     barriers missed in mixed junctions fall from 1698 to 590 (1240 to
     330 on the rows above the sea), with 0 false stone before and after,
     and every one of the 1108 blocks the constant adds is server stone.
     The two other readings are refuted on the same blocks (§11). What
     remained was the level a source carries below lambda — the dry
     sentinel and the ladder clamp — and it is now **LANDED**, which
     re-anchors the figures above: the same pooled reading is 1100 to 4,
     and the pure-junction misses 35 where they were 293, still 0 false
     stone. All 4 and all 35 sit on row lambda itself. See item 6.

  4. **Fluid TYPE — measured; the ceiling is now CLOSED too.** `fluid_type.hpp`
     scores 0.99873 per source on 3125 sources over four seeds against a
     0.94176 null, and the `lava` entry's own read position is settled:
     contracted indices on a SIXTY-FOUR block horizontal pitch, where the
     spread's is 16 (§11). **Strictness at exactly 0.3 is now CLOSED**:
     `aquifer-fluidtype-probe.sh` drove `lava` to the exact double 0.3 and to
     its two adjacent doubles, and the server's own answer lands exactly
     where the code already read it — strict `>`, confirmed to the ULP. The
     **level ceiling is now PINNED at -10**, inclusive: group D of the same
     tool reaches level -9 by the two routes the ladder's mod-3 lattice does
     not constrain — the sea branch, and the psl cap at two different sea
     levels — and reads -12/-11/-10 lava and -9/-8/-7 water on the two
     six-dimension arms, with the `sea_level` -70 arm running four of them
     (-12/-10 lava, -9/-8 water). The LEVEL ENTRY is 16384 of 16384 columns
     with 0 of the other fluid (a dimension's own total is larger: it also
     carries deep bodies and fluid-tick contact artefacts), on
     seeds 42, 7 and 999. The same run shows the ceiling is ABSOLUTE rather
     than sea- or lambda-relative (§11). A third piece is carried on the
     spec's word — whether a source already reading lava is exempt — and
     cannot be observed, since those sources sit below the lava sea.

  5. **CLOSED. `ChunkFiller` now calls the aquifer.** `aquifer::computeSubstance`
     (`lib/include/stratum/aquifer/substance.hpp`) combines everything above
     into the one decision a caller needs — rank the four nearest sources,
     read each of the three nearest ones' own fluid level, decide the
     barrier or fall through to the nearest source's own reading, and its
     type — and `ChunkFiller::fill()` calls it wherever `aquifers_enabled`
     is set and its own density is non-positive (Q2.2). The old refusal is
     gone.

     *Measured against real generation, not a void-column probe.*
     `tools/analysis/aquifer-on-probe.sh` builds vanilla's real overworld —
     aquifers and all, only `ore_veins_enabled` forced off — with the same
     empty-biome swap the aquifer-free sibling uses, so neither a carver nor
     a feature is ever what a mismatch gets blamed on.
     `golden_fill_aquifer_test.cpp` reads it two ways: RAW (no
     `surface::RuleGraph` at all, isolating the aquifer's own decision) is
     EXACT on 393216 of 393216 blocks' category over the four chunks
     `golden_fill_test.cpp` already uses. WITH the real 287-rule surface
     tree running, category is now ALSO 393216 of 393216. It first read
     392755 (99.883%, 461 short) — not a new gap: `golden_fill_test.cpp`'s
     own already-documented one (the unconditioned `deepslate` rule firing
     by Y alone), reached far more often because a real aquifer carves
     genuinely different terrain — air pockets and drained cells a flat sea
     level never produces — for that same rule to misfire on. Confirmed by
     construction: the RAW pass was already exact, so nothing past the
     first pass belonged to the aquifer. 451 of the 461 were air, not
     fluid, and closing them is what took the general fix (§11) rather than
     a fluid-only one.

     *A performance finding, not just a correctness one.* The first version
     of this recomputed every ranked source's own `preliminary_surface_level`
     scan — up to fourteen `density::Interpreter::evaluate` calls — on every
     block that reached the aquifer, with no caching: measured at over 30
     seconds a chunk, impractical for anything real. `aquifer::StatusCache`
     (then `LevelCache`; it memoizes the fluid TYPE too since Q6.4 started
     typing every ranked source) memoizes a cell centre's own status across
     one `fill()` call — a chunk touches dozens of distinct centres, not
     thousands of blocks' worth of them — and brought that to about 1
     second a chunk, the same shape of fix `ChunkFiller`'s own biome cache
     already used for surface rules.

     *Wider, still open — and one guess about it now REFUTED.* Over a
     64-chunk sweep of the same probe world (6291456 blocks), RAW category
     was recorded here as 6290723 exact, a 733-block residual, and this
     entry guessed the wrong direction (mostly missed barriers,
     `solid->fluid`) made it "consistent with" the two lattice-level pieces
     the sea -70 worlds separated — the mixed-type constant, since landed,
     and the level a dry or clamped source carries below lambda. The second
     half of that guess is wrong. Re-measured with the same loop widened to
     chunks 0..7, twice over — once with the level model item 6 landed and
     once with the old one linked into the same binary in its place — the
     two produce the IDENTICAL set of mismatching blocks, coordinate for
     coordinate: 6290839 of 6291456 either way, a 617-block residual the
     change moves by exactly zero. (617 rather than 733 because this figure
     predates other work and was, as the entry said, not re-measured since.)
     Item 6's change is large elsewhere — 121 three-source misses to 6 on
     `barrier3way` — so what this establishes is that this world's residual
     lies somewhere else, and the dedicated probe it still wants should not
     start from the level rule.

     *Now mostly ATTRIBUTED (item 7).* It was the barrier after all, though
     not the part anyone was looking at: `termFires` carried an agree-guard
     (`aFluid == bFluid` -> no barrier) that the spec does not have and the
     server refutes. With it gone the same 64-chunk RAW sweep goes to
     **6291264 of 6291456**, a 192-block residual which is a strict SUBSET
     of the 617 — 425 fixed, 0 introduced. The 425 are barriers the guard
     suppressed (289 water -> stone, 113 water -> deepslate, 17 water ->
     dirt, 6 air -> deepslate). The 192 that remain are all one shape, this
     build placing air where the server has water, which makes what is left
     a fluid-EXTENT question rather than a barrier one.

  6. **CLOSED. The level a source carries below lambda: the dry sentinel and
     the ladder clamp.** `cellFluidLevel` reported a DRY source as `lambda`
     where Q5.6's value is the sentinel `never`, and `ladderLevel` clamped a
     ladder that fell below `lambda` back up to it where Q5.7 has no clamp.
     Both are now as the spec states: `lattice.hpp`'s `kNeverLevel`, spelled
     as Q1.4's arithmetic `16 * min_y_limit` rather than the literal, per the
     spec's own open question 4.

     *Why it went unseen for four campaigns.* Neither is visible in a block
     readout at all. Q2.4 hands every `y < lambda` to the global lava sea
     before the lattice is consulted, so every candidate level at or below
     lambda paints identical chunks; the campaigns that recorded the dry
     outcome as "Λ" were not wrong, only under-determined — "at or below Λ"
     is the most a readout can establish, and the `sea_level` -56 world that
     separated -54 from Λ could not have separated Λ from anything lower.
     What CAN see it is the barrier's pressure Π, which weighs both levels
     whether or not either is readable at the block: with one level far
     below, `levelPressure`'s `t = r - |h|` reduces to a term independent of
     it on the `h > 0` side, while at `lambda` the same pair lands on the
     `h <= 0` side, whose divisors are 3/10 rather than 1.5/2.5, with a
     fluid plane sitting right under the block.

     *Measured, on rows lambda-1..+40 of the three water/lava probe worlds,
     against 88949 server stone blocks.* Mixed/pure real-barrier misses by
     level model: the old contract 704/1790; the sentinel alone 33/758; the
     unclamped ladder alone 677/1063; both 7/54. Both halves are
     load-bearing and neither alone reaches the pair. **No model in the
     sweep writes a single block of stone the server does not** — 0 false
     stone throughout, which is the reading that would have refuted the
     change outright. On the independent `barrier3way` world (15728640
     blocks, 11923 real barriers, three densities, real noise) the
     three-source miss count falls 121 (1.015%) to 6 (0.050%), and the two
     mismatch sets were compared coordinate by coordinate: the 6 are a
     strict SUBSET of the 121, 115 fixed and none introduced. The
     conformance suite's own figures moved with it — the water/lava case
     reads 1100 -> 4 mixed misses where it read 1698 -> 590, and 35 pure
     where it read 293.

     *What is measured and what is derived, kept apart.* The exact sentinel
     VALUE is NOT observable. Sweeping the dry level as `lambda - K`, the
     score is monotone in K and saturates at K = 32: K = 32, 64, 256 and
     -32512 are byte-identical, K = 16 is not (91 pure misses against 54).
     So the corpus measures "at least 32 below lambda"; -32512 itself is
     Q1.4's arithmetic and is carried as such, not as a reading.

     *Two things deliberately NOT changed, and said so rather than left to
     be assumed.* The near-surface aborting floor still returns `lambda`:
     every world in this sweep holds `preliminary_surface_level` constant at
     96, so that early return never runs in any of them, and the one
     conformance world that does reach it reads only `y >= lambda`, where
     the candidates are again indistinguishable. And the trailing guard's
     replacement stays the literal `kLavaLevel`, which the `sea_level` -56
     campaign pinned directly.

     *Q5.8's `L != never` conjunct is now representable, and lands as spec
     hygiene — never as a measurement.* It is also provably INERT: a source
     at `kNeverLevel` reads fluid at no real `y`, and every consumer of a
     source's type is guarded by a reading (`waterOverLava` needs
     `y < nearestLevel`, Π's mixed-type constant needs both sources reading
     fluid, the final `Fluid` return needs `nearestReadsFluid`). The corpus
     is blind to it for a specific reason worth recording: both Π worlds
     declare `lava` a constant 0.0, which short-circuits `fluidTypeOf`
     before the level term, and the `comb_*` worlds pin floodedness at 0.5,
     so no source is ever dry there. Calling it "measured" would repeat the
     error item 4's own history records.

     *The residual, recorded rather than tuned away.* 7 mixed + 54 pure over
     the 42-row water/lava band, of which 4 + 35 sit on row lambda itself
     where Q6.3, Q2.4 and the trailing guard all interact, plus 6 of 11923
     real barriers on `barrier3way`. Nothing was fitted to close them. (The
     6 on `barrier3way` are CLOSED by item 7 below; the water/lava band's
     own residual falls with it too, but not to zero.)

  7. **CLOSED. Q6.4's fourth divisor is 10 — and the agree-guard that hid it
     is REFUTED.** The `/10` arm (`u = (3 + t)/10` when `3 + t <= 0`) was
     carried on the clean-room spec's word through four campaigns, each of
     which recorded "0 uses" and read it as a world-shape problem. It was
     not one.

     *Why no world could ever have reached it.* `termFires` carried a guard
     of this build's own invention — `aFluid == bFluid -> return false` — so
     `levelPressure` was only entered by a pair that DISAGREED at the block.
     On that domain `t >= 0.5` for every integer `(L_A, L_B, y)`: a
     disagreement means `min(L) <= y < max(L)`, hence `|h| <= r - 0.5`.
     So `3 + t >= 3.5 > 0` always, and both the `/10` arm and the `/2.5`
     arm were unreachable by ARITHMETIC, not by accident of the corpus. An
     exhaustive sweep agrees: of 3135020 disagreeing combinations, `/1.5`
     takes 1580530 and `/3` takes 1554490, `/2.5` and `/10` take 0 each.
     Nothing in Q6.4 or Q6.6 gates Π on the pair agreeing; `Δ = 0 -> Π = 0`
     is the whole of what keeps an equal-level pair inert.

     *The world that can see it.* `tools/analysis/aquifer-deepfloor-probe.sh`
     pins `fluid_level_floodedness` to a constant 0.6 — strictly between
     `kFloodedLocalThreshold` (0.4) and `kFloodedSeaThreshold` (0.8) — so
     every cell takes the LADDER instead of `sea_level` and neighbouring
     cells hold DIFFERENT levels. That is the whole trick: the older barrier
     worlds hand nearly every flooded cell the sea, `Δ = 0`, and no arm is
     entered. On `barrier3way` the `/10` arm is entered a few hundred times
     and DECIDES nothing — substituting `/3` for `/10` there flips exactly 0
     blocks, which is why that world cannot measure the divisor however many
     blocks it holds.

     *The guard, scored against the server.* Three seeds x seven dimensions,
     110097250 blocks against 4982316 blocks of server stone: guarded
     480354 misses; both-air lifted 457970; both-fluid lifted 22384; no
     guard **0 misses and 0 false stone**, exact on every arm. The four
     older worlds (`barrier3way` plus three water/lava seeds, 90027838
     blocks, 243887 server stone) order the same way at 161 / 46 / 154 / 39.
     The abort condition set before the run — "the un-gated reading writes
     stone the guarded one does not" — never fired anywhere. The control
     dimension, real floodedness with nothing rescaled, reproduces
     `barrier3way` exactly, so the recipe distorts only the level diversity
     it was built for.

     *The divisor itself, bracketed from both sides.* The `/10` arm decides
     96292 blocks; on the 65231 where divisor 10 and divisor 3 disagree the
     server has stone on 65231 of 65231 — 10 right on all, 3 on none.
     Perturbing only that divisor (pooled misses / false stone): 1.5
     81856/0, 2.5 70317/0, 3 65231/0, 5 45584/0, 8 17787/0, 9 8868/0, 9.5
     4497/0, 9.9 937/0, **10 0/0**, 10.1 0/872, 10.5 0/4410, 11 0/8627, 12
     0/17177, 20 0/79432. Monotone, one-sided on each side, so 10 is pinned
     to within 1%. The `/2.5` arm, dead under the same guard, brackets the
     same way: 2.4 leaves 1104 misses, 2.5 is exact, 2.6 writes 1111 false
     stone. Both arms can now be named — `/2.5` is the barrier's LID a few
     blocks above the higher of two levels, `/10` its FLOOR four or more
     below the lower.

     *What it cost elsewhere.* 425 of the 64-chunk golden-fill residual
     (item 5), which had become unattributed. Pinned by
     `vanilla_aquifer_deepfloor_test.cpp`, which asserts exactness, that the
     arm still decides blocks, and that divisor 3 loses the head-to-head —
     the first of those is what the guard failed, and the second is what
     would go red if the arm ever fell out of reach again.

  The golden set's one standing requirement is **MET**: a conformance case
  with a spatially varying `preliminary_surface_level` now exists
  (`vanilla_aquifer_varying_surface_test.cpp`, generated by
  `tools/analysis/aquifer-psl-probe.sh`). It scores 0.99614 over 9039986
  blocks on six worlds, and it checks that the corpus can say anything before
  it reports the score — three arms reaching the gate, 96.7% of sources
  aborting the scan, and a prefix minimum differing from the whole-window
  minimum on most of them, none of which a constant surface can produce.

Each milestone closes only when its tests run in CI on all targets. MA is a
track rather than a step: the numeric milestones do not wait on it.

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

  **`find_top_surface` is settled (MA).** minecraft.wiki gives the semantics —
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

  Against the aquifer-free reference, over four chunks and 393216 blocks,
  WITH the overworld's full 287-rule surface-rule tree now running (below):
  **100% are the exact right block** (393216 of 393216). A 475-block gap sat
  here for a time — `deepslate`'s own bare, unconditioned `vertical_gradient`
  winning over real fluid inside deep ocean trenches' cave-voids — and closed
  once `ChunkFiller`'s own second pass, not the rule tree, turned out to be
  where vanilla actually gates it (§11).

  **What it refuses.** Neither of the two flags any more. `aquifers_enabled`
  stopped being refused at MA (below): `aquifer::computeSubstance` decides
  the block wherever the density alone would not, called from `fill()`
  itself. `ore_veins_enabled` stopped being refused at M3 (above):
  `ore::VeinSource` replaces a solid block wherever the vein system says so,
  confirmed per block on 79790 candidates across 11 contributing seeds.
  Vanilla's
  overworld sets both flags, and this filler now runs both — what remains
  between it and an exact overworld is the two narrow aquifer pieces §11
  names, not a whole subsystem.

  Surface rules are NOT refused, because they only ever replace blocks the
  filler already placed. A column without them is bare stone where grass and
  dirt belong — visibly incomplete rather than wrong.

  **Surface rules: `vertical_gradient`, SETTLED (M4).** Two of the overworld's
  three top-level surface rules are vertical gradients — bedrock at the world
  floor and deepslate — and between them they accounted for 4368 of the 4496
  blocks the filler got wrong.

  The probability was measured early and was never the hard part:

  ```
  p(y) = (false_at_and_above - y) / (false_at_and_above - true_at_and_below)
  ```

  clamped, drawn once per block. The random source took nineteen refuted
  derivations, several sessions of characterisation, and in the end came from
  somewhere else entirely: **the aquifer's cell-centre jitter, recovered first,
  showed what shape to look for.** It is the same primitive —

  ```
  source = rng::positionalSourceFor(worldSeed, random_name)   // fork, salt, fork AGAIN
  fires  = source.at(x, y, z).nextFloat() < p(y)
  ```

  — and the second fork is precisely what the nineteen were missing. With a
  single fork the identical code scores 34% where the correct one scores 100%.

  *Checked on 27 million blocks.* One band at seed 42: 786432 of 786432. The
  55-band bracket sweep, where every band shares one draw per position and
  differs only in its threshold, so all 55 must follow from a single value:
  21626880 of 21626880, worst band also exact. Two further world seeds against
  three names, one of them outside the `minecraft` namespace: 4718592 more,
  every combination exact. `vanilla_vertical_gradient_test.cpp` pins the first
  two.

  *One thing measured along the way that contradicts an obvious assumption.*
  Not every pair of names gives independent fields. Over one probe world the
  server's own `minecraft:deepslate` and `minecraft:bedrock_floor` differ on
  24.6% of blocks, where two unrelated names differ on 50.7%. Both are
  reproduced exactly here, so whatever brings those two salts close together
  is inherited rather than approximated — and a test asserting independence
  between them would be asserting a falsehood. `surface_rule_test.cpp` says so
  in place, since it is the kind of thing a later reader would otherwise
  "fix".

  With this the condition is no longer refused: `RuleGraph::unrunnableReason`
  returns nothing for `vertical_gradient`, and the earlier record of what was
  known and not known about it is superseded by the derivation above.

- **Octave amplitudes do not associate, and this build had it wrong (M3).**
  `OctaveNoise::sample` folded a noise's amplitude into its persistence at
  construction and multiplied the sample by the product — `(a * p) * s`, the
  obvious optimisation. Vanilla computes `(s * a) * p`, and floating-point
  multiplication does not associate, so the two are different doubles.

  It hid for as long as it did because both groupings are EXACT whenever the
  amplitudes are powers of two, and every noise this project had checked drew
  its amplitudes from {0, 1, 2}. Seven vanilla noises do not, among them
  `minecraft:temperature` — a biome-source parameter — at `[1.5, 0, 1, 0, 0, 0]`.
  Over a 48x48 grid at seed 4242, 308 of 2304 columns differ by one ulp.

  *The server was asked directly, and it is not ambiguous.* A probe put the two
  candidate values in as EXACT `noise_threshold` bounds — a degenerate interval
  `[w, w]` fires only where the server's own value is bit-for-bit `w` — with
  forty discriminating positions under each grouping. The server painted all
  forty of `(s * a) * p` and **none** of the other.

  *cubiomes sides with the old reading, and that is the interesting part.*
  244 of the 252 climate vectors are unaffected and remain asserted bit-exact;
  the 8 that move are all `temperature`, and 34 of the 42 temperature vectors
  are still identical, 6 out by one ulp, one by three, one by five. So cubiomes
  made the same fold. §2 already said agreement with it is strong evidence
  rather than proof, and this is the first place that distinction has had to be
  used: `vanilla_climate_test.cpp` now records the divergence and bounds it at
  the measured maximum instead of asserting equality it cannot have.

  Found by a surface-rule agent through `noise_threshold`, which is worth
  noting because nothing about the bug is a surface-rule matter — it had been
  sitting under the biome source and the density chain the whole time, invisible
  to every check that used a power-of-two amplitude.

- **The NormalNoise value factor is one ulp out, and the spelling is the
  answer (M3).** `(5n) / (3(n+1))` stood in `perlin.cpp`. Vanilla computes
  `1 / (6 * expectedDeviation)` with `expectedDeviation = 0.1 * (1 + 1/n)`,
  and although the two are the same real number they are not the same double:
  they part at effective octave counts 1, 2, 6, 9, 15, 16, 17 and 22.

  That is **39 of vanilla's own noises** — thirty at span 1, three at span 2,
  `patch` at 6, `continentalness`, `continentalness_large`, `gravel_layer` and
  `soul_sand_layer` at 9, and `jagged` at 16. Two of those are climate
  parameters the biome source reads.

  Nor is any algebraically equal spelling safe: `1/(6*(0.1*(1+1/n)))`
  diverges at fifteen further counts, and `(1.0/d)/6.0` at another set. The
  form above is the one that reproduces the server.

  *Settled the same way the grouping was, and more broadly.* Probes put both
  candidate doubles in as EXACT `noise_threshold` bounds on three noises
  spanning the affected range — `continentalness` (span 9),
  `jagged` (16) and `noodle_thickness` (1) — across two seeds. The server
  painted **all 120 positions of this form and none of the other**.

  **What this costs, and what it buys.** cubiomes made both simplifications
  too, so the climate vectors can no longer be asserted bit-exact throughout.
  They are still exact for every noise neither correction reaches, and bounded
  at the measured maximum for the three that they do — five ulps for
  `temperature` (the grouping, via its 1.5 amplitude), two for
  `continentalness` and `vegetation` (the value factor). §2 always said
  agreement with cubiomes was strong evidence rather than proof; this is the
  second place in one session where the server has been asked directly and
  disagreed with it.

- **Seven more surface conditions run, and the overworld is down to three (M4).**
  A round of measurement settled the semantics that were blocking them, and
  they are implemented in `stratum::surface::Executor` as:

  | condition | rule |
  |---|---|
  | surface depth | `(int)(2.75 * surface(x,0,z) + 3.0 + 0.25 * u)`, truncating, no clamp at 0 (`max(0, depth)` refuted; a clamp below -1 is unseparated) |
  | `hole` | `surfaceDepth(x, z) <= 0` — it never looks at the terrain |
  | `y_above` | `y + (add_stone_depth ? stoneDepthAbove : 0) >= anchor + multiplier * surfaceDepth` |
  | `water` | the same with the column's LATCHED water height plus `offset` in the anchor's place; unconditionally TRUE where the column holds no fluid |
  | `stone_depth` | `depth <= offset + (add_surface_depth ? surfaceDepth : 0) + (int)((secondary + 1) * 0.5 * range)`, on a 0-based depth |
  | `steep` | `(hW - hE >= 4) || (hS - hN >= 4)`, neighbours clamped inside the block's own chunk |
  | `noise_threshold` | `min <= noise.sample(x, 0.0, z) <= max`, closed, sampled at a literal y of zero |

  Three of those are worth stating twice because the obvious reading is wrong.
  The surface depth's jitter comes from the world seed's UNSALTED positional
  source — fork once, no name, no MD5 — where `vertical_gradient` forks, salts
  and forks AGAIN; mixing them up is the easiest mistake here. `stoneDepthAbove`
  SKIPS fluid without resetting, so a stone run below a water band does not
  inherit the top of the world. And `steep` is asymmetric on purpose: west
  minus east, south minus north, with `abs()` refuted by 17375 columns that
  have to stay false.

  **RESOLVED.** `biome` and `temperature` both run, and so, now, does
  `bandlands` — the list went ten, nine, three, one, zero. The overworld's
  own tree compiles WHOLE: nothing left in the schema refuses it. What used
  to keep `ChunkFiller` from actually running it was a missing INPUT, not an
  unrunnable construct — its one `temperature` condition had nowhere to get
  a biome's declared temperature from — and that closed too, with
  `biome::TemperatureTable` (`ChunkFiller` wiring, below); the overworld's
  own tree now runs end to end against real blocks. The Nether's tree needed
  neither `bandlands` nor `temperature` to begin with, so it was the first
  real dimension whose surface rules compiled whole; the other six joined it
  once `bandlands` closed.

  *An API trap found while testing, and closed.* An `Executor` keeps pointers
  to its graph, geometry and noises, so compiling from a TEMPORARY graph
  dangles — and `Executor::compile(RuleGraph::resolve(json, id), seed, geom)`
  reads like it ought to work. It is now a deleted rvalue overload, so that is
  a compile error rather than a crash at the first `apply`, which is where it
  actually surfaced.

- **Surface rules RUN, for the trees this build can run whole (M4).**
  `stratum::surface::Executor` walks a resolved graph and returns the block the
  rules place at a position, or nothing where they place none — which is the
  ordinary case and means the filler's own block stands.

  It refuses the same way the filler does, and for the same reason: a tree
  containing one unrunnable construct is refused ENTIRE at compile, by name and
  with the reason, rather than run with that branch quietly skipped. A surface
  rule that sometimes does nothing is a world that generates and is silently
  wrong, which §8 puts in the most severe class there is. As of `bandlands`
  closing, nothing in vanilla's schema triggers this refusal for any of the
  seven dimensions any more — the mechanism stays, for whatever a future data
  pack format adds that this build does not understand yet, but today it has
  nothing left to name.

  One consequence worth stating. Because compile refuses first, a `Context`
  missing a field for a construct this build genuinely CANNOT run is a
  programming error and never a wrong world. `biome` itself is not one of
  those any more — every condition type runs, given a `Context` that carries
  what it asks for — so that guarantee now belongs to the CALLER that builds
  the Context, not to `Executor::compile` alone: see `ChunkFiller`, below, for
  what happens to a tree that reads `biome` when no caller-supplied biome data
  exists to answer it. And because the positional sources are built once per
  `random_name` at compile, a gradient fired millions of times pays for no MD5
  and no forking.

  `above_preliminary_surface` is MEASURED, and it is not what its name says:

      above_preliminary_surface(x, y, z)  ==  y >= psl + surfaceDepth(x, z) - 8

  where `psl` is the router's `preliminary_surface_level` — NOT read at
  `(x, 0, z)`: it is sampled on a 16-block lattice and blended, which the
  measurement further down settles, and which every constant-psl probe in this
  sweep is blind to by construction. The condition opens `8 - surfaceDepth`
  blocks BELOW the level it is named after — two to eight on ordinary
  terrain — and the 8 is a literal,
  invariant under cell height (4/8/16), cell width (4/8/16), `min_y`, `height`
  and `sea_level`. `surfaceDepth` is the same quantity the table above
  defines; no new noise and no new RNG derivation enter here, which is part of
  why the fit is believable.

  The strictness this project called unmeasured for two milestones was never
  the open question. The condition is true at exactly that y and false one
  block under it on every column measured, so `y >= C` and `y > C - 1` are the
  same predicate; what was unknown was `C`, and `C` is not the preliminary
  surface. The earlier "true everywhere" reading was an artefact of the
  analysis, not of the probe: it read the terrain's top block, where a
  condition of this shape is true by construction, instead of the bottom edge
  of the band the rule paints. That probe is still on disk and shows a clean
  stone/marker step in all 36864 of its columns.

  The depth enters UNCLAMPED — negative values included. That is the one
  thing about this boundary that stayed open past the 52 dimensions below,
  and it is settled further down this section.

  Measured by `tools/analysis/aps-boundary-probe.sh` (52 probe dimensions
  across three specs and two seeds — 30 in `apsb`, 10 in `apsb2`, 12 in
  `apsb3` — read back by `aps-boundary-analyze.cpp`) and scored in
  `tests/conformance/vanilla_above_preliminary_surface_test.cpp`. The
  boundary's own evidence is 36864 columns at each of two seeds; the 12
  geometry dimensions re-score one seed's same 36864 columns twelve times
  over, which is what makes the 8 a literal rather than what makes the
  boundary a boundary.

  That the band's lower edge is the CONDITION going false, rather than the
  vanilla surface pass stopping at a height, is a separate statement and has
  its own control: in `probes/surf`, one world and one pinned
  `preliminary_surface_level`, `bandlands` (no condition at all) paints all
  36864 columns down to the world floor and `steep` (a condition that does not
  read y) paints all 6217 of the columns it selects down to it, while `aps`
  reaches the floor in none and stops between -8 and -2. A pass bounded by
  height would have clipped all three alike.

  Two consequences beyond the boundary itself:

  * `preliminary_surface_level` reaches the condition through a FLOOR, not a
    `static_cast`. Only a negative fraction separates the two, and vanilla's
    own `find_top_surface` returns whole multiples of its `cell_height`, so it
    is a datapack-only difference — measured rather than assumed, and fixed in
    `terrain::ChunkFiller`.
  * a tree naming this condition now needs `minecraft:surface` and
    `minecraft:surface_secondary` in its registry, since the boundary reads a
    surface depth. `surface::readsSurfaceDepth` is public for exactly that
    reason, and `ChunkFiller` reports a tree it cannot supply as blocked by
    name rather than throwing at the first block.

  **Where a varying `preliminary_surface_level` is SAMPLED — MEASURED, and
  it is not the column.** The entry that reaches `above_preliminary_surface`
  is

      psl(x, z) = floor( bilerp( floor(R(X0, Z0)), floor(R(X1, Z0)),
                                 floor(R(X0, Z1)), floor(R(X1, Z1)),
                                 (x - X0)/16, (z - Z0)/16 ) )

      X0 = floorDiv(x, 16) * 16,  X1 = X0 + 16   (Z likewise)

  where `R` is the router entry. A 16-block horizontal lattice anchored at the
  world origin — which, a chunk being 16 wide, is exactly each chunk's own
  four corners — blended linearly in x and z, with the floor falling TWICE:
  once at each lattice sample and once on the blend. Implemented in
  `terrain::ChunkFiller` (`kPreliminarySurfacePitch`,
  `preliminarySurfaceIn`), which now evaluates the entry four times per chunk
  rather than 256 times.

  Each part was measured separately rather than fitted together, and the
  lattice is then re-derived without any of them — which matters here because
  this question had already collected two wrong universal claims from
  under-powered samples:

  * **The pitch, by TRANSLATION, with no blending rule assumed.**
    `probes/psllat`'s `s_cNN` dimensions drive the entry with the SAME field
    shifted NN blocks in x (a `shifted_noise` constant shift, so the shift is
    exact); `t_cNN` the same in z. A per-column read gives
    `psl_NN(x, z) == psl_0(x+NN, z)` for every NN. Measured: it holds on
    every column for NN in {0, 16, 32} — 33792 of 33792 at 16, 30720 of
    30720 at 32 — and on 2.2% to 20.4% of them for NN in {1..8, 12}, the
    worst being 783 of 35520 at NN = 7. Repeated in z, and repeated at seed
    31337. That refutes a per-column read AND every pitch in {1, 2, 4, 8, 32}
    at once, because each would have made a different subset of those rows
    total.
  * **The anchor, as one scan over one parameter at that pitch, and it is
    not assumption-free.** Exactly 1 of the 256 phases reproduces all 36864
    columns, and it is (0, 0); the runner-up gets 7542 of 36864. What that
    scan ASSUMES is worth stating, because it is where the circularity would
    hide: each phase is scored at the pitch the TRANSLATION test measured and
    under the double-floored bilinear blend, so it is one axis of a joint
    pitch x anchor x blend search, not an independent measurement of the
    anchor. The pitch arrives from an argument that assumes no blend at all,
    and the blend from `f_*` dimensions whose verdict does not depend on the
    anchor — which is what keeps the three from leaning on each other.
  * **The blend and the two floors.** `probes/psllat`'s `f_half` (arms -0.5 /
    +0.5) and `f_quart` (-0.25 / +0.75) are the only probes of this entry
    whose arms are not integers, and therefore the only ones that can
    separate where the double becomes an int — every earlier probe used
    integer arms, where "floor at the sample" and "floor after the blend" are
    the same function. At pitch 16 anchor (0, 0), out of 36864 columns: the
    reading above 36864; flooring only after the blend 20423 / 12121;
    truncating at the sample 3119; truncating after the blend 4764; rounding
    after it 22010; quantising to the cell's LOWER corner 21039; quantising
    to the NEAREST of the four corners 21103. (Those last two are different
    functions and each carries its own count — the earlier version of this
    section called the lower-corner reading "nearest corner", which it is
    not.)

    `f_half` and `f_quart` are NOT two observations of this. Their arms land
    on the same side of every integer the server can return, so the two
    dimensions carry IDENTICAL server data — measured: the readbacks agree on
    all 36864 columns, 33745 of them at psl = -1 and 3119 at psl = 0 in both.
    What `f_quart` separates is the REJECTED models from one another, which
    is the one column of that table where the two rows differ. The floor
    placement itself is therefore measured at seed 42, again at seed 31337
    (`psllat2`'s own `f_half`: 36864 of 36864, against 20164 / 2488 / 5409 /
    21465 / 20664 / 20920 for the six refusals) and again below zero.
  * **The cell index is floorDiv.** `floorDiv` and a truncating division are
    the same function while x, z >= 0, so every probe this project had ever
    run on this entry was blind to it. `probes/psllat3` FORCELOADS the subset
    at chunk -12 (blocks -192..-65) and is read back over x, z in [-256, -1],
    all 65536 columns of r.-1.-1 — the server generates a border of chunks
    around every forceloaded square, and here that border shares the region
    file. floorDiv keeps 65536 of 65536 columns; truncation drops to 4216.

    The same correction applies to the other two specs, whose extent this
    section previously gave as the forceloaded square: `psllat` and `psllat2`
    are forceloaded at chunk 0 and read back over x, z in [0, 191] — 36864
    columns, not the 128x128 of the square — the negative half of their
    border falling in r.-1.-1, which is not copied. All three extents are
    measured from the marked columns themselves in
    `vanilla_psl_lattice_test.cpp`, "every dimension of the sweep, scored
    twice and with its extent measured".
  * **And the whole lattice again, MODEL-FREE.** The strongest form of the
    claim available needs none of the above: predict every interior column
    from the SERVER's own recovered psl at the four multiples of 16 around
    it. No probe-noise replica, no anchor (the corners are taken where the
    pitch says they are), and no calibration of the first floor, since a
    recovered corner value is already an integer. It scores 30976 of 30976 on
    each of the 24 `psllat` dimensions, 30976 of 30976 on each of the 9
    `psllat2`, 57600 of 57600 on each of the 10 `psllat3` and 30976 of 30976
    on each of the 6 `apsb4` — 1784064 interior columns, all exact.

  It is NOT the cell lattice, and that now has its denominator: `probes/apsb4`
  varies cell width 4/8/16 and cell height WITH the varying field in place and
  gets 36864 of 36864 identical columns each time. (`probes/apsb3` varied the
  same knobs under a CONSTANT psl, where a blend is a no-op and nothing could
  have shown — which is why apsb4 exists.) Wrapping the entry in `flat_cache`,
  which relocates its argument to the 4x4 column corner, also changes nothing,
  consistent with a lattice whose own samples already sit at multiples of 16.
  Nor is the pitch a property of the driving field: `xz_scale` 0.5, 1, 2 and 8
  all reproduce every column.

  **And it is corroborated on real terrain in BOTH directions.**
  `above_preliminary_surface` gates the overworld's surface-materials subtree
  and occurs exactly three times in the whole pinned tree — `overworld.json`,
  `amplified.json`, `large_biomes.json`, once each, all of them that same gate
  (asserted, not assumed, in `vanilla_psl_lattice_test.cpp`'s case
  "above_preliminary_surface occurs exactly three times in the pinned tree").
  So a block only that subtree can place is a block only that condition can
  gate. Neither direction below is decisive alone, which is why both are run
  — and this is not the only test on real terrain that could have said no,
  since `vanilla_above_preliminary_surface_test.cpp` runs the same two
  directions under the per-column reading.

  * **Forward — blocks the reading cannot place.** Over EVERY `grass_block`
    the server wrote in the eight golden regions — 627766 of them, not the
    subset below the per-column psl, so a lattice psl sitting HIGHER than the
    per-column one cannot hide a counter-example — **not one** falls below
    the band the lattice opens. For contrast, 14008 of those fall below the
    per-column boundary, and the per-column band leaves 2429 of them
    unexplained, spread very unevenly across the seeds (1398, 404, 285, 259,
    56, 26, 1, 0). Under the lattice the residual is 0 on every one of the
    eight seeds, not 0 on average. What makes one seed's share large is still
    untested — the steepness hypothesis is neither confirmed nor needed now
    that none of the residual is left.
  * **Reverse — the band the reading OPENS.** Counting only what the old
    reading cannot explain rewards a boundary for reaching further down: a
    boundary at the world floor would score perfectly forward. So the band
    `[psl + depth - 8, psl)` is also read at the column's own SURFACE, where
    the materials subtree is what decides the block. Under the lattice, 12916
    column surfaces fall in the band and 12403 carry a block only the gated
    subtree places — **96.03%** — against 10676 of 11953, **89.32%**, for the
    per-column band, both recomputed in the same pass. The 513 that are not
    gated-only are 503 `stone` (which the subtree itself places, so the block
    is silent either way), 9 `granite` and 1 `copper_ore`, both written by
    features after the surface pass. Absence of a material is never counted
    as a refutation here — the subtree is a `sequence` whose inner rules may
    decline at a position its gate opened — so this direction is a bound, and
    says so.

  **The shipped engine therefore reads this one router entry TWO different
  ways, deliberately.** `terrain::ChunkFiller` puts
  `preliminary_surface_level` through the 16-block lattice above for the
  SURFACE RULE, and reads it PER COLUMN, at `(x, 0, z)`, for the AQUIFER's
  four surface consumers. The lattice half is measured, through
  `above_preliminary_surface`, over everything in this section. **The aquifer
  half is UNMEASURED** — nothing in this sweep touches it, and the aquifer's
  own probes scoring well under a per-column read is not evidence either way,
  because a three-valued field on a 16-lattice and the same field per column
  agree on most columns. Whichever reading is wrong there is a parity bug
  waiting to be found; PROGRESS.md's M4 entry carries the probe that would
  settle it.

  Three things this does NOT say. It does not say the pitch is 16 for any
  consumer other than this condition — the AQUIFER reads the same router entry
  and its own reading is not measured here (see the paragraph above, and
  PROGRESS.md's M4 entry). Every field in the sweep is y-independent, so this
  is the HORIZONTAL sampling only; that the entry is read at absolute y = 0
  was settled separately and is unchanged. And it does not separate `a + (b - a) * t` from other
  algebraically equal spellings of a linear blend: it is exact on every column
  it was scored over, which makes those spellings indistinguishable here
  rather than decided. The population is the one the conformance file itself
  scores and no larger — **1871872** columns, every one of the 43 dimensions
  of `psllat`, `psllat2` and `psllat3`, against the probe-noise replica, plus
  the 1784064 interior columns of the model-free reading above. (An earlier
  version of this section quoted 1613824 across 36 dimensions, a figure that
  came from analyser runs and was pinned by no test. Numbers here are the
  ones the tests hold.)

  The counts the earlier, open version of this section quoted still stand and
  are still asserted: driven by the three-valued `range_choice` the psl
  recovered from the band takes 101 distinct integer values, -40 to 60 with no
  gaps, while the BOUNDARY takes 104, -47 to 56, since the column's own
  surface depth (0..6 there) widens it. Both are over `probes/apsb/v_psl`'s
  36864 columns and they are different quantities.

  And the 101 is now PREDICTED rather than only reproduced. Enumerate the
  model's reachable values
  — every assignment of the three arms to the four lattice samples, at every
  offset inside a cell, with no fixture involved — and pitch 16 gives exactly
  101 values, -40 to 60, contiguous. Read as a bound rather than as a fit: the
  contiguity rules out the pitches too coarse to reach every integer (4 gives
  57 values with 44 gaps, 2 gives 15 with 86) and says nothing against 8 or
  32, which also predict 101. Those two are refused by the translation test,
  not by this count — which is exactly why the pitch was measured by
  translation instead. Asserted in `tests/unit/terrain_filler_test.cpp`.

  Measured by `tools/analysis/psl-lattice-probe.sh` (43 probe dimensions
  across three specs, two seeds and both signs of the world origin), read back
  by `psl-lattice-analyze.cpp`, scored in
  `tests/conformance/vanilla_psl_lattice_test.cpp` — twelve cases, every
  number in this section among them.


  SETTLED, and it was the last thing open about the boundary's own FORMULA —
  the sampling question above is about `preliminary_surface_level` and stays
  open. The depth carries **no bottom clamp AT 0**: `max(0, surfaceDepth)` is
  REFUTED, and the boundary is `y >= floor(psl) + surfaceDepth - 8` with the
  depth as returned, negative values included.

  **Narrowed deliberately to "at 0", because that is all 49 columns can
  carry.** Every separating column is at depth exactly **-1** — the probe
  dimensions' own depth histograms show `-1` and nothing below it — and the
  lowest raw value anywhere in the 8589934592-column sweep is
  **-1.134416806**, which truncates to -1 as well. So no column at depth <=
  -2 has ever been observed, and a clamp at -1 or lower (`max(-1, depth)`, or
  a clamp at the world floor) predicts exactly the same lower edge as no
  clamp on every column in this reading. Such a clamp is NOT separated here
  and nothing below is evidence against it; what is refuted is the specific
  candidate this project held, `max(0, depth)`. Stratum applies no clamp
  because none is documented, not because a lower one was ruled out.

  The two candidates differ only where the RETURNED depth is negative, which
  needs a raw value at or below -1, since the cast truncates toward zero. The
  history of this question is two false universal claims, both from samples
  that could not carry them: first "the depth reaches -1 on about one column
  in 22000" (far too common), then "vanilla's amplitudes do not produce one at
  all" (false), the latter resting on a census of every column of the eight
  golden `r.0.0` regions — 2097152 — where the returned depth's minimum is 0,
  6745 columns sit at exactly 0, and the raw value goes below zero on 207
  columns (all at seed 0), bottoming out at -0.449658. That census is correct
  and reproduces exactly. It is also about 40x too small to expect a single
  hit, so it was never evidence either way.

  **The search.** `aps-boundary-analyze sweep` over the same eight golden
  seeds and x, z in [-16384, 16384) — 1073741824 columns each, **8589934592
  in all** — finds **98** columns at depth -1, on **six of the eight seeds**,
  in **ten distinct regions**: 1 in 87652393. Per seed: 0 (none), 1 (28),
  -1 (none), 42 (27), -4172144997902289642 (16), 2891948927356891 (1),
  9223372036854775807 (3), -9223372036854775808 (23). The raw tail falls
  smoothly through -1 rather than stopping there — in 0.01-wide bands from
  [-0.81, -0.80) downwards: 68, 62, 55, 50, 46, 50, 62, 41, 41, 33, 33, 34,
  16, 22, 26, 19, 18, 17, 13, **19** at [-1.00, -0.99), 13, 11, 12, 9, 8, 9,
  9, 1, 10, and 16 for everything below -1.09 (the lowest is -1.134416806).
  The bands strictly below -1.00 sum to 98, which is the negative-depth count
  arrived at independently — so the columns that cross are the continuation of
  the distribution, not an artefact of its edge. The narrower [-4096, 4096)
  sweep of the previous revision is a sub-window of this one and still
  reproduces exactly: four columns, all at seed -4172144997902289642, raw
  -1.033659749 / -1.048435300 / -1.008497344 / -1.010109149.

  Those 98 are NOT 98 independent draws, and the rate should not be read as
  though they were. The field is smooth, so a column that crosses has
  neighbours that nearly do: the 98 fall into **ten** spatially compact
  clusters, one per region — 23, 22, 18, 12, 7, 5, 4, 3, 3, 1. 1 in 87652393
  is the right figure for "how much area must be swept to find one", which is
  what it is used for here; the number of independent excursions behind it is
  ten, which is what bounds how well the rate itself is pinned. Saying 98
  where ten is meant would be the same error, one level up, as the two the
  history above records.

  **The reading that does NOT settle it, and why.** Generating region
  `r.4.3.mca` at seed -4172144997902289642 and reading the one block the
  candidates disagree about — `y = psl - 9` — returns `minecraft:stone` at all
  four columns, and that is *not* evidence for the clamp. All four are
  `warm_ocean` with the ocean floor at y = 36 and psl = 24, so `y = 15` is 21
  blocks deep in stone, and at `surfaceDepth == -1` every arm of the gated
  surface-materials subtree declines there whatever the gate does: arms 0, 2,
  4 and 3.0 need a solid-run depth of 0; arm 3.1 — the whole
  grass/dirt/gravel/mud family — is gated by `stone_depth(floor, offset 0,
  add_surface_depth true)` whose threshold is `0 + surfaceDepth = -1`, and a
  run depth is never negative, so that arm is OFF everywhere in such a column;
  arms 3.2 and 3.3 reach 5 and 29 blocks deep but only in
  warm_ocean/beach/snowy_beach and desert, and 21 > 5; arm 1 is badlands only;
  and `NOT(hole)` is false because `hole` is exactly `depth <= 0`. Both
  candidates predict the terrain filler's stone, and the server placed stone.
  A confirmed non-result, run rather than assumed
  (`aps-boundary-analyze window`).

  It is doubly a non-result, and the second reason is why the separation was
  done where it was: `y = psl - 9` and `y = psl - 8` are located here using
  **Stratum's own per-column `preliminary_surface_level`** — the psl of 24 is
  this engine's value, not one the server reported. How the server SAMPLES
  `preliminary_surface_level` is still open (see the sampling note above: a
  fixed horizontal lattice, interpolated, rather than read per column), so a
  wrong psl would move both candidate edges together and put the block read at
  the wrong y without saying so. That is circular in precisely the way a
  separation must not be. Hence this reading is pinned as a non-result, and
  the reading that settles the clamp is done in probe dimensions where
  `preliminary_surface_level` is a datapack CONSTANT: there the y being read
  is fixed by the datapack, the sampling question cannot reach it, and the
  edge tracking 100 / 40 / 0 / -20 is itself the check that it did not.

  **The reading that does settle it.** `tools/analysis/aps-clamp-probe.sh`
  takes the gated subtree out of the question. Each of its dimensions is solid
  from floor to roof, pins `preliminary_surface_level` to a constant, and
  carries the single surface rule
  `{ above_preliminary_surface -> diamond_block }`, so a column's LOWEST
  marker is the condition's own boundary at single-block resolution with
  nothing — no `hole`, no `stone_depth`, no biome, no materials tree — between
  the condition and the readout. The surface-depth field is a function of the
  world seed and (x, z) alone, so the negative-depth columns are at the same
  coordinates in a probe dimension as in the overworld; `density-probe.sh`
  gained `--origin-chunk` so the probe can be forceloaded where they are
  instead of at the world origin.

  Three cases, three different seeds, three different regions, five pinned
  `psl` values each (100, 100 in a second dimension, 40, 0 and -20):

  | case | seed                   | region    | separating columns |
  |------|------------------------|-----------|--------------------|
  | s1   | -4172144997902289642   | r.4.3     | 4                  |
  | s2   | 42                     | r.6.10    | 22                 |
  | s3   | -9223372036854775808   | r.13.26   | 23                 |

  Those three worlds carry **four distinct dimension configurations plus one
  deliberate byte-identical repeat** each (`k_p100_b` repeats `k_p100`, so
  "the reading is stable" is not "one dimension did something"): twelve
  distinct (seed, psl) pairs, three repeats, **15 generated dimensions** in
  all.

  Scored in `tests/conformance/vanilla_above_preliminary_surface_test.cpp`
  ("the surface depth carries no bottom clamp"). The numbers are in
  PROGRESS.md's M4 entry; the shape of them is that `psl + surfaceDepth - 8`
  is right on every painted column of every dimension, `psl + max(0, depth)
  - 8` is wrong on exactly the separating ones, and the measured lower edge
  tracks the pinned psl across +100 / +40 / 0 / -20 — the negative case ruling
  out sign handling in the boundary rather than a clamp.

  **Why 49 correlated columns are enough.** They are not 49 independent
  draws — they are three spatially compact clusters of a smooth field — and
  it does not matter, because the refutation is arithmetic rather than
  statistical. `psl + max(0, d) - 8 >= psl - 8` for ANY depth field `d`
  whatsoever, whatever its distribution and however its columns correlate,
  since `max(0, d) >= 0`. The clamped candidate predicts a lower edge at or
  above `psl - 8` everywhere, with no exceptions and no tail, so a SINGLE
  correct reading of an edge at `psl - 9` contradicts it outright. There is
  no sample-size question to answer and no independence caveat owed.

  What the three seeds and five psl values guard is the other failure mode —
  that the counter-example is not real. A misread region, a probe whose psl
  did not take, a fixed offset between what the server wrote and what the
  scorer reads, a coincidence at one seed: each would show at one seed or one
  psl and not at four others (a negative one included) across three
  independently generated worlds. Guards on the READING, not statistical
  power. The rate `1 in 87652393` two paragraphs up IS a statistical quantity
  and is stated as ten excursions rather than 98 columns for exactly that
  reason; the two statements are different in kind and this spec says which
  is which rather than hedging both the same way.

  What this does NOT say, stated so it cannot be read as more than it is:

  * it settles the CONDITION's boundary, not what a vanilla overworld visibly
    does at such a column. The one cluster read in a real overworld shows no
    block difference either way, for the structural reason above, and the
    other two clusters were not generated as overworlds at all. "The clamp is
    unobservable in vanilla's own block output" is a different claim and is
    not made here — it would need the gated subtree to be reachable at
    `y = psl - 9`, which needs a depth -1 column whose solid run TOP is at
    that y (or a desert/beach column within the 29/5-block reach of arms 3.3
    and 3.2), and no such column has been looked for.
  * the rate is pinned by ten independent excursions, not 98 columns, so
    1 in 87652393 carries roughly a third of its own size in statistical
    uncertainty. It is used only to say that a 2097152-column census finding
    none was uninformative, which holds at any plausible value.
  * the window is x, z in [-16384, 16384) on the eight golden seeds. No
    other seed and no further-out window was swept.

  It is a THREE-way separation, which is the one thing these 49 columns give
  that nothing else could. Every separating raw depth lies in (-1.14, -1.00),
  so the three candidate conversions predict three different lower edges:
  `floor` gives -2 and `psl - 10`, `(int)` gives -1 and `psl - 9`, a bottom
  clamp gives 0 and `psl - 8`. The measured edge is `psl - 9` on all 49, at
  every one of the five pinned psl values.

  `floor` is wrong on two disjoint sets and on nothing else: the separating
  columns (raw at or below -1) and the columns whose raw is negative but above
  -1 (where `(int)` is 0 and `floor` is -1). Everywhere raw is >= 0 the two
  are literally the same function. Measured per window at psl 100 / 100 / -20:
  65536 - 4 - 440 = 65092, 65536 - 22 - 436 = 65078, 49152 - 23 - 508 =
  48621. The separating set is what is new here — without a raw that reaches
  -1, `floor` and `(int)` cannot be told apart at all, which is why neither
  the 52 apsb dimensions nor the 2097152 golden columns separate them.

  **That floor row is an identity, not a second refutation**, and is labelled
  as one in the conformance case too. Once `unclamped right == painted` holds,
  every painted column's edge IS `psl + depth - 8`, so `floor` differs from
  the reading exactly where `floor(raw) != (int)raw` — the two sets above —
  and `floored right == painted - separating - nearZeroNegative` follows BY
  DEFINITION. It cannot come out any other way and it confirms nothing on its
  own. It is kept as a CHECK because it would catch a bug in the scorer (a
  `flooredFits` reading the wrong column, a `nearZeroNegative` predicate off
  by a boundary), which is a real thing to guard. What actually refutes
  `floor` is the unclamped row plus the separating raws lying in
  (-1.14, -1.00).

- **Surface rules load whole, and refuse by name (M4).**
  `stratum::surface::RuleGraph` resolves the tree for every one of vanilla's
  seven dimensions: the overworld's is 287 rules over 141 conditions naming 7
  noises, an order of magnitude larger than anything else here. All fifteen
  types are read; anything the schema does not define is an error naming the
  type, never a skip.

  Resolving is separated from RUNNING on purpose, and that split is what let
  RUNNING catch up construct by construct without either side waiting on the
  other. `unrunnableReason` reports what it cannot execute and why — and, as
  of `bandlands` closing, reports nothing at all: every one of the eleven
  condition types and all four rule types run. The reasons recorded along the
  way, while they still applied, were the measurements above rather than
  "unimplemented": `vertical_gradient` said its probability was settled and
  its random source was not, `hole` said it fired on 0.04% of terrain and its
  comparison was undocumented, `steep` said its predicate needed neighbouring
  columns the filler could not yet reach, and `bandlands` said its colour
  table's construction was not derived — until the clean-room provision
  derived it (below).

  `minecraft:end`, whose whole surface rule is one `block`, was the first
  dimension this build could decorate the moment there was an executor at
  all — no condition, so nothing to settle. The Nether's tree was the first
  REAL one: 41 conditions, none of them `bandlands`, all eleven types
  runnable, and it compiled whole before any of the other five did. All seven
  now compile whole; `vanilla_surface_rules_test.cpp` asserts it per
  dimension rather than once, so a future regression names which one broke.

  **The schema is generated, and the debt that said otherwise is closed
  (M4).** Ten of the fifteen come from `material_rule.mcdoc` and
  `material_condition.mcdoc` and are emitted into `lib/src/surface_schema.inc`
  and the two enumerator files beside it by `tools/mcdoc-sync`, exactly as the
  density-function tables are. The remaining five — `bandlands`,
  `above_preliminary_surface`, `hole`, `steep`, `temperature` — are absent
  from mcdoc altogether and are hand-written permanently, the precedent being
  `tools/mcdoc/schema.py`'s own `blend_alpha` and `end_islands`; each takes no
  fields, so a name in a list is the whole of what is written by hand about
  them.

  Three things were in the way, and the first two were not what the note
  standing here predicted. `use` lines were already skipped — the reader
  stopped at line 12, not line 1. What actually stopped it was the
  computed-dispatch spread `...minecraft:material_condition[[type]]`, a shape
  `density_function.mcdoc` also uses but only ever inside an unparsed
  type-alias text blob, so the spread reader had never met it; it now keeps
  such a target as text, since no single struct is named by one. The third
  file, `mod.mcdoc`, needed generic parameters — `type UniformInt<Base,
  Spread>`, `dispatch minecraft:int_provider[constant]<T>` — which nothing had
  needed before. A generic alias is recorded apart from the plain ones so that
  a lookup by bare name misses rather than expanding a body that mentions its
  own parameters. The three files are then merged into one namespace, because
  mcdoc refers across them by bare name (`CaveSurface` and `VerticalAnchor`
  are declared in `mod.mcdoc`) and this reader resolves by name rather than by
  import provenance; a name declared twice is refused rather than overwritten.

  Two things the generated table now decides that used to be written out, both
  of which §11's own rule about *expanding* rather than name-matching an alias
  demanded: `surface_type`'s `floor`/`ceiling` comes from `CaveSurface`, and a
  vertical anchor's three spellings are read out of mcdoc's union of one-field
  structs — so 26.3's `relative_to_sea_level` will arrive as a table entry,
  and the loader refuses it by name today rather than mis-reading it.
  `MaterialRuleRef`/`MaterialConditionRef` are expanded for the same reason:
  their identifier spelling arrives at 26.3, and `allowsReference` follows the
  union instead of a hard-coded answer.

  The refactor was verified to be behaviour-preserving rather than assumed to
  be: a dump of all seven dimensions' resolved trees — every node index, every
  member of every node, the unrunnable list, the referenced noises, 2285
  lines — is byte-for-byte identical before and after. One deliberate
  narrowing survives and is marked as such in the loader: mcdoc declares
  `biome_is` as a plain `[string]`, which permits an empty list, and this
  build has refused one since before the table existed. Widening it is a
  change to what the loader accepts and was not part of deriving the schema.

  Two classes of silent failure the generated table closes, neither of which
  the old switch could have caught. A field mcdoc RENAMES stops binding and
  is refused by name. A field mcdoc ADDS — `is_3d` on `noise_threshold` at
  26.2, the whole of `ore_vein` at 26.3 — is likewise refused rather than
  dropped unread, which is what the hand-written switch would have done with
  it. `tests/unit/surface_schema_test.cpp` builds its input FROM the schema
  precisely so that this fails on the day the table grows the field, not on
  the day somebody notices a wrong world.

- **`surface::Executor` is wired into `ChunkFiller` (M4).** Every unit and
  conformance test until now ran the executor directly, against a
  hand-supplied `Context` — the filler never called it, and `fill()` produced
  bare stone, fluid and air no matter what a dimension's surface rules said.
  `ChunkFiller::compile` now takes an optional resolved `RuleGraph`, an
  optional `biome::ParameterList`, and an optional `biome::TemperatureTable`,
  and when the tree runs whole, `fill()` makes a second pass over the chunk
  after its ordinary density pass and asks the executor what replaces what it
  just placed.

  The second pass is what supplies the `Context` fields the executor cannot
  derive on its own, each read back from the FIRST pass's own blocks rather
  than recomputed: `stoneDepthAbove`/`stoneDepthBelow` from a run counted top
  down and bottom up over the category (solid, fluid or air) the filler
  already decided, the latched water height from the same top-down scan, and
  `steep`'s four neighbour heights from a WORLD_SURFACE scan of the whole
  chunk, clamped to it by `fillSteepNeighbours` exactly as a per-column caller
  would have to. `biome` and `preliminary_surface_level` are resolved through
  the same noise router and `biome::ParameterList` the biome source itself
  uses (§ biome source), at the biome grid's own quarter resolution rather
  than once a block — and `temperature`, once a tree names it, rides the SAME
  grid, since it has to know which biome a block sits in before it can look
  up that biome's own declared value.

  Three things are refused the same way an unrunnable construct is, rather
  than either crashing on a missing `Context` field or running silently
  wrong: a tree that reads `biome` (directly, or through `temperature`) with
  no `ParameterList` supplied; and a tree that reads `temperature` with no
  `TemperatureTable` supplied — vanilla's `temperature` needs a biome's own
  DECLARED value, and 0.0F is not an honest stand-in for one.
  `ChunkFiller::surfaceRulesBlockedBy()` reports both alongside whatever
  `RuleGraph::unrunnable()` itself found, so a caller sees one list, not
  several different reasons several different ways.

  **`biome::TemperatureTable` closes the DECLARED-value gap.** It reads
  every `worldgen/biome` entry a `data::Pack` already parsed — the SAME
  per-entry registry surface `biome::ParameterList` sits beside, not a
  separate walk of the directory — and maps each biome's identifier to its
  own `temperature` field, float32 throughout to match how
  `Executor::freezing` compares it. `ChunkFiller` resolves a biome's
  identity once (the multi-noise search, same as `biome` itself) and looks
  its declared temperature up in this table, cached at the same quarter-grid
  cadence as the identity itself.

  Consequence for the two real dimensions with fixtures. **The overworld's
  own tree now RUNS**, wired end to end for the first time
  (`golden_fill_test.cpp`, above) — `bandlands` closed the last unrunnable
  CONSTRUCT, and this closes the last missing INPUT. Running the real
  287-rule, 141-condition tree against real blocks for the first time caught
  a real bug immediately, closed in the same change: see `stone_depth`,
  below. The Nether's tree compiles and needs neither `biome` data nor a
  temperature table to run — but its `noise_settings` sets
  `legacy_random_source: true`, and its surface rule needs eight noises whose
  LCG seeding is unsettled (§11), so its *rules* cannot run. Its density
  chain can: that refusal was later narrowed, the Nether's `final_density`
  names no noise at all, and its terrain is now measured against the goldens
  at 99.99591% (§11). What is missing is which solid a position holds, not
  whether it is solid. Closing the
  temperature gap did not, and could not, touch that one; it closes the
  distance between "the executor can run this tree" and "the filler asked
  it to and had everything the tree needed."

  **`stone_depth` was false everywhere off the top of a solid run — a real
  bug, found by this end-to-end run and closed in the same change.** The
  comparison read the stored run counter minus one against a threshold; a
  counter of 0 (Context's own doc: reset by air, meaning "not in a run at
  all") became `0 - 1 = -1`, which trivially satisfies almost any
  non-negative offset. Before the fix, every grass/dirt/sand composition
  rule the overworld's tree contains fired at EVERY position above the real
  terrain rather than only its top — grass painted the entire sky, deepslate
  painted over open water — 292005 of 393216 blocks in four chunks, caught
  directly by `golden_fill_test.cpp`'s category assertion the moment the
  temperature gap stopped hiding it. `Executor::test`'s `StoneDepth` case now
  refuses outright when the run counter is 0, rather than computing a depth
  that was never a real position in any run. No existing test exercised a
  0-valued counter before this — every isolated `Context` either defaulted
  to 1 or set one explicitly — so this was invisible until the whole tree ran
  against real terrain, the same way `bandlands`' pass(a) off-by-one only
  showed up once more than one seed was probed.

  **A second, narrower gap the same run surfaced — RESOLVED.** `deepslate`'s
  own rule — the overworld tree's third top-level sequence entry — is a bare
  `vertical_gradient` with nothing else gating it: `false_at_and_above: 8`,
  `true_at_and_below: 0`, no `stone_depth`, no category check. Measured in
  `golden_fill_test.cpp`: it won over real FLUID on 475 of 393216 blocks, all
  between y -40 and y 0, all inside this aquifer-free probe's deep ocean
  trenches, where the real server left the water untouched.

  The natural read was that `above_preliminary_surface` (the entry above it)
  was the missing gate. It is not, and the same black-box measurement
  technique that closed `bandlands` is what showed that: a datapack whose
  ENTIRE `surface_rule` is that one bare `deepslate` rule — no
  `above_preliminary_surface`, no bedrock floor, nothing else in the tree at
  all — generated against the real server on the same seed still left
  exactly the same 475 fluid blocks untouched. Every one of them sits inside
  a fluid-filled cave-void (a normal density-carved cavity, flooded because
  this probe runs with `aquifers_enabled: false`) with solid rock already
  crossed above it in the same column, and the real 287-rule tree's own
  count agrees with the isolated one exactly: 475 fluid blocks at y <= 0 in
  both.

  So the gate is not in the rule tree at all — no combination of conditions
  can express it, because it isn't a property of any one rule. It is
  `ChunkFiller::applySurfaceRules`'s own second pass, which previously asked
  the executor about every position regardless of what the first pass had
  placed there.

  The first fix landed was narrower than the real rule, and a second golden
  (below) is what caught the gap. A FLUID-only guard —
  `Context::stoneDepthAbove == 0`, meaning no solid crossed yet on the way
  down, so a deeper pocket's fluid (inheriting a nonzero run carried over
  from the solid above it, since fluid neither breaks a stone-depth run nor
  counts toward it) stays untouched while the column's first/topmost fluid
  body stays reachable — closed this file's 475-block gap exactly (both
  granularities, no residual) and left the pre-existing "water reads the
  filler's own latched height" unit test passing, since it exercises the
  topmost/open-water case a `water`-keyed rule (freezing ice onto a lake's
  own surface) needs. But `golden_fill_aquifer_test.cpp` — measuring the
  same rule against a REAL aquifer's own air/fluid transitions rather than a
  flat, aquifer-free sea level — still read 461 short after that fix, and
  451 of those 461 were AIR, not fluid: `stoneDepthAbove` resets on air BY
  DESIGN (`Context`'s own doc), which is exactly what makes a deep cave's
  own air indistinguishable from the column's open sky under a
  fluid-shaped guard.

  The general rule both goldens now confirm: a NON-SOLID position (fluid or
  air alike) stays eligible for surface rules only while it is part of the
  column's FIRST (topmost, reached straight from the sky) non-solid
  stretch. Once solid has been crossed anywhere above a position — a plain
  monotonic flag, deliberately NOT `stoneDepthAbove`, which resets on air on
  purpose — every non-solid position below that stays out of reach for the
  rest of the column, however many more solid runs and non-solid gaps
  follow, and real vanilla never rewrites any of them. Both
  `golden_fill_test.cpp` (393216 of 393216) and `golden_fill_aquifer_test.cpp`
  (393216 of 393216) read exact with this version — the aquifer-free probe
  has no air below sea level to have told the two guards apart on its own.
  "an unconditioned rule never rewrites a buried fluid pocket's own fluid"
  and its sibling "...a buried air pocket either" (`terrain_filler_test.cpp`)
  are the isolated regressions: a hand-built buried notch, solid on both
  sides, converted by an unconditioned `block` rule everywhere except the
  notch itself — once with the notch below sea level (fluid), once above it
  (air).

  Correctness, found while wiring rather than assumed: `y_above` and `water`
  both read `condition.addSurfaceDepth` for their `add_stone_depth` field —
  the WRONG one; `StoneDepth`'s own `add_surface_depth` field happens to share
  a type and a default with it, and nothing before this caught the two being
  swapped. Every `add_stone_depth: true` was silently treated as false. This
  table's own `y_above` row already documented the right field name; the code
  read the other one. Vanilla's overworld and Nether trees together set it on
  ten `y_above`/`water` conditions — seven of them in the Nether tree that
  just started compiling whole, so this would have been the first thing to
  get a real dimension's blocks quietly wrong the moment that tree could run
  end to end. Fixed by reading `addStoneDepth`; every existing test that
  exercised either field had it set to `false` in both spellings, so nothing
  masked the bug and nothing regressed fixing it.

- **A write path exists now, deliberately outside every milestone this
  document tracks.** `nbt::write` (the exact inverse of `nbt::read`,
  round-trip tested against a real chunk's own bytes), `chunk::encode` (the
  inverse of `chunk::decode`, taking a caller-built `ChunkData` — the same
  `Section`/`BlockState` shapes `decode` already produces), and
  `region::writeRegion` (the inverse of `RegionFile::open`/`readChunk`)
  together let this build's own terrain be written as `region/*.mca` files a
  real Java Edition 1.21.11 server loads and serves without regenerating —
  proven by injecting generated regions into a real server's world folder
  and force-loading them, not merely by round-tripping through this build's
  own reader. Deliberately scoped narrow: every chunk is written `Status:
  "minecraft:full"` with `isLightOn: 0` and no `SkyLight`/`BlockLight`
  anywhere — the server relights on load rather than this reproducing
  vanilla's sparse per-section light storage, which nothing here has
  measured closely enough to claim (this document's own standing rule
  against guessing silently). There is no `level.dat` writer; a caller
  supplies an existing world folder's `region/` directory rather than this
  producing a save on its own. `tools/analysis/generate-world.cpp` is the
  one caller today, and is deliberately not a `stratum` subcommand — a
  one-off deliverable script, not a feature this build claims end to end.
  A real bug surfaced building it and is now regression-tested
  (`chunk_writer_test.cpp`): `encodePalettedContainer`'s palette list
  hard-coded a Compound element type for every paletted container, correct
  for block-state palettes but wrong for biome palettes (bare strings) —
  invisible through `Chunk::decode()` alone, since it reads a list's
  elements directly and never checks its declared type, and only showing up
  once real bytes reached a real server's own NBT reader as `EOFException`/
  `unknown tag type 20` a few kilobytes downstream of the first bad list.
  Two performance fixes came out of the same effort, applied to
  `ChunkFiller::applySurfaceRules` itself rather than the one-off tool,
  since they are real for any caller that supplies `biome`/`temperature`
  data — its per-column climate-router reads had no `CornerCache` at all
  (measured 87 times slower per the `interpolated` node's own doc) and its
  biome search repeated the same query, uncached, for every one of up to 16
  columns sharing a quart-cell against the real overworld's 7593-row table;
  together these took one real chunk's second pass from about 8 seconds to
  about 0.5. Bit-for-bit identical before and after — `golden_fill_test.cpp`
  still reads 392741 — a cache only changes speed, never a value.

- **The other four undocumented surface constructs, measured (M4).** In
  vanilla's data these all sit under a `biome` condition, so no probe of
  vanilla's own overworld can reach them. Written directly into a probe's own
  rule tree they are reachable, and four of the five gave something up.

  * **`temperature` is SETTLED, and it compares a height-adjusted value.** It
    was recorded here for two milestones as a flat strict threshold at 0.30 on
    the biome's own temperature, on a sweep that varied temperature and never
    varied height. Sweeping height refuted it, and the rule is:

    ```
    origin = sea_level + 17
    t      = biomeTemperature                       // float32 throughout
    if (y > origin)
        t -= (8*simplex(x/8, z/8) + (float)y - (float)origin) * 0.05f / 40.0f
    fires  iff  t < 0.15f
    ```

    Three details are load-bearing and each was measured rather than assumed.
    Every step after the noise is FLOAT32 and evaluated left to right; widening
    to double disagrees with the server. `* 0.05f / 40.0f` is NOT the
    algebraically equal `* 0.00125f` — the grouping is what is pinned, and the
    folded constant is refuted. And the noise is a 2D simplex over a
    permutation from a Java LCG seeded with the CONSTANT 1234: no world seed,
    no salt, so this field is identical in every world.

    The origin follows `sea_level`, one for one, and moves with neither `min_y`
    nor the world top — checked at five sea levels. The slope is eight blocks
    per 0.01 of biome temperature; at any single column the integer boundary
    lands eight or nine apart depending where the noise puts the fraction,
    which is why the earlier "one position of 98304 at exactly 0.30 came out
    true" was the whole story rather than an anomaly.

  * **`bandlands` — RESOLVED. A 192-entry table, its construction and its
    read path both bit-exact.** `tools/analysis/bandlands-probe.sh` puts
    `bandlands` alone at the root of a probe's own tree, over a column made
    fully solid by a constant `raw_final_density`, and reads every block
    back — the same technique `temperature`, `hole`, `steep` and
    `above_preliminary_surface` were each measured with, since vanilla's own
    tree gates `bandlands` behind a `biome` condition no probe of the real
    overworld can reach around. `bandlands-dump.cpp` turns a run into a
    colour histogram, a per-column offset map and PNG slices;
    `bandlands-offset-check.cpp` and `bandlands-e2e-check.cpp` sample the
    registered noise and the compiled `Executor` directly for comparison.

    **The read path**, confirmed first and independently of the table's own
    construction:

    ```
    index(x, y, z) = (y + round(4 * clay_bands_offset(x, 0, z)) + 192) % 192
    block           = TABLE[index]
    ```

    `clay_bands_offset` is the pack's own registered noise
    (`worldgen/noise/clay_bands_offset.json`, one octave, firstOctave -8),
    built through the ordinary modern noise-registry path and sampled at
    `y = 0` always. `round` is round-half-up (`floor(v + 0.5)`), not
    truncation: measured colour transitions land exactly on the raw noise
    value's own ±0.125, ±0.375, ... boundaries — a quarter-step centred at
    zero — which only round-half-up produces. The `%` is Java's own,
    truncating, sign-follows-dividend operator — `javamath::floorMod` would
    be the reflex here and would be WRONG: for a sufficiently negative `y` or
    noise-derived offset, `y + offset + 192` is itself negative, and Java's
    `%` on a negative dividend returns a negative array index. This is not
    hypothetical: reproduced directly, `Executor::bandlandsAt` throws at
    `y = -2032` for every seed tried, matching vanilla's own reachable
    `ArrayIndexOutOfBoundsException` there (unreachable inside the
    overworld's own -64..319 height, reachable in a taller or deeper custom
    dimension). Checked against three world seeds and 532224 real blocks
    read directly off probe regions (`bandlands-e2e-check.cpp`), zero
    exceptions, zero remaining free parameters.

    **The table's construction** came from a clean-room derivation
    (`spec/bandlands-spec.md`, run 03) once black-box measurement alone
    stalled the same way `old_blended_noise`'s fold once did (§12). One
    continuously-advancing random source — the world seed's positional
    factory, forked once and salted with the MD5 of `minecraft:clay_bands`
    — is threaded through five passes in a fixed order, each free to
    overwrite what an earlier one placed: a sparse scatter of orange
    (step size `draw(0,4)+2`, split across two additions rather than one —
    see below); three "band" passes sharing one procedure, called with
    `(width 1, yellow)`, `(width 2, brown)`, `(width 1, red)` in that order,
    each drawing 6-15 runs of a `base+draw(0,2)`-wide, randomly-placed,
    truncated-not-wrapped stripe; and a final sparse scatter of white with an
    independent chance of light grey on each in-bounds neighbour — whose left
    guard is `index - 1 > 0`, strictly, not `>= 0`, an asymmetry that is
    vanilla's own and not a mistake to "fix". The table starts entirely
    plain terracotta.

    Implementing the spec's own prose caught two real, independently-found
    bugs, both settled by checking against real server output rather than by
    re-reading the prose harder:

    - The orange scatter's step is split across two `+1`s, not one `+2`
      before the write. `index0 = 0` reads literally — each iteration writes
      at `index + draw + 1`, THEN advances one further to the base the next
      iteration's loop condition and draw both see. Collapsing that into one
      `index += draw + 2` before writing looks equivalent and is not: it
      changes which value the loop condition sees at the boundary, so the
      pass stops one iteration early or late for some seeds. One of three
      probed seeds (42) agreed with the collapsed form by coincidence; the
      other two (-1, 12345) did not, which is what exposed it — checked
      end-to-end against 532224 real blocks per seed, not just the table's
      192 entries, since a coincidental match at 192 points is exactly the
      kind of thing worth over-verifying.
    - The final scatter's "fair coin" is bit 0 of the raw 64-bit draw, not
      this project's own `Xoroshiro128PlusPlus::nextBoolean()` (bit 63, the
      sign of the top 32 bits). Bit 0 predicted all 27 reachable coin flips
      across two probed seeds' tables exactly; bit 63 disagreed on more than
      a third of them. Nothing else in this codebase had exercised
      `nextBoolean()` against a server-verified vector before this — whether
      it needs revisiting for its other call sites is untouched by this
      finding and not investigated.

    **cubiomes (MIT) says nothing here** — it has no `terracotta` reference
    anywhere in it; it only ever places biome IDs, never blocks. **Cuberite
    (Apache-2.0)**, read before the clean-room provision as a structural
    hint, does implement a mesa/terracotta generator for an older version
    (`src/Generating/CompoGenBiomal.cpp`) whose SHAPE matches — a pattern
    array built once per seed, laid down in coloured layers separated by
    plain clay, offset by a low-frequency 2D noise — but not its numbers: a
    512-entry array against 1.21.11's 192, its own non-Mojang RNG, and a
    15-entry colour-weight list that does not fit 1.21.11's measured
    breakdown. That mismatch was the right call to make at the time: it is
    exactly why this was escalated to the clean-room provision rather than
    guessed from the structural hint alone.
  * **`steep` fires on 16.9% of columns** of a gently varying terrain — a
    workable signal, but deriving the predicate needs neighbouring columns'
    heights, which the filler's per-column API cannot currently reach.
  * **`hole` fires on 0.04%**, six columns in sixteen thousand. Too rare on
    ordinary terrain to derive from; it needs terrain built so that the
    surface depth is zero over a known area.
  * **`above_preliminary_surface` was recorded as true everywhere** on the
    probe's terrain, and that was WRONG about the probe's own region file.
    The analysis read the terrain's top block, where a condition of this
    shape is true by construction; the band the rule paints has a clean lower
    edge in all 36864 columns, 2 to 8 blocks below the level the condition is
    named after. Re-read from that edge, and with `preliminary_surface_level`
    swept instead of left at the probe harness's default constant, the same
    fixture settles the condition outright — see the surface-condition entry
    above and `tools/analysis/aps-boundary-probe.sh`. Recorded here as it
    happened: the measurement was available for two milestones and the
    readout was pointed at the wrong blocks.

  **The aquifer fill decision: what it is, and what it will cost (MA).**
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

  **The jitter, the gate and the barrier, measured together (MA).** Twenty-three
  probe dimensions at four seeds — 92 terrain-free worlds — were analysed by ten
  agents, each question attacked from two independent angles and then
  adversarially verified. The verification earned its place: it overturned
  twenty-one of the fifty-five claims put to it, including several of this
  project's own.

  *The jitter is one law on all three axes.* A cell's centre is
  `(16cx + jx, 12cy + jy, 16cz + jz)` with `j` drawn **per 3D cell**, and block
  assignment is nearest-centre. The width is an ABSOLUTE range rather than a
  fraction of each pitch — uniform `[0,12)` is out at 7 sigma, symmetric
  triangular by 18-60 log-likelihood, and pitch-proportional scaling predicts
  horizontal walls in residues 2-5 at 6.9% against 0.4% observed. **The law is
  a nine-valued INTEGER draw**, corrected below from the continuous reading
  first taken.

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
  **The ocean branch, settled — and it is not the shape recorded here twice
  before (M3).** Below `sea_level - 8` the decision changes. This SPEC recorded
  that change as ONE bonus `max(0, 1 - d/K)` added to the floodedness before
  the same two gates, first with `K` in 56.6-58.4 from five per-layer
  estimators and then with `K` in 55.32-55.56 from a per-cell fit. **Both are
  deleted, not amended. There is no single bonus, so no value of K can be
  right.**

  ```
  cy   = the cell's jittered centre y                    // never the cell index
  psl  = floor(preliminary_surface_level)                // FLOOR, see below
  L    = max(-54, min(40 * floorDiv(cy, 40) + 20 + spreadOffset(spread), psl))
  d    = psl - cy                                        // signed, may be negative

  if (psl < sea_level - 8) {                             // strict; sea-relative
      if (d < 4)                       return sea_level; // unconditional
      reach = max(0, 56 - d)
      if (f + reach * 11/640 > 0.8)    level = sea_level
      else if (f + reach * 3/160 > 0.4) level = L
      else                              level = -54
  } else {
      level = f > 0.8 ? sea_level : f > 0.4 ? L : -54
  }
  if (cy < -54 && level == sea_level)  level = L         // after `d < 4`, not before
  ```

  Both floodedness comparisons are strict; equality falls through.

  *The two gates carry different slopes, and that is what kills the old
  reading.* The refutation does not assume the bonus is linear. Any ONE bonus
  added before two fixed thresholds forces the gap between the two crossing
  floodednesses to be constant in depth. Measured, that gap runs

  | d | 8 | 24 | 48 | >= 56 |
  |---|---|---|---|---|
  | gap | 0.4750 | 0.4500 | 0.4125 | 0.4000 |

  Two verifiers found this independently, at different depths and on different
  seeds. Separately, a monotone single bonus would force
  `D_local(f) = D_sea(f + 0.4)`; two measured pairs are disjoint by up to three
  blocks. The slopes are 1/58.18 and 1/53.33 and their feasible intervals are
  disjoint by ~250 times their widths — within a single `psl`, not only pooled.

  *Where the constants come from.* About forty integer crossing depths were
  bracketed to a ten-thousandth of floodedness, in disjoint (depth, psl, seed)
  sets across four campaigns; every one lands on the two lines. Both lines
  extrapolate to the same zero-bonus depth, 55.900-56.000 and 55.951-56.000,
  and 56 is the only integer in either. Imposing 56 puts the slopes on
  `640/11` and `160/3` exactly and excludes the neighbouring candidates. Five
  of the crossing floodednesses are exactly representable in binary (0.25,
  0.078125, -0.5, -0.125, 0.4), so the strictness is settled with no
  floating-point ambiguity at all.

  *Why the old per-cell bracket was wrong, since it was this project's own.* It
  fitted one K to the sea gate only, over floodedness 0.60-0.79 only, at
  `psl` 0 only, through a readout that calls every cell centred below -54 dry
  whatever its level. Both verifiers reproduced that failure mode from the
  description and named it unprompted. Reading the LEVEL of each cell rather
  than whether its centre is wet — and refusing to guess when a barrier ends
  the fluid run ambiguously — moved agreement from 99.79% to 99.93% on its own.

  *Scale.* ~1370 probe dimensions, 12 world seeds, `psl` -54 to 141, sea levels
  32 to 200, spread 0/±0.3/±0.7/+0.9, barrier +1/-2, lava ±1. Four whole-model
  per-cell scores, never pooled across seeds: **99.9902%, 99.9806%, 99.9902%,
  99.9858%**. The same cells score 13-77% under the honest null (no ocean
  branch) and 86-97% under the K model this replaces. Of 48 of 63 `psl` values
  the score is exactly 100%.

  *Three details that a plausible reading gets wrong.*

  - **`psl` is floored, not truncated.** At -10.4 and -10.6 the server behaves
    as -11 in both cases while -10 behaves as -10. Truncation toward zero,
    round-half and carrying the raw double each predict a different one of the
    four observed outcomes, and all three are excluded. Measured twice
    independently (also at -20.5/-20.25).
  - **The `psl` cap is applied AFTER the spread offset.** At `psl` 67, a cell
    whose lattice point plus offset came to 69 was observed at 67.
  - **The deep-cell guard sits after the near-surface rule, not before it.** At
    `psl` -54, cells centred at -55, -56 and -57 with `d` = 1, 2, 3 DO take the
    sea level, at floodedness -2.0 and 0.95 alike. Putting the guard first
    writes lava where the server writes water in every ocean cell that is both
    below the lava sea and within three of the preliminary surface.

  *Floating point, and it is load-bearing.* `f + reach * slope` is exactly the
  shape a fused multiply-add contracts, and with contraction on the outcome
  flips at sea-gate depths 6 and 11 precisely at the crossing. §5's
  `-ffp-contract=off` / `/fp:precise` is required for this expression, and
  those two depths are the natural x86-64/ARM64 canary. The surviving
  equivalence class is bit-identical over 12.6M evaluations:
  `f + (56.0-d)*0.0171875`, `f + 1.1*((56.0-d)/64.0)`, `f + (1.1*(56.0-d))/64.0`
  and `f - (d-56.0)*0.0171875`. Spelling it as an amplitude over 56 —
  `1.05 * ((56.0 - d) / 56.0)` and relatives — is REFUTED at observed depths,
  from two independent sessions on different seeds. Parametrise by the slope.
  The integer restatements `11*d < 640*f + 104` and `3*d < 160*f + 104` are a
  reasoning oracle, not the predicate: they agree across the whole
  ten-thousandth grid except within about an ulp of a crossing, where they
  differ because the crossing floodedness is not itself representable. One
  rearrangement, `(56.0-d)*m > 0.8-f`, is still indistinguishable from the
  shipped form outside ~6 ulp of a crossing; it is measure-zero against real
  noise, but recorded so that a golden landing there is not read as a bug.

  **Refuse `psl <= min_y`.** At `psl` -64, the world floor, 65 of 130 cells
  disagree and the sea/ladder boundary sits at centre -43/-44 instead of where
  this rule puts it. `psl` -54 fits, and every value from -48 to 141 fits
  exactly; the band -63 to -55 is unexplored. This is a refusal, not an
  approximation (§8).

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

  **The jitter draw, recovered (MA).** Three rounds and about 1.9e9 refuted
  candidates in, the derivation fell out once two things changed: a ground
  truth refit under the integer constraint, and scoring by EXACT MATCH of a
  cell's draw rather than by correlation against a noisy table.

  ```
  base  = fork(fork(worldSeed) ^ md5("minecraft:aquifer"))      // two forks, one salt
  mix   = { l = (int)(cx * 3129871) ^ (cz * 116129781L) ^ cy;
            l = l * l * 42317861L + l * 11L;  l >> 16 }         // arithmetic shift
  g     = Xoroshiro128PlusPlus(base.lo ^ mix, base.hi)
  jx    = g.nextInt(10);  jy = g.nextInt(9);  jz = g.nextInt(10)
  centre = (16cx + jx, 12cy + jy, 16cz + jz)
  ```

  **The bounds are not equal, which corrects the previous entry.** Ten
  horizontally and nine vertically — the round before this one concluded "nine
  values, the same absolute width on every axis", and the horizontal half of
  that was wrong. At block level width 10 gives 78.4% of barrier slabs exactly
  against 40.5% for width 9, and in the phi-free tilt channel width 9 produces
  the textbook symmetric plus-or-minus-one shoulders of a wrong bound.

  *Checked independently before landing.* Reimplemented here from the written
  derivation alone and scored on an observable with no fitted quantity in it: a
  cell at layer -4 takes the -20 level rather than the lava floor exactly when
  its vertical draw reaches 8. Over four world seeds that is **256 of 256
  cells**, 34 predicted positive and the same 34 observed. The one subtlety is
  worth recording, because it cost 9 cells at the first attempt: a 16x16
  footprint is the WRONG way to decide which cell owns a column, since
  horizontal jitter moves a cell's territory by up to nine blocks. Assigning
  each column to its nearest predicted centre takes the score from 96.5% to
  exact, and is not circular — the assignment uses the horizontal draws while
  the readout tests the vertical one. `vanilla_aquifer_jitter_test.cpp` pins it.

  *Why it is not a fluke of a large search.* Every ablation collapses to a 3-5%
  null band at block level, with no table in the loop: a different salt,
  dropping either fork, swapping the MD5 halves, adding a third fork, shifting
  by 15 or 17 instead of 16, or a logical shift instead of an arithmetic one.
  The five other assignments of draw slots to axes score 3.9-15.3% against
  78.9%. Fed another world's seed the same model gives 10.8%. And it holds on
  fixtures the search never saw, including one with lava live and one at
  psl 120.

  *What the remaining 22% is.* Not the centres. Split by how far the third
  nearest source sits beyond the second, exactness runs 0.53 where three
  sources compete and 0.97 where only two do — so the two-source barrier rule
  is 97% block-exact, which bounds the per-cell jitter error at about 1.5%.
  The shortfall was the barrier THRESHOLD, whose apparent optimum moved
  between probes (23 on most, 22 on two, 12 on one) while the Voronoi
  boundaries stayed inside the observed slab 99.3% of the time. **That moving
  optimum was the clue, not the noise: there is no threshold on the
  separation.** 23 is the value the real rule takes over the commonest
  geometry.

  **The barrier rule, recovered (MA).** It is an exact, deterministic
  comparison. Every quantity in it is an integer and nothing divides, so no
  floorDiv arises; the only floating-point is the `barrier` router value, and
  only when the block sits within three of the nearer plane. For a block at
  integer position `P`, with `A` and `B` the two nearest cell centres by
  SQUARED euclidean distance from `P` itself (not from the block centre),
  `dA <= dB`, and `lA`, `lB` their fluid levels:

  ```
  if ((y < lA) == (y < lB))     no barrier      // the pair must disagree
  separation = dB - dA
  if (separation >= 25)         no barrier      // the similarity clamps at 0
  above = y - min(lA, lB)                       // >= 0, distance to the air plane
  below = max(lA, lB) - y                       // >= 1, distance to the fluid plane
  pressure = above < below ? 2*above + 7 : 4*below - 2
  if (nearer plane within 3)    pressure += 6 * barrier
  stone  <=>  (25 - separation) * pressure > 75
  ```

  The block centre `y + 0.5` sits `above + 0.5` from the lower plane and
  `below - 0.5` from the upper; the rule uses whichever is nearer, and a tie —
  which happens only for odd gaps, at `below == above + 1` — goes to the LOWER
  plane. The `barrier` router value is read at the block's own position, not at
  either centre, and only reaches three blocks: beyond that, thirteen barrier
  constants from -1.0 to +4.0 give byte-identical output.

  The comparison is STRICT, pinned by three independent exact-equality cases
  that all place no stone: `above` 4 with separation 20, `above` 9 with
  separation 22, and `below` 2 at `barrier` 1.5 with separation 20. The last of
  those is pinned by stone counts of 191403 / 191403 / 191405 at `barrier`
  1.4999999 / 1.5 / 1.5000001. The clamp at separation 25 is real rather than
  cosmetic: without it a negative similarity times a negative pressure makes
  stone, observed firing 478 times at `barrier` -1.0.

  Reproduces **251,658,240 blocks across 40 dimensions on six world seeds**,
  including a holdout generated after the rule was frozen.

  **Still short of the whole barrier, and this is why the filler keeps its
  refusal:** about 13% of the server's real barriers come from a THIRD source
  rather than from the nearest two. The predicate above is exact on the pairs
  it is given; the pair selection is not yet complete.

  **Selection and barrier, verified against the clean-room spec (MA).** The
  first campaign run under §12's clean-room provision. Every claim below is
  sorted as §12 requires — cited to the spec claim AND to the measurement that
  confirms it, or marked untested. Nothing was adopted on the spec's word.

  *Confirmed, and landed.* The cell index is computed on SHIFTED coordinates,
  `floorDiv(x - 5, 16)`, `floorDiv(y + 1, 12)`, `floorDiv(z - 5, 16)` — spec
  Q3.2. The shift is the unique survivor of an elimination sweep over all 270
  values the geometry admits, on five seeds at 6291456 blocks each, and was
  re-derived from scratch by a second instrument on two further seeds in both
  coordinate signs; 264 of the 266 wrong horizontal pairs are refuted by a real
  barrier block they call impossible. The vertical component needed its own
  experiment: `+1` and `+2` differ on ~0.0003% of blocks, and three probe
  worlds built at those exact coordinates came back barrier-stone where `+1`
  predicts possible and `+2` does not, 3 of 3.

  *Confirmed, not yet landed because the code to hold them does not exist.*
  The metric is integer squared euclidean from the block's own position, not
  the block centre — Q4.2, 1742/1742 and 1690/1690 against 0/n for a `+0.5`
  variant. The fourth-ranked source never reaches the substance decision —
  Q4.3, 2582 targeted blocks chosen because two models disagree on rank 4 alone.
  Ties displace toward the LATER candidate — Q4.4, 228/228 and 334/334, where
  this project's own "first wins" scores 12.3%. `s12 <= 0` short-circuits to
  the nearest source — Q6.2. The barrier block is the preset's `default_block`
  — Q6.7, 100% of solid positions on two worlds with different defaults.

  *Refuted.* This build's barrier predicate, as a general model. It has no
  density term and sees two sources; against a density sweep its agreement runs
  92.88% at D = -0.05 (below the 99.99% majority baseline), 95.87% at -1.5 and
  59.63% at -6.0. Q6.6's additive-D three-term shape is confirmed structurally,
  including monotonicity widened to three-way junctions with 0 violations in
  1053963 pairwise checks.

  *Untested, and named as such rather than passed through.* Q4.1's twelve-cell
  window — the asymmetric and symmetric candidate sets disagree on rank 2 in
  107 of 82944 blocks and every one lies outside the barrier-reachable shell,
  so the corpus cannot tell them apart. Q6.1's divisor 25 was not re-measured
  this round. Q6.4's four divisors, its `h > 0` split and its `|u| <= 2` gate
  are unmeasured beyond "some monotone density-dependent mechanism exists"; the
  above/below thickness asymmetry it predicts is inconclusive, showing +0.92
  points in the predicted direction at one density and -1.78 in the opposite
  direction at another. Q6.3, the water-over-lava exception, was touched by no
  angle at all.

  *A correction to this project's own arithmetic.* An earlier note here read
  "the spec's 12 candidates always contain the true nearest centre — 0
  exceptions in 1105920 blocks". A wider search finds that rate is small but
  NOT zero: about 19 rank-1 and 15300 rank-2 misses per 6291456 blocks, from
  the window being forward-only in x and z. No barrier verdict changed, but the
  claim as stated was too strong.

  *And a confirmation of the level rule from an unexpected direction.* Both
  first-pass instruments in this campaign disagreed with the server by 12-29%
  until a verifier traced it to their own oracles omitting the
  `centreY < lambda && tookSea` guard that this build already carries. With it
  restored both went to 100%. The guard was derived here independently, and two
  outside instruments had to rediscover it before they could measure anything.

  **The selection layer, built and scored (MA).** The prerequisite named by
  the campaign above: nothing in this build chose which cells compete, so
  `placesBarrier` had no caller and could not have had one. `selection.hpp`
  now ranks four sources per block — the twelve-cell window, the integer
  squared-euclidean metric from the block's own position, and the later-wins
  tie-break at all four ranks.

  *Two readouts of the open-void probe, and neither touches the refuted
  barrier predicate.* All three terms of Q6.6 carry `s12` as a factor, so a
  barrier can exist only where `d2 - d1 < 25`. Every one of **637252 stone
  blocks the server wrote across four seeds** satisfies that, with the bound
  TIGHT rather than roomy — about two thousand blocks sit at exactly 24. And
  on a block the server left non-solid the barrier has fallen through, so the
  substance is the nearest source's own reading: `y < cellFluidLevel(rank 1)`
  predicts **0.99993 to 0.99996 of 16663703 blocks**, which closes over the
  shift, the window, the metric, the tie-break, the centre jitter and the
  level rule at once, with no fitted quantity anywhere in the loop.

  *The first readout has teeth, and the second's residual is not the
  selection.* On the same stone the unshifted cell index puts 3.16% where it
  says no barrier can exist, and the horizontal neighbours `(-4,+1,-4)` and
  `(-6,+1,-6)` put 11 and 19 blocks there. What it does NOT separate is `+1`
  from `+2` vertically — both score zero, which is why that component needed
  three purpose-built probe worlds. The 5e-5 residual of the second readout is
  fixed by 0 blocks under a brute-force search over a 5x7x5 neighbourhood of
  cells, so it is the fluid TYPE rule or the `lava` entry (blocker 4), not the
  window.

  *Q4.1 stays untested, and the reason is now reproduced from this build's own
  centres rather than taken from the campaign.* The asymmetric and symmetric
  27-cell sets agree on the nearest source on all but about two blocks in a
  million, and where they part at rank 2 the nearest pair has already stopped
  competing — 44 of 3993 disagreements inside the barrier shell over 3538944
  blocks, and 0 of them changing a predicted block on any of the four probe
  worlds. `tests/unit/aquifer_selection_test.cpp` keeps that executable, so it
  speaks up if the rate ever moves.

  *Two spec claims sharpened rather than confirmed.* Q4.1 says the shift makes
  a forward-only window "still bracket the block"; measured over 786432 blocks
  on five seeds it is 0.9978-1.0000 horizontally rather than exactly 1, and
  exactly 1 vertically. And the earlier note here that the twelve candidates
  always contain the true nearest centre is re-measured against a 7x9x7
  brute force: 7 to 20 rank-1 misses per 3538944 blocks (2.0e-6 to 5.7e-6) and
  2.2e-3 to 2.7e-3 at rank 2. Without the shift those become 0.8-3.1% and
  8.6-13.2%, which is what makes the unit case discriminating.

  **The barrier's third source, closed (MA blocker 3).** The campaign above
  left Q6.4's divisors "unmeasured beyond 'some monotone density-dependent
  mechanism exists'" and Q6.3 "touched by no angle at all". Both moved, the
  first almost for free.

  *Q6.4's divisors, confirmed by arithmetic rather than a probe.* At `D = -1`
  — every Stratum aquifer probe's own density constant, this one included —
  Q6.4's pressure function Π, with its stated divisors (1.5/2.5 for the
  near-fluid branch, 3 for the near-air branch), reproduces the OLD
  two-source-only `placesBarrier` EXACTLY over an exhaustive sweep of 39150
  synthetic (level gap, block position, separation, barrier) combinations: 0
  mismatches. That rule's own 251,658,240-block, 40-dimension, six-seed
  server validation transitively confirms those two divisors — no new query
  needed. The fourth divisor (10, for the near-air branch's `3 + t <= 0` sub
  case) was recorded here as unmeasured, on the reading that it "never
  arises for an ADJACENT pair at all — `t` is provably `above + 0.5 >= 0.5`
  there — so only a genuine third source can reach it", and that a third
  source never once did (0 uses, 0 of the 866 total real-block mismatches).
  **The first half of that was right about the arithmetic and wrong about
  the cause, and it is now CLOSED (§11, item 7).** `t >= 0.5` held not for
  adjacent pairs specifically but for EVERY pair `termFires` would consider,
  because it refused any pair that read the same thing at the block — a
  guard of this build's own, absent from Q6.4, which no third source and no
  world could reach past. With it gone the arm is reached, measured, and
  bracketed to within 1% at 10; the guard itself is refuted by 480354
  missed server barriers against the un-gated reading's 0.

  *The third source itself, confirmed on real barriers.*
  `tools/analysis/aquifer-barrier-probe.sh` drives `barrier`,
  `fluid_level_floodedness` and `fluid_level_spread` with vanilla's own REAL
  noises — copied verbatim from the overworld's own `noise_router`, not a
  synthetic field, because a field built to answer MA blocker 2's question
  has no reason to produce the dense, irregular cell-to-cell variation a
  genuine three-way junction needs — at three density constants (-0.3, -1.0,
  -3.0) and two world seeds. For every stone/water/air block,
  `tools/analysis/aquifer-barrier-analyze.cpp` ranks the three nearest
  sources (selection.hpp) and calls this build's own `placesBarrier` twice:
  once with all three, once with the third pushed far enough away to be
  inert — the SAME committed function both times, not a parallel
  reimplementation. The three-source reading rescues 83-98% of the
  two-source rule's errors on 15,728,640 real blocks per seed, cutting the
  real-barrier miss rate from 13.19%/16.48% to 1.01%/1.02% overall — matching
  the ~13% this project had already measured by an entirely different route,
  and landing within about a point of it on both seeds independently.

  *Still open at the time; since closed.* Q6.3's water-over-lava exception
  was untested by every angle here, and `placesBarrier` did not implement
  it; it is now measured and landed in `computeSubstance` (see "The lava
  sea's two clauses" below). The mixed-fluid-type branch of Π (`Π = 2.0`
  when one source reads lava and the other water) was unmeasured here on
  purpose — every barrier probe held `lava` at a constant to keep that
  question separate rather than folding it in unverified; it is now
  measured on the sea -70 worlds and landed (see "The mixed-type Π"
  below). `ChunkFiller` did not yet call `placesBarrier` at all at this
  point; the wiring came later.

  **The lava sea's two clauses — Q6.3 measured, Q2.4 found (MA).** The
  clean-room spec's Q6.3 says a nearest source reading water directly over
  the global lava picker's lava is water, no barrier. The unlock is that
  `Global` is Q1.2's trivial picker — lava strictly below `lambda =
  min(-54, sea_level)`, a function of `y` alone — and Q2.4 already hands
  every block below `lambda` to the sea before the lattice is consulted.
  So the exception can fire on exactly ONE row per dimension, `y = lambda`,
  which is why no earlier probe ever saw it: `aquifer-barrier-probe.sh`
  sets `min_y` -48 specifically to keep the sea out of its world.

  `tools/analysis/aquifer-waterlava-probe.sh` puts it back: the barrier
  probe's own configuration (real barrier/floodedness/spread, psl a
  constant 96, lava a constant 0.0, three densities) at `min_y` -64, plus a
  `sea_level` -70 arm at `min_y` -80 so the row has to move with `lambda`.
  Three seeds (42, 31337, 8675309). The analyzer and the conformance case
  (`vanilla_aquifer_waterlava_test.cpp`) call the committed
  `computeSubstance` end to end and, on the same inputs, the BARE
  fall-through it used to be — `placesBarrier`, then the nearest source's
  reading — so the score is exactly "where the old logic writes stone and
  the server does not".

  *On the shipped sea the row is empty.* At sea 63, across 3 seeds x 3
  densities x 16384 columns, no source reads water at y = -54: a source's
  ladder there sits at -60 plus a multiple of 3, so water on the row needs
  a spread of 0.9 (offset +9) or a sea-gated source centred within five
  blocks of the sea, and neither happened once. Server and build both
  write 0 stone on the row. The water the server does show there is all
  flow — `level` 1-7 spreading over the sea, `level` 8 falling from bodies
  above; not one source block — which is also what makes the -54..-52 pile
  of "fluid this build calls air" in the varying-surface residual read as
  post-generation flow rather than as an aquifer decision.

  *At sea -70 the ladder hands the row water, and the exception is real.*
  It applies to 278 / 1102 / 1940 blocks; the bare logic writes stone on
  14 / 50 / 70 of them; the server writes stone on 0 of 3320. One row up
  the two decisions agree on every block (0 of 589824 across all rows
  above, all arms). And the asymmetry holds: where the nearest source
  reads AIR on the same row, the server still writes barriers (93 / 239 /
  244), exactly as on the rows above — Q6.3 is a water rule, not a row
  rule. Independent of `D`, as its position before Q6.6 requires: the three
  densities behave identically.

  *Q2.4 was not implemented either, and the same probe shows it.* Below
  the sea the bare decision read the nearest source's own TYPE, which is
  water wherever that source is centred above the sea: wrong on 7682 /
  5330 / 5736 of 16384 blocks per density at sea 63 — invisible to every
  category golden, which count water and lava alike as "fluid". Now lava
  before any source is read: 16384 / 16384 at sea 63. At sea -70 the
  server shows 16 / 166 / 41 water blocks below the row, all of them
  falling water (`level` 8) directly under water with obsidian beneath:
  water that arrived at the row after generation dropped into the lava
  under it before that lava could turn to obsidian. Post-generation
  mechanics, verified block by block (0 of 223 unexplained), not the
  aquifer.

  *What the low sea also showed, and the next change took up.* The
  sea -70 arm is the first world where lava-typed sources crowd the rows
  just above the sea, and there `placesBarrier` missed 27-45% of the
  server's real barriers on those rows, every miss in a junction with a
  lava-typed source. That was read as the mixed-fluid-type Π branch's
  size; the next paragraph measures it, and finds it to be two things.

  **The mixed-type Π — measured, and not the reading it looked like (MA).**
  The clean-room spec's Q6.4 opens with "if one reads lava and the other
  water, `Π = 2.0`", on two statuses `A = (L_A, T_A)`, `B = (L_B, T_B)`
  "at height `y`". That sentence has three readings, and they differ on
  real blocks: (i) compare the two TYPE FIELDS, applied where the pair
  already disagrees at `y` (one fluid, one air) — the reading the
  status-tuple notation suggests, and the one a first implementation
  here took; (ii) compare what each source READS at `y`, so the constant
  applies where both read fluid and the fluids differ — a lava body
  meeting a water body; (iii) the types differing regardless of readings.
  The same three sea -70 worlds (`aquifer-waterlava-probe.sh`, seeds 42 /
  31337 / 8675309) separate them, because with `lava` a constant 0.0 the
  only lava-typed sources are those centred below lambda, and those crowd
  exactly the rows just above the sea. `aquifer-waterlava-analyze.cpp` and
  the second case of `vanilla_aquifer_waterlava_test.cpp` call the
  committed `placesBarrier` twice per block on the same three ranked
  sources — as typed, and with every source retyped water, which is the
  predicate exactly as it was — and count, for each reading the predicate
  does NOT take, the blocks where it would have answered differently and
  what the server holds there.

  *Reading (i) makes every row worse.* Real-barrier misses in mixed
  junctions RISE under it (seed 42, y = -68: 36 -> 55; seed 8675309, y =
  -67: 241 -> 313) and it writes stone the level formula never does (31
  on one row). On the nearest pair, where the constant alone would fire
  and the formula does not, the server has stone on 0 of 33 blocks above
  the sea; where the formula fires and the constant would not, on 252 of
  252. A pair that disagrees at `y` takes the level formula whatever its
  types, and "old false stone = 0" on every row says the formula is right
  there.

  *Reading (ii) is the server's.* Where a mixed pair BOTH read fluid at
  `y` and `D + w·2 > 0`, the server holds stone on every block: 18 / 430 /
  665 per seed on the rows the lattice owns, 1113 of 1113. Landed:
  `termFires` takes the constant for such a pair — the one agreeing pair
  that fires — and the formula for a disagreeing one; both air, or both
  the same fluid, still fire nothing. Pooled over the three seeds and the
  rows the lattice owns (the Q6.3 blocks excluded, since the exception
  pre-empts the predicate there), the retyped predicate misses 1698 of
  the server's 3341 real barriers in mixed junctions and the committed
  one 590 — 1240 to 330 on the rows above the sea alone — with 0 false
  stone under either; the 1108 blocks the constant adds are all server
  stone; and where no pair is mixed the two are the same function (293 /
  293 misses, 0 false). `BarrierSource` carries a type, every ranked
  source is typed whether or not it reads fluid at the block (which one
  does depends on the block), and `StatusCache` memoizes level and type
  together per centre — one more router read where the level's scan was
  already the expensive part.

  *Reading (iii) is refuted by the blocks it would fill.* Stone between
  two DRAINED cells of different type: the server holds stone on 0-4 of
  the 424-1145 blocks per row above the sea the constant would carry,
  0.0-0.5%.

  *Q6.3's precedence holds against the new term.* On the sea's top row
  the constant now applies to a lava body against the row's water sources,
  so the bare predicate would write stone on 19 / 176 / 178 of the 278 /
  1102 / 1940 blocks the exception owns (14 / 50 / 70 before); the server
  writes 0 of 3320 either way.

  *What is left is a level, not a type.* The 590 (and the 293 "pure"
  misses, with no mixed pair at all) sit on the rows 0-3 above the sea.
  This build reports a DRY source as `level = lambda` where the spec's is
  the sentinel `never = -32512` (Q1.4, Q5.6), and `ladderLevel` clamps a
  ladder that falls below lambda up to it where Q5.7 has no clamp. Both
  are invisible to readings — every row below lambda is the sea's (Q2.4)
  — but Π's arithmetic sees them: with the plane at lambda a block 0-3
  rows above it sits on the `h <= 0` side of the midpoint (divisors 3 and
  10, offset 3); with the plane at `never` the same block is on the `h >
  0` side (1.5 and 2.5) and far from it. The analyzer builds a second
  `BarrierAt` per block at the spec's own levels — a closed form in this
  world, whose constant psl leaves Q5.3's short-circuits inert: past the
  sea gate `Global(Q).level`, past the local gate the unclamped ladder,
  else `never` — and re-scores: mixed-junction misses 590 -> 0 / 0 / 0
  and pure misses -> 0 / 0 / 0 on rows lambda+1..+3 on all three seeds,
  0 false stone; row lambda keeps 18 / 17 / 0. That is `cellFluidLevel`'s
  contract to change — readings at `y >= lambda` are untouched, but
  `vanilla_aquifer_selection_test.cpp` reads `y < level` from y = -64 and
  leans on the clamp below the sea, nine lattice unit assertions pin
  `lambda`/`kLavaLevel` for floored or dry outcomes, and Q5.8's `L !=
  never` conjunct (`fluid_type.hpp`) falls out of the same sentinel. One
  slice, named in PROGRESS.md, with the conformance suite as its guard;
  not folded into this change.

  **A second instrument for the depth path's gate, and a wrong first attempt
  at building one (MA blocker 1, CLOSED).** The near-surface path gates on
  `gate`, the scan's prefix minimum; the depth path gates on `anchor`, the
  single sample at the cell's own quart position — an asymmetry resting on
  one instrument, of exactly the shape that has been wrong twice before here.
  `tools/analysis/aquifer-depthgate-probe.sh` builds the named separating
  configuration: two psl regions, HIGH (100) and LOW (40), each around 100
  blocks — comfortably larger than the scan window's 48-block reach — so
  cells near a region boundary have their ANCHOR sample in the HIGH region
  while a WINDOW OFFSET pokes into the LOW one, separating the two by 60
  blocks against blocker 1's required eight. Nine floodedness values comb
  0.05 through 0.85.

  *The first readout was wrong, and worth recording precisely.* Scanning
  each candidate cell's WHOLE plausible level range (from lambda up to the
  ladder, roughly 74 blocks) for "the topmost fluid block" picked up
  neighbouring cells' territory almost everywhere — a cell's own vertical
  span is its ~12-block pitch, not 74 blocks — and produced a smooth,
  centreY-correlated smear that matched NEITHER hypothesis, on a corpus that
  had looked clean (a self-consistency filter even seemed to validate it,
  since neighbouring cells sharing a region can coincidentally share a
  boundary-adjacent reading). The fix was reading each cell at its OWN
  geometric centre — always its own nearest source by construction, and
  strictly inside its own territory — rather than searching a wide band.

  *Confirmed: anchor-gating, matching current code.* 202 of 210 genuinely
  discriminating cells — filtered to where the two hypotheses' predictions
  differ AT the cell's own centre, not merely where the predicted LEVELS
  differ somewhere — match anchor-gating, across eight floodedness values
  and dozens of distinct cell geometries. The eight exceptions are two
  specific cells, recurring across four floodedness values, whose ladder
  level exactly equals their own centreY — a boundary tie in the readout
  (testing the FIRST AIR block itself), not a rival pattern. No code
  changed; the second instrument backs what the first one found.

  **A world with `sea_level` below the lava, and two things it settles
  (MA).** `kLavaLevel = -54` is a compile-time constant; `lambdaLevel(seaLevel)`
  is not — it equals `sea_level` itself once `sea_level < -54`. At every
  `sea_level` this project had ever generated a world with, from 32 through
  200, the two coincide, so any place in the level rule that SHOULD read
  `lambda` and instead reads a bare `-54` or `-62` is invisible. One probe,
  `tools/analysis/aquifer-lowsea-probe.sh` at `sea_level` -70, is the first
  world shape that pulls them apart.

  *The abort threshold moves with `sea_level`, confirmed by exact-value
  bisection.* `kPslAbortBelow` used to be a bare `-62.0` that "coincided with
  `kLavaLevel - 8`" — flagged UNPROVEN because every prior measurement
  (`min_y` in five values, `sea_level` in {40,100,128,200}) never separates a
  bare constant from `lambdaLevel(seaLevel) - 8`, since they agree at every
  one of those. Eleven dimensions bisecting BOTH candidates at `sea_level`
  -70 settle it: every world from -55 up through -78.00 is BYTE-IDENTICAL to
  the others, and -78.01 alone flips — refuting the bare -62 outright (it
  sits nowhere near the true boundary here) and confirming
  `lambdaLevel(-70) - 8 = -78` exactly. It is now `abortThreshold(seaLevel)`,
  a function rather than a constant, reducing to the old figure at every
  sea_level this project had already verified.

  *The aborting near-surface floor is `lambda`, not the literal `kLavaLevel`
  — MA blocker 2's first correction, landed.* One dimension, psl pinned deep
  enough to abort under any candidate threshold, read as a column height
  profile that sweeps many cell centres for free: 251229 blocks the old
  reading calls wet up to y=-55 are observed dry at every one, 0/251229, over
  1966080 blocks total (0.8722 under the old reading, 1.0000 under `lambda`).
  The comparand (`centreY >= lambda` rather than `>= kLavaLevel`) cannot be
  separated from the return value the same way: `lambda` equals `sea_level`
  on every world low enough to ask the question, so a cell that takes the
  "true" branch and one that takes the "false" branch are indistinguishable
  downstream once both are lambda-based. That is a PERMANENT TIE, not a
  further measurement — using `lambda` in both places is adopted because it
  is a no-op everywhere already verified, not because the comparand was
  isolated on its own.

  *A methodological correction recorded rather than silently fixed.* The
  first version of the near-surface analysis found a smooth, centreY-varying
  spread of levels even in worlds that clearly should NOT abort, and nearly
  mistook it for a bug. It was the trailing guard and barrier stone, both
  already-verified, unrelated behaviour, doing exactly what they document —
  visible here only because the probe reads many different cell centres in
  one pass. The fix was not to model them but to compare whole-world
  signatures against a known-not-aborted reference rather than a single
  block: since `aborted` is the only psl-dependent quantity the off-depth-
  path sea branch reads, every world that does not abort must be
  BYTE-IDENTICAL to every other one that does not, regardless of how far
  apart their psl values sit — which is what the golden case checks.

  **The fluid TYPE, measured at last (MA).** This project built about 1370
  probe dimensions and every one of them pinned the `lava` router entry at the
  constant -1.0, so the type rule went four campaigns without a single
  measurement. The corpus that could answer it was already on disk: the
  `elava` arm of the comb probes, the one arm that gives `lava` vanilla's own
  noise. Nobody had looked, because nobody had a hypothesis worth testing
  until the clean-room spec supplied one (Q5.8) — which is the §12 provision
  working as intended.

  *The readout is per SOURCE.* Every fluid block is attributed to its rank-1
  source — which the selection layer above made possible for the first time —
  so a source is one observation rather than its thousands of correlated
  blocks, and the instrument is falsifiable before any candidate is scored:
  if the type were not a property of the source, sources would come out
  holding both fluids. 3125 sources over four seeds.

  *Confirmed.* A source's fluid is the dimension's `default_fluid`, overridden
  to lava when its level is low enough and `|lava| > 0.3` at the source's
  centre contracted to indices. **0.99873** against a **0.94176** majority
  null, predicting 180 of 182 lava sources with 2 false positives.

  *The sharpest part, and the one no analogy would have reached.* `lava` is
  read on a lattice of horizontal pitch **64** — not the **16** the spread
  uses, though both are contracted-index reads and look alike in shape. Pitch
  16 scores 0.9424, pitch 32 0.9472 and pitch 128 0.9418: every rival is at or
  BELOW the null, so 64 is not the best of four, it is the only one that beats
  guessing. The vertical pitch IS the spread's 40, and that was measured
  separately — 20 and 80 both lose.

  *The 0.3 threshold is a peak, not a plateau.* 0.25 leaves 78 false
  positives, 0.35 leaves 31 false negatives, 0.30 leaves 2 of each. It is the
  only aquifer constant published anywhere this project may read, and it is
  now measured rather than adopted. The comparison is on the ABSOLUTE value:
  signed scores 0.96896 on the same sources.

  *Strictness at exactly 0.3, closed.* `tools/analysis/aquifer-fluidtype-probe.sh`
  (group A) drives `lava` to a literal constant instead of through noise —
  the exact double 0.3, and the two doubles immediately adjacent to it — at a
  level forced to -32, deep enough that the ceiling question, then still
  open, could not interfere from either side of its own bracket. The transition is
  exact and lands exactly where the code already read it: `lava == 0.3` on
  the server comes back water, the very next representable double above it
  comes back lava. Strict `>`, confirmed to the ULP rather than assumed.

  *The level ceiling, pinned at -10 — and shown to be absolute.* The same
  tool (group B/C) had swept `fluid_level_spread` past the ±1.0 any real
  noise reaches and read -11 and -10 as lava, -8 as water, which bracketed
  the ceiling to {-10, -9} but could not choose between them: the sweep
  steps the level in threes and skipped both candidates. Group D closes it
  by reaching -9 through the two routes the ladder's mod-3 lattice does not
  constrain. Arm Q rides the sea branch (`fluid_level_floodedness` 0.9, past
  the measured 0.8 gate, so the level IS `sea_level`) and reads
  -12/-11/-10 lava and -9/-8/-7 water. Arm P binds the psl cap instead
  (`sea_level` -16, spread 6.0 so every rung lands above the cap) and reads
  the same six. Arm P′ repeats arm P at `sea_level` -70 over four of those
  levels (-12, -10, -9, -8). Every dimension is
  16384 of 16384 columns with 0 of the other fluid, identical on seeds 42, 7
  and 999. **The ceiling is -10, inclusive.**

  That is more than the question asked. Three different sea levels (63, -16,
  -70) and two different level-producing branches put the transition at the
  same ABSOLUTE pair, which refutes a sea-relative rule — `L ≤ sea_level −
  73` is indistinguishable from -10 at the shipped sea, but would have moved
  arm P's transition to -89 and arm P′'s to -143 — and a lambda-relative one
  with it, arm P′ having moved lambda to -70 for exactly that purpose.

  *Why the earlier attempt collapsed, now arithmetic rather than a mystery.*
  The ladder level is `40·floorDiv(centreY,40) + 20 + 3·floorDiv(floor(10·
  spread),3)`, so for a constant spread the offset is uniform world-wide and
  reaching -9 needs `yi ≡ 1 (mod 3)`. The attempt that collapsed picked
  `yi = 1` — base 60, cells centred in y ∈ [40,80), a whole rung above the
  observable band: those -9 sources own no block below their own level and
  place nothing, while every cell that does own a block in -53..0 lands on
  base -20 or -60 (levels -89 / -129), below lambda and therefore dry. What
  was left in the world was the global lava sea alone, which is what the
  first pass recorded as a collapse to the lava-sea floor. The 68-cell -10
  reading is the mirror image (base 20, `yi = 0`) and works.

  *And why no golden could ever have decided it.* The same arithmetic makes
  the gap structural: with a real spread noise (|s| ≤ 1) the offset is
  confined to [-12, +9], so the reachable uncapped levels are {-72..-51} ∪
  {-32..-11} ∪ {8..29} ∪ … and -10/-9 fall in the hole between -11 and +8.
  The conformance corpus contains no source at either level, and the group D
  reading pins the CODE's comparison boundary rather than anything ordinary
  terrain produces — its spread 6.0 and floodedness 0.9 are far outside any
  real noise, deliberately, as the rest of this corpus does to pin a
  constant. Group D also reproduces all three on-record readings in the same
  run and world (-11 lava 14150, -8 water 14004, -10 lava 68 — exact on seed
  42), which is what makes its level model trustworthy here.

  *One readout caveat, measured rather than assumed.* With the analyzer's old
  "≥5% of bodies" print filter removed, small-count entries appear that are
  not source levels: `r_b09` prints `-10:water(w1914/l0)` beside its
  `-11:LAVA(w0/l14150)`. A column dump resolves it — those columns hold a
  SINGLE water block at y=-11 over obsidian at -12 over a deep lava body
  (-13..-42), a water/lava contact film where two territories abut. Only the
  near-16384 entries are readings.

  *Still not measured, and marked in the header rather than guessed.*
  Whether a source already reading lava is exempt cannot be observed at all,
  those sources being below the lava sea. And no `default_fluid` but water
  has been in this position.

  *One thing left unexplained rather than explained away.* About 4% of sources
  hold both fluids above the global lava sea, and the minority blocks are
  spread over y -21 to -54 rather than piled at the boundary. The leading
  candidate at the time was Q6.3's water-over-lava exception; that is now
  RULED OUT by its shape rather than by a probe — Q6.3 only ever turns a
  would-be barrier into water on the sea's own top row, it never changes
  which fluid a block holds, and it cannot reach y -21. The next candidate
  was the mixed-fluid-type Π branch; that is now RULED OUT the same way —
  landed and measured ("The mixed-type Π" below), what it adds is stone,
  never the other fluid. No named candidate remains. It stays excluded
  from the score and counted in the test.

  **A surface that varies, and a model comparison that came out a tie for a
  provable reason (MA).** The probe SPEC §10 has demanded since the aquifer
  became a track: `preliminary_surface_level` as a THREE-valued spatial field
  — 96 above `sea_level - 8`, -20 below it, -70 below the scan's -62 abort —
  at three feature sizes crossed with two floodedness values, plus three
  constant controls. Three is the smallest number of arms that works: on a
  two-valued field an aborting prefix-minimum is identically a point read.

  *The field is observed rather than reconstructed.* Each scale ships a
  readout dimension whose terrain height names the arm per column, from the
  server's own arithmetic, and it is constant within every 4x4 quart — which
  is exactly the lattice the scan's anchors and its sixteen-block offsets live
  on. So a consumer reads the surface out of a region file, and an error in
  reconstructing it cannot be mistaken for a disagreement with the server.

  *This build scores 0.99614 on 9039986 blocks*, 0.9924 / 0.9991 / 0.9968 by
  feature size. The corpus is not degenerate and the test checks so rather
  than assuming it: 3150 of 3258 sources abort, and 72-83% have `gate != cap`
  — a configuration no constant-surface dimension can produce at all.

  *The comparison with the clean-room spec's Q5.3/Q5.6 is a TIE, and the tie
  is structural rather than lucky.* Implemented side by side, the spec's
  first-match scan and this build's four-value `PslRead` disagree on the
  LEVEL of 5 to 19 sources in 543, and on **0 blocks of 9039986**. The
  algebra says why. The two part only when the spec's case (a) fires while
  this build skips its near-surface path; that needs `qy > anchor + 20` and
  either `gate >= sea_level - 8` or `qy <= gate - 4`. The second contradicts
  the first, and since `gate <= anchor` the first forces
  `qy > sea_level + 12` — a source centred more than twelve blocks above the
  sea, which owns no block at or below it. Both models then predict air
  everywhere that source reaches. Measurement agrees: 0 disagreeing blocks.

  *So blockers 1 and 2 are answered by equivalence rather than by
  correction*, on this corpus. The spec independently reproduces the anchor
  gate — its `submerged` is `Prelim(centre) < sea_level - 8`, which is this
  build's single-sourced `anchor` test — and its `S_min` is this build's
  `cap`. Where the two genuinely differ is the reach: Q5.6 computes the depth
  from `S_min` where `cellFluidLevel` uses `gate`. That difference is ALSO
  unobservable here, and provably: `gate != cap` requires an aborting sample,
  an aborting sample is below -62, so `cap < -62` and the ladder floors to
  `lambda` either way. **The only world where it could be seen is one with
  `sea_level < -54`**, which moves that floor — so the campaign's third step
  is not merely a check on the -62 constant, it is the sole instrument for
  the reach.

  *The residual is shared, one-directional, and named rather than explained.*
  34878 blocks of 9039986 (0.39%), every one of them fluid this build calls
  air and never the reverse, and the spec's model misses exactly the same
  ones. They pile at y -54 to -52 — the top of the global lava sea — and just
  under `sea_level` at y 57 to 61. Candidates: the `never` sentinel Q5.6
  carries where this build floors to `lambda`, and — at the time — Q6.3's
  water-over-lava exception. Q6.3 is since measured and RULED OUT here by
  its shape: it turns a would-be barrier into water, never air into fluid,
  so it cannot produce a "fluid this build calls air" residual.

  *One thing this cost.* The probe first ran under the spec name `psl`, whose
  fixture directory already held a dozen arms from an earlier campaign; the
  harness copies its spec beside the regions, so those older arms lost their
  definitions. The regions survive and are now undocumented. The probe was
  renamed to `pslvar` and re-run. The harness overwrites `spec.json` silently
  and should not.

  **Where the aquifer reads its router inputs (MA).** Everything above is a
  PREDICATE, and every one of the ~1370 probe dimensions behind it held
  `preliminary_surface_level` and `fluid_level_floodedness` at constants — so
  the predicate was settled and the positions its inputs are read at were not.
  Six agents across eleven world seeds, on instruments built independently of
  each other, settled two of the three.

  **The three do NOT share a sample position, and that is measured rather than
  inferred.** On the same cells in the same worlds, the floodedness readout and
  the spread readout agree at 0.4895-0.5421 horizontally and 0.4986-0.5415
  vertically, which is chance. Each was established separately; the one agent
  who argued by analogy from another quantity was refuted outright.

  *`fluid_level_floodedness` — the cell's own jittered centre, in absolute
  block coordinates, verbatim.* No quantisation, no offset, no rounding, and no
  clamp: a cell centred below `min_y` still samples at its raw centre y
  (25166/25174 and 21737/21737 water blocks below a floor at 16). One read per
  cell. A per-block read is dead by three orders of magnitude rather than by a
  score — it would have split 99.6% of cells under a one-block field and the
  server split 4.8%. Five agents, ~30000 cells, per-seed scores 0.9954-1.0000
  and 1.00000 on control-clean cells, against a 0.498-0.574 chance baseline.
  The nearest rival is the centre quantised to two, at 0.618-0.627, wrong on
  all 136 cells where the two differ. Also excluded on the same cells: the low
  corner, the midpoint, quantisation to 4/8/16, the centre plus or minus one on
  any axis (BELOW chance on y — a wrong answer a majority-class baseline alone
  would have hidden), the cell index as a coordinate, the jitter alone, every
  neighbouring cell, a fixed y at 0/`min_y`/`sea_level`/the surface, the axes
  swapped, and the noise cell's corner. Closed by verification at negative
  coordinates, on the ocean branch, at chunk edges, at extreme jitter, and at
  negative cell indices in y.

  *`fluid_level_spread` — the cell's lattice INDICES, not a position in block
  space.* The cell index in x and z, and the 40-block band index
  `floorDiv(centreY, 40)` in y. **The jitter is inside the division**:
  `floorDiv(12 * cellY, 40)` differs on five to nine cells per world and is
  wrong on every one of them, while scoring 0.9954-0.9975 — high enough to read
  as noise and never 1.0000, and exactly what an implementer writes by mistake.

  Recovered without a candidate list, which is what makes it solid: nine
  dimensions each binary-encoding one bit of the sampled y gave per-band purity
  1000/1000 on every bit and spelled the answer out, and a verifier repeated it
  with three independent 13-arm combs searching y over [-2048, 2048]. Two
  agents, ~29000 cells, 1.0000 per seed on five seeds including a post-hoc
  holdout. Excluded: truncating the division (0.9417-0.9451), the centre y
  itself — floodedness's own position — (0.4283-0.5416), every affine
  `40b + k` for k in [-80, 120], and every horizontal candidate at or below the
  0.53-0.62 baseline. Not an aggregate either: min/max/mean over neighbouring
  bands 0.56-0.61, min over the 3x3 index neighbourhood 0.43-0.47. `cell.x`
  versus `floorDiv(centre.x, 16)` is a PERMANENT tie, not a gap — the
  horizontal jitter never reaches 16, so no world can separate them.

  *`preliminary_surface_level` — y settled at absolute 0; the horizontal read
  is NOT a point sample and is NOT settled.* The y is three-sourced, and one
  source approaches from outside: a psl differing from a constant only on
  y in [-1, 1] changes every block, one differing only on y in [300, 310]
  changes nothing, and 0 of 6291456 blocks differ otherwise — so it is a read
  near y = 0 and specifically not a minimum over y. Excluded: `min_y`,
  `min_y + 64`, `sea_level`, the cell's own centre y.

  **The horizontal read, settled (MA).** Three campaigns and six agents; the
  first two produced a confident wrong law and this one explains why. It is not
  a point sample, and it is not the symmetric neighbourhood the last entry
  guessed at.

  ```
  X = floorDiv(centre.x, 4) * 4        // floorDiv, never `/`
  Z = floorDiv(centre.z, 4) * 4
  m = full = psl(X, 0, Z)              // the anchor, read first and unconditionally
  aborted = false
  for (dx, dz) in the window, IN ORDER:
      v = psl(X + dx, 0, Z + dz)
      if (!aborted) { if (v < -62) aborted = true; else m = min(m, v); }
      full = min(full, v)
  gate = floor(m)                      // the ocean gate and the near-surface return
  cap  = floor(aborted ? full : m)     // the ladder's cap
  ```

  The window is thirteen positions on a 16-block lattice, and it is NOT a
  square — it reaches 48 blocks west and 16 east, north and south:

  ```
  dz = -16 :  dx = -32 -16   0  +16
  dz =   0 :  dx = -48 -32 -16   0  +16     <- the spur is on this row only
  dz = +16 :  dx = -32 -16   0  +16
  ```

  Three independent non-parametric sieves — each marking an offset impossible
  the moment one cell contradicts it, none hypothesising a shape — arrived at
  exactly these thirteen out of thousands of candidates, on seven seeds and
  both coordinate signs. **A later campaign re-derived them at twenty feature
  scales from half a block to a hundred**, on smooth, two-scale and
  discontinuous fields, on 23 seeds, by three more sieves over 2401, 83521 and
  103041 candidates: the intersections are exactly these thirteen with ZERO
  unexplained cells, and one dimension of one seed already collapses 1089
  candidates to them. The support does not grow at short wavelength — which was
  the open question, and the answer is that it never was the problem. Every dense or symmetric alternative loses on the same
  cells: the 4x3 without the spur 0.9261, the 5x3 0.8366, the 3x3 0.7082, the
  4x4 0.6732, the 5x5 0.4436, a point read 0.0700. Adding the spur's mirror at
  `(+32, 0)` or its neighbours at `(-48, ±16)` violates outright. Why it is
  asymmetric is unexplained.

  *The aggregation is `min`, and that is measured on exact integers rather than
  on bits.* Two agents built readouts returning the integer psl the aquifer
  actually used — one by lifting the ladder cap out of the way with a spread of
  3.0, the other by driving the cap branch directly — and scored rank 0 at
  1.0000 over 4769 and 2837 exact readings, against second-smallest 0.17-0.48,
  third-smallest 0.02-0.16, and median, mean, maximum and second-largest all at
  0.0000. A model-free argument agrees without any support model: under a
  minimum the number of value-permutations of a tercile field that read wet
  equals the number of level sets the window touches, and under a median, mode,
  maximum or "the value where the noise is extremal" it is exactly one; two or
  three were observed on every seed, and the non-contiguous pattern never once.

  *The scan ORDER is load-bearing*, because the scan aborts: `dz` outer
  ascending, `dx` inner ascending, spur first in its row. Pinned twice — 0 of
  200 random permutations reach the winning score and all eleven adjacent
  transpositions lose; separately, 19 rival orders score at most 0.9756 against
  1.0000, with x-outer at 0.82-0.89 and z-descending at 0.77-0.87. The break
  must leave both loops: a row-only break scores 0.8864 and skip-and-continue
  0.8662.

  *The abort is an absolute -62, strict.* -61.99 and -62.00 do not fire; -62.01
  does. Invariant under `min_y` in {-48, -64, -80, -96, -128} and `sea_level`
  in {40, 100, 128, 200} — which kills "the world floor plus two" outright,
  since at `min_y` -128 that would put it at -126 and a -70 arm would not
  abort, and it does. It coincides with the lava level minus eight; that
  identity is UNPROVEN, and separating it from an arbitrary constant needs a
  world with `sea_level` below -54.

  **Four consumers, one scan — and they do not agree.** The scan yields four
  values and the ocean branch reads a different one in each place. The near
  surface gate, its depth test and the reach read the window's PREFIX minimum;
  the ladder's cap reads the whole window's minimum; the DEPTH PATH's own gate
  reads the ANCHOR sample alone; and the trailing guard reads no psl at all
  (all eight candidate sources tie with zero differing cells).

  The anchor gate is the one finding here resting on a single agent and a
  single instrument family, and the asymmetry — near surface on the minimum,
  depth path on the anchor — is exactly the shape that has been wrong twice in
  this codebase. Its controls are good: worlds where the two readings coincide
  score 1.0000 for both, the effect moves one-for-one with `sea_level`, and all
  nine misses of the rival model in an earlier round fall in exactly this
  class. It still needs a second instrument before the filler leans on it, and
  it is the mandatory conformance case.

  **The anchor arms the abort.** The scan's first sample is not exempt from
  the threshold, and this SPEC said it was. The two spellings differ on exactly
  one shape — a cell whose anchor is below -62 while every window sample is
  above it — and 329 such cells across seventeen seeds and four independent
  instruments back the armed reading, none the exemption. The failure mode is
  worth naming: the experiment that established "the anchor is read first and
  unconditionally" measured the VALUE an aborting anchor contributes, which is
  the same either way, and that value was then read as evidence about the state
  of the FLAG.

  A free invariant falls out, and is asserted in the tests: with the anchor
  armed, `aborted` and `cap <= -63` are the same predicate over every field
  that can exist, since `cap` is the whole window's minimum always and -62 is
  an integer. Under the exempt spelling that identity fails on precisely the
  discriminating class. What is genuinely tied is only where the flag is
  TESTED — "the anchor arms it" and "the near-surface return additionally
  requires a clean anchor" are indistinguishable, because reaching the other
  branch with a low anchor forces `sea_level <= -55`, where lambda IS
  `sea_level` and both outcomes are the same number.

  **The abort refuses the sea.** When a window sample falls below -62 the
  floodedness-gated `sea_level` outcome does not happen. This is the whole
  explanation of a failure this SPEC recorded for a day as the ocean branch's
  slopes being wrong in the middle of the floodedness band. The clean control
  settles it: a low arm of exactly -62, where nothing aborts, scores 1.00000
  under the model that has no abort term, and -63 collapses it to 0.807-0.897.
  An aborting cell near the surface still floods if it clears the scan's own
  low sample by more than twenty blocks — the offset is exactly 20, and the
  term reads the whole-window minimum.

  **Every "-54" that is a LEVEL moves with `sea_level`.** Below
  `min(-54, sea_level)` the world is lava unconditionally, whatever the aquifer
  decides — measured with aquifers disabled at four sea levels, and nobody had
  done it. So no block readout can separate any level at or below that
  boundary, and four campaigns read the lava sea's top and recorded it as a
  level. A world with `sea_level` -56, where -54 sits two blocks above the
  floor, makes it visible: the third outcome and the ladder's lower clamp are
  both that boundary, on 9740 and 3501 discriminating cells.

  The trailing guard is the exception that proves it — its replacement is the
  LITERAL -54, which at `sea_level` -56 is ABOVE the boundary. And it tests
  which BRANCH produced the level rather than whether the number equals
  `sea_level`: those coincide for every ordinary sea and part exactly where the
  guard was measured, since at -56 the third outcome is also -56 and a numeric
  test would floor it. Same world, same cells: floodedness 1.0 reads -54 and
  0.3 reads -56.

  **The bonus is a product over a divisor, never a pre-divided constant.**
  `fl(11/640)` rounds UP by 1.39e-18, which is enough to fire the sea gate
  where the server does not — at exactly two reaches, 45 and 50, on 48 cells
  over thirteen dimensions, three seeds and both coordinate signs. The
  surviving spellings are `f + (reach * 11.0) / 640.0`, its FMA, and the
  cross-multiplied form; `reach / 640.0 * 11.0` and exact-rational comparison
  are both refuted against the server. **This corrects §5's own claim about
  this line:** `-ffp-contract=off` stays project policy, but it is not what
  makes this expression right — with the pre-divided constant the line IS an
  FMA shape and the contracted form happens to agree while the uncontracted one
  does not. Written as a product over a divisor there is nothing to contract.

  **Two consumers of the older reading, one scan.** The gate stops at the
  aborting sample and the cap does not, and the argument is arithmetic rather than a fit: with
  `sea_level` 40, floodedness 0.6, spread 0 and a ladder at -20, the settled
  level rule yields 40 or -20 for EVERY psl and both leave the cell wet — yet
  858 of 858 such cells with a non-centre sample below -62 are observed dry at
  the lava level, which requires `min(ladder, psl) <= -54` in a branch only
  reachable when `psl >= sea_level - 8`. No single psl value satisfies both,
  and constant-psl controls at -70, -64, -63, -62, -58, -54, -40, 0, 40 and 100
  all flood exactly those cells, so it is the spike and not the value that
  empties them. Corroborated by a second campaign on five further seeds with no
  cross-talk. One-value models score 0.7554-0.8199 against 1.0000.

  On an aborting cell `cap` is always below -54 and is only ever consumed
  through `max(-54, min(ladder, ·))`, so "the whole window's minimum", "the
  aborting sample" and "any sentinel at or below -54" are a PERMANENT tie
  rather than an open measurement; so is whether the abort short-circuits the
  anchor read.

  **Why the previous two campaigns got an exact 1.00000 for a wrong law.** On a
  TWO-valued psl field the aborting prefix-minimum is identically the point
  read, so a corpus that varies psl's spatial pattern and never its values
  cannot tell them apart. A three-valued field separates them immediately: of
  425 cells where the minimum is the low arm and the point read is the high
  arm, 258 return the MIDDLE arm, which neither model predicts. The same trap
  had already been named one level up — "sweep values, not frequencies" — and
  it caught a second instance of itself.

  Rounding is `floor` toward negative infinity: 3475/3475 against round 0.4855,
  ceil 0.0000, and truncation toward zero 0.9209, failing exactly on the
  negative values. The anchor's division is `floorDiv` for the same reason, and
  it is invisible in the origin quadrant — which is the only place any probe
  ever looked, because `tools/analysis/density-probe.sh` hardcodes a forceload
  of `0 0 127 127`. **That hardcoding hid the sign question for 1370
  dimensions and deserves an `--origin` flag before the next campaign.**
  Off-origin the two part decisively: floorDiv 1.0000 on 1294 exact readings
  against truncation 0.16-0.55.

  Neither `4` nor `16` is the noise cell: `size_horizontal` 2 and 4 leave both
  unchanged, cell-for-cell identical with 0 differing of 836.

  *The `cy < -54` guard is neither confirmed nor refuted.* One agent proposed
  replacing it with a psl-relative `cy < psl + 21`, fitted from a single
  dimension at psl = -64; its verifier ran constant-psl dimensions at -40, 0,
  60 and 120 and found the plain near-surface rule `psl - cy < 4` at all four,
  with no guard term. Four values beat one, and the one sits in the anomalous
  corner — which is `psl <= min_y`, the regime this SPEC already refuses. The
  guard stays as written; whether anything special happens below the lava sea
  is unresolvable until the horizontal read is.

  *Instrument findings worth more than the results.* Chunks are forceloaded, so
  water ticks after generation and flowing water can become a source, lifting a
  cell's apparent top — counting non-source water put "non-flat" cells at
  50-63% and drove every candidate to 0.53-0.65. **`default_fluid` of
  `minecraft:packed_ice` fixes it at the source**: the aquifer places it exactly
  where it places water (964/964 identical brackets) and nothing flows; the same
  world gives a +2 wobble on 99 cells with water and zero with ice. Suppressing
  barriers to free up evidence is the opposite of helpful — `barrier` = -50
  gives 1355/1371 with 16 anomalous cells, `barrier` = +2 gives 1380/1380.
  And a probe dimension whose name contains an UPPERCASE letter is silently
  dropped by the server as an invalid path, after which the harness times out
  on an empty world.

  **The one-block residual, localised (M3).** With aquifers out of the way,
  1.71% of columns are off — 16104 of 16384 exact over a full region — every
  one of them by exactly one block. This entry previously recorded that it
  "does not correlate strongly with the cell lattice" and that "an
  interpolation error would vanish at the corners, and this does not". **Both
  are wrong, and the opposite is true.**

  The earlier measurement bucketed disagreeing COLUMNS by the y offset of their
  height. Bucketing BLOCKS instead — every block in the region compared against
  what the server actually wrote, 741376 of them rather than one per column —
  gives:

  | y offset in the cell | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
  |---|---|---|---|---|---|---|---|---|
  | disagreements | **0** | 12 | 32 | 14 | 15 | 25 | 37 | 13 |

  Offset 0 is the cell boundary — the one y where `minecraft:interpolated`
  returns its argument rather than a blend — and it is clean over 94208 blocks.
  Under a uniform distribution 18.5 would be expected there; the probability of
  zero is about 2e-9. The x and z offsets show nothing: 0.016% to 0.024% across
  all four, with no ordering.

  *And it is not an absence of close calls.* At offset 0, 5029 blocks carry a
  density within 1e-2 of zero and 515 within 1e-3, against 5067/512 to
  5782/577 at the other seven. The exposure is identical; only the boundary
  never comes out wrong.

  *Nor is the cell height wrong*, which would produce the same shape by making
  our lattice coarser than vanilla's. Scored over the same blocks: height 8
  disagrees on 148, and 4, 16 and 2 on 9512, 22232 and 11992. The documented
  `size_vertical * 4` is right.

  *Nor is it the blend order*, which `density_interpreter.cpp` flags as
  unverified. Reassociating three lerps moves the last bits, of order 1e-16;
  the disagreements run to 7.8e-2.

  **It is two populations, not one**, and 145 of the 148 are one-directional —
  we call air what the server made solid.

  * **124 small**, |density| mostly under 1e-3, y 17..47. Ordinary one-block
    surface shifts; the disputed block is usually `minecraft:gravel`, which is
    simply what the surface rule paints on a sea floor.
  * **24 large**, |density| up to 7.8e-2, all at y 17..21, all `minecraft:stone`
    with **water directly below**. That is a cave roof, not the surface, and a
    7.8e-2 sign error is three orders of magnitude past rounding.

  `cache_all_in_cell` would explain the shape exactly — a value computed once
  per cell agrees at the corner and drifts inside — but the overworld's chain
  does not contain one: 15 `cache_2d`, 12 `cache_once`, 16 `flat_cache`, 8
  `interpolated`, and no `cache_all_in_cell` anywhere.

  So what remains is that our interior values are a lerp of two corner values
  the server agrees with, and the server's interior values are not that lerp.
  The next instrument has to read the server's own density BETWEEN cell
  boundaries: a datapack probe whose `raw_final_density` wraps vanilla's
  `final_density` in a `range_choice`, bisected across dimensions, read at
  positions where the indicator is exact. `golden_terrain_no_aquifer_test.cpp`
  pins the offset-0 claim so that a change here has to move it deliberately.

  `tests/conformance/golden_terrain_test.cpp` pins a cheap sample so the
  number has to move deliberately; it samples every eighth column across 8x8
  chunks rather than filling one chunk, because a single chunk of seed 42
  comes out 256 of 256 and would have hidden the residual entirely.

  **CORRECTION. "Zero at offset 0" does not hold, and the reason it looked
  that way is the same failure mode this project keeps finding: the 94208
  figure came from `golden_terrain_no_aquifer_test.cpp`'s own sampling — every
  eighth COLUMN, 256 of the region's 16384 — carried through to the block-level
  bucketing. A FULL scan of every column, at least across a realistic
  elevation band, finds 322 disagreements rather than 148, including 35 more
  at offset 0 alone. The earlier "probability of zero is about 2e-9" was
  correct arithmetic on an incomplete sample; it was never a property of the
  formula.

  **The next instrument — the wrapped `range_choice` — is now built**
  (`tools/analysis/final-density-probe.sh`) and VALIDATED before being trusted:
  `raw_final_density` is set to vanilla's own `final_density` tree, copied
  verbatim out of the extracted fixtures, wrapped in one `range_choice` whose
  output becomes the probe's own solid/air decision — so a bisection of the
  threshold across dimensions reads the SERVER's real density at any point
  directly, not just its sign. At a corner this build already agrees with
  vanilla on (8, 40, 124), the server's own value bisects to
  0.3095–0.30951 — matching this build's computed 0.3095045697 to five
  digits. The technique is faithful.

  **And at three of the NEW offset-0 disagreements, it is decisive: this
  build's own corner computation is wrong, not merely the interpolation
  between corners.** All three sit on the same lake floor, at y=48 — one
  cell above (6, 24, 8, 40, 124), a corner already confirmed correct on the
  very same column:

  | column | this build | vanilla (bisected) | gap |
  |---|---|---|---|
  | (8, 48, 124) | +0.0003472549 | [-0.002, -0.001) | 0.0013–0.0023 |
  | (6, 48, 120) | +0.0013010066 | [-0.0015, -0.001) | 0.0023–0.0028 |
  | (24, 48, 113) | +0.0008666713 | [-0.002, -0.0015) | 0.0024–0.0029 |

  Every one of these is a CORNER — offset 0, where `interpolated` returns its
  argument verbatim, no lerp involved. This build calls each barely solid;
  vanilla's own value is comfortably negative, off by roughly 2–3
  thousandths, three orders of magnitude past the 1e-16 blend-order noise
  already ruled out. **So the premise this project worked from — "our
  interior values are a lerp of two corner values the server agrees with" —
  is not the whole picture: at least this population is wrong at the corner
  itself**, before any interpolation runs at all. It shares nothing with the
  already-documented 24-large cave-roof population's DIRECTION — that
  population has this build reading LOW where vanilla reads high (we call
  air what the server made solid); this lake-floor population has this
  build reading HIGH where vanilla reads low (we call solid what the server
  left as water) — the opposite sign, and, since it is a corner rather than
  an interpolated point, a different mechanism.

  **NARROWED, via the wrapped-probe technique extended to bisect a NAMED
  SUB-function directly rather than only `final_density` as a whole** — the
  probe script now accepts an optional target `function`, so a wrong value
  can be chased down through `final_density`'s own reference tree, one named
  function at a time, in the same server run.

  *The noodle-cave branch is ruled out.* `final_density`'s outer `min` picks
  between the terrain tree and `minecraft:overworld/caves/noodle`; at every
  tested point — wrong and correct alike — the noodle branch's own value
  (0.4 to 64.0) is nowhere near the minimum, so it is never what gets
  selected. Whatever is wrong lives in the terrain branch.

  *`sloped_cheese` itself is wrong at all three lake-floor corners and right
  at the control*, bisected directly: this build's 0.0003–0.0013 (SOLID)
  against vanilla's confirmed-negative values at all three, and this
  build's 1.0015 matching vanilla's own bisected 1.0014–1.0015 at the
  control. Inside `sloped_cheese`, `jaggedness` reads exactly 0.0 and
  `factor` reads exactly 3.9500000477 at all four points — identical,
  ruling both out as the differentiator — which reduces the search to
  `depth` and `base_3d_noise`.

  *`depth` matches vanilla exactly at all four points, bisected directly* —
  this build's own computed values land inside the server's bisected
  bracket every time, including at the two lake-floor corners that share an
  identical depth value despite being different columns. `depth` is ruled
  out.

  ***`base_3d_noise` — `minecraft:overworld/base_3d_noise`, i.e.
  `old_blended_noise`'s Modern reading — is the source.*** Bisected
  directly at all three lake-floor corners: wrong at every one, by 0.0013
  to over 0.02. And it is NOT specific to this lake or to y=48: three more
  corners picked for scatter — two far-away columns, one at y=24 which no
  earlier measurement had touched — bisect to TWO more clear mismatches
  (y=48 at a second, unrelated column; y=24) against three matches (y=32,
  y=40, y=56). y=48 disagreeing at two independent, unrelated columns rules
  out "this lake" as the explanation; y=24 also disagreeing rules out "y=48
  specifically." **This reads as a genuine, non-trivial-rate disagreement in
  the Modern reading itself, not a location- or elevation-specific one.**

  This CORRECTS "settled above" a few paragraphs down: `old_blended_noise`
  was derived and landed, not verified end-to-end. `blended.hpp`'s own
  header already flagged the gap without anyone connecting it to this
  residual: "`smearScaleMultiplier` is not [checked]... where the number
  enters the formula is a guess this code makes and does not verify." This
  investigation is the first evidence that guess is measurably wrong rather
  than merely unconfirmed.

  **RESOLVED. The clean-room provision (§12) supplied exactly the tool the
  wrapped-probe technique could not be: a way to see inside a single
  compiled function.** `spec/blended-noise-spec.md` (run 02, differentially
  verified against the unmodified jar rather than merely read) gives the
  fold formula in full. Comparing it line by line against
  `lib/src/perlin.cpp`'s `PerlinNoise::sample` found one concrete
  difference: Q2.2/Q2.3's `fold = ⌊clamp_source/d + 1e-7⌋ · d` carries an
  epsilon before the floor that this build's fold did not have.

  Every other piece of the fold this build already had matched the spec
  exactly on inspection — the cap formula, the `c ≥ 0` condition (vacuously
  satisfied here since the Modern reading's cap is never negative above
  y=0), which axis the interpolation weight uses (unfolded, matching this
  build's existing `fadeY`) — which is why one line was the whole fix
  rather than a rewrite.

  **Confirmed independently of the spec's own claim, not merely adopted on
  its word (§12's rule).** Adding the epsilon and re-bisecting all five
  known corners via `tools/analysis/final-density-probe.sh`: the three that
  were wrong (the lake-floor cluster) now land exactly inside the server's
  own bisected bracket; the two that were already right (the scattered
  control corners) are bit-for-bit unmoved. A full exhaustive block-level
  rescan of the same fixture that found 322 disagreements now finds **0**.
  `tests/unit/blended_test.cpp` pins all five corners as known-answer
  vectors, cited to both the spec claim and this confirming measurement.

  **This was the entire M3 one-block residual, not a fraction of it.**
  `golden_terrain_no_aquifer_test.cpp` moves from 253/256 exact columns to
  256/256; `golden_terrain_test.cpp`'s seed -1 section moves from 249 to
  252 of 256, with the remaining four an entirely separate, already-known
  gap — the aquifer fill decision this build does not implement (MA), not
  a density error. Why one missing epsilon explains an error up to 2e-2 in
  the final density rather than something proportional to 1e-7: when it
  matters, the floor lands on the WRONG side of an exact-integer boundary,
  folding onto an adjacent, unrelated lattice cell — a discontinuous jump,
  not a rounding-scale correction. Recorded fully in `lib/src/perlin.cpp`'s
  own comment at the fix.

  *No documentation and no oracle here* — as it was, `end_islands`,
  `old_blended_noise`, `weird_scaled_sampler` and `blend_density`. **All four
  are now settled**, the last of them `end_islands`, whose oracle turned out
  to be sitting on disk unused: eight golden End regions, plus two probe
  regions generated for the two parts of the field they cannot reach (see
  "The End generates, exactly" below). minecraft.wiki documents neither
  `weird_scaled_sampler`'s rarity mapping nor what `blend_density` returns
  outside blending, and cubiomes models neither terrain density nor the End
  islands. Implementing them from memory would produce a world that
  generates and is quietly wrong, which §8 treats as the most severe class
  of bug, so each stayed refused until a golden could tell right from
  plausible — which is exactly how `end_islands`' first, analogy-based
  seeding was caught and replaced.

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

  **What measuring the field settled (MA).** Two of the three are now
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
  `RandomSource::Legacy` throws **when, and only when, the dimension actually
  names a noise** — see "The refusal was four dimensions wide and the gap was
  three" below, which narrowed it. How a noise's *name* becomes an LCG seed is
  not settled here. cubiomes seeds named noises only through Xoroshiro; it
  models the LCG for the blended noise alone, which is a different
  construction with no name hashing. An empty `wanted` has no identifier to
  derive and is built, which is what makes the legacy Nether's terrain and the
  End reachable. The rest of this paragraph describes the state before that
  narrowing: the four legacy dimensions could not be
  built, and `stratum validate` reports each one as a warning and leaves its
  router **unchecked** — deliberately not counted as fifteen failures, since
  "we did not look" and "we looked and it does not work" are different
  claims. That is why the headline count reads 39 of 45 across 3 of 7
  dimensions rather than 94 of 105: fewer entries, and all of them ones we
  can actually speak to.

  **Those headline numbers moved when the refusal narrowed** (below): the
  End needs no named noise, so it is checked like any other dimension and
  `stratum validate` now reads **60 of 60 router entries across 4 of 7
  dimensions**, with three warnings rather than four — each naming the three
  noises it could not build.

  One assumption, stated because it has not been verified: that the flag
  selects the generator for *all* of a dimension's noises rather than some
  subset. That matches a single RandomState being constructed once, but
  nothing here checks it, and if it is wrong this refusal is too broad.

  **It is wrong for at least one thing, and that is now measured.**
  `minecraft:end_islands` builds a simplex field of its own, and a
  `legacy_random_source` probe dimension and its flag-off control read that
  field identically on 1024 of 1024 shared columns, at two seeds
  (`tools/analysis/end-islands-analyze.cpp`). So the flag does not reach it.
  The assumption stands for everything a `worldgen/noise` identifier names;
  it does not stand for a source a density function builds for itself.

  **And "what the refusal is actually about" is no longer only noises —
  which is the correction this paragraph needed most.** After the narrowing,
  `NoiseRegistry::create`'s predicate is "this dimension names a
  worldgen/noise", and a legacy dimension draws on its declared random source
  for three things that are not noises and pass through no `wanted` list:
  `minecraft:vertical_gradient`'s `random_name`, the aquifer lattice's centre
  jitter, and the ore-vein source. This build derives all three with
  Xoroshiro128++. So the noise predicate alone is not enough, and each of the
  three is refused by name, by THROWING, from `ChunkFiller::compile` — the
  path every real dimension takes — and the gradient also from
  `surface::Executor::compile` for a caller that reaches it directly (SPEC
  §8: the construct is named, not merely the dimension).

  Two shapes were wrong on the way here and are recorded because each let a
  constructed pack through. The gradient refusal was first landed as a
  non-fatal entry in `ChunkFiller::surfaceRulesBlockedBy()` — the shape that
  list has for "a caller did not supply something" — and nothing on the
  public `CompiledDimension::compile` path consults that list, so a legacy
  End doctored with a gradient compiled clean through it and filled 13549
  blocks with its surface rules silently dropped. It throws now, and
  `vanilla_legacy_gradient_gap_test.cpp` drives exactly that doctored pack
  through `CompiledDimension::compile` and asserts the throw, beside the
  undoctored End compiling on the same path as the control. And the ore-vein
  refusal first sat inside the `veinsPlaceBlocks` gate, which is
  `ore_veins && aquifers`, so a legacy pack with veins ALONE was not refused
  — materially inert, since with aquifers off no vein random is ever drawn
  and the chunk is bit-identical to the undoctored one (measured), but
  `validatePack` warns on the flag alone, so validate and generation
  disagreed about one pack. It now refuses on the flag.

  *`vertical_gradient` under a legacy source is MEASURED not to be
  Xoroshiro.* `tests/conformance/vanilla_legacy_gradient_gap_test.cpp` scores
  this build's Xoroshiro-derived gradient against the golden **Nether's** own
  bedrock, over the four probabilistic levels of each of its two gradients,
  on 8x8 chunks of `r.0.0` — 65536 labelled positions per gradient per seed.
  It lands on the chance band — 39321.6 of 65536, computed from the anchors as
  sum p² + (1−p)² rather than assumed — at all four seeds, floor and roof
  alike:

  | seed | `bedrock_floor` | `bedrock_roof` |
  |------|-----------------|----------------|
  | 0    | 39303 / 65536   | 39543 / 65536  |
  | 1    | 39411 / 65536   | 39464 / 65536  |
  | -1   | 39419 / 65536   | 38912 / 65536  |
  | 42   | 39172 / 65536   | 39237 / 65536  |

  The **control is in the same file on the same seeds**: the identical code
  on the modern overworld's bedrock floor is **65536 of 65536 at all four**.
  So the shortfall is the seeding, not this build's gradient, region reader
  or anchor resolution. The full per-level ladder is asserted too — 100% at
  the certain anchor, 80/60/40/20% across the band, 0% at the impossible one
  — so the band being scored is pinned to the band the anchors name. Only
  gradients that place bedrock DIRECTLY are scored, which excludes the
  overworld's `minecraft:deepslate` gradient: it sits under further
  conditions, so its outcome is not readable from the blocks. That exclusion
  is a property of the tree's shape, decided before any block is read.

  *The aquifer and ore sources are structurally identical and UNTESTED.*
  Both draw a positional random from the same primitive under the same
  declared source. Every vanilla legacy dimension has `aquifers_enabled` and
  `ore_veins_enabled` false, so **no oracle for either is on disk** and none
  can be generated from vanilla data alone. They are refused on the
  structural argument — same source, same primitive, one member of the class
  measured wrong — and not on a measurement of their own. That is the
  boundary: one construct measured, two argued.

  *What this closed.* A legacy dimension naming no noise whose surface rule
  used vanilla's bedrock-floor gradient used to compile clean
  (`runsSurfaceRules()` true, `surfaceRulesBlockedBy()` empty) and fill a
  chunk with bedrock from a derivation measured above to be at chance — no
  error, no warning. That is the plausible-but-wrong world §8 forbids. It is
  closed on BOTH paths: `ChunkFiller::compile` throws, and the public
  `CompiledDimension::compile` is driven with the doctored pack and asserted
  to throw. The End is unaffected and still passes: its tree has no
  `vertical_gradient` and both flags are false, asserted in the same test,
  and it compiles through the public path as the control. A doctored MODERN
  overworld with the identical gradient still generates — the refusal keys on
  the legacy source, not on the construct.

- **A legacy dimension's `old_blended_noise` is settled; its named noises are
  not (M4).** `minecraft:old_blended_noise` carries no identifier, so it is
  the one noise a `legacy_random_source` dimension can seed without first
  answering how a *name* becomes an LCG seed. It is now measured off the
  server, implemented as `BlendedNoise::legacyFromWorldSeed`, and guarded by
  `tests/conformance/vanilla_legacy_blended_test.cpp`.

  *The rule.* `new java.util.Random(worldSeed)` handed straight in — no
  positional fork, no name salt, nothing derived — with the three octave
  stacks drawn in the order `BlendedNoise::legacy` already draws them, and
  the value read the **modern** way rather than the pre-1.18 way. The two
  readings differ by exactly 128x at y = 0.

  *The measurement.* `tools/analysis/legacy-blended-probe.sh` puts the same
  `old_blended_noise` (vanilla's own overworld parameters) into four
  dimensions of one world — a legacy and a modern dimension at each of two
  output scales — and `density-probe.sh`'s `flat_cache` + gradient inversion
  turns every cell-corner column's terrain height into a reading of the noise
  at (x, 0, z). `tools/analysis/legacy-blended-analyze.cpp` scores it. Across
  three world seeds (42, 0, -4172144997902289642), **13824 of 13824 columns**
  in the legacy dimensions, every one to within half a block, which is the
  floor of what the readback resolves. The denominator is generated columns:
  2304 of each dimension's 7056 cell corners, the other 4752 being chunks the
  server never built around the forceloaded square. The exclusion is geometric
  and carries no value-dependent bias — the analyzer reports it as `sat 0,
  empty 4752`, so no column is dropped for what it read. The mirror runs the other way in the
  same worlds: the modern derivation scores 2304/2304 in the flag-off
  dimensions and 33-36/2304 (fine) or 112-135/2304 (coarse) in the legacy
  ones, and `BlendedNoise::legacyFromWorldSeed` scores exactly those numbers
  in the flag-off ones — so neither is an artefact of the readback. The
  pre-1.18 reading, which shares every draw with the winner, scores 0-6 of
  2304 everywhere.

  *State the loser's score with its band or do not state it.* Those two
  rival numbers are the same rival in the same worlds; the only difference is
  the output scale, which sets the width of the agreement band — 0.00372 at
  scale 2.0 and 0.01488 at 0.5. A count with no band attached is not a
  quantity. An earlier write-up of this work gave the modern derivation's
  range on legacy dimensions as "8-65/1024" as though it were one number; it
  is a function of the readback, and a later run of the same comparison
  measured it below the bottom of that range.

  It was left reachable but unreached — `Interpreter` selects it on a Legacy
  registry, and `NoiseRegistry::create` refused a Legacy source before any
  graph was compiled. **It is reached now, with an empty registry**, and
  every passage that said otherwise has been corrected (`blended.hpp`,
  `density_interpreter.cpp`). The refusal narrowed (next entry), and the
  End's and the Nether's `final_density` name no noise, so this rule is what
  draws their terrain: the End's 25165824 exactly-matching blocks and the
  Nether's 15465864 of 15466496 block classes (99.99591%) are this seeding
  running in a real pipeline rather than in a probe dimension. Both
  denominators cover eight golden regions over **six independent worlds** —
  `java.util.Random` discards bit 63, so seed 0 is the same world as
  `Long.MIN_VALUE` and -1 the same as `Long.MAX_VALUE`, and the server
  confirms it: each colliding pair's regions differ in 0 of 2097152 blocks.

- **The refusal was four dimensions wide and the gap was three (M4).** The
  unsolved named-noise derivation above used to refuse every
  `legacy_random_source` dimension, before `wanted` — the list of noises the
  dimension asks for — was consulted at all. That was measured, and the
  refusal narrowed to fire only on a non-empty `wanted`.

  *What vanilla's four legacy dimensions actually name.* Walked from the
  pinned pack and pinned, per dimension and per router entry, by
  `tests/conformance/vanilla_legacy_named_noises_test.cpp`:

  | dimension          | router entries naming anything | router names | surface names spelled | surface noises needed |
  |--------------------|--------------------------------|--------------|-----------------------|-----------------------|
  | `end`              | none                           | 0            | 0                     | 0                     |
  | `nether`           | `temperature`, `vegetation`    | 3            | 6                     | 8                     |
  | `caves`            | `temperature`, `vegetation`    | 3            | 7                     | 9                     |
  | `floating_islands` | `temperature`, `vegetation`    | 3            | 7                     | 9                     |

  The last two columns differ on purpose. A rule tree SPELLS a noise only in a
  `noise_threshold`; it also NEEDS `minecraft:surface` and
  `minecraft:surface_secondary` wherever a surface depth is read and
  `minecraft:clay_bands_offset` wherever `bandlands` is placed, and none of
  those three appears in any field (`surface::requiredNoises`). The registry
  is asked for the second number, so that is the one the refusal fires on.

  Every terrain-shaping entry of all four — `final_density`, `depth`,
  `erosion`, `ridges`, `continents`, `barrier`,
  `preliminary_surface_level`, the two `fluid_level` entries, `lava` and the
  three vein entries — names nothing at all. So the derivation blocks biome
  climate and surface rules in three dimensions and **nothing whatsoever in
  the fourth**.

  *The three, not two.* The round that opened this work was given a table
  built by scanning each `noise_settings` document for `"noise": "<id>"`
  fields, which says `temperature` and `vegetation`. It undercounts, twice
  over: `shift`, `shift_a` and `shift_b` spell their noise field
  `"argument"`, and the climate entries reach them through a *reference* to
  `minecraft:shift_x` / `shift_z`, which live in another file. The third name
  is `minecraft:offset`. It changes nothing about the narrowing — three is as
  non-empty as two — and everything about what the refusal's message should
  say, which is why the message now lists the names.

  *What had to change with it.* `Graph::reachableFrom` /
  `noisesReachableFrom` walk a dimension's own router entries rather than the
  whole pack (one graph holds every dimension at once, so "what this pack
  names" was never the question) — ONE walk in `lib/`, with a single-root
  form, a noise filter over it, and a multi-root union over that, so
  "reaches" has one definition;
  `surface::requiredNoises` centralises the three names no condition spells;
  and `Interpreter` no longer refuses at construction for a noise some *other*
  dimension's node names — it refuses that node, by name, when a walk
  actually reaches it, which `ChunkFiller::compile` does for every root it
  will sample. Same loudness, same name, still before a block is filled
  (§8).

- **The End generates, exactly, and `end_islands` is settled (M4).** With the
  refusal narrowed, vanilla's End builds its (empty) registry and runs. It is
  scored block by block against the goldens the way `golden_fill_test.cpp`
  scores the overworld, and it comes back **exact**:

  | fixture                            | seeds | blocks per seed | exact |
  |------------------------------------|-------|-----------------|-------|
  | golden `end/r.0.0`, chunks 0..7    | 8*    | 2097152         | all   |
  | probe `r.64.0`, chunks 2048..2055  | 2     | 2097152         | all   |
  | probe `r.-1.-1`, chunks -8..-1     | 2     | 2097152         | all   |

  \* Eight golden regions, **six independent worlds** — `java.util.Random`
  discards bit 63, so 0 is the same End as `Long.MIN_VALUE` and -1 the same as
  `Long.MAX_VALUE`. The server confirms it rather than the arithmetic being
  taken on trust: each colliding pair's regions differ in 0 of 2097152 blocks,
  which is itself evidence that nothing in the End's terrain is seeded from
  outside those 48 bits. Read every End and Nether denominator against six.

  25165824 blocks in total, every one of them. The probe regions are
  `tools/analysis/end-islands-probe.sh`'s, and they exist because r.0.0 cannot
  reach two whole parts of the field (below).

  *Eight regions, SIX distinct worlds — say the denominator.* `java.util.
  Random` scrambles its seed as `(seed ^ 0x5DEECE66D) & ((1 << 48) - 1)`, and
  that mask discards bit 63, so 2^16 world seeds share every End. Two pairs in
  the fixed golden set collide: 0 with LONG_MIN, and -1 with LONG_MAX. Not
  inferred — the SERVER says so, and the fact is worth having on its own: the
  golden End regions of each colliding pair differ in **0 of 2097152 blocks**,
  which is an independent confirmation that nothing in the End's terrain is
  seeded from anything outside the 48 bits the LCG keeps. Both rows stay in
  the table because the collision is a claim about this build too: spell the
  scramble wrong and the pairs stop agreeing.

  *What it took.* Two things, one of them a bug in code the overworld had
  already exercised:

  **`minecraft:end_islands`, implemented.** A radial central-island term plus
  an outer term gated on a simplex field, mapped to density as
  `(height - 8) / 128` on a `/8` grid. Refused until now for want of an
  oracle (§11's own "no documentation and no oracle" entry named it); the
  goldens are that oracle.

  **The surface-rule scan did not start at the sky, and had for two
  milestones.** `ChunkFiller::applySurfaceRules` walked every column from the
  world ceiling down. It now starts at the column's topmost NON-AIR block
  (fluid counting as non-air, so an ocean column still starts at its water
  surface and the ice case is untouched). Invisible in every overworld
  fixture, because vanilla's overworld tree is gated top to bottom and fires
  nothing up there; catastrophic the moment a tree is not gated. The End's
  entire `surface_rule` is one unconditioned `block` placing end_stone, and
  without the bound it painted end_stone from the island's surface to y 127 in
  every column: **441481 of 2097152 blocks at seed 0, against 2097152 of
  2097152 with it.**

  *The two holes r.0.0 leaves, and why they needed their own fixtures.* The
  outer term is gated on `cellX^2 + cellZ^2 > 4096` with the cell being
  `blockX / 16`; inside r.0.0 even the far corner of the +/-12 cell
  neighbourhood reaches 43^2 * 2 = 3698, so the term cannot fire once. And
  r.0.0 is entirely non-negative, so it cannot tell `floorDiv` from Java's
  truncating `/` — which disagree for the *central* term too, since
  `floorDiv(-1, 8)` is -1 where truncation gives 0. `r.64.0` closes the first
  and `r.-1.-1` the second; floorDiv is what the second says.

- **The Nether's TERRAIN is unblocked too, and measured (M4).** The narrowing
  is not only about the End. The Nether's three names all come from its
  `temperature` and `vegetation` router entries and its surface rule; its
  `final_density` names **none**, so a registry built for the terrain alone
  builds, on a Legacy source, today.

  `tests/conformance/vanilla_legacy_nether_terrain_test.cpp` is the single
  case for this claim — an earlier round had two, one scoring 524288 blocks
  of 4x4 chunks and one scoring every fourth chunk of the whole region, and
  the second subsumes the first's denominator. It scores every fourth chunk
  of all eight golden Nether regions, y in [5, 123): **15465864 of 15466496
  block classes, 99.99591%**. Eight regions, **six distinct worlds** — the
  same 48-bit collision the End's table notes, computed in the test from the
  LCG's own scramble rather than asserted in prose.

  The surface rules are NOT run: the tree spells six noises and NEEDS eight
  (`minecraft:surface` and `minecraft:surface_secondary` on top), none of
  which can be seeded, and §8 refuses a tree whole rather than running it
  with branches skipped. (It is now refused for a second, independent reason
  as well — its two `vertical_gradient` conditions, above.) So block
  identity cannot be exact and is not asserted. Three things are asserted
  instead, and together they are stronger than a percentage:

  * the residual's SHAPE: longest disagreeing run down any column is **1**,
    it is one-directional (`solid` where vanilla has `lava`, never the
    reverse, air never involved), and all of it sits in y [20, 30];
  * the residual's SIZE against the right denominator: **632 of 226702**
    solid-to-fluid boundaries in the scored band, 0.279% — not 632 of 15.4
    million, which would make it look like noise;
  * **every** block that differs at all is one the Nether's own surface rule
    tree can place — and that set is read out of the resolved tree rather
    than written into the test, so a disagreement the surface rules cannot
    explain fails by name rather than hiding under a tolerance. The
    category-crossing ones are the tree's single `minecraft:lava` placement
    under a `hole` condition.

  *And the trivial baseline is stated, because 99.99591% against nothing is
  not a result.* A stub that ignores the density chain and answers "solid"
  everywhere scores **9792427 of 15466496 = 63.31%** — measured in the same
  file and pinned there. So the result is 36.7 points above the trivial
  answer. (A first guess put the baseline near 88%; it is not, because the
  scored band excludes the two bedrock bands and what remains is the
  cheese-and-lava interior.) The boundary counter behind the 226702
  denominator was corrected in the same pass — it tracked solidity only
  inside the scored band, so the band's first layer was compared against an
  assumption — and the count did not move, which is what shows the
  assumption happened to hold for this dimension.

- **`end_islands`' simplex is seeded by a 17292-step skip, and not by the
  dimension's random source (M4).** The seeding was initially carried over by
  analogy from `BlendedNoise::legacyFromWorldSeed` — the world seed straight
  into `java.util.Random`. That is wrong, and terrain could not say so: scored
  against the far probe it reached 87.9% of blocks at seed 0 against an 85.7%
  base rate for predicting pure void, which is to say the island positions
  were uncorrelated with vanilla's.

  *The instrument.* `tools/analysis/end-islands-field-probe.sh` reads the
  field itself rather than the terrain built on it — `density-probe.sh`'s
  `flat_cache` + gradient inversion, at a window 32768 blocks out, which
  needed `density-probe.sh` to grow an `--at CHUNK_X,CHUNK_Z` (every probe
  before this one generated at the origin, and the outer term does not exist
  there).

  *The rule.* `new java.util.Random(worldSeed)`, then **17292 LCG steps
  discarded**, then the ordinary three-doubles-and-a-permutation
  construction. 1024 of 1024 columns at seed 0 and 1024 of 1024 at seed 42,
  each to within half a quantum — 0.3 blocks of island height at the fine
  scale. The 17292 is a literal with nothing deriving it; it is what the
  measurement says.

  *And it ignores `legacy_random_source`.* Each probe world carries the
  function in a legacy dimension and a flag-off control. They read the same
  field on 1024 of 1024 shared columns, at both seeds. That is a fact about
  the flag as much as about this function, and it is recorded against the
  refusal's own "one assumption" above.

- **The named-noise derivation: searched, still open, and now reproducible
  (M4).** How a noise's identifier becomes a Java LCG seed is unanswered.
  What changed is that the search is in the repository rather than in a
  memory of it — `tools/analysis/legacy-seed-probe.sh` generates the worlds,
  `tools/analysis/legacy-seed-analyze.cpp` scores candidates against them,
  and `density-probe.sh`'s `legacy_random_source` is a per-entry field of a
  spec (it was hardcoded `False`, which meant not one measurement of this
  kind could be regenerated from the repository), with a `<spec>.noises.json`
  sidecar so a spec ships the noises it names.

  *What was scanned, exactly, BEFORE the widening below.* The space in this
  paragraph and the two after it is the one that stood until the stack-shape,
  frequency, per-octave-salting and declaration-order axes were added; the
  entry **"The stack rule was the suspect, and it is now searched"** records
  what replaced it, and re-indexes every rule number quoted here. 900 seed
  rules — 5 bases {world seed;
  `JavaRandom(worldSeed).nextLong()`; the first Xoroshiro draw off the world
  seed; the LCG's own scramble of it; zero} x 10 salt spellings {MD5 of
  "ns:path" as first-eight big-endian, first-eight little-endian, last-eight
  big-endian, last-eight little-endian, lo^hi, lo+hi; MD5 of the bare path,
  first-eight big-endian; `String.hashCode` of the id and of the path; none}
  x 3 combining operators {xor, add, sub} x {0, 1, 2} further LCG forks x 2
  generators {LCG, Xoroshiro} — times 300 block offsets = **270,000
  candidates per dimension**, over 9 probe dimensions and 2 world seeds.
  Nothing reached half agreement on the probe subset in any of them.

  *What it did not cover*, because a refutation is only as wide as its
  space: one stack rule only (every octave's Perlin block drawn in order from
  the single generator — the rule confirmed above for `old_blended_noise`),
  and no frequency variation whatever. An earlier write-up described the
  sweep as covering "frequency 2^(firstOctave +/- 1..3)"; no version of this
  code had swept frequency at all. **Both of those gaps are now closed and
  both came back empty** — see the widening entry below. The `end_islands`
  skip did not close and is now the largest one left.

  *Two gaps the `end_islands` result pointed at. The second is now tested;
  the first is not.*

  **A fixed skip is a shape vanilla uses.** `end_islands`' simplex is seeded
  by `new java.util.Random(worldSeed)` and then **17292 discarded LCG steps**
  — measured, in a dimension declaring the same flag. The scanned space has
  no skip dimension: it varies bases, salt spellings, operators and 0-2
  further *forks*, and a fork (`new Random(random.nextLong())`) is not a skip.

  **The stack rule is the likelier culprit, and it would defeat every seed
  candidate.** A modern `NormalNoise` does not draw its octaves in order from
  one generator at all: it takes two draws once and XORs them with a
  per-octave salt from the MD5 of `octave_<n>`, so an octave can be added or
  removed without disturbing the others (`OctaveNoise::create`). If a legacy
  `NormalNoise` keeps that shape and swaps only the generator and the hash,
  then no seed is right while the stack rule is wrong, and 270,000 candidates
  scoring at the null says nothing about the seed. Test the stack rule before
  widening the seed space again.

  **That instruction was followed and the suspicion did not pay off.** The
  per-octave MD5 salting scheme adapted to the LCG is now enumerated, with
  the String.hashCode spelling beside it, across every base, salt, operator,
  fork count and generator the seed axis carries. Nothing survives. See the
  widening entry below for the denominator.

  *What is newly measured about the question.* The identifier — or the
  construction order, which these fixtures cannot separate — genuinely enters
  the seed. The probes carry `stratum:na` and `stratum:nb`: identical
  parameters, different names, legacy dimensions of one world. Inverted by
  `legacy-seed-analyze <probe> <seed> --twin`, added for this, their fields
  agree on **21 of 2304 columns at seed 42 and 18 of 2304 at seed 31337** —
  0.91% and 0.78%, against the null mean of 0.6-0.8% measured for this band
  above. They are unrelated fields, so a name-blind rule is out. It does NOT
  separate "the name enters" from "the build ORDER enters"; that needs a probe
  whose packs differ in which other noises they define.

  *deepslate's derivation is inside the space and scores at the null.* base =
  `JavaRandom(worldSeed).nextLong()`, XOR the first eight bytes of
  MD5("ns:path"), one further LCG fork — **rule 290** at the baseline stack
  shape and frequency (it was rule 182 before the widening re-indexed the
  seed rules; see below), which `--candidate 290 0` scores by name rather
  than leaving it to be inferred from a survivor list it is absent from. On
  the legacy dimensions at band 0.00372 it gets 10-25 of 2304 (0.43-1.09%) at
  seed 42 and 10-23 (0.43-1.00%) at seed 31337, against a null mean of
  0.61-0.72% — that is the null, not a near miss. Re-measured after the
  widening through the new evaluator, the figures are unchanged, which is one
  more check that the re-indexing named the same rule. This is a second,
  independent refutation of it against the server, in an apparatus that can
  be re-run from the repository.

- **The stack rule was the suspect, and it is now searched: 729,535,800
  candidates, still no survivor (M4).** The entry above named the stack rule
  as "the likelier culprit" and said to test it before widening the seed
  space again. It is tested. `tools/analysis/legacy-seed-analyze.cpp` now
  enumerates four axes it did not have, and the answer on every one of them
  is the same as before.

  *The space, exactly, and it is a cross product rather than a union.* 5
  bases x **12** salt spellings x 3 operators x **4** fork counts x 2
  generators = **1,440 seed rules**; x 3 draw rules x 3 per-octave salting
  rules x 2 octave orders x 2 zero-amplitude rules = **36 stack shapes**; x 7
  frequency offsets (`firstOctave + d`, d in [-3, +3]); x 300 block offsets =
  **108,864,000 candidates enumerated per dimension**. A candidate needing
  two of the new axes at once is inside it, not only the single-axis arms.
  The four axes, and what each adds on its own off the old 900-rule baseline:

  | axis | new values | candidates when it alone is widened |
  |------|-----------:|------------------------------------:|
  | baseline (the old space) | - | 270,000 |
  | (a)+(c) stack shape | 35 | 9,450,000 |
  | of which (c) per-octave salting | 24 of the 35 | - |
  | (b) frequency offset | 6 | 1,620,000 |
  | (d) declaration order | 540 seed rules | 162,000 |
  | full cross product | - | 108,864,000 |

  Concretely: **(a)** the Perlin blocks drawn in order from one generator, or
  one generator per block seeded by the parent's `nextLong()`, or the second
  stack drawn from a generator forked once off the first's seed; the octaves
  consumed lowest-first or highest-first; a zero-amplitude octave consuming a
  block or not. **(c)** each octave's generator seeded `seed ^
  MD5("octave_<n>")` first-eight big-endian, or with `String.hashCode` in its
  place — the modern scheme's shape carried onto the LCG, which is exactly
  what the entry above predicted would defeat every seed candidate. **(b)**
  the declared `firstOctave` moved by -3 to +3. **(d)** the noise's
  DECLARATION ORDINAL in the pack entering the seed, as a salt, as MD5 of its
  decimal spelling, or as the fork count — the axis `--twin` could measure
  but not separate from the name.

  *The denominator is smaller than the enumeration, deliberately.* Both the
  seed rules and the stack shapes are deduplicated against each dimension's
  own noise before scoring: two rules that reduce to the same (seed,
  generator) for this identifier and ordinal are one candidate, and so are
  two shapes whose slot-to-block mapping is the same for this amplitude
  pattern. A 1-octave noise has **13** distinct shapes, not 36 — order and
  zero-consumption cannot reach anything when there is one octave and no zero
  — and counting 36 would have been a free 2.8x. What was actually scanned:

  | dimension | distinct rules | shapes | candidates (seed 42) | candidates (seed 31337) |
  |-----------|---------------:|-------:|---------------------:|------------------------:|
  | `leg_single`, `mod_single` | 866 | 13 | 23,641,800 | 23,641,800 |
  | `leg_twin` | 920 / 926 | 13 | 25,116,000 | 25,279,800 |
  | `leg_multi`, `mod_multi` | 920 / 926 | 16 | 30,912,000 | 31,113,600 |
  | `leg_skip`, `mod_skip`, `leg_skip_q4`, `leg_skip_q16` | 1,234 / 1,258 | 22 | 57,010,800 | 58,119,600 |

  **362,266,800 candidates at seed 42 and 367,269,000 at seed 31337 —
  729,535,800 over both worlds, of which 505,096,200 on the six legacy
  dimensions. Survivors: zero, on every dimension of both worlds.**

  *Against the right denominator:* the previous entry's 270,000 was a
  PER-DIMENSION count, and its total over 9 dimensions and 2 worlds was
  4,860,000. So the widening is **87.6x per dimension on the 1-octave ones,
  211x on the skip ones, and 150x in total** — not the 2,702x that dividing
  the new total by the old per-dimension figure would give, which is the
  mismatched-denominator mistake this section already records once.

  *The search is a cascade, and that changes what "survivor" means.* Scoring
  10^8 candidates on 128 columns each is not affordable, so every candidate
  is first scored on 8 columns spread across the region and only a candidate
  agreeing on ALL EIGHT is scored further. **That screen cannot reject a
  correct candidate** — the control measures the correct rule at 2304/2304,
  so a rule reproducing the readback agrees on all eight by construction —
  but it is emphatically NOT a general high-score search: a candidate
  agreeing on 60% of columns scattered would be dropped, and this scan would
  not report it. Two candidates in the whole sweep got past the screen, both
  on `leg_skip_q16` at seed 31337, the widest band in the probe; both then
  failed the 128-column gate. Everywhere else, zero.

  *So the null is reported twice, in two statistics, with their own
  denominators.* The SCREEN null is over every candidate in the space; the
  PROBE null is the 128-column statistic the pre-widening scan reported and
  the table further down tabulates, measured on a uniform 1-in-N subsample of
  about 65,600 candidates per dimension. They are not comparable to each
  other and quoting one as the other would be the same mistake this section
  already records once.

  | dimension | band | screen null (of 8, all candidates) | screen max | probe null (of 128, ~65,600 sampled) | probe max |
  |-----------|------|-----------------------------------:|-----------:|-------------------------------------:|----------:|
  | the seven at scale 2 | 0.00372 | 0.042-0.069 | 4 | 0.79-1.06 (0.61-0.83%) | 6-9 |
  | `leg_skip_q4` | 0.01488 | 0.229 / 0.269 | 5 / 6 | 3.26 / 3.60 (2.55%, 2.82%) | 19 / 16 |
  | `leg_skip_q16` | 0.05952 | 0.902 / 1.072 | 7 / 8 | 12.99 / 14.39 (10.15%, 11.24%) | 46 / 39 |

  (Seed 42 / seed 31337 where they differ.) The probe null reproduces the
  pre-widening measurement to two decimal places on every dimension — 0.88,
  0.85, 0.79, 0.92, 0.79, 0.82, 1.07, 3.26, 12.99 before against 0.88, 0.85,
  0.79, 0.92, 0.79, 0.82, 1.06, 3.26, 12.99 after at seed 42 — which is one
  more check that the widened evaluator is the same function at its baseline
  point.

  *Every new axis is calibrated, and by two different arguments.* A null on
  an axis means nothing if the scan cannot find an answer that lives there,
  and nothing either if the "axis" has only one point on it. Both are
  measured:

  **Plants, for findability.** `--plant-axes` synthesises the server's
  readings from a candidate that is off the baseline in one new axis, puts
  them through the same block quantisation, and runs the identical scan. Five
  plants — a stack shape (Xoroshiro rule 657, shape 24,
  `secondStackFork/noOctaveSalt/forward/skipZeros`), a per-octave salting
  (LCG rule 288, shape 4, `sequential/md5Octave/forward/skipZeros`), a
  frequency offset (rule 288, delta +2), an ordinal-seeded rule (rule 530,
  `lcgLong xor md5Ordinal, forks 1`), and one candidate off the baseline in
  three axes at once (rule 657, shape 28, delta -2) — run on all six legacy
  dimensions of both worlds is **60 plants, and all 60 came back at rank 1 at
  2304/2304 columns**, against 23.6M to 58.1M rivals each. 59 of the 60 were
  the sole survivor. The exception is the one place a runner-up survived at
  all: `leg_skip_q16`, the widest band in the probe, where the three-axis
  plant left 23 candidates past the 8-column screen and one of them reached
  668/2304 (29.0%) — still 3.4x below the plant's 100%, and 29.0% is inside
  that dimension's own null maximum of 36.7-40.6% recorded below. It is a
  CTest
  (`conformance.legacy_seed_control_tool`), run there on `leg_skip` per world
  because that is the only dimension whose noise has a zero amplitude and so
  the only one where the order and zero-consumption sub-axes are not
  degenerate; the full six-dimension run is this entry.

  (The five plants share one pass over the space rather than taking five. A
  planted reading is the same lattice with different values, so a candidate is
  evaluated once per screen column and compared against each plant; building
  the space is what costs. Measured: 95s to 20s for one dimension at seed 42,
  same five ranks.)

  It also caught a bug that would have read as a result: the ordinal plant
  first came back at rank 0 while being *perfectly recovered*, because the
  scan reports the LOWEST rule index that produces a candidate — it collapses
  duplicates before scoring — and the plant had been named by a
  higher-numbered alias. Rank is now decided by what the indices MEAN (the
  resolved seed and generator, and the shape's whole slot-to-block mapping),
  not by the indices.

  **Perturbations, for non-degeneracy.** A plant cannot catch an axis that
  has one point on it: if reversing the octave order changed nothing, the
  plant would still return rank 1 and the scan would have searched one
  candidate while reporting several.
  `tests/conformance/vanilla_legacy_seed_control_test.cpp` therefore perturbs
  the rule known to be right, on the mirror dimensions where it recovers
  every column, with its own independent forward model. That model
  reproduces `NormalNoise` bit for bit on **13,824 of 13,824** columns
  first — without which the perturbations perturb something else — and then
  every perturbation must fall away from it. Reversed octave order and all
  six frequency offsets land at **6-24 of 2304 (0.26-1.04%)**, which is the
  null.

  *One perturbation does not, and it is a real limit rather than a failure.*
  Exchanging which stack's blocks feed which stack reaches **80-122 of 2304
  (3.47-5.29%)** on the 3-octave and skip mirrors and **34 of 2304 (1.48%)**
  on the 1-octave ones, at both seeds — above the null everywhere. The two
  stacks of a `NormalNoise` are the same octaves at coordinates scaled by
  337/331, 1.8% apart, so the sum is very nearly symmetric under exchanging
  them and a swapped field stays partly correlated with the true one instead
  of becoming an independent draw. It is still 19x below a recovery, so the
  axis is not degenerate; but it is the sharpest limit measured on how well
  this readback can separate two stack shapes that differ only in which stack
  a block feeds, and a shape that differed from the truth *only* that way
  would be harder to refute than the other numbers here suggest. It carries
  its own ceiling in the test rather than being folded into the null's.

  *And the widened evaluator is the validated one.* The scan and the plants
  no longer run `sampleNormal`: they run a table-driven evaluator that
  resolves a stack shape and a frequency offset before summing, which is what
  makes 10^8 candidates affordable. `--control` gained an arm requiring that
  evaluator to reproduce `sampleNormal` **bit for bit at the baseline point**,
  on every column of every dimension: **20,736 of 20,736 per seed, 41,472
  over both**. Without it the widening would have moved every number this
  section reports onto code the control never touched.

  *What is STILL not covered, after the widening.* The list is shorter and it
  is not empty. The first item used to be billed as the largest gap; it was
  re-measured, it is not, and the correction is recorded in place rather than
  quietly dropped:

  * **A skip that is not a whole number of blocks** — narrower than this entry
    first claimed, and the correction is the point. The list used to lead with
    "a skip, still", on the ground that every axis here moves whole Perlin
    BLOCKS while `end_islands` discards **17292 LCG STEPS**, so the one shape
    vanilla is *known* to use under `legacy_random_source` lay outside the
    space. The arithmetic under that was never done. A Perlin block costs three
    `nextDouble`s at two raw steps each plus a 256-entry shuffle at one each =
    **262 steps**, measured at **4096 of 4096** seeds, and
    **262 x 66 = 17292 exactly**. `end_islands`' skip is therefore not a
    partial block: it is **block offset 66**, inside the 300 the block axis has
    swept all along — verified directly, at five seeds, as the same generator
    state and the same constructed block both ways
    (`tests/conformance/vanilla_legacy_seed_control_test.cpp`). What remains
    outside is a skip whose step count is NOT a multiple of 262, and that is a
    real gap; but it is no longer the largest one, and the only example anybody
    has does not live in it. Nothing measured under the flag now sits outside
    the space.
  * **Three draw rules, not all of them.** No third stack, no stack whose
    octave count differs from the amplitude count, no two stacks interleaved
    octave by octave.
  * **The 337/331 second-stack ratio is fixed** in every candidate.
  * **The persistence and `valueFactor` schedules are fixed** at the modern
    ones. The frequency axis moves octave frequencies and nothing else.
  * **The octave-salt strings are the modern ones**, `octave_<n>` with n the
    DECLARED `firstOctave` plus the octave index — not the frequency axis's
    shifted one, and no other spelling.
  * **The fork is always the LCG's.** `forks` and the second-stack fork both
    use `JavaRandom(seed).nextLong()` even when the generator axis selects
    Xoroshiro, because that is the shape `old_blended_noise` confirmed.
  * **The ordinals are this pack's.** The declaration-order axis reads the
    four-noise order `legseed_s*` ships (`na`, `nb`, `nmulti`, `nskip`, which
    is also their sorted order). Whether vanilla's own build order for
    `minecraft:temperature` and friends is registry order, sorted order or
    something else is settled by nothing here. What is tested is that the
    SHAPE "the seed comes from build position" is refutable, and on this pack
    it is refuted. The older way build order can enter — every noise in the
    pack drawn from ONE generator in declaration order — needs no axis at
    all: it is a block offset, and for these four noises the offsets are 0,
    2, 4 and 10, all inside the 300 the block axis already sweeps.
  * **The screen's own limit**, restated because it is a real narrowing: this
    scan searches for a candidate that REPRODUCES the readback, not for the
    best-scoring one. A partially-agreeing rule is outside what it reports.
  * **The known-correct MODERN rule is not a member of the space either**, and
    this has to be read before `--scan`'s own rows are. Pointed at a `mod_*`
    mirror — a dimension whose seeding is KNOWN, and which the control recovers
    at 2304/2304 — the scan reports **`past screen 0  survivors 0`**, exactly
    as it does on the legacy dimensions (measured at seed 42: `mod_single` over
    23,641,800 candidates, `mod_multi` over 30,912,000, `mod_skip` over
    57,010,800; `mod_single` repeats at seed 31337). That is not the scan
    failing where the answer is known; the modern rule is outside the 64-bit
    seed and generator axes by construction. Its generator is seeded
    from a full 128-bit state — `fromHashOf` XORs the identifier's MD5 into
    BOTH halves of a forked 128-bit base — while every candidate reaches
    Xoroshiro through the 64-bit constructor, i.e. through
    `upgradeSeedTo128Bit`'s image, 2^64 of the 2^128 states; and its per-octave
    salt is a `Seed128` XORed into both halves, where the octave-salt axis XORs
    64 bits into a 64-bit seed. Membership is decidable rather than argued
    (`mixStafford13` is a bijection): of the **36** states the modern rule
    builds over these four noises and both worlds — each noise's named state,
    plus each stack's per-octave state for each non-zero amplitude — **0 are
    reachable from any 64-bit seed**, while **1201 of 1201** states built FROM
    a 64-bit seed do report reachable, so the predicate is not one that refuses
    everything. This is why the control scores the modern rule DIRECTLY rather
    than through the scan — through the scan it could only ever report zero —
    and it is asserted in
    `tests/conformance/vanilla_legacy_seed_control_test.cpp`.
  * **The cascade's real-data half rests on the control's `exact` arm**, which
    is the seam the two calibrations leave between them. The 8-column screen
    and the 128-column gate meet the SERVER's readings only through candidates
    that are all wrong, and meet a CORRECT candidate only through a plant's
    SYNTHETIC readings — the two have never been put through the screen
    together, and by the item above they cannot be. What carries the claim that
    the screen never rejects a correct candidate is therefore `--control`'s
    `exact` arm: `quantise(model)` reproduces the server's own reading as a
    double on **6912 of 6912** columns over the three mirrors, at each seed, so
    a correct candidate lands inside the band on all eight screen columns by
    construction and not by luck.

  *And two things the widening does not change.* The step from "no survivor"
  to "absent from the space rather than invisible to the tool" still rests on
  everything the control validates being validated on the MODERN dimensions,
  bounded but not proved by the `--profile` comparison recorded below. And
  none of this says where the answer IS. It says the answer is not a
  sequential, per-block-forked or second-stack-forked stack, with or without
  an MD5 or hashCode per-octave salt, in either octave order, with or without
  zero-amplitude octaves consuming blocks, at any frequency within three
  octaves of the declared one, at any of 300 block offsets, from any of 1,440
  seeds built from five bases, twelve salt spellings including two that read
  the declaration ordinal, three operators, four fork counts and two
  generators — in either of two worlds.

- **"The empirical null" was one number and should have been a function
  (M4).** The correction matters more than the search it came from. A
  candidate "agrees" with a column when its value lands within half a
  quantum of the reading, and the quantum is `2 / height / (K * scale)` — so
  the null is set by the *readback*, not by the candidate. The probe carries
  one noise at three output scales with nothing about the seeding changed:

  | dimension      | band    | null mean (of 128)   | null max over 270,000 |
  |----------------|---------|----------------------|-----------------------|
  | the seven at 2.0 | 0.00372 | 0.78-1.07 (0.6-0.8%) | 7-9 (5.5-7.0%)      |
  | `leg_skip_q4`  | 0.01488 | 3.26-3.61 (2.6-2.8%) | 18-19 (14.1-14.8%)    |
  | `leg_skip_q16` | 0.05952 | 12.99-14.40 (10-11%) | 47-52 (36.7-40.6%)    |

  (Pre-widening figures, over the whole 270,000. The widened scan re-measures
  the same statistic on a subsample and reproduces the means to two decimal
  places; its table, and the separate 8-column screen null it also reports,
  are in the widening entry above.)

  (Both seeds; the same noise, the same seeding, only the output scale
  moving.) The sharpest single demonstration is one fixed wrong candidate
  rather than a distribution: deepslate's derivation scores **16/2304 (0.69%)
  on `leg_skip` and 318/2304 (13.80%) on `leg_skip_q16`** — the same rule,
  the same noise, the same world, differing in nothing but the band.

  Two ways a null becomes an apparent signal, both visible above: the band,
  which scales the mean linearly, and taking a *maximum* over a large
  candidate population, which sits 8-11x above its own mean. An earlier
  write-up reported every scanned candidate as landing "at the empirical
  null", singular, and a reproduced run of it then found one row at 168/1024
  (16.4%) against siblings at 0-48/1024. 16.4% lies inside the range of nulls
  measured across the readback bands above — but WHICH band that row sat at
  cannot be determined, because its probe world, its noise definition and its
  analyzer were never committed and the script that made it was a local edit.
  At the fine band 16.4% would be 2.3x the null maximum there and would not be
  explained by the band at all; and its siblings shared its band, so band
  variation alone cannot explain a 3.5x separation from them either. The row
  is unexplained and unre-checkable, and is recorded here as such. What is
  settled is narrower, and is only what these numbers support: a single global
  empirical null was the wrong premise to judge it against, because the null
  is a function of the readback rather than a constant.

  *And the search is calibrated — as rank-1 recovery by the scan itself, in
  the count statistic it actually reports. An earlier write-up did plant a
  candidate, but scored it against a null rather than recovering it.* A
  candidate planted inside the scanned space, put through the same
  quantisation the server's terrain imposes, is returned at **rank 1 as the
  sole survivor, 2304/2304 columns, in all six legacy configurations**
  against 270,000 rivals — twice, at an LCG rule (180, block 7, seed 42) and
  at a Xoroshiro one (421, block 3, seed 31337). Those two indices are the
  pre-widening ones; the same two rules are now 288 and 657, and the widened
  scan plants five candidates rather than two (see above). Without that, "N candidates
  refuted" is a number with no power behind it. It also caught a bug in the
  tool that would otherwise have passed for a result: the plant first scored
  51%, because the inverse of the readback was written as `floor(t - 0.5)`
  rather than `ceil(t) - 1`, and the search then failed to find its own plant
  on one dimension.

- **The plant calibrates the statistic; the control calibrates the model
  (M4).** The plant above is necessary and it is not sufficient, and saying
  so is the point of this entry. `--plant` synthesises its readings through
  the analyzer's *own* forward model — `layoutFor`, `sampleNormal`,
  `quantise` — so a wrong model (a wrong persistence schedule, a wrong
  `valueFactor`, a missing second stack, a wrong cell-corner assumption, a
  wrong inversion of the gradient) is shared by the plant and the scan alike
  and the plant still returns rank 1. It shows the statistic is *sensitive*.
  It cannot show the model is *right*, and until now nothing in the committed
  apparatus did.

  `tools/analysis/legacy-seed-analyze.cpp --control` is that second half. It
  scores a named-noise rule this repository already knows is right — the
  modern derivation `lib/src/noise_registry.cpp` implements and the
  conformance suite validates, `XoroshiroPositionalFactory(worldSeed)
  .fromHashOf(id)` then `NormalNoise::create` — against the probe's `mod_*`
  mirror dimensions, which name the same noises without the flag in the same
  world from the same server start. Same readback, same band, same columns
  the scan uses.

  *The conclusion is a chain, not three independent measurements,* and it is
  written out here because read as a flat table it claims more than it has.
  Two arms are measured against the **server**; the rest are
  transcription-equivalence checks against a **library** whose correctness is
  established somewhere else entirely:

  | arm | measured against | seed 42 | seed 31337 |
  |-----|------------------|---------|------------|
  | `library` within band | the **server's** readings | 6912/6912 | 6912/6912 |
  | `exact` through `quantise` | the **server's** reading, as a double | 6912/6912 | 6912/6912 |
  | `model` within band | the server, but *entailed* by the two rows below | 6912/6912 | 6912/6912 |
  | `identical` | the **library**: `sampleNormal` vs `NormalNoise::sample`, bit for bit | 20736/20736 | 20736/20736 |
  | `valueFactor` | the **library**: `layoutFor`'s vs `NormalNoise::valueFactor()`, bit for bit | identical | identical |

  (2304 columns in each of `mod_single`, `mod_multi`, `mod_skip`, so 6912;
  `identical` runs on all nine dimensions, 20736 per seed, 41472 over both.
  The `valueFactor` rows are `0.83333333333333326` at effective octave count
  1 and `1.25` at 3.) So the chain reads: the conformance suite validates
  `NormalNoise` against vanilla; `identical` and `valueFactor` show the
  analyzer's hand-rolled model is a bit-exact **transcription** of that
  library rather than a second opinion about it; `library` and `exact` show
  that function put through *this* readback recovers the server's own
  columns. `model` passing is then not a fourth piece of evidence — it is
  what the second and third links imply. Break any link and the conclusion
  goes; none of them is load-bearing alone.

  *And the mirror runs the other way.* The same rule on the *legacy*
  dimensions of the same worlds lands at each dimension's own null: at band
  0.00372, 13/2304 (`leg_single`), 14 (`leg_twin`), 15 (`leg_multi`) and 14
  (`leg_skip`) at seed 42, and 19, 12, 20 and 23 at seed 31337 — 0.52% to
  1.00%; at band 0.01488, 61 and 58 (2.65%, 2.52%); at band 0.05952, 262 and
  246 (11.37%, 10.68%). 379/13824 and 378/13824 across the six legacy
  dimensions. So the recovery above is the seeding and not the readback.

  *The negative control is the direct form of that argument,* and it was the
  confound this work was supposed to rule out. The legacy mirror varies the
  flag and the seeding **together**, so a null score there is consistent with
  either explanation. Scoring the same modern derivation at `worldSeed + 1`
  against the same `mod_*` dimensions varies one thing. It falls to the null:
  **45/6912 at seed 42** (`mod_single` 18, `mod_multi` 8, `mod_skip` 19) and
  **44/6912 at seed 31337** (13, 15, 16) — 0.65% and 0.64%, against 100% one
  seed away. `--control` treats a negative arm above a tenth of the columns
  as a control failure, and the conformance case asserts it per dimension.

  *Two caveats, and the second is new here.*

  1. It does not widen the space. The scan still covers one stack rule and no
     frequency variation (above), and a control run on a modern rule cannot
     tell you whether the legacy rule uses a stack shape the scan never
     enumerates — which remains the most likely place for it to be hiding.

  2. **Everything the control validates, it validates on the MODERN
     dimensions.** The readback, the coordinate convention, the band and the
     `quantise` inversion are shown correct where the legacy flag is absent,
     so the step from "no survivor" to "absent from the space rather than
     invisible to the tool" *assumes* the flag changes the **seeding** and
     nothing else in the terrain pipeline. Nothing measured here establishes
     that, and it is recorded as an assumption rather than left implicit.

  *That assumption is now bounded, cheaply.* If the flag also moved the
  gradient, the output scale, the cell spacing or the sampling coordinates,
  a legacy dimension's readback would not have the same **spread** or the
  same **neighbour-to-neighbour structure** as its modern mirror.
  `--profile` measures both — the standard deviation of the inverted column
  values, and their Pearson autocorrelation at lag 4, the probe's own column
  spacing, where a change in cell size or frequency bites hardest:

  | dimension | shape | sd (42 / 31337) | lag-4 (42 / 31337) |
  |-----------|-------|-----------------|--------------------|
  | `leg_single`   | 1-octave | 0.2881 / 0.2949 | 0.3145 / 0.3172 |
  | `mod_single`   | 1-octave | 0.3421 / 0.3190 | 0.4674 / 0.4254 |
  | `leg_twin`     | 1-octave | 0.3200 / 0.3388 | 0.4414 / 0.4717 |
  | `leg_multi`    | 3-octave | 0.2924 / 0.2984 | 0.8769 / 0.8824 |
  | `mod_multi`    | 3-octave | 0.2906 / 0.3233 | 0.8817 / 0.9022 |
  | `leg_skip`     | skip     | 0.2956 / 0.2646 | 0.9158 / 0.9084 |
  | `mod_skip`     | skip     | 0.2808 / 0.2786 | 0.9245 / 0.9090 |
  | `leg_skip_q4`  | skip     | 0.2957 / 0.2646 | 0.9150 / 0.9077 |
  | `leg_skip_q16` | skip     | 0.2982 / 0.2671 | 0.9039 / 0.8946 |

  Every dimension's spread lands in one interval, **0.2646 to 0.3421**, and
  the lag-4 figures group by noise *shape* rather than by the flag: 1-octave
  0.3145-0.4717, 3-octave 0.8769-0.9022, skip 0.8946-0.9245. The statistic is
  not blind: the gap between the widest 1-octave instance and the narrowest of
  the others is **0.405**.

  Read the comparison per MIRROR PAIR rather than pooled, because pooling
  understates it. The only true same-noise pair — `stratum:na` with the flag
  on and off, `leg_single` against `mod_single` — differs in lag-4 by **0.153
  (seed 42) and 0.108 (seed 31337)**, not by the 0.060 a shape-group mean
  gives. Pooling drags the legacy side up because `leg_twin` (`stratum:nb`)
  has NO modern mirror, so the 1-octave group is four legacy observations
  against two modern, of two different noise definitions.

  And the statistic's own NOISE FLOOR is the same size as the effect. Two
  legacy 1-octave realizations of the same shape — `leg_single` against
  `leg_twin`, same flag, same world — differ by **0.127 and 0.155**, i.e. by
  MORE than the mirror pair does. So this comparison bounds a flag effect only
  to within realization-to-realization scatter at 1 octave; it does not
  resolve anything smaller, and the 3-octave and skip rows (0.012 and 0.009)
  are the ones carrying real precision. Note also that `leg_skip_q4` and
  `leg_skip_q16` are NOT independent samples: they are the same field as
  `leg_skip` re-read at a coarser output scale, so the skip group holds one
  realization per flag, not three.

  So at this resolution the flag does not visibly move the pipeline's spatial
  structure — with "this resolution" meaning ~0.15 in lag-4 at 1 octave, and
  ~0.01 at the other two shapes.
  That is a bounded, refutable statement and not a proof — nothing short of
  the pipeline itself would be — but it replaces an assumption that was not
  being measured at all.
  `tests/conformance/vanilla_legacy_seed_control_test.cpp` asserts it, with
  tolerances about three times the measured differences.

  *Four figures supplied with the request for this measurement did not
  reproduce, and they are listed rather than quietly replaced.* The spread was
  given as "0.26-0.34 on both sides", which is right (0.2646-0.3421). The
  lag-4 ranges were not. Given "1-octave 0.305-0.480 across four instances",
  measured 0.3145-0.4717 across **six** — `leg_single`, `mod_single` and
  `leg_twin` at two seeds each. Given "3-octave 0.855-0.906", measured
  0.8769-0.9022; the quoted lower bound is below anything this probe
  produces. Given "skip 0.904-0.925", measured 0.8946-0.9245: `leg_skip_q16`
  at seed 31337 is 0.8946, under the quoted floor. And the legacy-side counts
  at scale 2 were given as "13-23", but `leg_twin` at seed 31337 scores
  **12**/2304, so the range is 12-23. None of these changes the conclusion —
  the shapes still separate by 0.405, an order above both the mirror-pair
  difference and the realization scatter above — but a range quoted one
  instance too narrow is how a later
  measurement outside it reads as a signal, which is the same mistake this
  section already records once for the null. (What did reproduce to the
  column: the negative control's 45/6912 at seed 42, `mod_single` 18,
  `mod_multi` 8, `mod_skip` 19, and the legacy counts at the two coarse
  scales, 58-61 and 246-262.)

  *What the control does NOT reach, inside the apparatus rather than outside
  it.* `blocksFor` under `Generator::Lcg` — Java `Random` driving
  `PerlinNoise::fromRandom`, which the half of the 270,000 candidates that
  use the LCG depends on — is exercised by neither `--control` nor the
  conformance case. Both run Xoroshiro128++ only, because the rule known to
  be right is the modern one. That path is validated against the server in
  `tests/conformance/vanilla_legacy_blended_test.cpp`, where
  `BlendedNoise::legacyFromWorldSeed` is `JavaRandom{worldSeed}` handed
  straight into the same `PerlinNoise::fromRandom`; the control's guarantee
  stops at the seed rules it actually runs.

  *One number reported to this project did not survive being measured.* A
  reviewer's external run gave the worst error on the mirror dimensions as
  "exactly 0.00372", equal to the band. It is not equal: the largest
  |ours - server| is 0.003719225, 0.003718785 and 0.003716169 at seed 42 and
  0.003717617, 0.003719963 and 0.003718207 at seed 31337, against a band of
  0.0037202380952380952 — strictly under it in all six, by 2.8e-7 to 4.1e-6.
  Equality would have meant a column sitting exactly on the acceptance
  boundary, where a one-ulp change in the model flips it; the margin is
  small because 2304 quantisation errors nearly fill their interval, which is
  what a correct model looks like, but it is a margin. The "exactly" was a
  `%.5f` printing the band and the error to the same five places. (The figure
  `--control` prints is now per arm, `worst(library)` and `worst(model)`
  separately: a single accumulator fed from the library arm reported nothing
  about the model arm, which is the one the plant and the scan actually
  share. They agree to the bit here, as `identical` says they must.)

  *The control is itself calibrated, by breaking the model on purpose.* Five
  one-line mutations of the analyzer's forward model, each scored at seed 42
  against the three mirror dimensions, and every one of them fails the
  control (exit 1) — but not all through the same arm, which is the reason
  there are three:

  | mutation | `model` in band | `identical` | `exact` |
  |----------|-----------------|-------------|---------|
  | drop the second stack | 27-59/2304 | 0/2304 | 27-59/2304 |
  | second-stack ratio 338/331 | 294-763/2304 | 1/2304 | 294-763/2304 |
  | `persistence *= 0.25` per octave | 2304 (1 octave), 71 and 129 (3) | 0 where it bites | as `model` |
  | `valueFactor` as `(5n)/(3(n+1))` | **2304/2304** | 327/2304 at n=1 | **2304/2304** |
  | `quantise` as `floor(t - 0.5)` | **2304/2304** | 2304/2304 | 1107-1166/2304 |

  The last two rows are the load-bearing ones. A one-ulp `valueFactor` — the
  algebraically equal spelling this project already measured off the server
  and rejected — is **invisible** to a band the width of a terrain block and
  invisible through `quantise`; only the bit comparison against
  `NormalNoise::valueFactor()` sees it. And the inversion bug that once made
  the plant score 51% (`floor(t - 0.5)` for `ceil(t) - 1`) is invisible to
  the band arm, which still reports every column, and shows up only in the
  exact arm, at 1107-1166 of 2304 (48.0-50.6%) — the historical 51%, reached
  by a route that names the cause rather than by a plant that merely fails. A
  control with one arm would have missed one of these two. The mutations are
  scratch edits rather than committed code; each is the single substitution
  named in the table, applied to
  `tools/analysis/legacy-seed-analyze.cpp`. The two load-bearing rows were
  re-measured against the committed tool while writing this entry and
  reproduce exactly — `valueFactor` as `(5n)/(3(n+1))` gives `model`
  2304/2304, `exact` 2304/2304 and `identical` 327/2304 at n=1; `quantise` as
  `floor(t - 0.5)` gives `model` 2304/2304, `identical` 2304/2304 and `exact`
  1148, 1107 and 1166 of 2304 — and both exit 1. The other three rows stand
  as originally measured.

  *And the analyzer is now inside the build, which is the only reason any of
  the above can be relied on tomorrow.* It used to be compiled by hand from a
  `g++` line in its own header: in no CMake target, and therefore reached by
  none of `tools/lint/format.sh`, `tools/lint/warnings.sh` or
  `tools/lint/tidy.sh`. A claim that its hand-rolled model agrees with the
  library bit for bit could go stale without one gate turning red — the
  measurement was real, but it was a one-shot manual one. It is now the
  target `stratum_legacy_seed_analyze` (`tools/analysis/CMakeLists.txt`), it
  builds under the project warning set, `format.sh` and `tidy.sh` name it
  (they list the analysis sources that are build targets rather than sweeping
  `tools/analysis`, most of which is one-shot investigation code nothing
  compiles), and `ctest --preset conformance` runs its `--control` on both
  probe worlds as `conformance.legacy_seed_control_tool`, skipping with exit
  77 and the regeneration command when they are absent. Entering those gates
  cost the file seven fixes it had been hiding: four `-Wfloat-equal`
  comparisons against literal amplitudes, and three `readability-identifier-
  naming` violations under clang-tidy 18. That test is
  where the `model`, `identical` and `valueFactor` arms are gated; the
  conformance case in `tests/` cannot carry them, because there the library
  *is* the model.

  *What this does and does not license.* It converts "270,000 candidates, no
  survivor" from an argument into a measurement: a correct named-noise rule
  **is** recoverable through this apparatus, so the legacy rule is absent
  from the searched space rather than invisible to the tool — subject to the
  two caveats above.
  `tests/conformance/vanilla_legacy_seed_control_test.cpp` guards the
  recovery, the null, the negative control and the spread/autocorrelation
  comparison, over both seeds. It requires **both** probe worlds: with either
  missing it SKIPs naming the one that is gone and the command that
  regenerates it, rather than scoring one world and reporting a pass. A skip
  is not a pass — Catch2 exits **4** when every selected case skips, and
  `ctest` reads that as a skip only because `catch_discover_tests` sets
  `SKIP_RETURN_CODE 4` (`Catch.cmake:219`); run the binary by hand and the
  shell reports 4.

- **The blanket refusal was far wider than the gap, and the Nether's terrain
  was never behind it (M4).** The refusal above fired before `wanted` was
  consulted, so it also refused a legacy dimension that named *no* noise —
  and the note at that site said nobody had measured whether such a case
  existed. It does, and it is most of what was blocked. Walking the pinned
  pack's own resolved graph per router entry, following shared
  density-function references *and* splines (`Graph::reachableFrom` /
  `noisesReachableFrom`, new here):

  | dimension | router named | which entries | surface rule *spells* | surface rule *needs* |
  |-----------|--------------|---------------|-----------------------|----------------------|
  | `end` | 0 | — | 0 | 0 |
  | `nether` | 3 | `temperature`, `vegetation` only | 6 | **8** |
  | `caves` | 3 | `temperature`, `vegetation` only | 7 | **9** |
  | `floating_islands` | 3 | `temperature`, `vegetation` only | 7 | **9** |

  **The last column is the one that matters, and an earlier version of this
  table had only the one before it.** A rule tree needs more noises than its
  fields spell: `minecraft:surface` and `minecraft:surface_secondary`
  wherever a depth is read — which `hole`, `above_preliminary_surface` and a
  depth-adding `stone_depth` all do without saying so — and
  `minecraft:clay_bands_offset` wherever `bandlands` is placed. None of the
  three appears in any field of any tree. `surface::requiredNoises` is the
  single answer to "what does this tree need", and it is why the refusal
  names 8/9/9 rather than 6/7/7.

  Thirteen of fifteen router entries reach zero in every one — `final_density`,
  `depth`, `erosion`, `ridges`, `continents`, `barrier`,
  `preliminary_surface_level`, both fluid levels, `lava` and the three vein
  entries. **Three, not two**: `minecraft:offset` arrives through the shared
  `minecraft:shift_x` / `shift_z`, whose `shift_a` / `shift_b` argument is a
  *noise* rather than a nested function, so a per-entry scan of the JSON
  never sees it. `tests/conformance/vanilla_legacy_named_noises_test.cpp`
  pins the table by TWO INDEPENDENT WALKS that must agree: a hand-written
  type-to-field table with its own reference resolution, and the engine's
  own `Graph::reachableFrom`. `tools/analysis/legacy-named-noise-reach.py`
  walks the raw JSON as a third route. There is exactly one reachability
  walk in `lib/`; the other two live in the test and the tool, on purpose,
  because two walks that share an implementation agree by construction and
  prove nothing. The same test also pins the SET of `legacy_random_source`
  entries to exactly these four, so a fifth cannot appear unnoticed.

  Two consequences, and they cut against the intuition the refusal was built
  on. The seeding question does **not** block a legacy dimension's terrain —
  only its biome climate and its surface decoration. And the one dimension
  that needs no named noise anywhere, the End, is unblocked all the way to
  its surface rules: `minecraft:end_islands` was the other thing standing in
  the way and it is now implemented and measured (below), so the End's
  terrain and surface rules reproduce vanilla's exactly.

  **"Unblocked" here is a ChunkFiller-level, terrain-and-surface-rules claim
  and nothing more, and it should be read nowhere wider.** The End's
  `biome_source` is `minecraft:the_end` — read from the pinned
  `world_preset/normal.json`, neither `multi_noise` nor `fixed` — and it is
  not implemented. A first version of this paragraph then said
  "`CompiledDimension::compile` still refuses **every** legacy dimension";
  that was a universal, it was false, and a reviewer refuted it by
  construction. The measured boundary is narrower: the public path refuses
  the End on `(end, end)` for ONE reason — vanilla ships no biome parameter
  list named `minecraft:end`, and the refusal names exactly that — and NOT
  for being legacy. Pair the End's settings with any list that does exist
  and it compiles and generates through `CompiledDimension::compile`
  (asserted in `vanilla_compiled_dimension_test.cpp`; the block-level result
  is `golden_end_test.cpp`'s). So the End IS available through the public
  compile path, with the wrong biomes; what is not implemented is its own
  biome source, and that is the whole of what "unblocked" must not be read
  past. The other three legacy dimensions are refused on that path by name
  of the noises they need.

  The refusal is therefore narrowed to a **non-empty** `wanted` under
  `RandomSource::Legacy`. That is the rule stated exactly rather than
  loosened: with no identifier there is nothing to hash and nothing is
  approximated. Nothing else about it changed, and no guessed derivation
  exists anywhere in shipped code.

  One further change was needed and is worth its own line, because it moves a
  refusal. `Interpreter`'s constructor bound every named noise in the graph
  and threw if the registry lacked one — but a pack resolves all seven
  dimensions into **one** graph, so a missing overworld noise
  (`minecraft:cave_entrance`) blocked the nether's roots. An unbuilt named
  noise is now left unbound and refused by `refuseIfUnevaluable` at the node
  that samples it, naming it, exactly as an inline noise already was;
  `requireEvaluable(root)` still answers up front per router entry. Loudness
  is unchanged (§8) — a node no root reaches generates nothing.

- **How close the legacy Nether's terrain actually is: 99.99591%, and the
  residual is one block (M4).** With an empty registry and no surface rules,
  `ChunkFiller` produces the Nether's netherrack / lava / air against all
  eight golden regions.
  `tests/conformance/vanilla_legacy_nether_terrain_test.cpp`, every fourth
  chunk of each region (512 chunks, 15466496 scored positions):

  | | |
  |---|---|
  | exact solid/lava/air class | 15465864 / 15466496 = 99.99591% |
  | disagreeing runs | 632, **longest run 1 block** |
  | direction | every one `solid` where vanilla has `lava`; never the reverse, air never involved |
  | location | every one in y [20, 30] |
  | excluded bands | 1310720 further positions, agreeing whole |

  *What is compared and what is not.* The goldens are terrain-only — the
  eleven block names occurring across all eight regions are censused in the
  test, and no glowstone, quartz or fire is among them, so carvers and
  features were removed (§7 Tier B). What remains between the filler and the
  golden is the surface rule, which needs eight named noises this build
  cannot seed — six spelled in the tree plus `minecraft:surface` and
  `surface_secondary`, which no field of it names — and whose two
  `vertical_gradient` conditions draw from the dimension's own random source,
  which is separately measured to be underivable here (the gradient gap,
  above). So the comparison is over a **class** (solid / lava / air), not a
  block name; a twelfth name fails rather than being bucketed (§8). The
  scored range is y [5, 123), excluding the two bands the surface rule writes
  regardless of density — `bedrock_floor` below 5, and `bedrock_roof` plus
  `y_above(below_top 5) -> netherrack` at 123 and above. At class level those
  bands agree anyway; the exclusion would matter to a comparison of names.

  *Eight regions, seven worlds.* Seeds 0 and Long.MIN_VALUE land on the same
  229 positions, because `java.util.Random` scrambles as
  `(seed ^ 0x5DEECE66D) & ((1 << 48) - 1)` and they differ only in bit 63.
  That is a check on the seeding, not a coincidence to explain away.

  *What the residual is, and the reading of it that was refuted.* A longest
  run of one means the solid/fluid **boundary** sits one block high; the shape
  is otherwise vanilla's. The obvious next step is to call that a knife-edge —
  a density a hair over zero landing on the wrong side of an integer `y`
  because the surface is nearly horizontal there, the same shape as the
  floating-point epsilon the overworld's chase already found once. Evaluating
  `final_density` at all 632 refutes it:

  | | |
  |---|---|
  | margin past zero | median 0.0101367, worst 0.0344581 |
  | one block of y is worth | median 0.0275955, smallest 0.0204291 |
  | iso-surface displaced by | **>= 0.367 blocks** at the median, up to 1.687 |
  | against the right denominator | 632 of **226702** solid-to-fluid boundaries = **0.279%** |

  Vanilla's density there is `<= 0` and this build's is `> 0`, so the margin
  is a *lower* bound on the gap. Where the two disagree they disagree by about
  a block of terrain, not by an ulp — but only at 0.279% of the boundaries
  where they could. **Sparse and large, not uniform and small**, which is the
  opposite of what an epsilon looks like.

  **Open, and now specifically:** the cause is localised and this does not
  name it. It does not distinguish the `interpolated` lattice over the
  nether's 4x8 cell, `blend_density` (the identity in a fresh world), or
  `BlendedNoise::legacyFromWorldSeed` itself — whose probe scored terrain
  height to within **half a block** and would have passed a third-of-a-block
  error without seeing it, so its 13824 of 13824 does **not** exclude this.
  `tools/analysis/final-density-probe.sh` bisects the server's own density at
  a point rather than reading the sign of its terrain, and is what would
  decide it; that needs a probe world and was not run here.

- **What the modern derivation would actually do to the Nether, measured
  (M4).** This section used to say only that substituting it "would produce a
  Nether that is not vanilla's". That is an assertion. The number, from
  `tests/conformance/vanilla_legacy_nether_climate_gap_test.cpp` — 4096 biome
  columns a seed spanning each full 512x512 region, 32768 over the eight, one
  y per column (justified in the run: **zero** columns of vanilla's own
  stored biomes vary with y, as `y_scale: 0.0` requires):

  | | |
  |---|---|
  | agreement with vanilla's biome | 8873 / 32768 = **27.08%** |
  | chance baseline for these marginals | **30.23%** |

  The baseline is `sum_b p_vanilla(b) * p_modern(b)`, the agreement two
  independent labelings with these marginals would reach. The modern
  derivation does not beat it; it comes in below it, and per seed sits on it
  — on three seeds to the last cell, which is what happens when the wrong
  seeding lets one biome swallow the sample. The marginals are wrong too:

  | biome | vanilla | modern rule |
  |-------|---------|-------------|
  | `basalt_deltas` | 3938 | **0** |
  | `crimson_forest` | 11109 | 12090 |
  | `nether_wastes` | 9566 | 19397 |
  | `soul_sand_valley` | 4804 | 283 |
  | `warped_forest` | 3351 | 998 |

  Total variation distance 0.33. Basalt deltas — 12% of vanilla's sample —
  do not occur at all; soul sand valleys occur at a seventeenth of their
  rate. The Nether's biome is a function of `temperature` and `vegetation`
  alone, because its other four climate parameters are the constant 0.0, so
  there is nothing to absorb a wrong seeding.

  So the gap is now quantified rather than asserted: **right blocks, right
  terrain, wrong world.** Not a Nether that looks slightly off. The test
  reaches around the refusal on purpose and is not shipped code; no shipped
  path can substitute the modern rule for a legacy dimension.

- **The Nether's own stored biomes read back as the legacy climate noises:
  270,000 candidates, no survivor, and the control that makes that a
  measurement (M4).** The scan above works on synthetic probe dimensions. This
  is the same question asked of a different oracle — the eight golden Nether
  regions, which are the real server's output from the real legacy seeding of
  the real noises, with no probe world involved. `tools/analysis/legacy-
  goldens-biome-analyze.cpp` runs it and
  `tests/conformance/vanilla_legacy_goldens_biome_test.cpp` plus
  `conformance.legacy_goldens_biome_control` guard it.

  *The oracle, and its denominator.* Eight seeds, **six independent worlds** —
  a legacy dimension's randomness is `new java.util.Random(worldSeed)`, whose
  scramble keeps only the low 48 bits, so 0 collides with `Long.MIN_VALUE` and
  -1 with `Long.MAX_VALUE`. Every second chunk of each 32x32 region, sixteen
  4x4 biome cells a chunk, one y per column (justified in the run: **zero**
  columns of vanilla's own stored biomes vary with y, as `y_scale: 0.0`
  requires):

  | | |
  |---|---|
  | cells | **24576** over 6 worlds |
  | boundary-adjacent (a 4-neighbour inside the chunk differs) | 979 (**3.98%**) |
  | interior | 23597 (96.02%) |
  | `crimson_forest` / `nether_wastes` / `soul_sand_valley` / `basalt_deltas` / `warped_forest` | 38.66 / 27.79 / 13.66 / 12.41 / 7.48 % |
  | chance floor for those marginals | **26.64%** |

  That last row is the readback's real limit and it is why every number below
  is quoted against it. A stored biome is a cell of a five-way partition of the
  (temperature, vegetation) plane, not a number: two unrelated climate fields
  already agree 26.64% of the time. This sieve separates **exactly right** from
  everything else. It cannot rank near-misses, and 50% over it is not halfway
  to an answer.

  *The preset table it decodes against is the SERVER'S OWN, not the wiki's.*
  The pack ships
  `worldgen/multi_noise_biome_source_parameter_list/nether.json` as nothing but
  `{"preset": "minecraft:nether"}`; the five parameter points are compiled into
  the jar. `tools/fetch-vanilla` already asks the server's data generator
  (`--reports`) for them and keeps the dump at
  `.fixtures/<version>/biome_parameters/minecraft/nether.json` — the same route
  the overworld's 7593-row table has used since M4 opened. That is observed
  server output, which the provenance rule permits where it forbids the source,
  so neither minecraft.wiki nor the jar is a dependency of this build. The five
  points (temperature, humidity; every other axis a single point at 0.0) are
  pinned by the conformance case: `nether_wastes` (0, 0), `soul_sand_valley`
  (0, -0.5), `crimson_forest` (0.4, 0), `warped_forest` (0, 0.5, offset
  0.375), `basalt_deltas` (-0.5, 0, offset 0.175).

  *THE CONTROL LANDED FIRST, and it has three arms, not two.* The decoder is
  "stored biome per cell against `ParameterList::find` over the dimension's own
  climate router". Run it on the OVERWORLD goldens, where the seeding is the
  modern one this build reproduces, at every fourth chunk of all eight regions
  and four heights — 32768 cells:

  | arm | |
  |-----|---|
  | worldSeed | 32748 / 32768 = **99.939%** |
  | worldSeed + 1 | 1858 / 32768 = **5.670%** |
  | null: vanilla at one seed vs vanilla at another, same cells, 28 world pairs | 7307 / 114688 = **6.371%** |

  The third arm is what makes the second readable, and leaving it out is how
  this control failed twice before it passed. **First failure:** the negative
  arm was read against the independence baseline `sum_b p(b) q(b)`, which a
  wrong seeding has no business reaching, because it is not a random
  relabeling — it shares the coordinates, the y structure and vanilla's own
  biome marginals. **Second failure:** even with a cross-world null in place,
  the sample was a 2x2-chunk CORNER, where the wrong-seed arm read **10.56%**
  against a **3.87%** null. Over 32x32 blocks a world is essentially one biome,
  so the effective sample size there is the number of REGIONS and not the
  number of cells — the same mistake, in the same shape, that the climate-gap
  case already records having made. Spread over the whole region the wrong-seed
  arm falls to 5.67% at a null of 6.37%, i.e. onto it.

  *And the positive arm is 99.939%, not 100%, which is a finding rather than a
  slackened threshold.* The `[biome]` conformance cases report the overworld
  biome source as exact, and over their sample it is — a 64x64-block corner of
  four regions, 98304 cells, no miss. Spread the identical decode over the full
  512x512 of eight regions and **five columns** disagree, each of them at every
  sampled height, so a horizontal disagreement and not a depth one:

  | world | column | vanilla | ours |
  |-------|--------|---------|------|
  | -1 | (196, 268) | `deep_ocean` | `ocean` |
  | -4172144997902289642 | (460, 200) | `mushroom_fields` | `deep_lukewarm_ocean` |
  | 0 | (328, 4) | `river` | `beach` |
  | 9223372036854775807 | (68, 128) | `river` | `forest` |
  | 9223372036854775807 | (72, 128) | `river` | `forest` |

  Five in 8192 columns, 0.061% of cells. `biome/parameter_list.hpp` already
  records that "ties go to the later entry" is a **measured match** to vanilla's
  search TREE and not a derivation of it; these five are the kind of
  counterexample that note anticipates, and they are named with seeds and
  coordinates rather than rounded into "exact". They are far too small to blunt
  a control that has to separate 99.94% from 6.37%, and chasing them is its own
  piece of work.

  *The forward model is controlled separately, because the scan cannot use the
  library's `NormalNoise` — it only builds from Xoroshiro128++, and half the
  candidate space drives a Java LCG.* `--model` compares, bit for bit, at the
  modern seeding where the library is known right, over 1024 points:

  | arm | |
  |-----|---|
  | this file's `sampleNormal` over modern Perlin blocks vs `NormalNoise::sample`, for all THREE noises | 1024 / 1024 |
  | this file's `shift_a`/`shift_b`/`shifted_noise`/`flat_cache` chain over library noises vs `Interpreter` on the Nether's own router | 1024 / 1024 |
  | the composition, which is exactly what the scan runs | 1024 / 1024 |

  All three noises, not a convenient one: `temperature` (firstOctave -10,
  amplitudes `[1.5, 0, 1, 0, 0, 0]`), `vegetation` (-8, `[1, 1, 0, 0, 0, 0]`)
  and `offset` (-3, `[1, 1, 1, 0]`), whose amplitude gaps and non-unit leading
  amplitude are exactly where a wrong persistence schedule or `valueFactor`
  would show.

  *And the number already on record reproduces through entirely different
  code.* `--modern` scores the MODERN derivation through this analyzer's own
  chain rather than through `Interpreter`, over all eight seeds as
  `vanilla_legacy_nether_climate_gap_test.cpp` measured it:
  **8873 / 32768 = 27.08%** against a **30.23%** baseline, every per-seed row
  and every biome marginal identical to that case's table. The CTest wrapper
  matches both figures as text.

  *THE RESULT: NO SURVIVOR.* 900 seed rules x 300 block offsets = **270,000**
  candidates, the same enumeration `legacy-seed-analyze.cpp` defines (read out
  of that file, re-implemented here; the two must agree on what rule index 182
  means and the CTest wrapper checks the string). Stage 1 scores all 270,000
  over 1536 cells; stage 2 re-scores the top 32 over the full 24576, so nothing
  is reported at a denominator smaller than the oracle offers.

  | stage 1, 1536 cells, all 270,000 | |
  |---|---|
  | null mean | 406.73 / 1536 = **26.48%** (the 26.64% chance floor, measured) |
  | null sd | 125.81 (8.19 points) |
  | p50 / p90 / p99 / p99.9 | 26.56% / 38.02% / 44.21% / 49.48% |
  | largest score at this denominator | 873 / 1536 = 56.84%, rule 341 block 5 — which is the **#1 row of the top-32 table below**, not a threshold anything in that table can be judged against |
  | the MODERN rule on the same cells, for reference | 389 / 1536 = 25.33% |

  | stage 2, 24576 cells | |
  |---|---|
  | best of stage 1's 32 survivors (rule 387 block 2, `xoroLo add md5FirstLE, forks 1, xoroshiro`) — and, by the enumeration below, the best in the whole space | 13064 / 24576 = **53.16%** |
  | deepslate's own rule (182) at its best block, 219 | 12241 / 24576 = 49.81% |
  | deepslate's own rule at block 0 | 10878 / 24576 = 44.26% |
  | what a correct rule would score | ~**100%**, per the control |

  **THE COMPARISON THAT CARRIES: ~100% against 53.16%.** A correctly seeded
  rule is not estimated at ~100%, it is measured there — 99.939% is what the
  control's positive arm reaches through this same decoder. The best of 270,000
  candidates reaches 53.16%, a score whose own chance baseline is 29.63%. That
  is the whole result, and it needs no null to be read: this sieve was built to
  separate exactly-right from everything else, and nothing in the space is
  exactly right.

  **What the top of the table does when the denominator grows is the second
  half of it.** Rescored from 1536 cells onto all 24576, **all 32 survivors
  fall** — mean 54.21% -> 48.43%, a drop of 5.78 points, no exception in
  either direction; rule 341 (stage 1's leader) 56.84% -> 50.96%, rule 387
  55.14% -> 53.16%. The ordering scrambles with them: stage 1's #1 finishes
  third and stage 2's winner was only #7 going in. Regression of that size,
  applied to every row, is the signature of a top table selected out of
  sampling noise. A rule that actually seeded these noises would not care how
  many cells it was counted over.

  *A correction, recorded because the wrong version of this claim was here
  first.* This section used to say the best candidate "does not even reach the
  maximum the null itself produces", 53.16% against 56.84%. That comparison was
  void twice over: the two figures have different denominators (24576 against
  1536), and 56.84% is not an independent null at all — it is rule 341 block
  5's own stage-1 score, the top row of the table it was being used to judge.
  The null in this space is real and it is measured, but that measurement was
  at 1536 cells, and a maximum does not travel between denominators. So the
  null was measured again where the answer is quoted.

  *THE NULL AT THE DENOMINATOR THE ANSWER USES.* `--null-full all` scores
  **every one of the 270,000 candidates over the full 24576 cells** — an hour
  of compute, and the only form of this null a stage-2 score can be held
  beside. Every candidate in the space is wrong, so these scores ARE the null:

  | the whole space at 24576 cells | |
  |---|---|
  | mean | 6276.93 / 24576 = **25.54%** |
  | sd | 1780.66 (7.25 points) |
  | p50 / p90 / p99 / p99.9 | 25.50% / 35.73% / 41.13% / 45.82% |
  | min | 887 / 24576 = 3.61% |
  | max | 13064 / 24576 = **53.16%**, rule 387 block 2 |

  Two things fall out of it. **Stage 1 lost nothing:** the largest score
  anywhere in the space, at the full denominator, is the same candidate stage 2
  reports, so 53.16% is the best this space can do and not merely the best of
  32 promoted rows — the two-stage shortcut is vindicated by the exhaustive
  run. And **that best is the maximum of the null, necessarily** — it is a
  member of the distribution, which is exactly why 56.84% could never have
  served as a threshold for it. What can be said is where it sits: 3.81 sd
  above the mean, against 3.71 sd for stage 1's maximum on a sixteenth of the
  cells. The same shape at both denominators, and an unexceptional place for
  the largest of a quarter-million draws to land (no correlation model is
  claimed here; the 300 block offsets of one rule are plainly not independent,
  which pulls the expected maximum down rather than up). What separates a
  correct rule from this is not 3.81 sd, it is the 47 points between 53.16%
  and the ~100% the control measures.

  *What this EXCLUDES.* Through a decoder shown to recover a known-correct
  seeding at 99.94% and to put a known-wrong one at the null, and a forward
  model identical to the library's to the bit: no rule in the 270,000 — five
  bases x ten salt spellings x three combining operators x three fork counts x
  two generators x 300 discarded Perlin blocks — seeds `minecraft:temperature`,
  `minecraft:vegetation` and `minecraft:offset` the way vanilla's Nether does,
  for any of six independent worlds.

  *What it does NOT exclude,* and this is the larger half. It inherits every
  gap of the space it borrows: ONE stack rule (sequential Perlin blocks out of
  one generator), no frequency variation, no per-octave salting, and no
  fixed LCG-STEP skip of the kind `end_islands` turned out to use. It adds one
  of its own: the three noises are seeded by **one rule at one block offset**,
  so a derivation that seeded `offset` differently from `temperature`, or drew
  all three from one shared generator in declaration order, is outside the
  space even if each individual noise's rule is inside it. Half of that is now
  measured rather than assumed: `--split` re-runs the entire scan with the
  shift **held at zero**, which takes `offset` out of the candidate altogether
  and asks only the temperature and vegetation rules to be right. Nothing
  moves. The null is the same distribution (mean 26.48%, sd 8.19 points, its
  largest score 878 / 1536 = 57.16% and again rule 341 block 5), the same rules
  fill the top of the table, and the best full-sample score goes from 53.16% to
  **53.21%** — five hundredths of a point, on the same rule 387 block 2. So
  whatever is holding this scan at the floor, it is not that one rule is being
  asked to seed `offset` as well; that half of the assumption costs nothing.
  A score under `--split` is of course not a match to anything — vanilla's
  chain is not shift-free — which is why it is a diagnostic and not a result.
  And the readback is coarse: it can only find an EXACTLY correct rule. A
  rule that got `temperature` right and `vegetation` wrong would score in the
  null band and be indistinguishable from one that got nothing right, so "no
  survivor" says nothing about partial correctness. Widening the stack rule
  remains the most likely place for the answer to be hiding, and this result
  does not narrow that.

  *And one assumption that is not a seed rule at all, which could account for
  the whole null by itself.* A Perlin block is allocated **only for a non-zero
  amplitude** — that is what the modern draw does, so it is what `layoutFor`
  does, and the candidate's generator then hands blocks out one at a time in
  that order. A legacy draw that consumed a block **per declared octave**,
  advancing across the zero amplitudes as well, produces a different block for
  every noise and is outside this space **at every one of the 300 block
  offsets**: the offsets slide the whole stack together and cannot re-space it
  internally. `--model` cannot catch this, and it is worth being exact about
  why — it checks the stack against `NormalNoise` at the MODERN seeding, where
  the blocks come from per-octave salted forks and nothing is consumed
  sequentially at all, so the two readings of "which block" agree there by
  construction and can only differ where the scan actually looks. The exposure
  is large: `minecraft:temperature` is `[1.5, 0, 1, 0, 0, 0]` and
  `minecraft:vegetation` is `[1, 1, 0, 0, 0, 0]` — **four zero amplitudes each,
  of six** — with `minecraft:offset` at `[1, 1, 1, 0]`. Nine dead octaves
  across the three noises, every one of them a block a sequential legacy draw
  might have paid for. This is the single widest hole in the result and it is
  the same shape as the stack-rule gap the probe scan inherits.

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

  **A follow-on the surface-rule work found (M4).** The rule that picks the
  identifier arm out of that union asked whether the word "string" occurred
  in the arm's text. Two arms can both contain it: at 26.3 `biome_is` is
  `[#[id=...] string] | #[id=...] string`, an array of identifiers beside a
  single one, and the looser test then found no inline arm at all and died
  with a `StopIteration` rather than a refusal naming the field. It now asks
  what the arm IS — attributes stripped, exactly the word `string` and
  nothing else — which an array of them is not. No effect on any generated
  table at the pinned version; all four regenerate byte-identical. It is
  recorded because the failure was reachable only from a version gate this
  build does not target, and the next pin bump is when it would have landed.

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

- **M5 started: biome mapping landed, block state mapping's source is the
  open question.** `lib/mapping/` was empty by design until this; it is now
  a real CMake target (`stratum_mapping`) `stratum_core` does not link,
  matching §9's "downstream of the conformance boundary" rule structurally
  rather than only by convention.

  *Biome mapping (§9's second bullet), landed.* `tools/mapping-sync` fetches
  GeyserMC/mappings (MIT) at a pinned commit and generates
  `lib/mapping/src/biome_table.inc`, a flat Java-id-to-Bedrock-numeric-id
  table `bedrockBiomeId()` reads. All 65 vanilla biomes at the pinned
  version resolve, checked two ways: the generator itself refuses to write
  a table missing one of this build's own fetched `worldgen/biome/*.json`
  entries, and `vanilla_biome_mapping_test.cpp` makes the same check again
  independently through the compiled loader.

  *The version gap, measured rather than assumed.* GeyserMC/mappings has no
  branch for exactly this build's pinned Java version — the closest is
  `feature/1.21.9`. Before pinning to it, vanilla's own registries were
  compared directly between 1.21.9 and 1.21.11: `generated/reports/
  blocks.json` (both versions fetched, both run through `net.minecraft.
  data.Main --reports`) has an identical block count (1166), identical
  property definitions, identical default states and identical state
  counts; `tools/fetch-vanilla`'s own `worldgen/biome/*.json` extraction
  lists the same 65 biome ids at both versions. Neither registry moved
  between the two, and this table only needs the biome one.

  *Not landed: the "nearest vanilla Bedrock biome" fallback §9 also wants*,
  for a custom or datapack biome outside the table. `bedrockBiomeId()`
  returns nothing rather than a placeholder default — a fixed fallback
  would look like the real feature from the outside while being a weaker
  one, and SPEC's own "never a crash, never silent" rule for block mapping
  applies here in spirit: an unresolved gap named is safer than one hidden
  behind a plausible-looking answer. The real fallback needs a similarity
  search over biome climate parameters this library does not yet have.

  *Block state mapping has not started, and why is now a measured finding
  rather than an assumption of ease.* Two candidate sources were checked
  directly, not assumed interchangeable:

  - GeyserMC/mappings' own `blocks.nbt` — which biome mapping's source repo
    also ships — is not self-contained data. Inspected directly (decoded
    and read through this project's own NBT reader), its `bedrock_mappings`
    list is a sparse diff keyed by Java block state ordinal: most entries
    are empty ("no override"), and reconstructing a full Bedrock block
    state from a non-empty one needs the resolution logic documented only
    in GeyserMC's `mappings-generator` tool (MIT, permitted to read; not
    yet read for this specific schema).
  - PMMP's own `src/data/bedrock/block/convert/VanillaBlockMappings.php` is
    the more direct source — it is literally what `ext/`'s zend binding
    needs to match, since PMMP's own `BlockStateData` (`{name, states,
    version}`) is Bedrock's own blockstate NBT shape, not a PMMP-specific
    scheme. But it is executable PHP registration code, not declarative
    data: extracting it needs either parsing PHP source (fragile — depends
    on reading patterns correctly rather than running real code) or
    standing up a working PMMP runtime to execute it and dump the result
    (heavier setup, no parsing risk).

  Neither is a quick fetch-and-generate the way biome mapping was. Which
  approach to take is an open decision, not a blocker discovered too late
  to matter — see PROGRESS.md's M5 section for the standing options.

- **M5's block state mapping moves out of `lib/mapping/` entirely — a
  measured correction to the entry above, not a preference.** The question
  was never which data source to read; it was which LAYER the translation
  belongs in at all, and three independent, publicly inspectable codebases
  answer it the same way. (Permitted references for this: community
  Bedrock-facing middleware — NetherGamesMC's and pmmp's own public repos,
  GeyserMC/Geyser and GeyserMC/mappings, CloudburstMC/Nukkit — read for
  architecture, the same standing CLAUDE.md's provenance rule already gives
  cubiomes and Cuberite for worldgen; none of it is Mojang source.)

  *Multi-version Bedrock support is real, public, and per-connection.*
  `NetherGamesMC/PocketMine-MP` (a public fork; its dedicated
  `multiversion` repo is private and was not read) adds exactly the
  machinery upstream `pmmp/PocketMine-MP`'s own `BlockTranslator.php`
  names but does not build — its comment reads "...in case someone wants
  to implement multi version." NGMC's version: a `PATHS` table keyed by
  `ProtocolInfo` constant, one `canonical_block_states-{version}.nbt` per
  protocol, a `TypeConverter` cached per protocol via
  `ProtocolSingletonTrait`, and `NetworkSession::setProtocolId()` picking
  the right one once the connecting client's version is known.
  GeyserMC/Geyser's own `BlockRegistryPopulator.java` does the identical
  thing independently — one `BlockMappings` per protocol-codec entry, all
  held at once rather than one shared table serving every version.

  *Why one compiled-in table cannot serve more than one Bedrock version.*
  Since Bedrock 1.16.100, a block's network runtime id is not a stored
  constant — it is the block's own array INDEX after the client's full
  block state list is sorted by the FNV1 64-bit hash of each name
  (Geyser's own source comment: "we no longer send a block palette... the
  palette is sorted by the FNV1 64-bit hash of the name"). Adding,
  renaming or removing any one block shifts the index of every unrelated
  block that hashes near it in that order. Diffing GeyserMC/mappings'
  `feature/1.20.70` against `feature/1.20.80` shows exactly this:
  `minecraft:sapling` splitting into per-species identifiers restructures
  the sorted list under every other entry. A table compiled for one
  version is not merely stale for another — it is wrong the moment a
  differently-versioned client connects, and fixing that from inside a
  single shared C++ core means becoming N tables plus per-connection
  selection logic, which is exactly the shape the two codebases above
  already put in the native binding layer instead.

  *CloudburstMC/Nukkit needs its own separate translation regardless of
  any of this.* `BlockID.java` is Nukkit's own legacy id:meta scheme,
  structurally the same shape as PMMP's own, with its own
  `BlockStateMapping`/`BlockStateUpdaterVanilla` doing the Bedrock-network
  translation — independent confirmation that block state translation is
  a property of the CONSUMING platform, not something one shared engine
  layer can own on every consumer's behalf at once.

  *Biome mapping stays exactly where it landed — checked against the same
  three codebases, not assumed safe by analogy to blocks.* A real
  reassignment exists: GeyserMC/mappings' `minecraft:pale_garden` moves
  from `bedrock_id` 62 to 193 between adjacent version branches — a
  newly-introduced biome's placeholder id corrected, not an established
  biome moving underfoot, but real drift either way. Despite that,
  Geyser's own `BiomeIdentifierRegistryLoader` loads exactly ONE
  `biomes.json`, unversioned, with its own comment explaining why: "The
  server sends the corresponding Java network IDs, so we don't need to
  worry about that now." NetherGamesMC's fork has a versioned
  `BlockTranslator.php` and no `BiomeTranslator.php` at all — the
  multi-version machinery they built for blocks was never extended to
  biomes, in a codebase with every reason to build it if biomes needed it
  too. Biome id drift happens at Minecraft-version granularity, the same
  granularity `tools/mapping-sync`'s pin-and-regenerate model already
  handles; it is not the per-connection, per-protocol problem block state
  is.

  Net effect: `lib/mapping/` keeps exactly what it already had (biome
  mapping, §9) and loses block state mapping entirely. That work moves to
  `ext/`'s PHP layer — PMMP's own `BlockStateDeserializer` already turns a
  Bedrock blockstate NBT compound into a real `Block` object, so `ext/`
  need only produce that NBT shape from this engine's own Java block
  state, per connected protocol version, the way `BlockTranslator.php`
  already does for PMMP's own world loading — and, eventually, an
  equivalent Java-side translation for a Nukkit/CloudburstMC binding.
  `lib/`'s engine gains no Bedrock awareness at all.

- **M5-Nukkit: JNI chosen and a real second binding started, not merely
  designed.** CloudburstMC/Nukkit is the second target the block-state
  decision above already anticipated. Its own `build.gradle.kts` pins
  `JavaLanguageVersion.of(8)` (read directly, not assumed) — Java's Foreign
  Function & Memory API needs JDK 22+, so it is not available at that
  floor, leaving JNI as the only real option. This is not a new risk for
  Nukkit specifically: it already ships `leveldbjni`, a native library, as
  a production dependency.

  *The `Generator` contract, read from Nukkit's own source rather than
  guessed.* A `Generator` implementation supplies `init`, `generateChunk`,
  `populateChunk`, `getSettings`, `getName`, `getSpawn`, `getChunkManager`
  and `getId`; each async chunk-generation worker gets its own `Generator`
  instance via a `ThreadLocal`, created once per thread and reused for that
  thread's lifetime — confirmed by reading the calling code, not inferred
  from the interface alone.

  *Design consequence: one compiled `Pipeline` per world, shared across
  every worker thread, not one per thread.* Recompiling the whole density
  graph, surface rules and biome parameters once per `ThreadLocal`
  `Generator` would be pure waste — nothing about them is thread-specific.
  `ext-nukkit/`'s `StratumGenerator.java` holds one native handle per
  `ChunkManager` (one per world) in a `ConcurrentHashMap`, `computeIfAbsent`
  compiling it exactly once regardless of how many worker threads ask for
  it. This mirrors CLAUDE.md's own determinism rule — "Pipeline objects are
  immutable after compile" — the same invariant `terrain::ChunkFiller`
  already relies on internally.

  *What is built, not merely specified: `ext-nukkit/`'s C++ half.* Two CMake
  targets (`stratum_nukkit_pipeline`, plain C++, testable without a JVM;
  `stratum_nukkit`, the thin JNI shim `find_package(JNI REQUIRED)` needs to
  build) behind `-DSTRATUM_BUILD_EXT_NUKKIT=ON`, off by default.
  `stratum_nukkit_pipeline::Pipeline` reuses `terrain::ChunkFiller` and
  `mapping::bedrockBiomeId()` (already built for the PocketMine-MP path)
  unchanged, scoped to `minecraft:overworld` only — the same
  `legacy_random_source` (M4) limit `tools/analysis/generate-world.cpp`
  already lives under. `jni_bridge.cpp` is marshaling and exception
  translation only, matching `ext/`'s own "thin; marshaling only" rule for
  the PHP side. See `ext-nukkit/README.md` for the component's full scope
  and open gaps.

  *A dangling-pointer bug, found by testing rather than assumed absent.*
  `terrain::ChunkFiller::compile()` stores raw pointers into whatever
  surface rules, biome parameters and biome temperatures it is given,
  documented as requiring them to outlive the `ChunkFiller`. An early
  version of `Pipeline::compile()` built those three as local variables,
  passed their addresses to `ChunkFiller::compile()`, then `std::move()`'d
  the locals into a separately-constructed `Impl` afterward — moving each
  object's data to a new address while `filler` kept pointing at the old,
  now-destroyed stack frame. It did not crash consistently: one test case
  (a caller-sized output span deliberately too small) hit an out-of-bounds
  read inside `Interpreter::Scope::has`, caught by `_GLIBCXX_ASSERTIONS`
  rather than by a clean, reproducible failure. Fixed by building every
  such member in place inside `Impl`'s own constructor, in dependency
  order, so nothing `ChunkFiller::compile` or `Interpreter`'s constructor
  takes a pointer into is ever a value about to be moved out from under it
  — see `ext-nukkit/src/pipeline.cpp`'s `Pipeline::Impl` for the permanent
  writeup, so the mistake has a name if it is ever tempting to reintroduce.

  *What remains open, by design, not by oversight:* `resolveNukkitFullId()`
  always throws — no Java-block-state-to-Nukkit-legacy-id table has been
  sourced yet, matching the block-state-mapping entry above: this is real,
  unstarted M5 work, not something this slice attempted. `PalettedBlock
  Storage`'s exact per-section coordinate convention for biome storage
  (block-local 0-15 vs. quart-local 0-3) is flagged unconfirmed in
  `StratumGenerator.java`'s own header, since verifying it needs a real
  Nukkit build dependency this repo does not have. The Java side
  (`StratumGenerator.java`, `StratumGenerationException.java`) has never
  been compiled against real Nukkit classes for the same reason — it is
  written to match Nukkit's confirmed API exactly, but unverified by a
  real build.

- **M5's block state mapping returns to `lib/mapping/` — a measured
  correction to the "moves out of `lib/mapping/` entirely" entry above.**
  That entry's evidence holds: Bedrock's network runtime id is a hash-sorted
  index, and multi-version servers keep one table per protocol. But it never
  checked where in the pipeline that translation runs. It runs at network
  send, never at generation. Read directly from `pmmp/PocketMine-MP`
  (`stable` @ `6a7cc02`) and `CloudburstMC/Nukkit` (`master` @ `058e213`):

  *Generators never see a protocol version.* `Generator::generateChunk(
  ChunkManager, int, int)` has no client or protocol parameter. PMMP's own
  `Normal` and `Flat` generators write `VanillaBlocks::X()->getStateId()`
  through `Chunk::setBlockStateId(int, int, int, int)`. That id comes from
  `RuntimeBlockStateRegistry`, PMMP's own process-local numbering, which is
  the same whichever client later connects.

  *Storage never sees one either.* `LevelDB::saveChunk()` serializes through
  `GlobalBlockStateHandlers::getSerializer()` — a different object from the
  network `TypeConverter`. It is versioned by PMMP's own world-data version,
  not by protocol.

  *Protocol-specific translation lives only in the network layer.* Every
  `TypeConverter`/`getBlockTranslator()` call site is under
  `src/network/mcpe/` (`ChunkRequestTask`, chunk send) or in `World::
  createBlockUpdatePackets` (live block-change broadcast). There are zero
  under `src/world/generator/`. NetherGamesMC's multi-version fork keeps the
  same boundary: its per-protocol `TypeConverter` is picked in `NetworkSession
  ::setProtocolId()`, at session negotiation.

  *Both platforms already resolve a named Bedrock blockstate into their own
  id.* PMMP's world loader does exactly this for every palette entry on
  disk: `BlockDataUpgrader::upgradeBlockStateNbt()`, then
  `BlockStateToObjectDeserializer::deserialize(BlockStateData): int`, which
  returns the internal state id. Nukkit's `BlockStateMapping` keys its
  palette by the same `{name, states, version}` `NbtMap`. `updateState()`
  upgrades an older state forward, and the resulting `BlockStateSnapshot`
  carries `getLegacyId()`/`getLegacyData()`. The M5-Nukkit entry's
  assumption that Nukkit has nothing to resolve an identifier against was
  wrong: `BlockID.java` alone doesn't, but `BlockStateMapping` does.

  *So only one table is needed, and it is platform-neutral.* Java block
  state → Bedrock blockstate triple depends only on the pinned Minecraft
  version, the same granularity as biome mapping, and both platforms take
  that shape as input. It belongs in `lib/mapping/` beside the biome table.
  Only the final triple → internal id step is platform-specific, and each
  platform already ships the code for it. §9 is rewritten to match.

  *Two hazards this exposes, each measured rather than assumed away:*
  - **Version coupling.** PMMP's `BlockStateUpgrader::upgrade()` skips any
    schema older than the input's version and never downgrades. Nukkit's
    updater is also forward-only. PMMP `stable`'s `BlockStateData::
    CURRENT_VERSION` is 1.21.60.33 (`WorldDataVersions::BLOCK_STATES`), and
    Nukkit's leveldb palette is `block_palette_729.nbt` (protocol 729,
    Bedrock 1.21.30). Both are older than the Bedrock release matching this
    build's Java 1.21.11 pin. A table generated straight from GeyserMC's
    1.21.9 branch may therefore name states neither platform understands.
    Whether that matters for the blocks terrain generation actually emits
    has not been measured yet. It is the first question `tools/mapping-sync`
    must answer.
  - **Silent platform fallbacks.** PMMP's loader swaps any unresolvable
    state for `info_update`, and Nukkit's `getState(NbtMap)` does the same
    with only a `log.warn`. Bindings must catch the miss themselves (PMMP's
    `UnsupportedBlockStateException`, Nukkit's `getStateUnsafe()` returning
    null) so §9's explicit fallback table applies, not the platform's.

  *What this changes downstream.* `ext/` no longer needs any per-protocol
  table; it resolves `lib/mapping/`'s triples once per distinct state when
  the generator starts. `ext-nukkit/`'s `resolveNukkitFullId()` stub
  resolves in C++ today, but the resolver it needs (`BlockStateMapping`) is
  on the Java side. The JNI boundary will likely change so the Java side
  builds the id lookup once and native code only indexes it. Not
  implemented in this change.

- **M5's block state table landed — and measuring it corrected three things
  the entries above had assumed.** `lib/mapping/`'s
  `javaBlockStateId()`/`bedrockBlockState()` now resolve every Java block
  state at 1.21.11 (29,671 of them, in 1,166 blocks) to one of 10,491
  distinct Bedrock blockstates. The table is generated by
  `tools/mapping-sync` and checked two ways: the generator refuses to write
  anything unverified, and `vanilla_block_state_mapping_test.cpp` repeats
  the checks through the compiled loader.

  *`blocks.nbt` is not a sparse diff, and no oracle is needed.* The entry
  above that first scoped this work called it one. Decoded directly, 27,877
  of its 29,671 entries carry data, and GeyserMC/Geyser's own loader
  (`BlockRegistryPopulator.buildBedrockState`, MIT) states the whole rule:
  entry `i` is vanilla's state id `i`, its Bedrock name is `"minecraft:" +
  (bedrock_identifier, else the Java path)`, and its states are the `state`
  compound or empty. Running GeyserMC/mappings-generator as a black-box
  oracle was the plan; reading its source showed it is a Fabric datagen mod
  whose output is this same file, so running it would only reproduce the
  input.

  *The pin moves to the mappings Geyser itself shipped for Java 1.21.11.*
  Geyser's history shows `2f0a8da` as its mappings submodule at `b728af2`,
  its last commit before starting on Java 26.1. `biomes.json` is
  byte-identical to the previous `7a9bfc5` pin, so the biome table does not
  change; 171 block entries differ, every one a `huge_mushroom_bits` fix.
  The earlier "no branch for exactly 1.21.11" version gap is gone.

  *What the table needs from vanilla, and from Geyser, beyond `blocks.nbt`.*
  Vanilla's own `generated/reports/blocks.json`, now kept by
  `tools/fetch-vanilla` from the data-generator run it already made, names
  each state id and each block's default. Its property listing is not the
  numbering order for 12 blocks (chests, copper chests, pistons); the
  generator finds the one order that numbers every state and refuses if
  there is not exactly one. The Bedrock `version` and a membership check
  come from Geyser's `block_palette.1_21_130.nbt` at `b728af2`, one of the
  two palettes Geyser paired those mappings with, identity-remapped. Every
  one of its 15,845 states is at 1.21.60.33, and every resolved state is in
  it, tag types included.

  *Version coupling, measured — it is per state, not a version gate.* The
  "returns to `lib/mapping/`" entry above left this as the first open
  question. Measured against PMMP `stable` @ `6a7cc02` and Nukkit `master`
  @ `058e213`:
  - PMMP's `BlockStateData::CURRENT_VERSION` is 1.21.60.33, the table's own
    version, so `BlockStateUpgrader::upgrade()` applies nothing.
  - Nukkit's `block_palette_729.nbt` is uniformly 1.21.30.7. 2,538 of the
    1.21.130 palette's states are not in it (doors, fence gates and heads
    restructured, blocks added since), but none is a state terrain
    generation emits. Every block state any of vanilla's seven
    `noise_settings/*.json` names, plus water, lava and air (35 distinct),
    is present in Nukkit's palette by `{name, states}`. Nukkit keys that
    palette by the full `NbtMap`, `version` included, so `ext-nukkit/` must
    match on name and states rather than pass 1.21.60.33 through.
  - PMMP registers deserializers for 34 of those 35. The 35th,
    `minecraft:powder_snow`, which the overworld's surface rules emit, has
    no block in PMMP at all: `VanillaBlockMappings.php` references powder
    snow only as a cauldron liquid, and throws "Powder snow is not supported
    yet" there. §9's fallback table therefore has a real first entry, and
    §9 now says so.
  §9's "a binding refuses a table newer than its platform" would have
  refused Nukkit outright over states that are identical. It is replaced by
  per-state resolution with misses routed to the fallback table.

  *Provenance.* The owner approved committing this table (2026-09-15),
  scoped to block state identifiers and defaults only. The committed table
  carries Java block, property and value identifiers and each block's
  default. These are the same kind of
  identifier data `biome_table.inc` already commits, and nothing from the
  jar's own files is copied: the report is read at generation time and
  never vendored.

- **M5-PMMP: the zend binding's boundary, read from source, and the plan the
  owner approved.** Read from `pmmp/PocketMine-MP` `stable` @ `6a7cc02`,
  `pmmp/ext-chunkutils2` 0.3.5 @ `036b4af` (the only line PMMP 5 accepts —
  `PocketMine.php` refuses anything outside 0.3.x), `pmmp/PHP-Binaries`
  `stable` @ `3296ff3` and `pmmp/ext-pmmpthread` 6.3.0 @ `b453d10`:

  *How PMMP runs a generator.* `AsyncGeneratorRegisterTask` constructs one
  `Generator($seed, $generatorSettings)` per worker thread and parks it in a
  per-thread static (`ThreadLocalGeneratorContext`) until
  `AsyncGeneratorUnregisterTask` drops it at world unload. The generator is
  handed an options string and never the world's folder. A `Chunk` is
  exactly 24 sub-chunks (indices -4 to 19, so y -64 to 319), vanilla's
  overworld extent with no remapping, and PMMP's own `Normal` generator
  already builds whole ones: `setSubChunk($y, new SubChunk(
  Block::EMPTY_STATE_ID, [$blockArray], $biomeArray))`. PMMP's `BiomeIds`
  are the Bedrock ids `lib/mapping/` produces.

  *How native data gets into a sub-chunk.* `PalettedBlockArray::fromData(
  int $bitsPerBlock, string $wordArray, array $palette)`. The words are
  host-endian `uint32` in XZY index order, packed from the low bit, with
  exact lengths per width; widths are {0,1,2,3,4,5,6,8,16} (7 is refused);
  the palette is at most 2^width entries and is not type-checked, so
  entries must be real PHP ints. chunkutils2 installs no header, and its
  object layout changed between 0.3.5 and master, so building its objects
  directly from another extension is unsupported; calling `fromData` is the
  boundary.

  *What PMMP's PHP builds are.* PHP 8.2 by default, ZTS
  (`--enable-zts`), extensions compiled in statically on Linux and macOS.
  Release tarballs delete `include/`, so no release can build an extension,
  and there is no Linux aarch64 release. pmmpthread starts every thread with
  its own request, so an extension keeps per-thread state in module globals
  and shares nothing mutable across threads without a lock.

  *The binding, as approved.* (1) `stratum_pmmp` core, plain C++: fills a
  chunk into 24 ready-to-load `{bitsPerBlock, wordArray, palette}` block
  and biome layers — block palettes hold Java state ids, deduplicated, at
  the smallest allowed width; all-air sections get no block layer. (2) A
  thin zend shim (`stratum.so`) sharing one immutable compiled pipeline per
  world across worker threads, locked only while compiling. (3) A PMMP
  plugin whose `Generator` translates each sub-chunk's few-entry palette to
  PMMP state ids through a per-worker cache (misses resolved via PMMP's
  upgrader and deserializer, `UnsupportedBlockStateException` routed to §9's
  fallback table), then calls `fromData` and `setSubChunk` — no per-block
  PHP. Three decisions, each the recommended option: **worlds load from
  their frozen blob from day one** (format 3, §6, landed first), **the
  compile-and-fill construction becomes one shared core** used by both
  `ext/` and `ext-nukkit/` (it holds the dangling-pointer ordering hazard
  §11's M5-Nukkit entry describes, which should live in one place), and
  **the extension is built against a minimal ZTS PHP 8.2 compiled from
  php/php-src**, locally and in a CI job, with chunkutils2 0.3.5 built by
  phpize for tests — no full PMMP server yet.

- **M5's PocketMine-MP binding is built end to end — and everything it
  touches on PocketMine-MP's side was read twice.** `ext/` now holds three
  layers: `stratum_pmmp_core` (packs a chunk into chunkutils2 0.3.x's own
  `PalettedBlockArray::fromData` arguments), the `stratum` zend module
  (§4.3's PHP surface), and `ext/plugin/` (registers the generator, builds
  `Chunk`s, translates palettes). Each layer's boundary was read from
  PocketMine-MP 5.44.4 / chunkutils2 0.3.5 source and then independently
  re-checked by a second reader against the same files; five claims were
  corrected that way, and the corrections are the reason several of these
  decisions are what they are:

  *The failure modes that do not announce themselves.* A sub-chunk key
  `Chunk::__construct` does not find becomes air AND an all-ocean biome
  array (`Chunk.php:76`), so the plugin always supplies every index the
  dimension covers, keyed signed (-4..19) exactly as that constructor
  indexes. An unmapped biome id is silently swapped for `OCEAN` on the wire
  (`ChunkSerializer.php:166`) — no exception, no log — which is why biome
  coverage is asserted in `lib/mapping/`'s own tests rather than trusted to
  fail loudly at runtime. And a world naming an unregistered generator does
  not merely fail to load: `startupPrepareWorlds()` returning false takes
  the whole server down (`Server.php:1072-1075`), so registration happens in
  `onLoad()`, before worlds are loaded.

  *What a generator may assume.* It is constructed per worker thread with a
  seed and an options string and nothing else — no server, no plugin, no
  config (`GeneratorExecutorSetupParameters::createGenerator`) — so the path
  to the world's frozen pipeline travels in the options string, and
  `WorldFactory` writes the blob into the world's folder BEFORE calling
  `generateWorld()`, which constructs the world (and therefore opens the
  blob on a worker) immediately. `generateChunk` is also called for
  neighbouring chunks, up to nine per population task, so it must be
  correct at arbitrary coordinates rather than only the requested one.

  *Blocks PocketMine-MP does not have.* `minecraft:powder_snow` is a real,
  measured example — PocketMine-MP 5.44.4 has no powder snow block, and the
  overworld's surface rules place it. The plugin resolves it through an
  explicit fallback (snow block), logged once per state; anything else
  unimplemented falls back to `info_update`, also logged. Never a crash,
  never a silent stone (SPEC §9).

  *What is NOT verified, and cannot be here.* The plugin has never run:
  PocketMine-MP cannot be installed on this machine (its PHP releases ship
  no headers, and it needs pmmpthread, leveldb, igbinary, morton and more
  that `tools/php-dev` does not build). CI checks that every plugin file
  parses and runs the one class that is pure PHP against a faithful stub.
  Running it against a real server is the binding's next real step, and is
  the same shape of gap `ext-nukkit/`'s Java side has.

  *Speed, measured rather than assumed.* An optimised build generates the
  overworld at **237 ms/chunk** on the development machine (freezing a
  pipeline 0.06 s, compiling a dimension 0.01 s, 2 MB peak). That is
  comfortable for background generation and slow beside PocketMine-MP's own
  generators; the density evaluation and the per-quart biome search dominate
  it, not the packing. M5's performance pass now has a number to work
  against.

- **The legacy named-noise seeding, read straight out of the golden Nether
  regions — a second oracle, a passing control, and still no survivor.**
  Until now every attempt on this question inverted a *synthetic probe
  dimension's terrain height* (`tools/analysis/legacy-seed-analyze.cpp`).
  The eight golden Nether regions were sitting unused and are a different
  and much larger oracle: **262144 columns a region, 8 regions, SIX
  independent worlds** (java.util.Random discards bit 63, so 0 ==
  Long.MIN_VALUE and -1 == Long.MAX_VALUE), every one of them painted by the
  real legacy seeding of exactly the six noises the Nether's surface rule
  names. `tools/analysis/legacy-goldens-surface-analyze.cpp` and its shared
  decoder `legacy-goldens-surface-decoder.hpp` read them;
  `tests/conformance/vanilla_legacy_goldens_surface_test.cpp` includes the
  same decoder rather than a second copy of it.

  *The decoder is derived, not hand-read.* A `noise_threshold` is a sign
  test, so a placed block is one bit about a noise — but only for the
  conditions the tree consulted before placing it. The decoder walks the
  resolved `surface::RuleGraph` at each position, evaluates every condition
  it can evaluate from the golden region, and BRANCHES on every one it
  cannot: the noise thresholds themselves, a `vertical_gradient` strictly
  inside its own band (outside it the gradient is certain, which is
  arithmetic and not a seeding assumption), `above_preliminary_surface` and
  `temperature` unless supplied. A condition is observed only where every
  assignment reproducing the golden block agrees on it. Two conditions on
  one noise are not independent, and an assignment is pruned as soon as the
  intervals it asserts about that noise have no value in common.

  *The surface depth is ENUMERATED, not assumed.* `minecraft:surface` is
  itself a named noise, so under a legacy source every `stone_depth`
  carrying `add_surface_depth`, every `hole` and every
  `surface_depth_multiplier` would be unreadable if the depth had to be
  known. It does not: depth is `(int)(2.75 * surface(x,0,z) + 3.0 + 0.25u)`,
  and `minecraft:surface` is firstOctave -6 with amplitudes [1,1,1], whose
  persistence schedule sums to 1 per stack, two stacks, valueFactor
  `(1/6)/(0.1*(1+1/3))` = 1.25 — so with |perlin| <= 1 the noise is in
  [-2.5, 2.5] and the depth in **[-3, 10]**. The decoder enumerates
  **[-4, 11]** and reports a bit only where it is determined for every
  depth in that range, so widening the range can lose bits and never invent
  one. (For scale: over the 8589934592 columns swept for
  `above_preliminary_surface` the raw value never left [-1.14, 1.14].)

  *THE CONTROL, and it passes.* The identical decoder over the eight golden
  OVERWORLD regions, whose surface-rule noises are modern-seeded and already
  exact here. At stride 1 — **262581476 positions decoded**:

  | arm | result |
  | --- | --- |
  | replay: the library's own `surface::Executor` on the RECONSTRUCTED Context reproduces the golden block | **262406910 / 262581476 = 99.9335%** |
  | positions the tree cannot explain at all, among those | **0** |
  | recovery: decoded bits against the TRUE modern noise | **30104 / 30104 = 100.0000%** |
  | the trivial predictor (always the commoner answer) | 86.37% |
  | negative: the same bits against the same noises at worldSeed + 1 | **78.97%** — *below* the trivial predictor |

  The replay arm is the one that matters and it was added because the first
  control failed at 99.77%. A Context is reconstructed out of a POST-rule
  region, and the overworld's tree places `minecraft:water` and
  `minecraft:air`, so a rule that freezes a water surface to ice moves the
  column's water height and no post-rule region can undo it. The water
  height is therefore carried as an INTERVAL — the golden's topmost fluid,
  up through any rule-placeable solid above it — and a `water` condition is
  branched on wherever the two ends disagree; the residual 0.0665% is gated
  out and counted rather than absorbed. **The Nether's tree contains no
  `water` and no `steep` condition at all**, so that whole failure mode is
  absent from the run this control is for.

  *WHAT THE NETHER READBACK REACHES.* At stride 1 over all eight regions:
  **176537818 positions decoded**, **262795 (0.1489%)** of which the tree
  cannot explain at all. Decoded COLUMNS per noise, after pooling the
  several conditions that read each one at the same threshold:

  | noise | threshold | columns | bit is true |
  | --- | --- | --- | --- |
  | `netherrack` | >= 0.54 | **688833** | 2.2-2.3% |
  | `nether_wart` | >= 1.17 | **674596** | **0.00% — constant** |
  | `nether_state_selector` | >= 0.0 | **545395** | 47.5-50.2% |
  | `patch` | >= -0.012 | **56330** | 40.8-43.9% |
  | `soul_sand_layer` | >= -0.012 | **41448** | 88.0% |
  | `gravel_layer` | >= -0.012 | **5182** | 65.3% |

  The right-hand column is why a bare count is not enough:
  `minecraft:nether_wart`'s threshold is 1.17 and **not one column of any
  golden region reaches it**, so its bit is constant, its null equals its
  signal, and it is not scanned at all. `netherrack`'s bit is 2.3% true, so
  its trivial predictor already scores 97.8%.

  *THE IDENTITY TEST — the one positive result here.*
  `minecraft:soul_sand_layer` and `minecraft:gravel_layer` are
  byte-identical (firstOctave -8, amplitudes [1,1,1,1,0,0,0,0,0.01333…])
  and differ only by name. The `nether_wastes` branch reads both at -0.012
  at the same column, and the decoder's joint table over 6 worlds is
  **(F,F) 1800, (F,T) 1398, (T,F) 0, (T,T) 6** — the shape the tree
  predicts, since `gravel_layer` is only consulted where `soul_sand_layer`
  already failed. **Under one field the (F,T) cell is impossible, and it is
  1398 of 3204 columns (43.63%).** So the identifier — its hash, or the
  order the noises are built in, which this cannot separate — reaches the
  seed. That agrees with, and is independent of,
  `legacy-seed-analyze --twin`.

  The (T,T) cell is the decoder's own error bar and is read as one rather
  than hidden: the tree cannot consult `gravel_layer` at a column where
  `soul_sand_layer` succeeded, so those **6 of 3204 columns (0.19%)** are
  the decoder being wrong. The claim rests on a cell three orders of
  magnitude larger than that rate.

  *THE SCAN, and its MEASURED null.* The same 270,000 candidates (900 seed
  rules x 300 block offsets) scored against the decoded bits. The null had
  to be measured rather than computed: the decoded columns are spatially
  clustered and a candidate noise is spatially smooth, so a wrong
  candidate's effective sample size is patches, not columns, and `sqrt(n)`
  would call every leader an impossible outlier. An evenly spaced
  thousandth of the space (997 candidates) is rescored on the FULL column
  set to get it, and the leaders of the whole space are rescored there too:

  | noise | columns | trivial predictor | BEST of 270000, full set | measured null, full set | deepslate's own rule (182, 0) |
  | --- | --- | --- | --- | --- | --- |
  | `nether_state_selector` | 545395 | 50.63% | **51.36%** | 50.02% ± 0.61, max 52.21% | 49.68% |
  | `netherrack` | 688833 | 97.77% | **96.02%** | 95.66% ± 0.32, max 96.66% | 95.61% |
  | `patch` | 56330 | 59.87% | **64.74%** | 49.74% ± 3.04, max 59.25% | 47.53% |
  | `soul_sand_layer` | 41448 | 68.69% | **83.96%** | 51.03% ± 9.11, max 81.90% | 51.98% |
  | `gravel_layer` | 5182 | 65.26% | **89.89%** | 50.33% ± 11.03, max 81.42% | 32.61% |

  **No survivor.** On the two noises with hundreds of thousands of columns
  the null is tight and NOTHING in the space clears the null's own observed
  maximum — the best of 270000 candidates for `nether_state_selector` is
  51.36% against a null max of 52.21%, and for `netherrack` it is 96.02%
  against a null max of 96.66% and a trivial predictor of 97.77%. The three
  small-column noises have nulls 3 to 11 points wide, precisely because
  their columns sit in a few patches, and their leaders are 3 to 4 sigma out
  — which is where the extreme of 270000 draws from such a null belongs, and
  is nowhere near a correct rule. **A correct rule scores 100%**, and that
  is not an assumption:

  *THE SCAN CAN FIND A CORRECT RULE.* `--plant <rule> <block>` replaces the
  server's bits with the bits that candidate would have produced and runs
  the identical scan: it returns **rank 1 at 543/543 thinned and 7599/7599
  full — 100.0000% — against a runner-up at 66.1%**. So "no survivor" is a
  measurement of the space, not a property of the apparatus.

  *What this readback EXCLUDES.* The 270,000 candidates of that space, for
  the five noises whose decoded bit is two-sided, against up to 688833
  columns each of the server's own Nether — a second, independent oracle
  refuting the same space the probe dimensions refuted, with a control that
  recovers a known-correct seeding through the identical decoder.

  *What it does NOT.* It does not widen the space: the same gaps stand —
  ONE stack rule (sequential Perlin blocks from one generator), no
  per-octave salting, no frequency rule but the declared `firstOctave`, and
  no discarded-LCG-STEP offset of the kind `minecraft:end_islands` uses.
  `minecraft:nether_wart` is observed on 674596 columns and carries NO
  information, as the table above says. The Nether run has no replay arm,
  because its tree cannot be compiled under a legacy source at all, so its
  reconstruction is bounded only by the overworld's 99.9335% and by its own
  two reported rates: positions the tree cannot explain (**262795 of
  176537818, 0.1489%**) and CONTRADICTIONS — columns where two positions
  decode one condition both ways, which cannot honestly happen because a
  `noise_threshold` samples at (x, 0, z) and is constant down a column.
  **8514 of 238576** columns on the worst-affected condition, 0 on the
  best. Contradicting columns are dropped whole rather than resolved
  first-wins, which would keep exactly the wrong half. Their likely cause is
  named rather than waved at: vanilla's own `hole` branch replaces a solid
  block with LAVA below y = 32, which turns a solid position fluid and
  leaves the reconstructed stone-depth run one short, and `hole` — a surface
  depth of 0 or less — fires on about 0.3% of columns.

  And the scan is one NOISE at a time against one THRESHOLD. It does not
  test whether the six noises share a seeding rule with one another, and it
  cannot: each is scored on its own decoded columns, so six independent
  refutations is what this is, not one refutation of a joint rule.

---

## 12. Content, provenance & distribution policy

- The engine ships **no Mojang data**: no vanilla worldgen JSON, no
  extracted assets, no golden fixtures derived from them. Users obtain
  vanilla presets by pointing `tools/fetch-vanilla` (or the in-server
  equivalent) at the official jar they download from Mojang.
- No Mojang source code — decompiled or unobfuscated — is read, pasted,
  transcribed, or paraphrased in this repository or by Implementer
  sessions. Since commit 7f8a5ebe, behavior may additionally derive from
  **clean-room specifications**: filtered, audited documents produced by
  an isolated Researcher process under `RESEARCHER-BRIEF.md`, containing
  methods, constants, and behavioral contracts only. Prior to that
  commit, no Mojang source was read anywhere in this project. Clean-room
  specs are hypotheses: every value they supply still enters `lib/` only
  through golden verification (§7), and each adopted value's §11 entry
  cites both the spec claim and the confirming evidence.
- Third-party datapacks are user-supplied content; the engine loads them,
  the repo does not redistribute them.
