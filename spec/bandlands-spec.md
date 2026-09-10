# Bandlands surface rule — clean-room specification (run 03)

- **Provenance:** see `PROVENANCE.md` run-03 section (same 1.21.11 artifacts as runs 01–02; digests re-verified fresh this run, before any reading). Every claim below is grounded in the decompiled tree, or in differential measurement against the unmodified remapped jar via an external driver — never in prior recollection.
- **Date:** 2026-09-10 · **Brief:** `RESEARCH-RUN-03.md` §4 question order
- **Scope:** the complete mechanism behind the `minecraft:bandlands` surface rule — the per-dimension data it builds, and the per-column read path that turns a position into a placed band. Every other surface rule, condition type, and the rest of the surface-rule system are out of scope.
- **Status:** hypothesis document, verified-by-default per this run's bar (`RESEARCH-RUN-03.md` §5). `verify-by` reads "already executed" with trial counts where that happened.
- **Context note:** this run was conducted deliberately withheld from the Implementer's behavioral model and ground-truth captures, in both directions of implication (`RESEARCH-RUN-03.md` §3). Nothing about that material is referenced anywhere below; every verification seed and corner here was chosen fresh from reading the mechanism, not from any external hint.

## Notation

- `(x, y, z)` — the block position under evaluation, as three independent integer coordinates.
- `clay_bands` — the constructed per-dimension array (Q1).
- `round(v)` — the mechanism's own rounding rule, defined precisely in Q4.4 (not assumed to be symmetric).
- `mod(a, n)` — ordinary mathematical (always-nonnegative) modulo, used only in prose to describe intent; the mechanism's *actual* reduction is given as an exact expression in Q4.5, not assumed equal to `mod`.
- Registry keys and pack-data names, `snake_case`: `minecraft:bandlands`, `clay_bands_offset` — runtime data, permitted.

---

## Q1 — Structure

**Q1.1** The mechanism builds one per-dimension array, `clay_bands`, of exactly **192** block-state entries. `192` is a literal in source, not derived from any pack parameter, any noise, or any other configurable value.
· kind `constant` · confidence `verified(differential-harness)`
· verify-by: already executed — harness section B, array length checked across 4 distinct seeds, always exactly 192.

**Q1.2** The array is built **once per dimension**, at the point a dimension's shared per-dimension random/noise state is constructed (alongside several unrelated per-dimension noise samplers built at the same moment) — not per-chunk, not per-column, not per-request. That shared state is itself constructed once per loaded dimension, from that dimension's generator settings and the world seed, and held for the dimension's lifetime.
· kind `contract` · confidence `read-directly`
· verify-by: none stronger available — this is a wiring/lifetime fact about *when* construction happens, not a value a differential test can probe; confirmed by reading the one construction call site and its own construction site in turn.

**Q1.3** No noise sampler and no additional per-position state is built alongside `clay_bands` for this mechanism specifically. A second noise sampler (used only at read time, Q4) is built independently in the same neighborhood of code, but is a separate construction with its own registered pack parameters (Q6) — not part of `clay_bands` itself and not itself randomized by this mechanism's own draws.
· kind `contract` · confidence `read-directly`
· verify-by: covered by Q2/Q3's harness sections, which fully account for every draw made while building `clay_bands`.

---

## Q2 — Construction

**Q2.1** All 192 entries start at one fixed default color, then are selectively overwritten by four passes, in this fixed order: (a) a sparse single-cell scatter, (b) three separate multi-cell "band" passes at different color/width parameters, (c) a final sparse scatter with an extra one-sided coloring rule. All four passes read from one shared, continuously-advancing random source (Q3); nothing is parallel or reordered.
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed — harness section A, full detail below.

