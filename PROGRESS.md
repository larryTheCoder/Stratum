# Progress

A tracking dashboard for Stratum's milestones. `SPEC.md` is the source of
truth for every claim here (its own §10 for milestone definitions, §11 for
the measured narrative behind each) — this file exists to be scanned in a
few seconds, not to duplicate SPEC.md's prose. Update it whenever a
milestone or a named blocker moves.

Last swept: 2026-09-18 (M5: the PocketMine-MP binding is built end to end, never run).

## At a glance

| Milestone | Status |
|---|---|
| M0 — repo scaffolding | Closed |
| M1 — core primitives + conformance harness | Closed¹ |
| M2 — 2D pipeline | Closed (its goal folded into M3) |
| M3 — 3D density | Closed for the overworld²; ore veins closed too (below) |
| M4 — biomes + surface | Open — blocked on legacy RNG |
| MA — Aquifers (parallel track, does not gate M4-M6) | Nearly closed — 1 constant unpinned, 1 golden residual unattributed |
| M5 — integration (Bedrock mapping, PMMP binding, perf) | Started — mapping tables, shared generation core, `ext/` encoder + zend module + plugin all landed; never run against a real PocketMine-MP server; perf pass open (237 ms/chunk) |
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
      real barriers missed fell from 1698 to 590 pooled (1240 to 330 on
      the rows above the sea), 0 false stone before and after; every one
      of the blocks the constant adds is server stone. (Re-anchored by the
      slice below: the same pooled reading is now 1100 to 4.) The reading a
      first attempt took — comparing the two TYPE FIELDS behind the
      disagree guard — made every row worse and is refuted 0 of 33 against
      252 of 252; "types differ regardless of readings" fills open air the
      server leaves open on 99.5-100% of its blocks. Pinned by
      `vanilla_aquifer_waterlava_test.cpp`'s second case.
- [x] **The level a source carries below lambda — the dry sentinel and the
      ladder clamp.** LANDED. `cellFluidLevel` reported a DRY source as
      `lambda` where the spec's is the sentinel `never` (Q1.4, Q5.6), and
      `ladderLevel` clamped a ladder that fell below lambda back up to it
      (Q5.7 has no clamp). Neither is visible in a block readout — Q2.4
      hands every row below lambda to the global lava sea, so every
      candidate level at or below it paints identical chunks — which is why
      four campaigns recorded the dry outcome as "Λ" and were not wrong,
      only under-determined. Π sees it: on rows lambda-1..+40 of the three
      water/lava worlds, against 88949 server stone blocks, mixed/pure
      real-barrier misses run 704/1790 for the old contract, 33/758 for the
      sentinel alone, 677/1063 for the unclamped ladder alone and 7/54 for
      both, with **0 false stone under every model**. On the independent
      `barrier3way` world the three-source miss count falls 121 of 11923
      real barriers (1.015%) to 6 (0.050%), the 6 a strict SUBSET of the
      121 — 115 fixed, none introduced, over 15728640 blocks. The exact
      sentinel VALUE is not observable: the sweep saturates at 32 below
      lambda (K=32, 64, 256 and -32512 byte-identical), so `-32512` lands
      as Q1.4's arithmetic `16 * min_y_limit`, not as a reading. Q5.8's
      `L != never` conjunct became representable and is carried in
      `fluid_type.hpp` as spec hygiene — provably inert, since a source at
      the sentinel reads fluid at no real `y` and every consumer of a type
      is guarded by a reading. Pinned by the whole conformance suite;
      `vanilla_aquifer_selection_test.cpp`'s readout two now runs from
      lambda up (below it the readout's premise is Q2.4's, not a source's)
      with the 999 misses identical to the digit but the population 655360
      blocks smaller — every one of them a prior unearned agreement, since
      under the old clamp `y < level` was trivially true below lambda. That
      gate is load-bearing, not cosmetic: without it the new model predicts
      air where the server has fluid on all 655360 and the case scores
      0.96060 against its own 0.9999 bound. And
      `vanilla_aquifer_barrier3source_test.cpp`'s bound is tightened from
      5% to 0.5%.
      *One guess refuted in passing:* SPEC attributed the 64-chunk
      golden-fill residual to this very defect. Re-measured under both level
      models in the same binary, the mismatching blocks are IDENTICAL
      coordinate for coordinate — 6290839 of 6291456 either way. That
      residual was unattributed again; 425 of its 617 blocks are now
      attributed to the barrier's agree-guard instead (see the "/10" entry
      below), leaving 192.
