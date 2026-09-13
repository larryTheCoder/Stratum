# Progress

A tracking dashboard for Stratum's milestones. `SPEC.md` is the source of
truth for every claim here (its own §10 for milestone definitions, §11 for
the measured narrative behind each) — this file exists to be scanned in a
few seconds, not to duplicate SPEC.md's prose. Update it whenever a
milestone or a named blocker moves.

Last swept: 2026-09-13 (the aquifer's mixed-type Π).

## At a glance

| Milestone | Status |
|---|---|
| M0 — repo scaffolding | Closed |
| M1 — core primitives + conformance harness | Closed¹ |
| M2 — 2D pipeline | Closed (its goal folded into M3) |
| M3 — 3D density | Closed for the overworld²; ore veins tracked separately, below |
| M4 — biomes + surface | Open — blocked on legacy RNG and ore veins |
| MA — Aquifers (parallel track, does not gate M4-M6) | Nearly closed — 1 level-representation slice left, 1 constant unpinned |
| M5 — integration (Bedrock mapping, PMMP binding, perf) | Not started |
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

Not started. `lib/mapping/` (Java → Bedrock block/biome mapping) and `ext/`
(the PocketMine-MP zend binding — the actual point of this project) are
both empty stubs by design, waiting on this milestone. Also unstarted:
chunkutils2 output, the PMMP world-load path, the performance pass.

## M6 (v2) — Staged features

Out of scope for now: features/structures, read/write radii, the scripting
escape hatch.

## Smaller loose ends

- The loader's strict-rejection-by-default policy for user-supplied
  datapacks is an explicitly open question in SPEC.md — not blocking
  anything, just undecided.
- `README.md` is stale relative to this session's work: it still describes
  aquifers as unwired and most surface rules as refused. Worth a pass.
