# Progress

A tracking dashboard for Stratum's milestones. `SPEC.md` is the source of
truth for every claim here (its own §10 for milestone definitions, §11 for
the measured narrative behind each) — this file exists to be scanned in a
few seconds, not to duplicate SPEC.md's prose. Update it whenever a
milestone or a named blocker moves.

Last swept: 2026-09-15 (M5: PMMP binding planned; freeze format 3).

## At a glance

| Milestone | Status |
|---|---|
| M0 — repo scaffolding | Closed |
| M1 — core primitives + conformance harness | Closed¹ |
| M2 — 2D pipeline | Closed (its goal folded into M3) |
| M3 — 3D density | Closed for the overworld²; ore veins tracked separately, below |
| M4 — biomes + surface | Open — blocked on legacy RNG and ore veins |
| MA — Aquifers (parallel track, does not gate M4-M6) | Nearly closed — 1 level-representation slice left, 1 constant unpinned |
| M5 — integration (Bedrock mapping, PMMP binding, perf) | Started — biome mapping landed; `ext-nukkit/` JNI interface built; block state table landed in `lib/mapping/`; binding-side resolution not started |
| M6 (v2) — staged features/structures, scripting escape hatch | Out of scope for v1 |

¹ StrictMath: only `log` is vendored (fdlibm); `exp`/`pow`/`sin`/`cos`/`atan2` deferred until a node needs them.
² `weird_scaled_sampler`'s End-dimension remainder has not been re-surveyed since M3 closed for the overworld.

## MA — Aquifers

Landed and measured: the cell lattice, the centre jitter, the fluid level
rule (ladder, ocean branch, depth-path gate), source selection (four ranked
candidates), the three-source barrier predicate with its mixed-type
pressure, and the fluid type rule. `ChunkFiller` now calls all of it
directly — the old refusal is gone.

Open:

- [x] **Q6.3's water-over-lava exception — closed, and smaller than it
      read.** It can only ever fire on ONE row, `y = min(-54, sea_level)`,
      because Q2.4 hands everything below it to the sea first. Measured on
      the server (`tools/analysis/aquifer-waterlava-probe.sh`, three
      seeds): where the row has water sources at all (a `sea_level` -70
      arm) it applies to 278 / 1102 / 1940 blocks, the bare barrier logic
      wrote stone on 14 / 50 / 70 of them and the server on 0 of 3320; one
      row up the two decisions agree on every block; a nearest source
      reading AIR on the same row still gets the server's barriers (93 /
      239 / 244), so the asymmetry is real. On the shipped sea (63) its
      population is EMPTY — no source reads water at y = -54 on any of 3
      seeds x 3 densities x 16384 columns, and both sides write 0 stone
      there. Landed in `computeSubstance`, pinned by
      `vanilla_aquifer_waterlava_test.cpp`.
- [x] **Q2.4, the global lava sea — found and closed by the same probe.**
      The substance decision consulted the lattice below the sea, so a
      block whose nearest source is centred above -54 came out WATER
      there: 7682 / 5330 / 5736 of 16384 blocks per density wrong at sea
      63, invisible to every category-only golden (water and lava are both
      "fluid" to them). Now lava before any source is read: 16384 / 16384.
- [x] **The mixed-fluid-type Π branch — measured, landed, and not the
      reading it looked like.** The spec's "`Π = 2.0` if one reads lava
      and the other water" allows three readings, and the same `sea_level`
      -70 worlds (three seeds) chose: it is what each source READS at `y` —
      a lava body meeting a water body, both fluid, takes the constant,
      while a pair that disagrees at `y` keeps the level formula whatever
      its types. Every ranked source is now typed (`StatusCache`) and
      `placesBarrier` reads the types. In mixed junctions the server's
      real barriers missed fall from 1698 to 590 pooled (1240 to 330 on
      the rows above the sea), 0 false stone before and after; every one
      of the 1108 blocks the constant adds is server stone. The reading a
      first attempt took — comparing the two TYPE FIELDS behind the
      disagree guard — made every row worse and is refuted 0 of 33 against
      252 of 252; "types differ regardless of readings" fills open air the
      server leaves open on 99.5-100% of its blocks. Pinned by
      `vanilla_aquifer_waterlava_test.cpp`'s second case.