- [x] **The fluid-type level ceiling.** PINNED at `-10`, inclusive — the
      value the code already carried, now measured rather than assumed, and
      no source change. `aquifer-fluidtype-probe.sh --group d` reaches level
      `-9` by the two routes the ladder's mod-3 lattice does not constrain
      (the sea branch; the psl cap at two different sea levels) and reads
      `-12/-11/-10` lava and `-9/-8/-7` water on the two six-dimension arms
      (the third, at `sea_level` -70, runs four: -12/-10 lava, -9/-8 water),
      the level entry 16384 of 16384
      columns per dimension with 0 of the other fluid, identical on seeds
      42, 7 and 999. Beyond the question asked, the same run shows the
      ceiling is ABSOLUTE: three sea levels (63, -16, -70) put the
      transition at the same absolute pair, refuting a sea-relative rule
      (`L <= sea_level - 73`, indistinguishable at the shipped sea) and a
      lambda-relative one. The earlier collapse is explained too — that rung
      sat a whole band above the observable one, so its `-9` sources placed
      nothing and only the global lava sea was left (SPEC §11). Pinned in
      `aquifer_fluid_type_test.cpp` by a literal `-9` assertion, the one the
      old suite lacked: every other assertion there is phrased relative to
      `kLavaLevelCeiling` and so survived either value.
- [x] **Q6.4's fourth divisor (the "/10" branch).** CONFIRMED at 10 on real
      server data, bracketed to within 1%. The "0 uses" was never a
      world-shape problem, which is why any number of further probe seeds
      would have kept returning the same null: `termFires` carried an agree-guard
      (`aFluid == bFluid -> no barrier`) of this build's own invention, and
      on the domain that guard admits `t >= 0.5` for EVERY integer
      `(L_A, L_B, y)` — so `3 + t >= 3.5 > 0` and the `/10` arm could not
      fire for any input at all. The `/2.5` arm was dead the same way. That
      is a proof rather than another null count: 0 of 3135020 disagreeing
      combinations reach either arm.
      *The guard is REFUTED by the server.* `aquifer-deepfloor-probe.sh`
      pins `fluid_level_floodedness` to a constant 0.6, between the two
      gates `lattice.hpp` measures, so every cell takes the LADDER rather
      than `sea_level` and neighbouring sources hold DIFFERENT levels —
      the one thing the older barrier worlds could not produce. Over three
      seeds x seven dimensions, 110097250 blocks against 4982316 server
      stone: guarded 480354 misses, both-air lifted 457970, both-fluid
      lifted 22384, un-gated **0 misses and 0 false stone**. The four older
      worlds order the same way (161/46/154/39 of 243887). The control arm,
      real floodedness with nothing rescaled, reproduces `barrier3way`
      exactly.
      *The divisor.* The arm decides 96292 blocks; on the 65231 where 10 and
      3 disagree the server has stone on 65231 of 65231. The bracket is
      two-sided and monotone — 9.9 leaves 937 barriers unwritten, 10 is
      exact, 10.1 writes 872 blocks of false stone — and `/2.5` brackets the
      same way (2.4 misses 1104, 2.5 exact, 2.6 writes 1111). Both arms can
      now be named: `/2.5` is the barrier's LID above the higher of two
      levels, `/10` its FLOOR four or more below the lower.
      *Bonus:* 425 of the 617-block golden-fill residual, unattributed since
      the level defects were refuted as its cause, ARE the guard. The new
      residual is a strict subset (192, 0 introduced) and is all "we say air,
      the server says water" — a fluid-extent question, not a barrier one.
      Pinned by `vanilla_aquifer_deepfloor_test.cpp`, which fails loudly if
      the arm ever stops deciding blocks.

## Ore veins (SPEC's M3 section)

**Closed.** No clean-room spec exists for this (unlike the aquifer) — the
starting hypothesis came from minecraft.wiki's public documentation, treated
throughout as something to confirm, not transcribe.

The derivation is confirmed per block against the vanilla server on **79790
of 79790 candidate positions across 11 contributing seeds, 100.000%** — on
every seed alone and on copper and iron alone. (Three of the fourteen seeds
run, 200/24680/99991, contribute zero candidate rows and are not counted.)
A separate PLACEMENT probe agrees on its 11455 solid candidates, its other
25509 being air; those are seed-100 positions already inside the 79790,
re-measured at a different density, so they are reported beside the RNG
figure rather than added to it. All three draws come from one generator,
`rng::positionalSourceFor(worldSeed, "minecraft:ore").at(x, y, z)`:
`nextFloat() < 0.7` for membership, then the mapped-probability ore/filler
roll against `vein_gap > -0.3`, then `nextFloat() < 0.02` for raw metal.
`ore_veins_enabled` is no longer refused; `lib/src/ore_vein.cpp` implements
it and `ChunkFiller` calls it. Coverage: `tests/unit/ore_vein_test.cpp` and
`tests/conformance/vanilla_ore_vein_test.cpp` and
`tests/conformance/vanilla_ore_vein_placement_test.cpp`.

Placement was measured separately from the derivation, because the solid
probe is solid everywhere and so cannot see it: veins replace **solid ground
only**. On `tools/analysis/ore-vein-placement-probe.sh`'s sign-varying world the
confirmed chain would have placed 17813 vein blocks at positions the server
left as air and the server placed zero, over 25509 air positions, while all
11455 solid positions came back exact. The same probe's third dimension
settles what happens afterwards: surface rules repaint the default block and
never a vein block (12934 vein blocks kept, 5548 stone positions repainted),
which the filler's surface pass now honours — without it the overworld's own
`deepslate` rule, unconditionally true below y = -8, would erase every iron
vein in the world.

