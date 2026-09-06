# Aquifer — clean-room specification

- **Provenance:** see `PROVENANCE.md` (1.21.11 release; decompiled tree digest `1a324c8a…e873a`). Every claim below is grounded in that tree, in the shipped bytecode, or in measurement against a running server built from it — never in prior recollection.
- **Date:** 2026-09-06 (revised same day after empirical verification)
- **Brief:** `RESEARCH.md` §4 question order · **Scope:** aquifer substance decision only (`RESEARCH.md` §2)
- **Status:** hypothesis document. Only Q4.4–Q4.8 and Q8.7 have been verified against a running engine; every other claim still rests on reading and carries an *unexercised* `verify-by`. At least one `verify-by` in this document has already proved unworkable (see `AUDIT.md` §10), so treat them as proposals, not as a discharged obligation.

## Notation

- `P = (x, y, z)` — block position under evaluation; `D` — the density value supplied for `P` by the caller (sign convention: `D > 0` means solid).
- A **fluid status** is a pair `(L, T)`: an integer level and a fluid type. Its *reading* at height `h` is `status(h) = T if h < L else air`.
- `Global(P)` — the global picker of Q1. `Prelim(x, z)` — preliminary surface level. `Barrier`, `Floodedness`, `Spread`, `Lava` — the four aquifer noises.
- `Erosion`, `Depth` — two further router values, used only in Q5.9.
- `⌊·⌋` is floor; `floordiv(a,b) = ⌊a/b⌋` for all signs.
- Pack inputs in `snake_case`; thresholds `θ`; grid indices `i, j, k`.

---

## Q1 — Global picker

**Q1.1** Three fixed statuses exist, as pure functions of pack inputs: `A_lava = (−54, lava)`; `A_default = (sea_level, default_fluid)`; `A_void = (2·min_y_limit, air)`, where `min_y_limit = −2032` is the engine's absolute lower bound (Q1.4), giving `A_void = (−4064, air)`.
· kind `constant` · confidence `read-directly`
· verify-by: probe deep columns in a superflat-like preset with varied `sea_level`; the level at which the fallback reading flips gives `A_default`.

**Q1.2** `Global(x, y, z) = A_lava if y < min(−54, sea_level), else A_default`. It is independent of `x` and `z`.
· kind `formula` · confidence `read-directly`
· verify-by: ablation — set `sea_level` below `−54` and confirm the crossover tracks `sea_level` rather than staying pinned at `−54`.

**Q1.3** Lava therefore sits at a **fixed absolute depth**, not a depth relative to `sea_level` or `min_y`: the lava level is the constant `−54` whenever `sea_level > −54` (all shipped presets). `min_y` does not enter Q1 at all.
· kind `contract` · confidence `read-directly`
· verify-by: golden-region predicate — across two presets differing only in `min_y`, the global lava boundary is identical.

**Q1.4** `min_y_limit = −2032` and the sentinel `never = 16·min_y_limit = −32512`. Derivation: the position encoding reserves `64 − 2·(1 + ⌈log2 30000000⌉) = 12` bits for `y`, giving a span of `2^12 − 32 = 4064`, an upper bound of `4064/2 − 1 = 2031`, and `min_y_limit = 2031 − 4064 + 1`.
· kind `constant` · confidence `inferred-from-reading` (arithmetic composed from four separately-read definitions; the products are not literals in-tree)
· verify-by: assert the two values in the harness against a direct recomputation of the same arithmetic.

**Q1.5** `A_void` is reachable only under a build-time debug switch that disables fluid generation entirely; in shipped builds it is unobservable.
· kind `contract` · confidence `read-directly`
· verify-by: none needed — record as a deliberate non-implementation.

---

## Q2 — Applicability

**Q2.1** When `aquifers_enabled` is false, the substance rule degenerates to: `D > 0 → solid (caller's default block)`; otherwise `Global(P)(y)`. No noise, no lattice, and fluid updates are never scheduled.
· kind `contract` · confidence `read-directly`
· verify-by: ablation — toggle `aquifers_enabled` in a custom preset; below the surface every non-solid block must become `default_fluid` or air with a level boundary exactly at `sea_level`.

**Q2.2** With aquifers enabled, `D > 0 → solid` holds first and unconditionally. The aquifer can only ever *replace what would otherwise be non-solid*, or add solid via the barrier of Q6.
· kind `contract` · confidence `read-directly`
· verify-by: golden-region predicate — the set `{P : D(P) > 0}` is identical with aquifers on and off.