- [ ] **The level a source carries below lambda — the dry sentinel and the
      ladder clamp.** What the branch above leaves (590 of 1698) is not a
      type question: `cellFluidLevel` reports a DRY source as `lambda`
      where the spec's is `never = -32512` (Q1.4, Q5.6), and clamps a
      ladder that falls below lambda up to it (Q5.7 has no clamp). On the
      rows 0-3 above the sea that puts a plane right under the block on
      Π's `h <= 0` side (divisors 3/10) where the spec has none. Measured
      by `aquifer-waterlava-analyze.cpp` re-scoring the same blocks at the
      spec's levels: mixed-junction misses 590 -> 0 and pure misses -> 0 on
      rows lambda+1..+3 on all three seeds, 0 false stone; row lambda keeps
      18 / 17 / 0. Not landed here because it is `cellFluidLevel`'s
      CONTRACT — readings at `y >= lambda` are unchanged, but
      `vanilla_aquifer_selection_test.cpp` reads `y < level` from y = -64
      and leans on the clamp there, nine lattice unit assertions pin
      `lambda`/`kLavaLevel` for floored or dry outcomes, and Q5.8's
      `L != never` conjunct in `fluid_type.hpp` falls out of the same
      sentinel — one slice, with the conformance suite as its guard.
- [ ] **The fluid-type level ceiling.** Narrowed to `{-10, -9}`, not pinned
      to one value; `-10` (the current code) fits every reading measured so
      far. SPEC names a next step: the one other reachable rung that should
      hit `-9` directly collapsed to the lava-sea floor instead, for a
      reason not yet understood — worth chasing before assuming `-10`
      without it.
- [ ] Q6.4's fourth divisor (the "/10" branch) — implemented per the
      clean-room spec, but never yet exercised by real data (0 uses across
      two probe seeds' worth of real barriers).

## Ore veins (SPEC's M3 section)

Started. No clean-room spec exists for this (unlike the aquifer) — the
starting hypothesis came from minecraft.wiki's public documentation,
treated throughout as something to confirm, not transcribe.

Confirmed, across five seeds and 25608 real non-stone blocks, with zero
exceptions on every deterministic gate: the y-range (iron `[-60,-8]`,
copper `[0,50]`), the type/sign correspondence, the richness threshold, the
`vein_ridged`/`vein_gap` membership gates, and the mapped-probability
formula (tracks the documented curve closely — 29.44% observed vs. 29%
predicted on the largest bin). A real, non-obvious coupling was found along
the way: ore veins only activate when `aquifers_enabled` is ALSO true, even
though the aquifer's own fluid logic never runs in this probe's fully solid
column.

Open:

- [ ] **The RNG derivation behind the three random draws** (the 30%
      membership roll, the mapped-probability ore/filler roll, the 2% raw
      roll). Confirmed only in aggregate rate and shape so far — not the
      per-block algorithm a bit-exact reimplementation needs. This is the
      reason `ore_veins_enabled` is still refused by name.

      A systematic salt search for the membership roll (the
      `rng::positionalSourceFor` mechanism already confirmed for
      `vertical_gradient` and for the aquifer's own `"minecraft:aquifer"`
      salt) is a genuine research wall, not a queued step: ~30 hand-picked
      candidates plus a 1196-entry systematic word list, sequential draws
      2-5 from every real noise/router salt (not just the first), and a
      per-seed/per-type validated test harness — all refuted (see SPEC.md's
      M3 section for the full list and the structural checks that ruled
      out a non-RNG explanation first). Both of CLAUDE.md's permitted
      external reference codebases were checked directly: cubiomes doesn't
      implement ore veins at all (false-positive grep hit only), and
      Cuberite's own README confirms it supports protocol 1.8-1.12.2 only
      — it predates the 1.18 terrain rewrite that introduced this feature
      by years, confirmed via zero hits for `NoiseRouter`/`DensityFunction`/
      `ore_veininess` in its source. Neither offers a lead. Reconsidering
      whether the mechanism differs from `positionalSourceFor` altogether
      remains the main open avenue.
- [ ] Pinning "30%"/"2%"/"20 blocks" to more precision than a ~25000-block
      sample gives, once the RNG derivation makes single-block prediction
      possible at all.

## M4 — Biomes + surface

- [ ] **`legacy_random_source` (the Java LCG derivation).** Not implemented
      at all. Blocks 4 of vanilla's 7 dimensions (Nether, End, caves,
      floating islands) from building their density chain in the first
      place. No oracle exists yet for how a noise's name becomes an LCG
      seed — a genuine open research gap, not a queued experiment.