The earlier salt search's ~1226 refuted candidates were swept against a
hard-coded threshold of 0.3, while the measured marginal touch rate — 69.729%
— was recorded in the same document. At the correct salt a 0.3 threshold
reads 60.294%, a near-miss well under the instrument's 85% bar, so
`"minecraft:ore"` was tried and refuted along with everything else. SPEC's M3
section keeps the full account; the transferable part is that an instrument
which can only be wrong in one direction refutes the right answer as
confidently as the wrong ones.

Open:

- [ ] `nextFloat()` vs `nextDouble()` for the three thresholds, and whether
      draw 2 is consumed when `vein_gap <= -0.3`. Both pairs read 100.000%
      on all 79790 rows — one `nextLong()` backs both precisions and no row
      lands between them, and nothing downstream reads a position's stream.
      Separating either needs a probe that takes a FOURTH draw from the same
      generator. Recorded in `lib/include/stratum/ore/vein.hpp` beside the
      code; neither can change a block this build places.

## M4 — Biomes + surface

- [ ] **`legacy_random_source` for NAMED noises (the Java LCG derivation).**
      Still open, still blocks 4 of vanilla's 7 dimensions (Nether, End,
      caves, floating islands) from building their density chain — but it is
      now a searched wall with a reproducible apparatus rather than an
      untried gap, and the old wording here ("no oracle exists yet") was
      wrong twice over.

      An oracle exists and it disagrees with vanilla. deepslate's
      derivation — base = `JavaRandom(worldSeed).nextLong()`, XOR
      `md5_first8("ns:path")`, one further LCG fork — is a member of this
      project's own scanned candidate space (rule 182, block 0) and scores
      10-25 of 2304 columns, 0.43-1.09%, on the legacy probe dimensions at
      two world seeds, against a measured null mean of 0.64-0.72%. That is
      the null.

      The scan around it: 900 seed rules x 300 block offsets = 270,000
      candidates per dimension, over 9 probe dimensions and 2 seeds, no
      survivors. `tools/analysis/legacy-seed-analyze.cpp`'s header states the
      space exactly, and states what it does NOT cover (one stack rule; no
      frequency sweep at all). The search is calibrated: a planted candidate
      comes back at rank 1, sole survivor, 2304/2304, in every legacy
      configuration.

      And `tools/analysis/density-probe.sh` can now generate the worlds any
      of this rests on — `legacy_random_source` is a per-entry spec field
      instead of a hardcoded `False`, and a spec ships the noises it names in
      a `<spec>.noises.json` sidecar. Before that, none of this kind of
      measurement could be reproduced from the repository at all. SPEC §11
      carries the numbers.
- [x] **The half of it that is settled: legacy `old_blended_noise`.** A
      dimension declaring the flag seeds it with
      `new java.util.Random(worldSeed)` — no fork, no name salt — read the
      **modern** way, not the pre-1.18 way (they differ by exactly 128x at
      y = 0). Implemented as `BlendedNoise::legacyFromWorldSeed`, selected by
      `Interpreter` on a Legacy registry, and guarded by
      `tests/conformance/vanilla_legacy_blended_test.cpp`: 13824 of 13824
      cell-corner columns over three world seeds and two output scales — the
      denominator being generated columns, 2304 of each dimension's 7056
      corners, the rest chunks the server never built (a geometric exclusion,
      not a value-dependent one) — with the mirror control holding in the
      flag-off dimensions of the same worlds. Reachable but unreached — `NoiseRegistry::create` refuses a
      Legacy source first, and whether that refusal should be narrowed is a
      separate open question this did not touch. Worth more than its size:
      the End's router references zero named noises and the Nether's
      `final_density` is pure `old_blended_noise`, so `minecraft:end_islands`'
      own seeding is the obvious next probe.