**Q2.3** For `D ≤ 0`, the local aquifer is consulted only when `y ≤ y_skip` (Q2.5). Above that, the result is `Global(P)(y)`.
· kind `contract` · confidence `read-directly`
· verify-by: instrument the harness to record whether the lattice was consulted; assert it is never consulted above `y_skip`.

**Q2.4** For `D ≤ 0` and `y ≤ y_skip`, if `Global(P)(y)` is lava the result is lava immediately — the lattice is skipped. So the global lava sea is never overridden by a local aquifer.
· kind `contract` · confidence `read-directly`
· verify-by: golden-region predicate — no block below `min(−54, sea_level)` holds water.

**Q2.5** `y_skip` is a per-chunk constant: `y_skip = 12·(floordiv(S_max + 8 + 12, 12) + 1) + 10`, where `S_max` is the maximum of `Prelim` over the lattice-covered rectangle of Q3.5, sampled on a stride of 4 blocks in both horizontal axes, inclusive of both endpoints.
· kind `formula` · confidence `read-directly`
· verify-by: ablation — force `Prelim` to a constant and confirm the observed cutoff height matches the closed form exactly.

**Q2.6** The constant `+8` appearing here and throughout Q5 is a single "surface adjustment" applied to every `Prelim` reading before use. It is never applied twice to the same reading.
· kind `constant` · confidence `read-directly`
· verify-by: covered by Q2.5 and Q5.4 probes.

---

## Q3 — Lattice

**Q3.1** Cell dimensions are `(16, 12, 16)` blocks in `(x, y, z)`.
· kind `constant` · confidence `read-directly`
· verify-by: autocorrelation of the aquifer field over a large flat region; peaks must fall on these periods.

**Q3.2** The cell index of a block is computed on **shifted** coordinates: `i = floordiv(x − 5, 16)`, `j = floordiv(y + 1, 12)`, `k = floordiv(z − 5, 16)`. The shift vector `(−5, +1, −5)` is part of the mapping, not a separate step.
· kind `formula` · confidence `read-directly`
· verify-by: locate one cell boundary empirically and confirm it sits at `x ≡ 5 (mod 16)` rather than `x ≡ 0`.

**Q3.3** Each cell `(i, j, k)` carries exactly one point at `Q(i,j,k) = (16i + u, 12j + v, 16k + w)` with integer offsets `u ∈ [0,10)`, `v ∈ [0,9)`, `w ∈ [0,10)`. The per-axis slack (`16−10 = 6`, `12−9 = 3`, `16−10 = 6`) guarantees a minimum separation between points of adjacent cells along each axis.
· kind `formula` · confidence `read-directly`
· verify-by: histogram recovered point offsets over many cells; support must be exactly `[0,10) × [0,9) × [0,10)` and uniform.

**Q3.4** RNG derivation for those offsets: a positional fork of the world random, salted by the string **`minecraft:aquifer`**, then positioned at the *cell indices* `(i, j, k)` — not at block coordinates. Positioning mixes the indices as `m = (i·3129871) xor (k·116129781) xor j`, then `((m·m·42317861) + 11m) >> 16`, combined with the stream seed. Draws are consumed in the order `u, v, w` with bounds `10, 9, 10`; the order is output-visible.
· kind `formula` · confidence `read-directly` (salt, ordering and mixing read directly; the fork/positional composition is `inferred-from-reading` in that the two shipped random sources differ in how the salted seed is combined)
· verify-by: reproduce offsets for a known seed against a deepslate-oracle probe at a handful of cells; a wrong draw order shows up immediately as swapped axes.

**Q3.5** The lattice extent allocated for a chunk spans `i ∈ [floordiv(x_min − 5, 16), floordiv(x_max − 5, 16) + 1]`, `k` likewise in `z`, and `j ∈ [floordiv(min_y + 1, 12) − 1, floordiv(min_y + height + 1, 12) + 1]`.
· kind `formula` · confidence `read-directly`
· verify-by: consumed only by Q2.5's rectangle; verify jointly with it.

**Q3.6** A declared constant "maximum reasonable distance to aquifer centre" of `11` exists but is **not read anywhere in the aquifer path** in this version. Recorded for completeness; it must not be implemented.
· kind `open-question` · confidence `read-directly` (absence of use verified by exhaustive search of the tree)
· verify-by: none — implement nothing; revisit if a future version wires it up.