- [ ] `above_preliminary_surface` strictness (`>=` vs `>`) — unmeasured.
- [x] **Surface-rule mcdoc schema generation debt — closed.** All 10 of the
      types mcdoc declares are now generated (`tools/mcdoc/surface.py`,
      `lib/src/surface_schema.inc`), and the loader reads them through the
      generated table rather than a hand-written switch. The other 5 —
      `bandlands`, `above_preliminary_surface`, `hole`, `steep`,
      `temperature` — are absent from mcdoc entirely and stay hand-written
      permanently, exactly as `blend_alpha`/`end_islands` already are for
      density functions; each takes no fields, so the name is all that is
      written by hand. Behaviourally identical, measured rather than
      assumed: all 7 dimensions' resolved trees dump byte-for-byte the same
      before and after (2285 lines covering every node index, every member,
      the unrunnable list and the referenced noises).

## M5 — Integration

Started. `ext/` (the PocketMine-MP zend binding — the actual point of this
project) is still an empty stub, waiting on block state mapping below —
which is back in `lib/mapping/` as a platform-neutral Java → Bedrock
blockstate table, with each binding doing only the last step through its
platform's own resolver (see the corrected architecture bullet). Also
unstarted: chunkutils2 output, the PMMP world-load path, the performance
pass. `ext-nukkit/` (a second, CloudburstMC/Nukkit binding) is further
along than `ext/` itself — a real, compiling, tested JNI interface exists —
but waits on the same table and is untested against a real Nukkit build.

- [x] **Biome mapping — landed.** `lib/mapping/` is a real CMake target
      (`stratum_mapping`) now, downstream of the conformance boundary by
      construction: `stratum_core` does not link it. `tools/mapping-sync`
      generates `lib/mapping/src/biome_table.inc` from GeyserMC/mappings
      (MIT) at a pinned commit; `bedrockBiomeId()` resolves all 65 vanilla
      biomes at 1.21.11. Now pinned to `2f0a8da`, the mappings Geyser itself
      shipped for Java 1.21.11 (it was `feature/1.21.9`'s tip; `biomes.json`
      is byte-identical between the two, so the table did not change).
- [ ] **The "nearest vanilla Bedrock biome" fallback**, for custom/datapack
      biomes outside the table. `bedrockBiomeId()` returns nothing rather
      than a placeholder default (see the function's own header for why a
      fixed fallback was rejected). Needs a similarity search over biome
      climate parameters this library does not have yet — plausibly built
      on the same `biome::ParameterList`/climate-distance machinery M4's
      biome source already uses, unexplored so far.
- [x] **Architecture corrected: block state mapping splits at a
      platform-neutral midpoint, and the shared half is back in
      `lib/mapping/`.** Supersedes the earlier "lives entirely in each native
      binding" call. Its evidence still holds (Bedrock's network runtime id
      is a hash-sorted index; multi-version servers keep one table per
      protocol), but that translation runs at network send, never at
      generation. Read directly from PMMP and Nukkit source (SPEC.md §11's
      "returns to `lib/mapping/`" entry has the citations): generators write
      the platform's own protocol-independent block id, storage is
      versioned by the platform's own data version, and every
      protocol-specific translation call is in the networking layer. Both
      platforms already resolve a named `{name, states, version}` Bedrock
      blockstate into their own id — PMMP through `BlockStateUpgrader` +
      `BlockStateToObjectDeserializer::deserialize()`, Nukkit through
      `BlockStateMapping`. So: one Java → Bedrock blockstate table in
      `lib/mapping/`, pinned per Minecraft version like biomes, and each
      binding resolves it once per distinct state through its own platform.