- [x] **`above_preliminary_surface` — measured, and the question was wrong.**
      It was carried for two milestones as "strictness (`>=` vs `>`),
      unmeasured". The strictness was never the unknown; the COMPARAND was.
      Measured:

          above_preliminary_surface  ==  y >= psl + surfaceDepth(x, z) - 8

      with `psl` floored. The condition opens 2 to 8 blocks BELOW the level it
      is named after, and the 8 is a literal — invariant across cell heights
      4/8/16, cell widths 4/8/16, three floors, three heights and three sea
      levels: 12 geometry dimensions, each scoring all of its own 36864
      columns, 442368 of 442368. That 442368 is 12 RE-SCORINGS of the same
      36864 columns at one seed, not 442368 independent columns — the geometry
      varies, the columns do not. The independent column evidence is 36864
      columns x 2 seeds. Since the boundary is true at that y and false one
      block under it on every column, `y >= C` and `y > C - 1` are the same
      predicate: the strictness could never have been the answer.

      What made it measurable was not more chunks. The probe that "found the
      condition true everywhere" is still on disk (`probes/surf`, entry `aps`)
      and was never true everywhere: the earlier analysis read the terrain's
      TOP block, where a condition of this shape is true by construction,
      instead of the bottom edge of the band the rule paints. That edge is a
      clean single-block step in all 36864 columns. The second half was a
      lever the old probe did not have — `density-probe.sh` pins
      `preliminary_surface_level` to the constant 0 unless an entry overrides
      it, so the old run measured one psl and could not have told a psl
      dependence from a fixed offset.

      Scale: 3 probe specs, 52 dimensions, 2 seeds (42 and 31337), 36864
      scored columns each. Candidates refuted, each on every column rather
      than by a margin: `psl` itself (the implemented reading, 0 of 36864);
      `psl - (surfaceDepth + 3)`, the leading hypothesis going in, which had
      the depth's sign backwards (0 of 36864); a flat `psl - 6`, i.e. the
      modal column (16509 of 36864); the depth's dependence with the sign
      flipped, `psl + 8 + depth` (0 of 36864); with the magnitude doubled,
      `psl + 2*depth - 8` (516 of 36864, the depth-0 columns only); the depth
      without its `0.25 * nextDouble` jitter, which is the near miss worth
      naming (31991 of 36864 — it agrees on seven columns in eight, so a
      harness that sampled rather than scored every column would have
      accepted it); the 8 as `2 * cellHeight`
      or as any function of cell width, floor, height or sea level (refuted by
      the 12 geometry dimensions).

      SEPARATED, and this was the last thing open about the boundary's own
      FORMULA — where a spatially varying `preliminary_surface_level` is
      sampled is a separate question and stays open, below. There
      is **NO bottom clamp AT 0**: `max(0, surfaceDepth)` is refuted and the
      depth enters as returned, negative values included. At 0 only — every
      separating column is at depth exactly -1 and the lowest raw in the
      8589934592-column sweep is -1.134416806, so a clamp at -1 or lower is
      not separated by any of this (below).

      The two differ only where the RETURNED depth is negative. The depth is
      `(int)(2.75*surface + 3 + 0.25*u)` and that cast TRUNCATES toward zero,
      so the raw value has to reach -1, not merely go negative. This project
      answered that twice from samples that could not support either answer —
      first "about one column in 22000" (far too common), then "vanilla's
      amplitudes do not produce one at all" (false). The second rested on
      every column of the eight golden `r.0.0` regions, 2097152 of them: 0
      negative, minimum 0, 6745 at exactly 0 (one in 311), 207 columns with a
      negative RAW value (all at seed 0), lowest -0.449658. That census is
      correct and reproduces; it is also ~40x too small to expect one hit.

      THE SEARCH, widened. `aps-boundary-analyze sweep` (new, committed,
      parallel — 6.85M columns/sec on 12 threads) over the same eight seeds
      and x, z in [-16384, 16384): 1073741824 columns each, **8589934592 in
      all**, **98** columns at depth -1, **1 in 87652393**, on six of the
      eight seeds and in ten distinct regions. Per seed: 0 → none, 1 → 28,
      -1 → none, 42 → 27, -4172144997902289642 → 16, 2891948927356891 → 1,
      9223372036854775807 → 3, -9223372036854775808 → 23. The lowest raw
      reached is -1.134416806. The raw tail falls smoothly across -1 (0.01
      bands from [-0.81,-0.80) down: 68, 62, 55, 50, 46, 50, 62, 41, 41, 33,
      33, 34, 16, 22, 26, 19, 18, 17, 13, **19** in [-1.00,-0.99), 13, 11, 12,
      9, 8, 9, 9, 1, 10, and 16 for everything below -1.09), so the crossing
      columns are the distribution's continuation rather than its edge. The
      bands strictly below -1.00 sum to 98 — the same count arrived at
      independently, which is the histogram's own consistency check.

      The 98 are NOT 98 independent draws. The field is smooth, so a crossing
      column has near-crossing neighbours: they fall into TEN spatially
      compact clusters, one per region — 23, 22, 18, 12, 7, 5, 4, 3, 3, 1.
      1 in 87652393 is the figure for "how much area to sweep to find one",
      which is all it is used for; ten is what bounds how well that rate is
      itself pinned. The three probe cases take three DIFFERENT clusters at
      three different seeds for exactly this reason. The previous
      revision's [-4096, 4096) sweep is a sub-window and still reproduces
      exactly: four columns, all at seed -4172144997902289642.

      THE OVERWORLD READ, which does NOT settle it — run rather than assumed.
      Region `r.4.3.mca` at seed -4172144997902289642 was generated and the
      one disagreed-about block, `y = psl - 9 = 15`, read at all four columns:
      `minecraft:stone`, four for four. That refutes nothing. All four are
      `warm_ocean`, ocean floor at y = 36, psl = 24, so y = 15 is 21 blocks
      deep in stone, and at `surfaceDepth == -1` every arm of the gated
      subtree declines there under BOTH candidates — arms 0/2/4/3.0 need a
      solid-run depth of 0; arm 3.1, the whole grass/dirt/gravel/mud family,
      is gated by `stone_depth(floor, offset 0, add_surface_depth true)` whose
      threshold is `0 + surfaceDepth = -1` and a run depth is never negative,
      so that arm is off everywhere in such a column; arms 3.2/3.3 reach 5 and
      29 deep but only in warm_ocean/beach/snowy_beach and desert, and 21 > 5;
      arm 1 is badlands only; `NOT(hole)` is false because `hole` is exactly
      `depth <= 0`. Both candidates predict stone. Features were not a
      confound either way: the world is generated terrain-only, carvers and
      features stripped per biome, so nothing ran after the surface pass.

      And it could not have settled it for a second reason, which is why the
      real separation is done where it is: `y = psl - 9` and `y = psl - 8` are
      located using STRATUM's own per-column `preliminary_surface_level` — the
      psl of 24 is this engine's value, not one the server reported — and how
      the server SAMPLES that quantity is still open (lattice + interpolation,
      not per column). A wrong psl moves both candidate edges together and
      reads the wrong y without saying so, which is circular exactly where a
      separation must not be. Hence the case is pinned as a non-result, and
      the reading that does settle the clamp lives in probe dimensions where
      `preliminary_surface_level` is a datapack CONSTANT: the y is fixed by
      the datapack, the sampling question cannot reach it, and the edge
      tracking 100 / 40 / 0 / -20 is itself the check that it did not.

      THE READ THAT DOES SETTLE IT. `tools/analysis/aps-clamp-probe.sh` drops
      the gated subtree from the question entirely: solid column, pinned
      `preliminary_surface_level`, and the single surface rule
      `{ above_preliminary_surface -> diamond_block }`, so the marker band's
      lower edge IS the boundary at single-block resolution. The surface-depth
      field is a function of (world seed, x, z) alone, so the same columns
      carry depth -1 in a probe dimension; `density-probe.sh` gained
      `--origin-chunk` so the probe is forceloaded where they are.

      Three cases — three seeds, three regions — times five pinned psl values
      (100, 100 again in a second dimension, 40, 0, -20):

        s1  seed -4172144997902289642  r.4.3    4 separating columns
        s2  seed 42                    r.6.10  22 separating columns
        s3  seed -9223372036854775808  r.13.26 23 separating columns

      49 separating columns and 245 separating readings over 15 generated
      dimensions — which are FOUR distinct dimension configurations plus one
      deliberate byte-identical repeat (`k_p100_b` repeats `k_p100`) in each
      of three worlds: twelve distinct (seed, psl) pairs, three repeats.

      The denominator, MEASURED with `aps-boundary-analyze clamp` rather than
      multiplied out: **901120 painted columns**, of which 245 are separating
      readings and **900875 are controls**. Composition, per dimension:
      65536 × 10 (s1 and s2, whose windows each painted a full 16×16 chunk
      block) + 49152 × 5 (s3, which settled at 12 of those 16 chunk rows in
      z — 192 chunks, the four missing rows on the low-z edge, all 23 of its
      separating columns inside what was painted). Not 983040: that figure
      assumed all fifteen windows painted 65536 and five of them did not.

      `psl + surfaceDepth - 8` is right on every one of the 901120;
      `psl + max(0, surfaceDepth) - 8` is right on every control and wrong on
      every separating column. The measured lower edge tracks the pinned psl
      exactly (100 → 91, 40 → 31, 0 → -9, -20 → -29), and the negative case is
      what rules out sign handling in the boundary rather than a clamp. Scored
      in the conformance file's "the surface depth carries no bottom clamp",
      which prints the painted total so the 901120 above is reproducible from
      this repository rather than arithmetic done by hand.

      NO CLAMP **AT 0**, and not one word further. Every separating column is
      at depth exactly -1 and the lowest raw in the 8589934592-column sweep is
      -1.134416806, so a depth of -2 or below has never been observed. A clamp
      at -1 or lower predicts the same edge as no clamp on every column in
      this reading and is NOT separated by it. What is refuted is
      `max(0, surfaceDepth)`, the candidate this project actually held.

      WHY 49 CORRELATED COLUMNS SUFFICE, since the count looks small and the
      columns sit in three compact clusters of a smooth field. The refutation
      is arithmetic, not statistical: `psl + max(0, d) - 8 >= psl - 8` for ANY
      depth field `d`, whatever its distribution and however its columns
      correlate, so the clamped candidate predicts an edge at or above
      `psl - 8` everywhere with no tail, and one correct reading of an edge at
      `psl - 9` refutes it. No sample-size question arises and no independence
      caveat is owed. The three seeds and five psl values guard the READING
      instead — a misread region, a psl that did not take, a fixed offset, a
      one-seed coincidence — which is a different job from statistical power.
      The rate below (1 in 87652393) IS statistical, and is stated as ten
      excursions rather than 98 columns for exactly that reason.

      WHAT IT DOES NOT SAY. It settles the CONDITION's boundary, not what a
      vanilla overworld visibly does at such a column: the one cluster read as
      a real overworld shows no block difference either way (above), and the
      other two were never generated as overworlds. "The clamp is unobservable
      in vanilla's block output" is a different claim and is NOT made — that
      would need a depth -1 column whose solid run TOP is at `y = psl - 9`, or
      a desert/beach one within arms 3.3/3.2's 29- and 5-block reach, and none
      has been looked for. The rate is pinned by ten independent excursions
      rather than 98 columns, so 1 in 87652393 carries about a third of its
      own size in uncertainty; it is used only to say the 2097152-column
      census was uninformative, which holds at any plausible value. And the
      window is x, z in [-16384, 16384) on the eight golden seeds — no other
      seed, no further-out window.

      AND IT IS A THREE-WAY SEPARATION. Every separating raw depth lies in
      (-1.14, -1.00), so the three candidate conversions of that double
      predict three DIFFERENT lower edges: `floor` → -2 → `psl - 10`,
      `(int)` → -1 → `psl - 9`, a bottom clamp → 0 → `psl - 8`. Measured:
      `psl - 9`, on all 49 columns at all five pinned psl values.

      `floor` is wrong on exactly two disjoint sets: the separating columns
      (raw at or below -1) and the columns whose raw is negative but above -1
      (`(int)` 0, `floor` -1). Everywhere raw >= 0 the two ARE the same
      function — per window at psl 100 / 100 / -20: 65536 - 4 - 440 = 65092,
      65536 - 22 - 436 = 65078, 49152 - 23 - 508 = 48621. The first set is
      what is new: without a raw reaching -1 nothing separates `(int)` from
      `floor` at all, which is why neither the 52 apsb dimensions nor the
      2097152 golden columns do.

      That floor row is an IDENTITY, not a second refutation, and is labelled
      so in the conformance case. Once "unclamped right == painted" holds,
      every painted column's edge IS `psl + depth - 8`, so `floor` differs
      exactly where `floor(raw) != (int)raw` — the two sets above — and
      `floored right == painted - separating - nearZeroNegative` follows BY
      DEFINITION. It is kept as a CHECK because it would catch a bug in the
      scorer itself, not because it measures anything about vanilla. What
      refutes `floor` is the unclamped row together with the separating raws
      lying in (-1.14, -1.00).

      The claim ALL of this replaces was wrong too: `surface_executor.cpp`
      and `executor.hpp` both said the depth "reaches -1 on about one column
      in 22000". It reaches -1 on none of the 2097152 golden columns and on 98
      of 8589934592 wider ones, 1 in 87652393 — nearly four orders of
      magnitude rarer than claimed.

      On real worlds, and now in BOTH directions, because counting only what
      the old reading cannot explain rewards a boundary for reaching further
      down — one at the world floor would score perfectly on it:

      * forward. Across 8 golden overworld regions, 14008 `grass_block`s the
        server placed sit BELOW `preliminary_surface_level` — blocks the old
        reading cannot place at all, since it switches the whole gated
        surface-materials subtree off there. 11579 of them fall inside the
        measured band.
      * reverse. 11953 columns have their own SURFACE — the first block from
        the sky that is neither air nor fluid, which is the position the
        materials subtree decides — inside the band the new reading opens and
        the old leaves shut. 10676 of them carry a block that subtree CAN
        place and the terrain filler cannot, which is an upper bound on
        confirmations rather than a count of them: vanilla's features place
        gravel, sand, dirt, podzol and the rest after the surface pass, and
        gravel alone is 5711 of the 10676. The remaining 1277 neither confirm nor refute it: 1265 are
        `stone`, which the subtree itself can place, and 12 are granite and
        copper ore, written by features after the surface pass. So the reverse
        direction produced no counter-example — and no confirmation of those
        1277 either. It cannot: the subtree is a `sequence` whose own inner
        rules may decline at a position its gate opened, so absence of a
        material is never a refutation. What is structural rather than
        measured is that the new boundary is never ABOVE the old one, since
        the depth never exceeded 6 in 2097152 columns.

      Landed: `lib/src/surface_executor.cpp`, plus a FLOOR in place of
      `static_cast` in `lib/src/terrain_filler.cpp` (measured: psl -0.5
      reaches the condition as -1, not 0 — datapack-only, since vanilla's
      `find_top_surface` returns integers), and the tree now requires the two
      surface noises. `surface::Executor::surfaceDepthRaw` exposes the value
      before the cast, which is what makes the clamp census a measurement
      rather than an assertion. `tools/analysis/aps-boundary-probe.sh` and
      `tools/analysis/aps-clamp-probe.sh` + `aps-boundary-analyze.cpp` (modes
      `probe`, `golden`, `band`, `bands`, `depth`, `sweep`, `window`,
      `clamp`) regenerate and re-score it;
      `tests/conformance/vanilla_above_preliminary_surface_test.cpp` scores
      it, in ten cases: the boundary, the floor-not-truncation conversion,
      the 8 as a literal, the second seed, both directions on real
      overworlds, the surface-pass control, the depth census, the four
      negative-depth columns, the bottom clamp, and the varying-psl counts
      below.