---

## Q4 — Neighbourhood & selection

**Q4.1** The candidate set for a block is the **12** cells `(i + a, j + b, k + c)` with `a ∈ {0,1}`, `b ∈ {−1,0,1}`, `c ∈ {0,1}`. The neighbourhood is asymmetric in `x` and `z` (only forward) and symmetric in `y`; combined with the `(−5,+1,−5)` shift of Q3.2 this still brackets the block.
· kind `constant` · confidence `read-directly`
· verify-by: ablation — widen to a symmetric 27-cell set and confirm the output diverges, establishing the asymmetry is load-bearing.

**Q4.2** The metric is **squared Euclidean distance** in blocks from `P` to each candidate point. No axis weighting, no cheaper surrogate.
· kind `formula` · confidence `read-directly`
· verify-by: golden-region predicate over a region containing near-ties.

**Q4.3** The **four** smallest squared distances `d₁ ≤ d₂ ≤ d₃ ≤ d₄` and their points are retained. `d₄` influences only the fluid-update flag of Q8, never the substance.
· kind `contract` · confidence `read-directly`
· verify-by: ablation — retain only three; block output must be unchanged while scheduled-update positions change.

**Q4.4** Ties are resolved toward the **later** candidate in iteration order: a candidate whose squared distance **equals** the current value at a rank displaces it, cascading the previous occupant down. This holds independently at all four ranks. Iteration order is the `x` cell-offset outermost, the `y` offset in the middle, and the `z` offset innermost. Contrary to this document's earlier estimate, ties are **common, not measure-zero**: see Q4.5.
· kind `contract` · confidence `verified(bytecode+harness)` — the four rank comparisons each compile to the branch form produced by a non-strict `≥`, and the three loop bounds and their per-axis accumulations were read as opcodes, in the shipped class rather than in decompiled source; the rule was then exercised against a running engine (Q4.6)
· verify-by: **already verified — see Q4.6.** Re-run that differential harness against any future version before trusting this claim again.

**Q4.5** Tie frequency, measured over 53 897 588 selection evaluations during real chunk generation (4 096 forceloaded chunks, single fixed seed). Duplicate squared distances among the 12 candidates occur in **15.40%** of evaluations. Per rank pair:

| tied pair | ties | share of evaluations |
|---|---|---|
| ranks 1–2 | 581 785 | 1.079% |
| ranks 2–3 | 792 338 | 1.470% |
| ranks 3–4 | 941 739 | 1.747% |

Ties are therefore **common, not measure-zero**. The rates are seed-specific measurements, not invariants.
· kind `constant` · confidence `verified(differential-harness)` (measured, not derived)
· verify-by: re-measurement on a different seed should reproduce these rates to within sampling noise; the exact counts must not be asserted as invariants.

**Q4.6** Verification record for Q4.4. A differential harness ran inside a real dedicated server: for every selection, all 12 candidates were captured in iteration order, then ranked by (a) this specification's model and (b) a strict alternative in which the earlier candidate holds on a tie. Both were compared against the engine's own selection. Result: **0 mismatches for this specification's model across 53 897 588 evaluations**; **3 259 052 mismatches for the strict alternative**. The two models were separately shown to be distinguishable (they disagree on 47% of randomized synthetic inputs), so the null result is not vacuous. Scope limit: because candidates are captured in the engine's own traversal order, this experiment tests the tie-break **rule** only. The **iteration order** in Q4.4 is established from bytecode, where the outermost loop runs `[0,1]` and accumulates into the `x` cell index, the middle runs `[−1,1]` into `y`, and the innermost runs `[0,1]` into `z`. Fidelity of the second harness (the one behind Q4.7, which restructured the post-selection logic so it could be invoked twice) is evidenced by both builds reporting **identical** totals to the digit: 53 897 588 evaluations and 581 785 rank-1 ties. Byte-comparing saved region files is **not** a valid fidelity check — they embed timestamps and independently compressed payloads, so two runs of identical logic never match byte-for-byte.