**Q2.2** Pass (a), the sparse scatter: define `index₀ = 0`; while `indexₖ < 192`, draw a fresh uniform integer `dₖ` in `[0, 4]` and set `indexₖ₊₁ = indexₖ + dₖ + 2`; whenever `indexₖ₊₁ < 192`, overwrite that cell with the scatter color. **The per-step advance is `draw + 2`, not `draw + 1`** — missing this produces a scatter with every gap systematically one cell too short. A step can draw without writing, when its start was still below `192` but its result lands at or past it. The total number of steps (and hence total draws this pass consumes) is therefore not fixed: it ranges from `192/7` (every draw at its maximum) to `192/2` (every draw at its minimum).
· kind `formula` · confidence `verified(differential-harness)`
· verify-by: already executed — harness section A. This +2 subtlety was caught and confirmed in the same pass as the rest of Q2, by full-array equality against the real method across 10 seeds; an independent reimplementation using only the naive `+1` advance was tried first and diverges from the real array immediately (not included in the published harness, since the faithful `+2` version was written first this run — the aquifer and blended-noise precedent for catching exactly this class of loop-arithmetic error is why this subtlety was checked for specifically rather than assumed).

**Q2.3** Passes of type (b) — the "band" passes — share one procedure, parameterized by a base width and a color, called three times with parameters `(1, yellow)`, `(2, brown)`, `(1, red)`, in that order. Each call: draw a run count uniformly in `[6, 15]` (inclusive both ends); for each of that many runs, independently draw a width uniform in `[base, base+2]` and a start index uniform in `[0, 191]`, then overwrite cells `start, start+1, …` with the color, up to `width` cells, **truncating silently at the array's end rather than wrapping** — a run whose start is near the end simply colors fewer cells than its drawn width. Runs are **not** prevented from overlapping each other or overwriting earlier passes' cells; a later run's color always wins where it overlaps an earlier one.
· kind `formula` · confidence `verified(differential-harness)`
· verify-by: already executed — harness section A (same full-array check covers all three calls of this procedure).

**Q2.4** Pass (c), the final scatter: draw a target count uniform in `[9, 15]` (inclusive both ends). Starting from index `0`, repeatedly: place the scatter color at the current index; independently, if the index minus one is still a valid positive index, flip a fair coin and on success color that one cell a second, distinct color; independently, if the index plus one is still in bounds, flip a second fair coin and on success color that cell the same second color; count this placement toward the target; advance the index by a fresh uniform draw in `[4, 19]` (16 possible values, offset by 4). Stop once the target count of placements is reached, or the index runs past the array's end — whichever comes first. Every step here draws exactly once and writes exactly once — there is no draw that goes unwritten the way pass (a)'s final over-the-end step can.
· kind `formula` · confidence `verified(differential-harness)`
· verify-by: already executed — harness section A (same full-array check).

**Q2.5** Consumption order across the whole construction, as a single flat sequence (this is the *complete* consumption contract; every draw in Q2.2–Q2.4 happens in exactly this order, on one shared source): pass (a)'s draws, in the order its steps occur; then pass (b)'s three calls, each fully consuming its own run-count draw followed by all its runs' width/start draws, in call order `(1,yellow)`, `(2,brown)`, `(1,red)`; then pass (c)'s target-count draw, followed by its own placement loop's draws in placement order (index color is free — no draw — then the two independent coin flips only where their position guard holds, then the advance draw).
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed — this ordering is exactly what harness section A's full-array match confirms; any wrong ordering would desynchronize the shared source and produce a diverging array from the first affected pass onward.

---

## Q3 — Randomness

**Q3.1** The mechanism uses **one continuously-advancing random source**, obtained once before construction begins and threaded through every pass of Q2 in sequence — not a per-index or positional derivation. Nothing in Q2's construction re-derives or re-seeds a source partway through.
· kind `contract` · confidence `read-directly`
· verify-by: covered by Q2.5's harness — a positional/per-index scheme would not reproduce a shared, order-dependent array the way the harness's sequential replay does.