- [ ] **Where a spatially varying `preliminary_surface_level` is sampled —
      open, and found by the sweep above.** `terrain::ChunkFiller` evaluates
      the entry per column. The server does not: driven by a three-valued
      `range_choice` (-40 / 0 / 60), the value that reaches the condition
      takes 101 distinct integer values — every integer from -40 to 60, both
      extremes included and no gaps — so it is sampled on a horizontal lattice
      and INTERPOLATED before the floor. Two different counts live here and
      the earlier wording ran them together: 101 counts the psl recovered from
      the band as `boundary - surfaceDepth + 8`, while the BOUNDARY itself
      takes 104 distinct values, -47 to 56, also contiguous, because the
      column's own depth (0..6 over these columns) widens it. Both are over
      the same 36864 columns of `probes/apsb/v_psl`, and both are now asserted
      in the conformance file, so neither can drift from the fixture again.
      It is not the cell lattice — cell widths 4, 8 and 16 give identical
      output, as does wrapping the entry in `flat_cache`. A constant psl makes
      the interpolation a no-op, which is why the boundary entry above stands
      without it.

      Measured part by part rather than fitted, since this question had
      already taken two wrong universal claims:

      * pitch, by TRANSLATION and with no blending rule in the argument
        (`s_cNN` / `t_cNN` drive the same field shifted NN blocks): the
        identity a per-column read would satisfy holds on 33792 of 33792
        columns at NN = 16 and 30720 of 30720 at 32, and on 783-7489 of
        ~36000 for NN in {1..8, 12}. Both axes, two seeds.
      * anchor: 1 of 256 phases reproduces all 36864 columns; runner-up 7542.
        Stated with what it assumes — the scan runs at the translation-
        measured pitch under the double-floored blend, so it is one axis of a
        joint pitch x anchor x blend search rather than a free-standing
        measurement of the anchor.
      * blend and floors, on the only non-integer arms ever pointed at this
        entry (`f_half`, `f_quart`): 36864 of 36864 against 20423 / 12121 for
        flooring only after the blend, 3119 for truncating at the sample,
        4764 for truncating after it, 22010 for rounding, 21039 for
        quantising to the cell's lower corner and 21103 to the nearest of the
        four. `f_quart` is NOT a second observation — it is byte-for-byte the
        same server data as `f_half` (measured: 36864 of 36864 identical,
        33745 columns at psl = -1 and 3119 at 0 in both) and separates only
        the rejected models from one another. So the placement is re-measured
        at seed 31337 (36864 of 36864, refusals 20164 / 2488 / 5409 / 21465 /
        20664 / 20920) and below zero.
      * `floorDiv` vs a truncating cell index, which only NEGATIVE
        coordinates can see: `probes/psllat3`, forceloaded at chunk -12 and
        read back over x, z in [-256, -1], gives 65536 of 65536 against 4216.
      * and the whole lattice again MODEL-FREE, which assumes none of the
        above: every interior column predicted from the SERVER's own
        recovered psl at the four multiples of 16 around it — no noise
        replica, no anchor, no floor calibration. 30976 of 30976 on each of
        the 24 `psllat` dimensions, the same on each of the 9 `psllat2` and
        the 6 `apsb4`, 57600 of 57600 on each of the 10 `psllat3`: 1784064
        interior columns, all exact.

      Not the cell lattice, and that claim now has its denominator rather than
      only its conclusion: `probes/apsb4` moves cell width 4/8/16 and cell
      height WITH the varying field in place and gets 36864 of 36864 identical
      columns each time. (`probes/apsb3` moves the same knobs under a CONSTANT
      psl — the right probe for the separate question of whether the 8 is cell
      geometry, and blind to this one, since a blend under a constant is a
      no-op.) `flat_cache` around the entry changes nothing either, and
      `xz_scale` 0.5/1/2/8 all reproduce every column, so the pitch is not a
      property of the driving field.

      **The residual this item owned: 2429 of 14008 -> 0.** Per seed rather
      than pooled (1398, 404, 285, 259, 56, 26, 1, 0 -> 0, 0, 0, 0, 0, 0, 0,
      0). The hypothesis that the spread tracked terrain STEEPNESS is still
      untested — this measured where the value comes from, not what makes one
      seed's share of the residual large — and it is no longer load-bearing,
      since every block of the residual is now inside the band.

      **And both directions on real terrain, not one.** Forward, over EVERY
      `grass_block` the server wrote in the eight golden regions — 627766,
      rather than the 14008 the per-column reading had already singled out,
      so a lattice psl HIGHER than the per-column one cannot hide a
      counter-example: 0 fall below the band the lattice opens. Reverse, at
      the column's own surface: 12916 surfaces inside the lattice band, 12403
      of them a block only the gated subtree places (96.03%), against 10676
      of 11953 (89.32%) per column, both recomputed in the same pass; the 513
      that are not are 503 `stone`, 9 `granite`, 1 `copper_ore`. The universal
      underneath both — that `above_preliminary_surface` occurs exactly three
      times in the pinned tree — is now an assertion rather than a comment.

      And the count that opened the item is now PREDICTED, not just
      reproduced: enumerate the model's reachable values for arms
      {-40, 0, 60} — no fixture — and pitch 16 gives exactly 101, -40 to 60,
      contiguous. As a bound it refuses pitch 4 (57 values, 44 gaps) and 2
      (15, 86 gaps) and says nothing against 8 or 32; those die to the
      translation family, which is why the pitch was measured that way.

      `tools/analysis/psl-lattice-probe.sh` + `psl-lattice-analyze.cpp`
      (43 dimensions, 3 specs, 2 seeds, both signs of the origin);
      `tests/conformance/vanilla_psl_lattice_test.cpp` scores it in twelve
      cases, over all 43 dimensions — 1871872 columns against the replica —
      and no number in SPEC §11 now comes from an analyser run that no test
      repeats. The probes' own EXTENTS are measured there too, and they are
      not the forceloaded square: `psllat`/`psllat2` read back x, z in
      [0, 191] and `psllat3` x, z in [-256, -1], because the server generates
      a border of chunks around the 128x128 square and the whole region file
      is copied.
      `tools/analysis/density-probe.sh` gained `--origin`, without which the
      negative-coordinate half could not have been generated at all.