**Anchor.** The harness build was subsequently shown to reproduce the untouched shipped engine's generation output. Scope, stated exactly: on a feature- and structure-stripped pack, under a frozen regime (world frozen before generation and never unfrozen, random ticks off, weather pinned), across 4 096 chunks and four channels — block arrays, `block_ticks`, `fluid_ticks`, and post-processing, all as position sets — the patched build and the untouched official jar differ in **nothing**, except one whole-list post-processing consumption race that the same comparison reproduces between two runs of the *official jar against itself*. Aquifer-domain equivalence was additionally shown on the **unstripped** pack: zero differences anywhere in the deep terrain skeleton. Two caveats kept in view: the frozen regime's own noise floor is small but not zero (two runs of the untouched jar differ in 2 cells of 402 653 184, both surface water flow-state), and the anchor therefore certifies equivalence at that resolution, not exact bit-identity of the engine. Full record in `PROVENANCE.md` §3d/§3e.
· kind `contract` · confidence `read-directly`
· verify-by: n/a — this *is* the verification.

**Q4.7** Output visibility of the tie-break, measured by running the **real** post-selection logic twice per tied evaluation — once with the engine's ordering and once with the tied pair swapped — and comparing both the emitted substance and the fluid-update flag:

| tied pair | substance changed | flag changed | share of those ties |
|---|---|---|---|
| ranks 1–2 | 47 | 1 | 0.008% |
| ranks 2–3 | 0 | 13 962 | 1.762% |
| ranks 3–4 | 1 649 | 3 494 | 0.374% |

Three consequences, none of them intuitive: (a) a rank-1 tie almost never matters — 47 blocks in 4 096 chunks; (b) **rank 3–4 ties change ~35× more blocks than rank 1–2 ties**, despite rank 4 never entering the substance formulas of Q6 directly; (c) rank 2–3 ties change **no** blocks at all — 0 of 792 338 — only the fluid-update flag. This is an *observation, not an explained mechanism*: the barrier predicate of Q6.6 is not obviously invariant under swapping the second and third points (the two affected terms carry different weights, `s₁₂` versus `s₁₂·s₁₃`), so an exact zero is stronger than the algebra alone predicts. The cause is unresolved — see open question 8. Do not rely on it as an invariant.
· kind `constant` · confidence `verified(differential-harness)` (measured, not derived)
· verify-by: re-measure on another seed; the qualitative ordering (rank 3–4 ≫ rank 1–2 for blocks; rank 2–3 flag-only) should be stable even though the counts are not.

**Q4.8** **Correction.** An earlier revision of this document asserted that 56 092 rank-1 ties change the emitted block. That was wrong by roughly three orders of magnitude. The measured quantity was "the two tied points carry different fluid statuses", from which changed output was *inferred*; the inference is invalid because the barrier predicate of Q6.6 usually collapses the difference. Direct measurement gives 47. Recorded here rather than silently edited, because a consumer may have already read the wrong figure.
· kind `contract` · confidence `read-directly`
· verify-by: n/a — this records a correction.

---

## Q5 — Per-point fluid status

For a lattice point `Q = (qx, qy, qz)`, its status `A(Q) = (L, T)` is a pure function of seed, position and pack inputs, as follows.

**Q5.1** Thirteen horizontal probe offsets, in **chunk** units, are used in this fixed order: `(0,0), (−2,−1), (−1,−1), (0,−1), (1,−1), (−3,0), (−2,0), (−1,0), (1,0), (−2,1), (−1,1), (0,1), (1,1)`. Offset `o` probes the column `(qx + 16·o_x, qz + 16·o_z)`. The set is asymmetric: `o_x` ranges over `[−3, 1]` at `o_z = 0` but only `[−2, 1]` at `o_z = ±1`, and `o_x = 2` never occurs. Order matters (Q5.3).
· kind `constant` · confidence `read-directly`
· verify-by: assert the literal table in the harness; any permutation changes output via Q5.3.

**Q5.2** Write `a(o) = Prelim(qx + 16·o_x, qz + 16·o_z) + 8` for the adjusted surface at probe `o`, and `a₀ = a((0,0))`. `Prelim` is itself evaluated on **quarter-resolution** columns: the sample position is snapped by `4·⌊·/4⌋` in both `x` and `z` before evaluation, and it is evaluated at `y = 0`.
· kind `formula` · confidence `read-directly`
· verify-by: confirm `Prelim` is piecewise-constant on 4×4 column blocks.