**Q3.2** That source is obtained by hash-salting the string **`minecraft:clay_bands`** from the dimension's shared positional random factory (the same per-dimension factory that other per-dimension mechanisms derive their own sources from, via their own distinct salts) — a positional fork, then a hash-of-string derivation, matching the derivation *style* already established for other per-dimension mechanisms in this project's prior runs. No further draws from this source occur outside Q2's construction; it is not reused, saved, or read from again afterward.
· kind `contract` · confidence `read-directly`
· verify-by: none stronger available — a wiring fact at the one construction call site, not itself a numeric claim a sweep can strengthen.

---

## Q4 — Read path

**Q4.1** The read path is a single method taking a position and returning one band entry. It performs no randomness of its own — every random draw happened once, at construction (Q2/Q3); reading is a pure function of `(the constructed array, one noise sampler's value, the queried position)`.
· kind `contract` · confidence `read-directly`
· verify-by: covered by Q4.5's harness.

**Q4.2** One registered noise, salted `clay_bands_offset`, is sampled at every read — at coordinates `(x, 0, z)`. **The sample never varies with the query's own `y`** — only `x` and `z` reach the noise call; `y` enters the mechanism exclusively through the index arithmetic of Q4.5, never through the noise itself.
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed — harness section F, 14 000 trials confirming the sampled value at a given `(x,z)` is independent of which `y` is being queried, cross-checked against the call-site reading that hardcodes the noise's own `y`-argument to `0`.

**Q4.3** The noise's registered pack parameters: first octave `-8`, a single amplitude of `1.0`. (Runtime pack data, permitted.)
· kind `constant` · confidence `read-directly`
· verify-by: read from the data pack directly, mirrored in the constants file.

**Q4.4** The sampled noise value is multiplied by `4.0`, then rounded to the nearest integer using **Java's own tie-breaking rule — ties round toward positive infinity, not away from zero and not toward even.** `-2.5` rounds to `-2`; `2.5` rounds to `3`. This is not the "round half away from zero" rule a reader might assume by default.
· kind `formula` · confidence `verified(differential-harness)`
· verify-by: already executed — harness section C, eleven hand-constructed exact-tie doubles spanning both signs and several magnitudes, all confirming the floor-based (round-toward-positive-infinity) rule.

**Q4.5** The final index into the 192-entry array is computed as **`(y + offset + 192) mod 192`, using a single Java `%` after a single `+192`** — not a form that is provably nonnegative for the full range of reachable inputs (see Q5.1). This is the mechanism's own literal expression, not a paraphrase of "always-positive modulo"; the two are equal only where the pre-reduction sum is already nonnegative.
· kind `formula` · confidence `verified(differential-harness)`
· verify-by: already executed — harness section D, a wide sweep (9045 trials across 3 seeds, hand-picked corners at both coordinate signs and both world-height extremes plus 3000 random points per seed) comparing an independent reimplementation using this *exact* reduction shape against the real method; 0 mismatches, including exact agreement on every trial where the real method itself throws (below).

---

## Q5 — Conditional behavior

**Q5.1** The read path contains no explicit conditional that changes its *formula* — Q4's five steps run unconditionally for every input. But the index expression of Q4.5 is **not safe for the full range of positions the mechanism can be asked about**: for sufficiently negative `y` combined with a sufficiently negative noise-derived offset, `y + offset + 192` is itself negative, and Java's `%` on a negative dividend returns a negative result, which is then used directly as an array index. **This is not a hypothetical edge case — it was reproduced directly against the real, unmodified method.** Over a 200 000-sample sweep of the offset noise, observed offsets ranged `[-6, 5]`; at the worst observed offset, the index goes negative for any `y < -186`, comfortably inside the reachable world-height range (down to at least `-2032`). A direct call to the real method at `(x=0, y=-2032, z=0)` throws `ArrayIndexOutOfBoundsException` for every seed tried. **This is a genuine reachability finding, not a DO-NOT-IMPLEMENT dead branch** — the opposite of run 01/02's usual pattern. A faithful implementation must reproduce the crash-prone behavior exactly (Q4.5), *or* the Implementer must decide, as an explicit policy choice outside this spec's authority, whether to special-case it. What actually prevents this from crashing real gameplay — if anything does — lives in the surface-rule tree surrounding this mechanism (depth-bounded conditions deciding *when* bandlands is even consulted), which is explicitly out of this run's scope and was not investigated.
· kind `open-question` · confidence `verified(differential-harness)`
· verify-by: already executed — harness section E (systematic threshold derivation and demonstration) and section D (4073 of 9045 wide-sweep trials threw, all correctly reproduced by the faithful independent reimplementation).