- [x] **Block state table — landed in `lib/mapping/`.**
      `javaBlockStateId()`/`bedrockBlockState()` resolve all 29,671 Java
      states at 1.21.11 to Bedrock `{name, states, version}` (10,491
      distinct, blockstate version 1.21.60.33). `tools/mapping-sync` builds
      it from GeyserMC/mappings `blocks.nbt`, vanilla's own
      `reports/blocks.json` (now kept by `tools/fetch-vanilla`) and Geyser's
      Bedrock 1.21.130 palette, and refuses anything it cannot verify.
      `vanilla_block_state_mapping_test.cpp` re-checks every state through
      the compiled loader; CI regenerates the table and diffs it. Measuring
      it corrected three earlier assumptions (SPEC.md §11 has the evidence):
      `blocks.nbt` is complete, not a sparse diff, so no
      `mappings-generator` oracle is needed; the pin moved to the mappings
      Geyser shipped for Java 1.21.11; and version coupling turned out to
      be per state, not a version gate. PMMP `stable` is at exactly the
      table's version, and Nukkit's older 1.21.30.7 palette still holds all
      35 states vanilla's noise settings emit.
- [ ] **The PocketMine-MP binding (`ext/`) — plan approved, started.**
      Boundary read from PMMP, chunkutils2 0.3.5, PHP-Binaries and
      pmmpthread source (SPEC.md §11's M5-PMMP entry): native code builds
      each sub-chunk's `{bitsPerBlock, wordArray, palette}` and PHP only
      calls `PalettedBlockArray::fromData` + `setSubChunk`, translating a
      few palette entries per sub-chunk. Slices, in order:
      1. [x] **Freeze format 3** — the blob now carries the biome parameter
         lists, biome temperatures and every pack noise, so a world
         generates from its blob alone (SPEC §6). `freeze::resolve` is the
         one builder; `NoiseRegistry::create` builds from stored parameters.
      2. [ ] **One shared compile-and-fill core** used by `ext/` and
         `ext-nukkit/`, compiled from a frozen pipeline, with a conformance
         check that a thawed pipeline fills the same chunk as the live pack.
      3. [ ] **`stratum_pmmp` sub-chunk encoding** — chunkutils2's word
         format, smallest allowed width, Java-state palettes; unit tested.
      4. [ ] **`stratum.so` zend shim** against a minimal ZTS PHP 8.2 built
         from php/php-src (locally and in a CI job), chunkutils2 0.3.5 via
         phpize, PHPT tests.
      5. [ ] **The PMMP plugin** — `StratumGenerator`, per-worker palette
         cache, the fallback table (powder snow first), world creation
         writing the frozen blob.
- [ ] **Block state resolution in the bindings — not started.**
      1. **`ext/` resolves the table through PMMP** — upgrader then
         deserializer, once per distinct state at generator start, catching
         `UnsupportedBlockStateException` itself so SPEC §9's explicit
         fallback table applies rather than PMMP's silent `info_update`.
         Needs a fallback entry for `minecraft:powder_snow` from day one:
         PMMP `stable` has no powder snow block, and the overworld's
         surface rules emit it (measured, not hypothetical).
      2. **`ext-nukkit/` resolves it through `BlockStateMapping`** — see
         that binding's own bullet below.
- [x] **Biome mapping's placement re-checked against the same three
      codebases — confirmed correct, not merely assumed by analogy to
      blocks.** A real biome id reassignment exists (GeyserMC/mappings:
      `minecraft:pale_garden`'s `bedrock_id` moves 62 → 193 between
      adjacent version branches — a new biome's placeholder id corrected,
      not an established one moving underfoot), so biome ids are not
      perfectly stable either. But Geyser's own
      `BiomeIdentifierRegistryLoader` loads exactly ONE unversioned
      `biomes.json` ("The server sends the corresponding Java network ids,
      so we don't need to worry about that now" — its own comment), and
      NetherGamesMC's fork has a versioned `BlockTranslator.php` but no
      `BiomeTranslator.php` at all. Biome drift happens at
      Minecraft-version granularity, which `tools/mapping-sync`'s
      pin-and-regenerate model already handles; it is not blocks' per-
      connection, per-protocol problem. `lib/mapping/`'s biome table stays
      exactly as landed above.
- [x] **Architecture decided and interface built: `ext-nukkit/`, a JNI
      binding for CloudburstMC/Nukkit.** JNI, not the Foreign Function &
      Memory API — Nukkit pins Java 8 (`build.gradle.kts`, read directly),
      and FFM needs JDK 22+. Not a foreign ask: Nukkit already ships
      `leveldbjni` in production. `ext-nukkit/` builds two CMake targets
      behind `-DSTRATUM_BUILD_EXT_NUKKIT=ON` (off by default):
      `stratum_nukkit_pipeline` (plain C++, no JVM needed to build or test
      it — `ext-nukkit/tests/pipeline_test.cpp` runs as part of
      `stratum_unit_tests` whenever this target exists) and `stratum_nukkit`
      (the JNI shim, needs `find_package(JNI REQUIRED)` — set `JAVA_HOME`
      if CMake can't find a JDK on its own). One `Pipeline` is compiled and
      shared per world, not per generation thread — Nukkit hands each async
      worker its own `Generator` via `ThreadLocal`, and `StratumGenerator
      .java` shares one native handle across them via a `ConcurrentHashMap`
      keyed by `ChunkManager`, matching CLAUDE.md's "Pipeline objects are
      immutable after compile" rule. Biome writing is fully functional
      (reuses `mapping::bedrockBiomeId()` verbatim). Found and fixed a real
      dangling-pointer bug along the way — `ChunkFiller::compile()` stores
      pointers into its `surfaceRules`/`biomeParameters`/`biomeTemperatures`
      arguments, and an early draft let those be locals `std::move()`'d
      elsewhere afterward; see `ext-nukkit/src/pipeline.cpp`'s
      `Pipeline::Impl` doc comment and SPEC.md §11's M5-Nukkit entry for the
      full mechanism. Full verification: compiles clean under
      `-Wall -Wextra`, zero clang-tidy 18 findings, clang-format clean, and
      all 376 unit tests (incl. the 4 new Nukkit ones) pass.
      `ext-nukkit/README.md` has the component's full scope.
- [ ] **`ext-nukkit`'s block state mapping — not started, waits on the
      shared table above.** `resolveNukkitFullId()` always throws today,
      naming the block, rather than guessing (SPEC §8) — real columns cannot
      fill until this lands. Nukkit resolves the same `{name, states,
      version}` triple `ext/` uses, through `BlockStateMapping.updateState()`
      → `BlockStateSnapshot.getLegacyId()/getLegacyData()`, so no
      Nukkit-specific table is needed. The resolver lives on the Java side,
      though, so the JNI boundary likely changes: Java builds the id lookup
      once, native `fill()` only indexes it. Detect misses with
      `getStateUnsafe()` (null), not `getState()` (silently `info_update`).
      Nukkit keys its palette by the full `NbtMap`, `version` included, and
      is at 1.21.30.7, so match on name and states with Nukkit's own version
      stamped; every terrain state measured present that way.
- [ ] **`ext-nukkit`'s Java side — written, never compiled.** `StratumGenerator
      .java`/`StratumGenerationException.java` match Nukkit's `Generator`
      contract as confirmed by reading Nukkit's source, but no Nukkit
      Gradle/Maven dependency exists in this repo to actually build or run
      them against. `PalettedBlockStorage`'s exact per-section coordinate
      convention for biome storage (block-local vs. quart-local) is flagged
      unconfirmed in the file's own header for the same reason.

## M6 (v2) — Staged features

Out of scope for now: features/structures, read/write radii, the scripting
escape hatch.

## Smaller loose ends

- The loader's strict-rejection-by-default policy for user-supplied
  datapacks is an explicitly open question in SPEC.md — not blocking
  anything, just undecided.
- `README.md` is stale relative to this session's work: it still describes
  aquifers as unwired and most surface rules as refused. Worth a pass.