**Q5.3** The status short-circuits, in this priority: (a) if `qy − 12 > a₀` then `A(Q) = Global(Q)`; (b) otherwise, let `o*` be the **first** offset in Q5.1's order satisfying both `qy + 12 > a(o)` and `Global(qx+16·o_x, a(o), qz+16·o_z)(a(o)) ≠ air`; if `o*` exists, `A(Q)` is that global status evaluated at `(·, a(o*), ·)`. Case (a) says: a point far enough below its own surface is governed globally. Case (b) says: a point near a column whose adjusted surface is *submerged* inherits that column's fluid.
· kind `contract` · confidence `read-directly`
· verify-by: golden-region predicate near coastlines, where (b) fires often.

**Q5.4** If neither short-circuit fires, define `S_min` as the minimum of `Prelim` (unadjusted) over **all thirteen** probes. `S_min` is well-defined precisely because the non-short-circuit path visits every probe.
· kind `formula` · confidence `read-directly`
· verify-by: covered by Q5.6.

**Q5.5** A boolean `submerged` records whether the **centre** probe's global reading at `a₀` is non-air. It is set independently of whether the centre probe also satisfied the `qy + 12 > a₀` condition of Q5.3(b).
· kind `contract` · confidence `read-directly`
· verify-by: ablation — tie `submerged` to the Q5.3(b) condition instead and confirm divergence in a band of `qy` just below the surface.

**Q5.6** Floodedness. Let `d = S_min + 8 − qy` and `φ = clamp((64 − d)/64, 0, 1) if submerged, else 0` (equivalently: `φ` interpolates `1 → 0` as `d` runs `0 → 64`, clamped). Let `n = clamp(Floodedness(Q), −1, 1)` and `θ_hi = 0.8 + φ·(−0.3 − 0.8) = 0.8 − 1.1φ`, `θ_lo = 0.4 + φ·(−0.8 − 0.4) = 0.4 − 1.2φ`. Then `L` is: `Global(Q).level` if `n > θ_hi`; else the randomized level of Q5.7 if `n > θ_lo`; else the sentinel `never = −32512`. Note `θ_lo < θ_hi` for all `φ ∈ [0,1]`, so the three cases are ordered.
· kind `formula` · confidence `read-directly`
· verify-by: ablation on `φ` — clamp `submerged` to false and confirm the `64`-block ramp disappears.

**Q5.7** Randomized level. With `xi = floordiv(qx,16)`, `yi = floordiv(qy,40)`, `zi = floordiv(qz,16)`: `L_rand = min(S_min, 40·yi + 20 + 3·⌊10·Spread(xi, yi, zi)/3⌋)`. The `Spread` noise is sampled **at the grid indices themselves as coordinates**, not at block coordinates — a 1/16, 1/40, 1/16 contraction of the sampling lattice. The `3·⌊·/3⌋` is a quantization to multiples of 3 with floor rounding (so negative values round away from zero).
· kind `formula` · confidence `read-directly`
· verify-by: histogram `L_rand − 40·yi − 20` over many points; support must be multiples of 3, and the `min` with `S_min` must be the only source of non-multiples.

**Q5.8** Fluid type. `T = Global(Q).type`, overridden to lava when **all** of: `L ≤ −10`, `L ≠ never`, and `Global(Q).type` is not already lava — and then `|Lava(floordiv(qx,64), floordiv(qy,40), floordiv(qz,64))| > 0.3`. The `Lava` noise is likewise sampled at contracted indices, on a different horizontal divisor (`64`) from `Spread`'s (`16`).
· kind `formula` · confidence `read-directly`
· verify-by: golden-region predicate — the lava/water split of local aquifers at `L ≤ −10`; the `0.3` threshold is recoverable by bisection.

**Q5.9** Deep-dark override. If `Erosion(Q) < −0.225` **and** `Depth(Q) > 0.9`, both floodedness comparands are forced to `−1`, which (since `θ_lo ≥ −0.8`) always selects the sentinel: such points are **dry**. This is the only place the aquifer reads `Erosion` or `Depth`, and it reads them as router values at `Q` — there is no biome lookup (see Q7.3).
· kind `formula` · confidence `read-directly`
· verify-by: ablation — disable the override and confirm deep-dark regions flood.

**Q5.10** Noise scaling. All four aquifer noises use horizontal scale `1.0`. Vertical scales are: `Barrier` `0.5`, `Floodedness` `0.67`, `Spread` `0.7142857142857143`, `Lava` `1.0`. A noise evaluated at context `(cx, cy, cz)` samples at `(cx·1.0, cy·y_scale, cz·1.0)`. These compose with Q5.7/Q5.8's index contraction rather than replacing it.
· kind `constant` · confidence `read-directly`
· verify-by: assert against the pack-visible values; `0.7142857142857143` is the `double` nearest `5/7` and must be carried as the literal, not recomputed.

