# Progress

A tracking dashboard for Stratum's milestones. `SPEC.md` is the source of
truth for every claim here (its own §10 for milestone definitions, §11 for
the measured narrative behind each) — this file exists to be scanned in a
few seconds, not to duplicate SPEC.md's prose. Update it whenever a
milestone or a named blocker moves.

Last swept: 2026-09-11 (ore-vein probing session, uncommitted at last sweep).

## At a glance

| Milestone | Status |
|---|---|
| M0 — repo scaffolding | Closed |
| M1 — core primitives + conformance harness | Closed¹ |
| M2 — 2D pipeline | Closed (its goal folded into M3) |
| M3 — 3D density | Closed for the overworld²; ore veins tracked separately, below |
| M4 — biomes + surface | Open — blocked on legacy RNG and ore veins |
| MA — Aquifers (parallel track, does not gate M4-M6) | Nearly closed — 2 narrow pieces left |
| M5 — integration (Bedrock mapping, PMMP binding, perf) | Not started |
| M6 (v2) — staged features/structures, scripting escape hatch | Out of scope for v1 |

¹ StrictMath: only `log` is vendored (fdlibm); `exp`/`pow`/`sin`/`cos`/`atan2` deferred until a node needs them.
² `weird_scaled_sampler`'s End-dimension remainder has not been re-surveyed since M3 closed for the overworld.

## MA — Aquifers

Landed and measured: the cell lattice, the centre jitter, the fluid level
rule (ladder, ocean branch, depth-path gate), source selection (four ranked
candidates), the three-source barrier predicate, and the fluid type rule.
`ChunkFiller` now calls all of it directly — the old refusal is gone.

Open:

- [ ] **Q6.3's water-over-lava exception.** Untested by every angle, not
      implemented. A water block directly above the global lava floor can
      still get a spurious barrier.
- [ ] **The mixed-fluid-type Π branch** (`Π = 2.0` when two competing
      sources are different fluid types). Unmeasured — every barrier probe
      so far holds `lava` constant specifically to keep this question
      separate from the barrier shape itself.
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
      salt) is a genuine research wall, not a queued step: ~30 candidates
      tried — every noise name the real density functions reference, all
      three router field names, the aquifer's own salt, and natural-
      language guesses — all refuted under per-seed and per-type
      validation (see SPEC.md's M3 section for the full list and the two
      structural checks that ruled out a non-RNG explanation first).
      cubiomes was checked and does not implement this feature at all.
      Next leads: Cuberite has not yet been consulted (it predates 1.18's
      terrain rewrite, so it likely doesn't help either, but it's
      unverified); reconsidering whether the mechanism differs from
      `positionalSourceFor` altogether remains open too.
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
- [ ] Surface-rule mcdoc schema generation debt — 5 of 15 condition/rule
      types are hand-written rather than generated from schema.

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