- [ ] **Whether the AQUIFER reads the same entry through the same lattice —
      open, and untouched by the item above.** SPEC §11 now says this in the
      specification itself rather than only here: the shipped engine reads
      this one router entry two different ways on purpose, and the aquifer
      half is unmeasured. `above_preliminary_surface` is
      where the lattice was measured; `aquifer::` reads
      `preliminary_surface_level` at its own cell centres and still reads it
      per column here. Nothing in this sweep says whether that is right, and
      the two readings are NOT close: on the three-valued field the
      per-column reading matches the server on 3600 of 36864 columns
      (`probes/apsb/v_psl`, pitch 1 in the analyzer's fit), so the aquifer is
      reading a different number from the surface rule on about nine columns
      in ten. That the aquifer's own conformance cases pass under it is
      therefore worth something — but those cases were fitted under it, and
      the aquifer's four surface consumers are gates and caps that quantise
      hard, so passing is weak evidence rather than none. What would settle
      it: re-score `probes/pslvar`'s six aquifer dimensions under BOTH
      readings and report the two counts. The readout dimensions naming each
      column's arm are already on disk beside them.
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
      2. [x] **One shared compile-and-fill core** —
         `world::CompiledDimension`, compiled from a frozen pipeline, now
         what `ext-nukkit/` generates through. A thawed vanilla blob fills
         every block and every quart biome identically to a reference built
         straight from the pack (3 chunks × 2 seeds, surface rules on).
         `ext-nukkit/` still resolves its pipeline from a live pack rather
         than a world's blob: it has no world-creation hook to store one.
      3. [x] **`stratum_pmmp_core` sub-chunk encoding** — chunkutils2
         0.3.x's word format, the smallest width it accepts, Java-state
         block palettes, Bedrock-id biome palettes, no block layer for
         all-air sub-chunks. Built on every CI platform; unit tests pin
         chunkutils2's own byte lengths, and real overworld chunks from a
         thawed blob decode back to exactly what was generated.
      4. [x] **`stratum.so` zend module** — `Stratum\freezePipeline`,
         `Stratum\Dimension::open` (one compiled dimension per process per
         world, shared across worker threads), `encodeChunk`,
         `bedrockBlockState`. Built with warnings as errors against PHP
         8.2.30 ZTS from `tools/php-dev`; PHPT tests hand every sub-chunk of
         real vanilla chunks to chunkutils2 0.3.5's own
         `PalettedBlockArray::fromData`, and the `ext-pmmp` CI job runs them.
      5. [x] **The PocketMine-MP plugin** (`ext/plugin/`) — registers the
         generator in `onLoad()`, builds `Chunk`s from the engine's packed
         sub-chunks, translates palettes through PocketMine-MP's own
         upgrader + deserializer per worker, falls back (powder snow ->
         snow block) with a log rather than a crash or a silent swap, and
         `WorldFactory` freezes the pipeline into a new world's folder
         before creating it. Written against PMMP 5.44.4 source, read and
         independently re-verified (SPEC §11); **never run** — PocketMine-MP
         cannot be installed here. CI checks every file parses and runs the
         options test.
- [ ] **Run the plugin against a real PocketMine-MP server.** The one thing
      no check here can stand in for: registration, chunk assembly and block
      translation have never executed. Needs a PMMP install (its PHP build
      needs pmmpthread, leveldb, igbinary, morton and more, which
      `tools/php-dev` does not build) — the same shape of gap
      `ext-nukkit/`'s Java side has.
- [ ] **Generation speed.** Measured at **237 ms/chunk** for the overworld
      in an optimised build on this box (freeze 0.06 s, compiling a
      dimension 0.01 s, peak 2 MB). Fine for background generation, slow
      next to PocketMine-MP's own generators; M5's performance pass has a
      real number to work against now. Most of it is the density evaluation
      and the per-quart biome search, not the packing.
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
      `Pipeline::Impl` doc comment (now `world::CompiledDimension`'s, the
      shared core) and SPEC.md §11's M5-Nukkit entry for the
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