**Q5.11** Noise parameters are pack data: `aquifer_barrier` first-octave `−3`, `aquifer_fluid_level_floodedness` `−7`, `aquifer_fluid_level_spread` `−5`, `aquifer_lava` `−1`; each with a single amplitude `1.0`.
· kind `constant` · confidence `read-directly`
· verify-by: read from the data pack at runtime rather than hardcoding.

---

## Q6 — Blending & barrier

**Q6.1** Similarity. `sim(p, q) = 1 − (q − p)/25`, applied to squared distances. It is `1` at equality and decreases as the pair separates. Define `s₁₂ = sim(d₁,d₂)`, `s₁₃ = sim(d₁,d₃)`, `s₂₃ = sim(d₂,d₃)`, and let `A₁, A₂, A₃` be the statuses of the corresponding points.
· kind `formula` · confidence `read-directly`
· verify-by: assert the `25` by bisection against a golden region containing barrier walls.

**Q6.2** If `s₁₂ ≤ 0` the nearest point wins outright: result is `A₁(y)`, with no barrier evaluation and no noise consumed.
· kind `contract` · confidence `read-directly`
· verify-by: golden-region predicate — interiors of large aquifers are barrier-free.

**Q6.3** Water-over-lava exception: if `A₁(y)` is water and `Global(x, y−1, z)(y−1)` is lava, the result is `A₁(y)` (water) with no barrier. Note the test is on the **global** picker one block below, not on `A₁`.
· kind `contract` · confidence `read-directly`
· verify-by: probe the band immediately above the global lava level.

**Q6.4** Pressure. For two statuses `A = (L_A, T_A)`, `B = (L_B, T_B)` at height `y`: if one reads lava and the other water, `Π = 2.0`. Otherwise let `Δ = |L_A − L_B|`; if `Δ = 0` then `Π = 0`. Otherwise, with `m = (L_A + L_B)/2`, `h = y + 0.5 − m`, `r = Δ/2`, `t = r − |h|`:

`u = t/1.5 if h > 0 and t > 0`; `u = t/2.5 if h > 0 and t ≤ 0`; `u = (3 + t)/3 if h ≤ 0 and 3 + t > 0`; `u = (3 + t)/10 if h ≤ 0 and 3 + t ≤ 0`.

Then `b = Barrier(P)` if `|u| ≤ 2`, else `b = 0`; and `Π = 2·(b + u)`. The asymmetry between the `h > 0` and `h ≤ 0` branches (offset `0` vs `3`, and divisor pairs `1.5/2.5` vs `3/10`) makes barriers thicker below the fluid midpoint than above it.
· kind `formula` · confidence `read-directly`
· verify-by: ablation on each of the four divisors independently; each should perturb barrier thickness in one half-space only.

**Q6.5** The `Barrier` noise is evaluated **at the block position** `P`, at most once per block, and the same value is reused across all pressure evaluations for that block. Since it is a pure function of `P`, the reuse is unobservable and carries no ordering requirement.
· kind `contract` · confidence `read-directly`
· verify-by: none needed (purity rule); implement as a plain function of `P`.

**Q6.6** Barrier predicate. The block is **solid** iff any of: `D + s₁₂·Π(A₁,A₂) > 0`; `s₁₃ > 0` and `D + s₁₂·s₁₃·Π(A₁,A₃) > 0`; `s₂₃ > 0` and `D + s₁₂·s₂₃·Π(A₂,A₃) > 0`. Otherwise the result is `A₁(y)` — the nearest point's reading, which may be air. The three products differ: the first is single-weighted, the others doubly.
· kind `formula` · confidence `read-directly`
· verify-by: golden-region predicate over a region with three-way aquifer junctions; also assert that dropping the `s₁₃ > 0` / `s₂₃ > 0` guards changes output (they are not redundant, since `Π` may be negative).

**Q6.7** The barrier block is **the caller's default block for the preset** (the same block a positive density yields), not a distinct material. The aquifer signals "solid" and the caller substitutes; it never names a block itself.
· kind `contract` · confidence `read-directly`
· verify-by: ablation — change `default_block` in a preset; barrier walls must change material with it.

**Q6.8** Fluid placement is suppressed wholesale under a build-time debug switch, yielding air in place of every aquifer fluid. Not implementable behaviour.
· kind `contract` · confidence `read-directly`
· verify-by: none — deliberate omission.

