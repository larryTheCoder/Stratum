# Blended noise — clean-room specification (run 02)

- **Provenance:** see `PROVENANCE.md` run-02 section (same 1.21.11 artifacts as run 01; digests re-verified fresh this run). Every claim below is grounded in the decompiled tree, or in differential measurement against the unmodified remapped jar via an external driver — never in prior recollection.
- **Date:** 2026-09-09 · **Brief:** `RESEARCH-RUN-02.md` §4 question order
- **Scope:** the complete sampling path of `minecraft:old_blended_noise`, from its five pack parameters to its scalar output, including the legacy per-octave noise it is built on and that noise's RNG initialization.
- **Status:** hypothesis document, but a stronger one than run 01's initial delivery — every mechanism claim below was differentially tested against the unmodified jar (verified-by-default per this run's raised bar), not merely read. `verify-by` reads "already executed" with trial counts where that happened.
- **Context note:** this run was conducted deliberately withheld from any information about the Implementer's own code, guesses, or failure point (`RESEARCH-RUN-02.md` §3). Nothing about the Implementer's side is referenced anywhere below.

## Notation

- `(x, y, z)` — the block position under evaluation, as three independent integer coordinates.
- Pack parameters, in `snake_case`: `xz_scale`, `y_scale`, `xz_factor`, `y_factor`, `smear_scale_multiplier`.
- `⌊·⌋` is floor for reals→integers; `frac(v) = v − ⌊v⌋ ∈ [0,1)`.
- `wrap(v)` — the coordinate pre-wrap defined in Q1.6.
- `smoothstep(t) = t³(t(6t − 15) + 10)`, the standard quintic ease curve.
- `lerp(t,a,b) = a + t(b−a)`.

> **Floating-point amendment to the notation** (2026-11-09, REVIEW-15 §5). `smoothstep(t)` is evaluated as `((t·t)·t)·((t·((t·6.0) − 15.0)) + 10.0)` and `lerp(t, a, b)` as `a + t·(b − a)`, both in double precision. The expanded polynomial `6t⁵ − 15t⁴ + 10t³` differs in the last bits on most inputs; implement the factored form exactly. Evidence: legacy-named-noise Q2.6 floating-point amendment (the same engine function).
- octave index `n` counts up from `0` (finest/highest-frequency layer) as a sampler's internal array position; `scale(n) = 2⁻ⁿ`.

---

## Q1 — Structure

**Q1.1** The mechanism is built from **three independent legacy-Perlin samplers**: a "min-limit" sampler and a "max-limit" sampler, each with **16 octaves**, and a "main" (selector) sampler with **8 octaves**. Each octave layer, where present, carries amplitude exactly `1.0` — there is no amplitude ladder in the shipped sense (no octave is weighted differently from another); the only per-octave scaling is the `scale(n) = 2⁻ⁿ` frequency/amplitude-reciprocal pairing applied uniformly in the combination formula (Q1.4).
· kind `constant` · confidence `read-directly`
· verify-by: already executed — harness section A, 200 octave-slot inspections across 5 seeds, confirms exactly 16+16+8 populated layers per instance with no gaps.

**Q1.2** The octave **counts and ranges are fixed internal constants of the mechanism, not exposed by any of the five pack parameters.** Sixteen octaves for each limit sampler, eight for the main sampler, always. A pack cannot change how many octaves are summed.
· kind `constant` · confidence `read-directly`
· verify-by: covered by Q1.1's harness section.

**Q1.3** Two frequency multipliers are derived from two of the five pack parameters by one shared constant: `xz_multiplier = 684.412 · xz_scale`, `y_multiplier = 684.412 · y_scale`. `684.412` is a literal, not derived from anything else.
· kind `constant` · confidence `read-directly`
· verify-by: implicit in every harness section that exercises `compute()` (C, G, H).

**Q1.4** The scalar output, as a pure function of (seed, position, the five pack parameters), in full:

Let `px = x·xz_multiplier`, `py = y·y_multiplier`, `pz = z·xz_multiplier`. Let `fx = px / xz_factor`, `fy = py / y_factor`, `fz = pz / xz_factor` (the **main**-sampler's coordinates — factor-divided). Let `smear = y_multiplier · smear_scale_multiplier`, `smear_f = smear / y_factor`.

Main selector, summing all 8 main-sampler octaves that exist (all 8 do, per Q1.1): `main_sum = Σₙ noise_main(n; wrap(fx·scale(n)), wrap(fy·scale(n)), wrap(fz·scale(n)); smear_f·scale(n), fy·scale(n)) / scale(n)` `selector = (main_sum/10 + 1) / 2`

Limit sums, over all 16 octaves of each limit sampler: `min_sum = Σₙ noise_min(n; wrap(px·scale(n)), wrap(py·scale(n)), wrap(pz·scale(n)); smear·scale(n), py·scale(n)) / scale(n)` `max_sum = Σₙ noise_max(n; wrap(px·scale(n)), wrap(py·scale(n)), wrap(pz·scale(n)); smear·scale(n), py·scale(n)) / scale(n)`

Result: `clamp_lerp(selector, min_sum/512, max_sum/512) / 128`, where `clamp_lerp(t,a,b) = a` if `t<0`, `b` if `t>1`, else `lerp(t,a,b)`.

`noise_X(n; …)` is the per-octave fold-noise primitive of Q2, evaluated on octave `n`'s lattice-noise layer of sampler `X`.

**One structural asymmetry worth stating plainly:** the min/max-limit samplers are evaluated at the **factor-undivided** coordinates (`px,py,pz`, only frequency-multiplied); the main/selector sampler is evaluated at the **factor-divided** coordinates (`fx,fy,fz`). `xz_factor`/`y_factor` therefore only ever coarsen the *selector*, never the limit noise itself.
· kind `formula` · confidence `verified(differential-harness)`
· verify-by: already executed — harness section G (8 hand-picked corners spanning the full Y range) and section H (8000-trial random sweep across 6 pack-parameter corners including all four codec boundary values and the full reachable Y range [−2032, 2031]); 0 mismatches, max absolute difference 0.0, tolerance 1e-9.

> **Width amendment** (2026-11-09, REVIEW-14 §1). The text above stands as first published; this block adds integer widths and rounding semantics only.
>
> The block coordinates `x`, `y` and `z` are 32-bit signed integers, each converted **exactly** to double before the first multiply. `scale(n) = 2⁻ⁿ` is exact in double precision for every octave used (`n ≤ 15`). No integer operation follows.
>
> · kind `formula` · confidence `verified(bytecode)`
> · verify-by: already executed — exact integer-to-double conversions read in the disassembly. No boundary exists, so no mutant applies.

> **Field amendment** (2026-11-09, REVIEW-15 §2 / CLAUDE.md rule 14a). This claim's wording was touched by the identifier scrub, so it now carries the mandatory fields. They make the published form explicit, and change no value.
>
> · widths: block coordinates are 32-bit signed, converted exactly to double (width amendment above); the octave index is a small non-negative integer.
> · fp-form: double precision throughout. `px = double(x)·xz_multiplier`, and likewise `py` and `pz`. `fx = px/xz_factor`, and likewise. `smear = y_multiplier·smear_scale_multiplier`, then `smear_f = smear/y_factor`. `scale(n)` starts at `1.0` and is halved once per octave (exact). Each octave's arguments are formed as `wrap(fx·scale(n))` and so on, with divisor `smear_f·scale(n)` and cap `fy·scale(n)` (main sampler), or `smear·scale(n)` and `py·scale(n)` (limit samplers). Each term is `noise(…)/scale(n)`, a division, accumulated in ascending `n` from `0.0`. Then `selector = (main_sum/10.0 + 1.0)/2.0`, and the result is `clamp_lerp(selector, min_sum/512.0, max_sum/512.0)/128.0`, with `lerp(t, a, b) = a + t·(b − a)`. All literals are double. This is the published formula's own order, confirmed against this session's reading.

**Q1.5** The optimization by which the real implementation skips summing the min-limit (respectively max-limit) sampler entirely once the selector has saturated to `≥1` (respectively `≤0`) is **not a behavioral branch** — `clamp_lerp`'s own saturation makes the skipped sum's value irrelevant to the output. It is omitted from Q1.4's formula because including it would describe an implementation detail with zero observable effect, per the purity rule.
· kind `contract` · confidence `read-directly`
· verify-by: none needed — a direct algebraic consequence of `clamp_lerp`'s definition, not a separate behavior.

> **Floating-point amendment** (2026-11-09, REVIEW-15 §5). The text above stands as first published; this block states the engine's floating-point form.
>
> **Correction (rule 20).** The skip **is** behavioral, at one value. When the selector is exactly `1.0`, the engine uses `0.0` in place of the min-limit sum, so the result is exactly `(max_sum/512.0)/128.0`. Evaluating `clamp_lerp(1.0, min_sum/512, max_sum/512)` without the skip gives `a + 1.0·(b − a)`, which is not `b` in general. At a selector of exactly `0.0`, the max-limit skip leaves the result `a` either way. For any selector `> 1` or `< 0`, the skip is indeed unobservable.
>
> Reaching exactly `1.0` needs the main sum within a few units in the last place of `10`. That has not been observed, but it is not excluded. Implement the skip as stated: it is exact and costs nothing.
>
> · kind `contract` · confidence `verified(bytecode+harness)`
> · fp-form: the skip flags are `selector ≥ 1.0` (min-limit sum not summed, and stays `0.0`) and `selector ≤ 0.0` (max-limit sum not summed, and stays `0.0`). `clamp_lerp(t, a, b)` is `a` if `t < 0.0`, `b` if `t > 1.0`, and otherwise `a + t·(b − a)`.
> · verify-by: already executed. The skip flags and the zero-initialized sums were read in the disassembly. The engine's own clamped lerp at `t = 1` returned a value other than `b` for 8 157 of 100 000 random `(a, b)` pairs, and exactly `b` whenever `a = 0`.

**Q1.6** `wrap(v) = v − ⌊v/33554432 + 0.5⌋ · 33554432`, applied to the `x, y, z` position arguments of every octave's noise call (never to the two extra fold-mechanism arguments — see Q2). The floor here uses a wider (64-bit) integer floor than the position-grid floor of Q2.
· kind `formula` · confidence `read-directly`
· verify-by: implicit in every harness section that exercises `compute()`.

---

## Q2 — The fold/cap mechanism

**Q2.1** Each octave's noise call takes, beyond the wrapped `(x,y,z)`, **two further scalar arguments**: a **divisor** and a **cap bound**. From Q1.4's formula: for the main sampler, divisor `= smear_f·scale(n)`, cap bound `= fy·scale(n)`; for each limit sampler, divisor `= smear·scale(n)`, cap bound `= py·scale(n)`. In both cases, **divisor equals cap bound multiplied by `smear_scale_multiplier`, exactly** — `smear_scale_multiplier` enters at (and only at) the divisor, never the cap bound.
· kind `formula` · confidence `verified(differential-harness)`
· verify-by: already executed — harness section E, 6048 hand-picked corner probes (4 x-values × 7 y-values × 3 z-values × 9 divisor magnitudes × 8 cap-bound-to-divisor ratios spanning negative, zero, and both sides of the fractional-part range), driven directly against the unmodified per-octave lattice-noise primitive; 0 mismatches.

> **Correction and field amendment** (2026-11-09, REVIEW-15 §2 / CLAUDE.md rules 14a and 20). The text above stands as first published. Its verify-by wording was touched by the identifier scrub, so this block adds the mandatory fields. It also **corrects one sentence**.
>
> **Correction.** The sentence "divisor equals cap bound multiplied by `smear_scale_multiplier`, exactly" is **false** except at block `y = 1`. The divisor does not depend on the block's `y`: it is `smear_f·scale(n)` for the main sampler, where `smear_f = y_multiplier·smear_scale_multiplier/y_factor`, and `smear·scale(n)` for the limit samplers, where `smear = y_multiplier·smear_scale_multiplier`. The cap bound does depend on `y`: it is `fy·scale(n)`, where `fy = y·y_multiplier/y_factor`, or `py·scale(n)`, where `py = y·y_multiplier`. So `divisor / cap = smear_scale_multiplier / y`, in exact arithmetic. The other half of the sentence stands: `smear_scale_multiplier` enters the divisor only. The divisor and cap formulas stated earlier in this claim, and in Q1.4, are correct. Run 02's value verification used those formulas, so no verified value changes.
>
> · kind `formula` · confidence `verified(bytecode)`
> · widths: block `y` is 32-bit signed, converted exactly to double; no other integer operation.
> · fp-form: double precision. `smear = y_multiplier·smear_scale_multiplier`, then `smear_f = smear/y_factor`. The per-octave divisor is `smear_f·scale(n)` (main) or `smear·scale(n)` (limit), formed as that product. The cap is `fy·scale(n)` (main) or `py·scale(n)` (limit), formed as that product. All literals are double.
> · verify-by: already executed — the divisor's operands were read in the disassembly: the y multiplier times the smear multiplier, with no block-`y` factor. A driver is not needed, because the error is in a stated relation, not in a value: the published operand formulas already match the bytecode.

**Q2.2** Full fold/cap formula. Let `y0 = ⌊y+yo⌋` (the noise layer's own y-offset `yo` added first), `yfrac = (y+yo) − y0`. Given divisor `d` and cap bound `c`:
- if `d ≠ 0`: let `clamp_source = c` if (`c ≥ 0` and `c < yfrac`), else `yfrac`; then `fold = ⌊clamp_source/d + 10⁻⁷⌋ · d`.
- if `d = 0`: `fold = 0`.

The value actually dotted against the lattice gradient at each corner uses `yfrac − fold` as its y-offset. **The interpolation *weight* blending the y0 and y0+1 corners together, however, uses `smoothstep(yfrac)` — the *unfolded* fraction, not `yfrac − fold`.** Folding changes which noise value gets sampled at each lattice corner; it does not change how smoothly the sampled corners are blended into each other.
· kind `formula` · confidence `verified(differential-harness)`
· verify-by: already executed — harness sections D (20 000-trial baseline primitive check, 0 mismatches) and E (above). The interpolation-weight subtlety was caught by this very harness on a first attempt that used the folded fraction for the weight; that attempt failed 5976 of 6048 corners, was corrected, and re-verified to 0 mismatches. Recorded in `AUDIT.md` as a genuine harness-authoring error, not a source-side one.

> **Width amendment** (2026-11-09, REVIEW-14 §1). The text above stands as first published; this block adds integer widths and rounding semantics only.
>
> - `y0 = ⌊y + yo⌋` is a **32-bit** floor, toward −∞, and the lattice x and z corners are the same.
> - `fold = ⌊clamp_source/d + 10⁻⁷⌋ · d`: the floor is **32-bit**, and its integer result is converted exactly to double before the multiply by `d`.
> - **No saturation is reachable:**
>   - the floor's argument is below `1/d + 1`;
>   - by the codec bounds of Q3.1, `d ≥ 684.412·0.001·1/1000·2⁻⁷ ≈ 5.35·10⁻⁶` for the main sampler, and `d ≥ 684.412·0.001·2⁻¹⁵ ≈ 2.09·10⁻⁵` for the limit samplers;
>   - so the argument stays below `1.9·10⁵`;
>   - the lattice floors' arguments are bounded by the Q1.6 wrap plus `256`.
>
>   A 64-bit floor would therefore agree on every reachable input.
>
> · kind `formula` · confidence `verified(bytecode)`
> · verify-by: already executed. 32-bit floors and the exact conversion were read in the disassembly, and the saturation bound was derived from the published codec ranges. Values were already verified bit-exact by run 02's harness (sections D–H). No reachable boundary exists, so no mutant applies.

> **Field amendment** (2026-11-09, REVIEW-15 §2 / CLAUDE.md rule 14a). This claim's own wording was not touched by the scrub. The fields are added because Q2.1, which was, supplies this formula's arguments. They make the published form explicit, and change no value.
>
> · widths: lattice and fold floors are 32-bit (width amendment above).
> · fp-form: double precision. `X = x + xo`, and likewise. The fraction is `X − double(⌊X⌋)`. The fold is `double(⌊clamp_source/d + ε⌋)·d`, where `ε` is the single-precision literal `10⁻⁷` widened to double (Q2.3), and it is added after the division. The corner y offset is `yfrac − fold`. The y interpolation weight is `smoothstep(yfrac)`, the unfolded fraction. This is the published order, confirmed against this session's reading.

**Q2.3** The `10⁻⁷` epsilon added before the divisor-floor is a single-precision (float) literal in source, widened to double for the arithmetic. It nudges an exact multiple of the divisor to round up rather than down under floating-point error; it was not separately isolated in the harness (it is exercised implicitly by every corner in Q2.2's 6048-probe sweep, including several placed exactly at `clamp_source = k·d` for small integers `k`).
· kind `constant` · confidence `read-directly`
· verify-by: covered by Q2.2's sweep; a dedicated exact-multiple probe would isolate it further but was judged unnecessary given the sweep's density.

---

## Q3 — Conditional behavior

**Q3.1** The `d = 0` fallback of Q2.2 is **dead for every reachable input.** `d` is a product/quotient of `y_multiplier` (`= 684.412 · y_scale`), optionally `smear_scale_multiplier`, optionally `1/y_factor`, and `scale(n) = 2⁻ⁿ`. `y_scale` and `y_factor` are pack-codec-constrained to `[0.001, 1000]` (never zero, never negative); `smear_scale_multiplier` to `[1.0, 8.0]` (never zero); `scale(n)` is strictly positive for any finite octave index. `d` is therefore strictly positive for every valid pack configuration and every octave — the `d = 0` fallback (`fold = 0` unconditionally) can never execute. **DO-NOT-IMPLEMENT** the `d = 0` branch as a reachable case; implement only the `d ≠ 0` formula.
· kind `open-question` · confidence `inferred-from-reading` (a direct algebraic consequence of the codec bounds read in Q4.4 of the constants file; not itself independently re-derivable by a numeric sweep, since a sweep cannot prove a universal negative)
· verify-by: none stronger available — this is a proof from the pack schema's own value ranges, not a measurement. Recorded as an open question only in the sense that a future pack format allowing zero-valued scales would reopen it.

**Q3.2** The **cap** sub-branch of Q2.2 (`clamp_source = c` instead of `yfrac`) requires `c ≥ 0`. `c`'s sign exactly tracks the sign of the block's world-Y coordinate at every octave and in both sampler families, because every factor multiplying Y into `c` (`y_multiplier`, optionally `1/y_factor`, `scale(n)`) is strictly positive by the same codec bounds as Q3.1. **For `y < 0`, the cap sub-branch can never bind, at any octave, under any in-range pack parameters** — it is not merely rare, it is provably unreachable below `y = 0`. The fold itself still occurs unconditionally (Q3.1); only the *cap* — which would otherwise clamp `yfrac` down to `c` before folding — never activates there. For `y ≥ 0` the cap **can** bind, exactly when `0 ≤ c < yfrac`; whether it does depends jointly on octave, position, and pack parameters, and is not further reducible to a closed predicate.
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed — harness section F, sign-tracking cross-check over 112 (Y, octave) pairs spanning the full reachable range `[−2032, 2031]`, 0 mismatches; separately corroborated end-to-end by Q1.4/Q2.2's direct-value verification, which included boundary and negative-Y points.

**Q3.3** No other conditional governs this mechanism's *form*. There is no separate branch for `x` or `z` sign or magnitude, and no separate branch triggered by pack-parameter magnitude (beyond the codec's own range clamp, which is enforced at pack-load time, not at sample time, and is therefore not part of the sampling mechanism's own conditional structure).
· kind `contract` · confidence `read-directly` (exhaustive read of the compute path; no further conditional exists to find)
· verify-by: none beyond Q1.4/Q3.2's coverage — a negative claim about absence, supported by having read the complete mechanism.

---

## Q4 — Initialization

**Q4.1** Each of the three samplers (min-limit, max-limit, main) is constructed via the **legacy, sequential-consumption** RNG path — not the modern positional-fork path. (The modern path exists on the shared underlying sampler class but is reached only through a different, unscoped entry point; it is never invoked for this mechanism.)
· kind `contract` · confidence `read-directly`
· verify-by: none needed beyond Q4.2–Q4.4, which behaviorally confirm the sequential path's specific consumption pattern.

**Q4.2** All three samplers **share one random stream**, advanced in sequence: the min-limit sampler is constructed first, then the max-limit sampler continues from wherever the min-limit sampler left the shared source, then the main sampler continues from wherever the max-limit sampler left it. Construction order is therefore output-visible, and this is that order: **min-limit, then max-limit, then main.**
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed — harness section B. A from-scratch, independently-ordered replay (a fresh random stream at the same seed; construct 16 layers, then 16, then 8, each layer via one ordinary sequential draw) was compared against the real instance's internal per-layer state, extracted read-only. Exact match across all 40 layers; any wrong ordering would have produced a diverging state from the first layer onward, since each layer's construction consumes a variable, seed-dependent number of draws from the shared source.

**Q4.3** Within each sampler, layers are constructed **highest-frequency octave first** (octave `0`), then descending through the coarser octaves in strict numeric order (octave `−1, −2, …` down to that sampler's coarsest octave — `−15` for each limit sampler, `−7` for the main sampler), all from the one shared, continuously-advancing random stream.
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: covered by Q4.2's same harness section — the per-layer ordering was part of what was matched, not just the sampler-level ordering.

**Q4.4** Each octave layer's construction draws, from the shared source, in this order: three floating-point draws (becoming that layer's own internal `(xo, yo, zo)` position offsets), then 256 further draws performing an in-place shuffle of a 256-entry permutation table. No draws are skipped or discarded for this mechanism: **every one of the 40 total octave layers (16+16+8) that this mechanism ever constructs is genuinely present with amplitude `1.0`** (see Q4.5), so the "skip a layer's draws without building it" fallback that exists on the shared sampler class is never exercised here.
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed — harness section A (0 of 200 inspected layers were ever the "skipped" null placeholder, across 5 distinct seeds) plus section B/C's exact-match replay, which would have diverged immediately had any layer's draw-count differed from the hypothesized fixed pattern.

**Q4.5** The "skip a layer without constructing it" fallback exists on the shared sampler class and is **dead for this mechanism specifically** — its use is gated on a layer's amplitude being exactly zero, and every layer this mechanism ever requests has amplitude exactly `1.0` by construction (the octave range passed is always a full contiguous span with no gaps, so no zero-amplitude layer can ever occur). **DO-NOT-IMPLEMENT** the skip path for this mechanism; a correct implementation always constructs exactly 40 layers per `(seed, five pack parameters)` combination, never skips one.
· kind `open-question` · confidence `read-directly` (the reachability argument follows directly from the fixed, hardcoded octave ranges of Q1.2, which admit no gap)
· verify-by: already executed — same evidence as Q4.4 (harness section A).

---

## Q5 — Completeness

**Q5.1** The complete external input set to the mechanism's own compute path is: the derived random stream (itself a pure function of the world seed, via a positional fork salted by the string **`minecraft:terrain`** — runtime data, permitted), the block position `(x,y,z)`, and the five named pack parameters. Nothing else.
· kind `contract` · confidence `read-directly` (exhaustive read of the compute path and its transitive reads; the seed-derivation salt was read directly at the wiring site, one level up from the mechanism itself)
· verify-by: harness-level — every harness section constructs instances directly from exactly this input set and nothing more, and all pass.

**Q5.2** The octave *counts and ranges* (Q1.2) are **not** part of the five named pack parameters — they are fixed internal constants of the mechanism. This is the one place where "beyond (seed, position, five pack parameters)" genuinely applies: the mechanism's shape (40 total layers, split 16/16/8) cannot be changed by any pack, only its scale/geometry can.
· kind `contract` · confidence `read-directly`
· verify-by: covered by Q1.1/Q1.2.

**Q5.3** A derivative-computing variant of the shared per-octave primitive exists on the same class as the fold/cap mechanism (Q2), but is **never called anywhere in this mechanism's compute path, and is not called by anything else in the decompiled tree either** — it is unreferenced dead code as far as this repository's build is concerned. It is not part of this mechanism and was not investigated further.
· kind `open-question` · confidence `read-directly` (confirmed by an exhaustive reference search across the whole decompiled tree, not just this mechanism's own files)
· verify-by: none — out of scope by construction; flagged so a future run does not need to rediscover its irrelevance.

**Q5.4** A debug-string-producing method exists on the mechanism's class, printing fixed literal values (not the instance's actual pack-parameter fields) for some of its display fields. It is marked test-only in source and is not part of the sampling computation; not investigated further.
· kind `open-question` · confidence `read-directly`
· verify-by: none — test-only tooling, out of scope.

---

## Coverage note

Every mechanism claim above except Q3.1 (a universal-negative proof, not a sweep target) and Q2.3 (an epsilon, exercised only implicitly) carries a `verified(differential-harness)` or equivalent executed-check tag. This is a stronger evidentiary bar than run 01's initial delivery achieved on a first pass, per this run's raised acceptance bar (`RESEARCH-RUN-02.md` §5). One harness-authoring error was caught and corrected in the process (Q2.2) — recorded in full in `AUDIT.md` rather than silently fixed.

## Open questions & deliberate omissions

1. **Q3.1 — the `d = 0` fallback.** DO-NOT-IMPLEMENT; unreachable under current codec bounds. Would reopen only if a future pack format allowed a zero-valued scale/factor.
2. **Q4.5 — the "skip a layer" fallback.** DO-NOT-IMPLEMENT for this mechanism specifically; unreachable given the fixed, gapless octave ranges. (The fallback itself is real code, exercised elsewhere on the shared sampler class by mechanisms this run did not investigate — out of scope.)
3. **Q5.3 — the derivative-computing primitive variant.** Confirmed entirely unreferenced in this tree. Not investigated; flagged so it isn't mistaken for part of this mechanism.
4. **Not investigated, by design (`RESEARCH-RUN-02.md` §3):** anything about the Implementer's own code, its guesses, or where it disagrees with the engine. This document was written with zero visibility into that side, and nothing in it was shaped by it.
5. **Legacy random source.** As with run 01, presets selecting it derive seeds differently; explicitly out of scope for this run (`RESEARCH-RUN-02.md` §2) and not investigated.
