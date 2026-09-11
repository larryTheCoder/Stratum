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

  **Ore veins, started — the deterministic shape confirmed, the RNG still
  open.** Unlike the aquifer, no clean-room spec covers this: there is no
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

  *Confirmed, across five seeds and 25608 real non-stone blocks, with ZERO
  exceptions on every deterministic gate:* the y-range (`y` in `[-60, -8]`
  for iron, `[0, 50]` for copper — the dead zone `[-8, 0)` between them
  produces neither), the type/sign correspondence (`vein_toggle > 0` for
  copper, `<= 0` for iron), the richness threshold (`|vein_toggle|` must
  clear 0.6 at either y limit, falling linearly to 0.4 at 20 blocks
  inside), `vein_ridged < 0` as necessary for any vein block at all, and
  `vein_gap > -0.3` as necessary for ore over filler. The mapped-probability
  formula (ore chance = `|vein_toggle|` mapped from `[0.4, 0.6]` to
  `[0.1, 0.3]`) tracks closely: the largest bin (10584 blocks,
  `|vein_toggle|` in `[0.59, 0.60)`) reads 29.44% against a predicted 29%.
  The raw-ore rate reads 1.87% (98/5235), close to the documented 2% but
  not yet pinned to it. Filler and ore block identities are confirmed too:
  granite/copper_ore/raw_copper_block for copper, tuff/deepslate_iron_ore/
  raw_iron_block for iron — iron's whole range sits below y=0, so it is
  never anything but the deepslate variant.

  *Still open, and the reason this is not yet code.* The three random
  draws above (the 30% membership roll, the mapped-probability ore/filler
  roll, the 2% raw roll) are confirmed only in aggregate rate and shape —
  not in the per-block algorithm a bit-exact reimplementation needs to
  match the same seed's exact blocks rather than merely its statistics.
  `ore_veins_enabled` stays refused by name until it has.

  *The membership roll's salt search, run and refuted.* `rng::
  positionalSourceFor` (MD5-hashed salt → Xoroshiro128++, `.at(x,y,z).
  nextFloat()`) is the leading hypothesis — it is the same mechanism
  already confirmed bit-exact for `vertical_gradient`'s `random_name`
  condition on 27 million real blocks, and `aquifer_lattice.cpp` already
  uses the same construction under the literal salt `"minecraft:aquifer"`.
  `tools/analysis/ore-vein-rng-dump.cpp` isolates the roll: it scans probe
  worlds for every position clearing the confirmed deterministic gate
  above (y-range, sign/type, richness, `vein_ridged < 0`) and records
  whether the server actually touched it — 36725 candidate rows across
  4 seeds (69.729% touched, notably close to `1 - 0.3`). Two structural
  checks first ruled out a non-RNG explanation: touch rate is flat
  (~69-70%, no trend) when binned against `vein_ridged`'s own magnitude,
  against `vein_gap`'s value, and against `|vein_toggle|`'s value — the
  roll genuinely does not correlate with any density value already in
  hand, consistent with an independent per-block coin flip rather than a
  hidden deterministic threshold.

  `tools/analysis/ore-vein-rng-test.cpp` then tests salt candidates
  against the dataset, checking both threshold directions (`draw < 0.3`
  and `draw >= 0.3`) and reporting three views that catch different false
  positives: the aggregate rate, the per-seed min/max (an aggregate hit
  that is not ~uniform across every seed is an artifact of unequal
  per-seed sample counts, not a real derivation — caught exactly this on
  `"minecraft:vein_gap"`: 71.6% aggregate, but 45%/74%/80%/60% per-seed),
  and a copper-vs-iron split (in case the two vein types use different
  salts, which would dilute either type's real match into a confusing
  ~65-80% on a combined test). Every candidate tried so far — all four
  noise names the real density functions themselves reference
  (`minecraft:ore_veininess`, `minecraft:ore_vein_a`, `minecraft:ore_vein_b`,
  `minecraft:ore_gap`), the three router field names
  (`minecraft:vein_toggle`, `minecraft:vein_ridged`, `minecraft:vein_gap`),
  the confirmed `"minecraft:aquifer"` salt itself, and ~25 natural-language
  guesses (`ore`, `ore_vein`, `ore_veins`, `vein`, `veins`, `mineral_vein`,
  `mineral_veins`, `copper`, `iron`, `copper_ore`, `iron_ore`,
  `raw_ore`, `vein_type`, `ore_type`, `vein_membership`,
  `ore_membership`, `richness`, and others, each with and without the
  `minecraft:` namespace) — is refuted: none reads anywhere near a
  uniform ~100% in every seed and every type. Best so far is
  `"minecraft:copper"` on copper-type positions only, at 80.4% aggregate
  with a 58-81% per-seed spread — well short of the bar, not a hit.
  cubiomes (MIT, permitted) was also checked and does not implement
  block-level ore vein placement at all (its only "vein" grep hits are a
  false positive substring inside `octaveInit`), so it offers no lead
  here.

  This is a genuine open research gap, not a queued mechanical step: the
  mechanism (positional-source-plus-salt) is a strong hypothesis on
  precedent, but the salt string itself has resisted a systematic search
  of the obvious candidate space.

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

  Four things stood between MA and closing. Two (1, 2) are now fully CLOSED.
  The barrier's third source (3) is closed for three of its four parts, with
  Q6.3's water-over-lava exception still untested. Fluid TYPE (4) is narrowed
  to one open piece, the level ceiling's exact value. **`ChunkFiller` now
  calls all of it** (`aquifer::computeSubstance`, wired into `fill()`):
  measured against a real, aquifer-on overworld region, the wiring's own
  category decision is EXACT on 393216 of 393216 blocks over the four chunks
  `golden_fill_aquifer_test.cpp` pins, and 6290723 of 6291456 (99.988%) over
  a wider 64-chunk sweep — a residual small enough that the two still-open
  narrow pieces above plausibly explain the whole of it. WITH the real
  287-rule surface tree also running, `golden_fill_aquifer_test.cpp` is now
  393216 of 393216 too (§11: the deepslate surface-rule gap
  `golden_fill_test.cpp` named is CLOSED, not carried). What remains is the
  two still-open barrier/fluid-type pieces themselves, plus ore veins (M3,
  untouched) — none of which is unique to aquifers, which is why this still
  reads as a track.

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
     barrier.hpp`), and (d) remains: **Q6.3's water-over-lava exception is
     still untested by every angle**, and this predicate does not implement it
     yet. The mixed-fluid-type Π branch (`Π = 2.0`) is also still unmeasured —
     every barrier probe so far holds `lava` at a constant specifically to
     keep that question separate. Both are now called from real generation
     (`ChunkFiller`, below) rather than only from purpose-built probes, and
     neither has shown up as the identified cause of a real mismatch yet —
     see the wiring note below for the numbers.

  4. **Fluid TYPE — measured, and down to one open piece.** `fluid_type.hpp`
     scores 0.99873 per source on 3125 sources over four seeds against a
     0.94176 null, and the `lava` entry's own read position is settled:
     contracted indices on a SIXTY-FOUR block horizontal pitch, where the
     spread's is 16 (§11). **Strictness at exactly 0.3 is now CLOSED**:
     `aquifer-fluidtype-probe.sh` drove `lava` to the exact double 0.3 and to
     its two adjacent doubles, and the server's own answer lands exactly
     where the code already read it — strict `>`, confirmed to the ULP. The
     **level ceiling is NARROWED, not yet pinned**: the same tool brackets it
     from the original [-14, -5] down to {-10, -9} (-11 and -10 measure as
     lava, -8 as water), and -10 — this build's own value — fits every
     reading; -9 could not be reached to rule out on the attempt made. A
     third piece is carried on the spec's word — whether a source already
     reading lava is exempt — and cannot be observed, since those sources
     sit below the lava sea.

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
     seconds a chunk, impractical for anything real. `aquifer::LevelCache`
     memoizes a cell centre's own fluid level across one `fill()` call — a
     chunk touches dozens of distinct centres, not thousands of blocks'
     worth of them — and brought that to about 1 second a chunk, the same
     shape of fix `ChunkFiller`'s own biome cache already used for surface
     rules.

     *Wider, still open.* Over a 64-chunk sweep of the same probe world
     (6291456 blocks), RAW category is 6290723 exact — 99.988%, a 733-block
     residual too small to localise further without a dedicated probe of its
     own, but the wrong direction (mostly missed barriers, `solid->fluid`)
     is consistent with the mixed-fluid-type Π gap rather than a new one.

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

  **What it refuses.** A dimension with `ore_veins_enabled` is refused by
  name at compile, not filled approximately — not implemented at all, and
  §8 puts a world that generates and is quietly wrong in the most severe
  class there is. `aquifers_enabled` is no longer refused (MA, below):
  `aquifer::computeSubstance` decides the block wherever the density alone
  would not, called from `fill()` itself. Vanilla's overworld sets both
  flags, so this filler still cannot generate it exactly today — ore veins
  are the reason now, not aquifers.

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
  | surface depth | `(int)(2.75 * surface(x,0,z) + 3.0 + 0.25 * u)`, truncating, no clamp |
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

  `above_preliminary_surface` is implemented from the DOCUMENTED reading,
  `y >= preliminary_surface_level`, and its strictness has never been
  separated by measurement — the one probe that reached it found the condition
  true everywhere, which cannot distinguish `>=` from `>`. Both the executor
  and its test say so in place, since it is exactly the kind of thing a later
  reader would otherwise quietly "fix" in the wrong direction.

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

  The schema is written out rather than generated, and that is a debt (§11).
  Five of the fifteen are absent from mcdoc entirely, so they would be
  hand-written whatever happens — the precedent is `tools/mcdoc/schema.py`,
  which already hand-writes `blend_alpha` and `end_islands`. The other ten
  could be generated and are not: the generator cannot parse either
  surface-rule mcdoc file, and the types they refer to live in a third file it
  cannot parse either.

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
  `legacy_random_source: true`, and `NoiseRegistry::create` refuses
  `RandomSource::Legacy` outright (§11: nothing available says how a name
  becomes an LCG seed here), so there is no way yet to build the density
  chain the Nether's blocks would need in the first place. Closing the
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
  * **`above_preliminary_surface` was true everywhere** on the probe's
    terrain and so said nothing. It needs terrain where the preliminary
    surface and the real one differ, which the probe did not arrange.

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
  case) stays unmeasured: it never arises for an ADJACENT pair at all — `t`
  is provably `above + 0.5 >= 0.5` there — so only a genuine third source can
  reach it, and across every third-source pair either barrier-probe seed
  produced, it never once did (0 uses, 0 of the 866 total real-block
  mismatches). Implemented as the clean-room spec states; structurally inert
  until a configuration that exercises it is found.

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

  *Still open.* Q6.3's water-over-lava exception remains untested by every
  angle, and `placesBarrier` does not implement it. The mixed-fluid-type
  branch of Π (`Π = 2.0` when one source reads lava and the other water) is
  also unmeasured — every barrier probe so far holds `lava` at a constant on
  purpose, to keep that question separate rather than folding it in
  unverified. `ChunkFiller` still does not call `placesBarrier` at all;
  wiring it in is untouched.

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
  level forced to -32, deep enough that the still-open ceiling question
  cannot interfere from either side of its own bracket. The transition is
  exact and lands exactly where the code already read it: `lava == 0.3` on
  the server comes back water, the very next representable double above it
  comes back lava. Strict `>`, confirmed to the ULP rather than assumed.

  *The level ceiling, narrowed from ten candidates to two.* The same tool
  (group B/C) sweeps `fluid_level_spread` — past the ±1.0 any real noise
  reaches, the same kind of extreme point already used elsewhere in this
  corpus to pin an exact constant — and reads -11 and -10 as lava, -8 as
  water. That brackets the true ceiling to {-10, -9}: the -10 this build
  already used fits every reading and needs no correction, but -9 was not
  directly reachable to rule out — the one other rung whose base is
  congruent to it mod 3 collapsed to the lava-sea floor instead of the
  target level on the attempt made, for a reason not yet understood. A
  future probe should chase that rather than assume -10 without it.

  *Still not measured, and marked in the header rather than guessed.*
  Whether a source already reading lava is exempt cannot be observed at all,
  those sources being below the lava sea. And no `default_fluid` but water
  has been in this position.

  *One thing left unexplained rather than explained away.* About 4% of sources
  hold both fluids above the global lava sea, and the minority blocks are
  spread over y -21 to -54 rather than piled at the boundary. The leading
  candidate is Q6.3's water-over-lava exception, which no instrument in this
  project has touched. It is excluded from the score and counted in the test.

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
  carries where this build floors to `lambda`, and Q6.3's water-over-lava
  exception, which still nothing has measured.

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