---

## Q7 — Completeness

**Q7.1** The complete external input set is: the four aquifer noises (`Barrier`, `Floodedness`, `Spread`, `Lava`); the router values `Erosion`, `Depth`, and `Prelim`; the caller-supplied density `D`; the pack inputs `sea_level`, `default_fluid`, `default_block`, `aquifers_enabled`; the dimension bounds `min_y` and `height`; and the world seed via the salted positional RNG of Q3.4. Nothing else.
· kind `contract` · confidence `read-directly` (enumerated by exhaustive read of the aquifer path and its transitive reads)
· verify-by: harness-level assertion that no other router field is touched.

**Q7.2** `min_y` and `height` affect **only** the lattice extent of Q3.5 and hence `y_skip`; they never enter a fluid-level or barrier formula.
· kind `contract` · confidence `read-directly`
· verify-by: ablation — vary `height` alone; only the chunk-level cutoff moves.

**Q7.3** **No biome data and no structure data are read.** This surprised me and is worth stating plainly: the deep-dark test of Q5.9 is named for a biome and lives, in the source, alongside biome-parameter code — but it is a pure threshold pair over two router values, evaluated at the lattice point. There is no biome lookup, no climate sampling, and no structure query anywhere in the aquifer path.
· kind `contract` · confidence `read-directly`
· verify-by: ablation — generate with a single-biome source and confirm the aquifer field is unchanged from a multi-biome source at the same seed.

**Q7.4** Two build-time debug switches (disable-aquifers, disable-fluid-generation) gate behaviour described in Q2.1 and Q6.8. They are compile-time constants, false in shipped builds.
· kind `contract` · confidence `read-directly`
· verify-by: none — deliberate omission.

**Q7.5** The `Spread` and `Lava` noises are the only inputs sampled at contracted coordinates (Q5.7, Q5.8); `Barrier` and `Floodedness` are sampled at true block or point coordinates. Getting this backwards is the most likely implementation error in the whole system.
· kind `contract` · confidence `read-directly`
· verify-by: cross-check the four sampling positions against a deepslate-oracle probe before adopting any of Q5.

---

## Q8 — Interactions

**Q8.1** Alongside the substance, the aquifer emits a boolean **schedule-fluid- update** flag for the block. Its observable consequence: when the flag is set and the block that was placed has a non-empty fluid state, that position is marked for post-processing in the stored chunk, which causes a fluid tick to be scheduled when the chunk is loaded.
· kind `contract` · confidence `read-directly`
· verify-by: inspect the stored chunk's post-processing list for a golden chunk.

**Q8.2** The flag is `false` whenever the aquifer returns early (Q2.1–Q2.4), and in the `s₁₂ ≤ 0` case of Q6.2 it is `false` unless `s₁₂ ≥ θ_flow`, in which case it is `A₁ ≠ A₂` (status inequality, comparing both level and type).
· kind `contract` · confidence `read-directly`
· verify-by: as Q8.1.

**Q8.3** `θ_flow = sim(10², 12²) = 1 − 44/25 = −0.76` exactly.
· kind `constant` · confidence `read-directly`
· verify-by: assert the literal; it is derived in-tree from `10` and `12`, so carry `−0.76` and the derivation together.

**Q8.4** In the full path (Q6.6 falling through to fluid), the flag is `true` if any of `A₁ ≠ A₂`, or (`s₂₃ ≥ θ_flow` and `A₂ ≠ A₃`), or (`s₁₃ ≥ θ_flow` and `A₁ ≠ A₃`). If none hold, it is instead (`s₁₃ ≥ θ_flow` and `sim(d₁,d₄) ≥ θ_flow` and `A₁ ≠ A₄`) — the sole use of the fourth-nearest point.
· kind `formula` · confidence `read-directly`
· verify-by: as Q8.1; the `A₄` term is exercised only in near-degenerate four-way junctions, so target those deliberately.

**Q8.5** The flag is also consumed by the carver system, which reads it when carving intersects aquifer fluid. That consumer is **out of Tier-A scope**; the contract it depends on is exactly Q8.1's.
· kind `contract` · confidence `read-directly`
· verify-by: defer to the carver run of this brief.

**Q8.6** The aquifer writes nothing else and schedules nothing else. It does not mutate the router, the chunk, or any shared state visible to another system.
· kind `contract` · confidence `read-directly`
· verify-by: harness-level assertion.