**Q5.2** Nothing else conditions the mechanism's form. No biome check, no dimension check, and no y-bound guard exists inside this mechanism itself; if a caller wants to keep `y` inside a safe band, that restriction is the caller's, not this mechanism's.
· kind `contract` · confidence `read-directly` (exhaustive read of the read path; no further conditional exists to find)
· verify-by: none beyond Q5.1's coverage — a negative claim about absence, supported by having read the complete read path.

---

## Q6 — Completeness

**Q6.1** The complete external input set to this mechanism, positively stated: the world seed (via the per-dimension positional factory and the `minecraft:clay_bands` salt of Q3.2, used only at construction); the queried block position `(x, y, z)` (used only at read time, and only `x, z` reach the noise call — `y` reaches only the index arithmetic); and the one registered noise's pack parameters (Q4.3). Nothing else.
· kind `contract` · confidence `read-directly` (exhaustive read of both the construction and read paths)
· verify-by: harness-level — every harness section constructs and queries using exactly this input set and nothing more.

**Q6.2** **No biome is read anywhere in this mechanism.** This is worth stating plainly, the same way run 01 flagged an analogous absence: `bandlands` is invoked from within a broader per-column system that *does* check biomes for other purposes nearby in the same file, and a reader skimming that surrounding code could easily assume biome-dependence carries into this mechanism too. It does not — `clay_bands`'s construction and Q4's read path both depend on nothing but Q6.1's three inputs.
· kind `contract` · confidence `read-directly`
· verify-by: none beyond the exhaustive read already performed; a biome-independence sweep would need a full biome-source harness that this mechanism's own inputs make unnecessary to build.

**Q6.3** World-shape parameters (minimum Y, world height, sea level) are **not** read by this mechanism. The array size (`192`) and the index arithmetic are both fixed regardless of the dimension's actual height range — which is exactly how Q5.1's crash becomes reachable in a tall or deep dimension without this mechanism itself ever consulting that dimension's bounds to protect itself.
· kind `contract` · confidence `read-directly`
· verify-by: none stronger — an absence claim, directly supported by Q6.1's exhaustive input enumeration.

---

## Open questions & deliberate omissions

1. **Q5.1 — the negative-index crash.** Confirmed reachable and reproduced directly against the real method. Not a dead branch to mark DO-NOT-IMPLEMENT; a live hazard in the shipped mechanism that a faithful implementation must decide how to handle, as an explicit policy call outside this spec's authority.
2. **What (if anything) keeps Q5.1 from firing in real play.** Not investigated — lives in the surrounding surface-rule tree, out of scope for this run per `RESEARCH-RUN-03.md` §2.
3. **The second per-dimension noise samplers built alongside this one** (used by unrelated surface features in the same neighborhood of source) were read only far enough to confirm they are not part of this mechanism (Q1.3); not otherwise investigated.
4. **Not investigated, by design (`RESEARCH-RUN-03.md` §3):** the Implementer's behavioral model or ground-truth captures, in either direction. This document was written and its verification corners chosen with zero visibility into that material.
5. **Legacy random source and the carver system** remain closed per prior runs' standing scope; not investigated here either.