**Q8.7** Measured sensitivity of the flag to candidate ordering (see Q4.7): swapping two points tied at ranks 2–3 changes the flag in 1.76% of such ties while changing no blocks; swapping ranks 3–4 changes it in 0.37%. Across all tied ranks the flag changes ~17 500 times against ~1 700 substance changes, so the flag is roughly an order of magnitude more order-sensitive than the substance. Note this does not contradict Q4.3: the *value* `d₄` is read only by the flag logic, but tie-breaking decides which point occupies the third slot versus the fourth, and the third slot does enter Q6.6.
· kind `constant` · confidence `verified(differential-harness)` (measured)
· verify-by: covered by Q4.7's re-measurement.

---

## Coverage budget

Rare-event rates dictate how large a golden region has to be before a code path is exercised at all. All rates below are **measured on one seed over 4 096 chunks** and are **order-of-magnitude guidance, not invariants** — the same caveat as Q4.5.

Let `r` be the events per chunk. For an expected count of at least `N` exercises, generate

    chunks ≥ N / r

(an expectation, not a guarantee; for a high-probability floor rather than a mean, treat occurrences as Poisson with mean `N` and size up accordingly).

| path | events / 4 096 chunks | `r` (per chunk) | chunks for `N` |
|---|---|---|---|
| rank 1–2 tie changes the substance | 47 | 0.0115 | 87 · `N` |
| rank 3–4 tie changes the substance | 1 649 | 0.403 | 2.5 · `N` |
| rank 2–3 tie changes the flag (never a block) | 13 962 | 3.409 | 0.30 · `N` |
| rank 3–4 tie changes the flag | 3 494 | 0.853 | 1.2 · `N` |
| rank 1–2 tie changes the flag | 1 | 0.00024 | 4 096 · `N` |
| Q8.4's `A₄` term (four-way near-degenerate junctions) | **unmeasured** | — | — |

Two consequences worth stating outright:

- A golden region sized for the *common* case badly under-covers the rare one. Ten expected exercises of the rank-1–2 substance path needs ~870 chunks; ten of the rank-2–3 flag path needs ~3. A region that comfortably exercises one tells you nothing about the other.
- The rank-1–2 **flag** path is effectively untestable by sampling — one event in 4 096 chunks. Reaching it deliberately requires construction, not volume.

**`A₄` term — unmeasured, by approval.** Q8.4's fourth-nearest-point term was never instrumented; no defensible bound is available from the existing data, and inventing one would repeat the error corrected in Q4.8. It is on the shopping list for the next instrumented run.
· kind `open-question` · confidence `inferred-from-reading` (that the term is rare follows from its guard conditions; the rate itself is unknown)
· verify-by: add a counter on the `A₄` branch during the next instrumented run and report events per chunk; until then treat any golden region as providing **zero** coverage of this path.

---

## Open questions & deliberate omissions

1. **Q4.4 tie-breaking** — **RESOLVED.** Verified empirically against a running server (Q4.6) and at bytecode level; no longer an open question. The earlier claim that ties are measure-zero was **wrong** and is corrected by Q4.5.
2. **Q3.6** — a declared distance constant that is dead in this version. Deliberately not implemented.
3. **Q1.5, Q6.8, Q7.4** — debug-switch behaviour, deliberately not implemented.
4. **Q1.4** — `min_y_limit` and `never` are arithmetic compositions, not literals; recompute rather than trusting the transcribed values.
5. **Q5.1** — the thirteen-offset table is asymmetric in a way I could find no principle behind. Treat it as opaque data; do not "fix" the asymmetry.
6. **Not investigated:** how `Prelim`, `Erosion` and `Depth` are themselves defined. They are treated here as opaque inputs. If the Implementer does not already have them verified, they need their own run of this brief.
7. **Legacy random source.** Presets selecting the legacy random source derive Q3.4's seed by a different combination step. Only the modern source was traced in full; the legacy path is unverified.
8. **`A₄` coverage rate.** Q8.4's fourth-nearest-point term was never instrumented; the coverage budget carries it as `unmeasured` and golden regions provide zero measured coverage of it.
9. **Why rank 2–3 ties never change a block.** Measured as exactly 0 in 792 338 ties (Q4.7), but the barrier predicate is not visibly symmetric under that swap. Either a structural invariant exists that I have not identified, or the measurement is masking one. Unresolved; do not encode the zero as a rule.
