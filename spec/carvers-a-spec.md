# Configured carvers, Part A (`cave`, `nether_cave`, shared machinery) — clean-room specification (run 05)

- **Provenance:** see `PROVENANCE.md`, run-05 sections (the same 1.21.11 artifacts as runs 01–04; digests re-verified at the start of every session, before any reading). Every claim is grounded in this run's reading of the recorded decompiled tree, the shipped bytecode, or differential measurement against the unmodified remapped jar. Nothing is from recollection.
- **Date:** 2026-11-09 · **Brief:** `RESEARCH-RUN-05.md` §3 question order; split per REVIEW-19 §1. Part A answers Q1–Q9 and Q11–Q13 for `minecraft:cave` and `minecraft:nether_cave`. Q10 and every `minecraft:canyon` delta are Part B.
- **Scope:** from (world seed, target chunk, pack configuration) to the state of every cell the cave and nether-cave carvers can affect in that chunk, at the moment carving completes. The aquifer is cited by claim id from `spec/aquifer-spec.md`, not re-derived.
- **Status:** hypothesis document, verified by default. `verify-by` reads "already executed" with counts where the experiment ran.
- **Context note:** the Implementer's model, measurements and lab design were withheld from this run, and nothing here refers to them. Every seed, square and configuration was chosen fresh.

## Notation

- `W`: the world seed, a 64-bit signed integer.
- `(Tx, Tz)`: the **target chunk**, the chunk whose cells are being decided. Its minimum block coordinates are `X₀ = 16·Tx`, `Z₀ = 16·Tz`, and its middle is `Xm = X₀ + 8`, `Zm = Z₀ + 8`.
- `(Sx, Sz)`: a **start chunk**, a chunk whose carvers may carve into the target.
- `k`: a configured carver's 0-based position in the start chunk's biome carver list (Q11.2).
- `y_min`, `H`: the generation floor and generation height. `y_min = max(dimension floor, noise-settings floor)` and `H = min(dimension height, noise-settings height)`. Shipped values: overworld `y_min = −64`, `H = 384`; nether `y_min = 0`, `H = 128`.
- **The legacy generator** `G`: the 48-bit linear congruential generator of Q1.2. Its draws are written `F()` (a float in `[0, 1)`), `I(n)` (an int in `[0, n)`) and `Lg()` (a 64-bit long). Every draw advances `G`.
- `S(a)`, `C(a)`: the table sine and cosine of Q9.1.
- `f32(·)`: rounding to single precision. Arithmetic marked *float* is IEEE-754 single precision. *double* is IEEE-754 double precision. Every float-to-double conversion is exact.
- Pack field names in `snake_case` (`probability`, `y`, `yScale`, `lava_level`, `replaceable`, `horizontal_radius_multiplier`, `vertical_radius_multiplier`, `floor_level`, `debug_settings`, `debug_mode`, `air_state`, `water_state`, `lava_state`, `barrier_state`) and registry keys (`minecraft:…`) are runtime data, and permitted.

---

## Q1 — Start

**Q1.1** Per-start seeding. For start chunk `(Sx, Sz)` and list position `k`, a legacy generator is seeded with `W + k`. Its first two long draws are `A = Lg()` and `B = Lg()`. The generator is then re-seeded with `σ = (Sx·A) ⊕ (Sz·B) ⊕ (W + k)`. Every draw for this start (Q1.3–Q1.5) comes from the re-seeded generator. This holds in every dimension, whatever the noise settings' legacy-random flag.
· kind `formula` · confidence `verified(differential-harness)`
· widths: `W + k`, both products and both exclusive-ors are 64-bit signed with two's-complement wrap. `Sx` and `Sz` are 32-bit signed, sign-extended to 64 bits before multiplying. `k` is 32-bit, sign-extended.
· fp-form: none (no floating-point operation).
· verify-by: already executed. 661 849 trials against the unmodified jar, 0 mismatches. They included `W + k` overflow, chunk coordinates at the world edge, and non-legacy dimensions.

**Q1.2** The legacy generator. The state `s` is a 48-bit unsigned integer.
- **Seeding with `v`:** `s = (v ⊕ 0x5DEECE66D) mod 2⁴⁸`.
- **One step:** `s ← (s·0x5DEECE66D + 0xB) mod 2⁴⁸`. `next(b)` is the top `b` bits of the new `s`, taken as a 32-bit signed integer.
- **`F()`:** `next(24)·2⁻²⁴`. The value is exact in single precision.
- **`I(n)`, for `n > 0`:**
  - When `n` is a power of two, `I(n) = (n·next(31)) >> 31`.
  - Otherwise, repeatedly draw `b = next(31)` and set `v = b mod n`, until `b − v + (n − 1)` does not overflow. `I(n) = v`.
- **`Lg()`:** `(next(32) << 32) + next(32)`. The second term is sign-extended before the 64-bit addition.
· kind `formula` · confidence `verified(differential-harness)`
· widths:
  - `s` is 48-bit unsigned. The step's product and sum are reduced modulo 2⁴⁸.
  - `next(b)` returns 32-bit signed. In `I(n)`, the power-of-two product is 64-bit, followed by an arithmetic right shift. The rejection test is 32-bit signed addition, where overflow means a negative result.
  - `Lg()` is 64-bit signed, with wrap.
· fp-form: `F()` is a single-precision product of an exact integer and `2⁻²⁴`; it is exact.
· verify-by: already executed. Every draw of every start in the Q1–Q9 driver (9 028 435 comparisons in all) matched the jar.

**Q1.3** Start test. A start happens iff `F() ≤ probability`, compared in single precision. `probability` is the pack's float. This is the first draw after Q1.1's re-seeding.
· kind `formula` · confidence `verified(differential-harness)`
· widths: none (no integer operation).
· fp-form: single-precision comparison, non-strict. The pack value is stored as the float nearest to the decimal literal; shipped `0.15`, `0.07` and `0.2` are therefore their float roundings.
· verify-by: already executed, as part of Q1.2. The start tests matched in every trial. The carving-stage acceptance (Q11.6) repeats them, with 273 371 passing starts in all.

**Q1.4** The complete draw sequence of one start, after a passing start test. All draws are from the same re-seeded generator, in this order.
- **System count** `N = I(I(I(β) + 1) + 1)`, with `β = 15` for `cave` and `β = 10` for `nether_cave`.
- **Per system** `t = 0 … N−1`:
  - the system origin's x, y and z, in that order: `x = 16·Sx + I(16)`, then `y` from the `y` provider (Q1.5), then `z = 16·Sz + I(16)`;
  - then `horizontal_radius_multiplier`, `vertical_radius_multiplier` and `floor_level`, each from its provider (Q1.5) and in that order: `h_m`, `v_m`, `f_l`;
  - then the room test `I(4) = 0`;
  - when it passes: `yScale` from its provider, then the room radius draw `r = 1 + F()·6` (float), then the extra tunnel count `I(4)`. The system has `1 + I(4)` tunnels. When the room test fails, it has one tunnel.
- **Per tunnel**, in order:
  - heading `θ = F()·6.2831855` (float);
  - pitch `φ = (F() − 0.5)/4` (float);
  - thickness `τ`:
    - `cave`: `a = F()·2 + F()`, then `I(10)`. When that draw is `0`, `τ = a·(F()·F()·3 + 1)`; otherwise `τ = a`.
    - `nether_cave`: `τ = (F()·2 + F())·2`, with no conditional draw.
  - length `L = 112 − I(28)`;
  - finally the tunnel's own seed `Lg()` (Q2.1).
- The room itself is carved before the system's tunnels (Q12.5).
· kind `formula` · confidence `verified(differential-harness)`
· widths: `16·Sx + I(16)` and `16·Sz + I(16)` are 32-bit signed sums, converted exactly to double. `112 − I(28)` is 32-bit.
· fp-form:
  - All of `θ`, `φ`, `τ` and `r` are computed in single precision, in the written order: draws are taken left to right, and products associate left to right.
  - `6.2831855` is the single-precision rounding of 2π. `0.5`, `4`, `2`, `3`, `1` and `6` are float literals.
  - The system origin and the three provider values are widened exactly to double.
· verify-by: already executed. Draw-for-draw equality on every start of the driver scenarios, for both carvers. Bit equality of the derived walk state (Q3) covered 179 699 cross-section evaluations.

**Q1.5** Which providers draw, and when.
- **A uniform height** (the `y` field) resolves its two anchors (Q13.3) to `lo` and `hi`, then draws `I(hi − lo + 1) + lo`. When `lo > hi` it draws **nothing** and yields `lo`.
- **A uniform float** draws `F()·(max − min) + min`, in single precision, with `max` exclusive.
- **A constant float** draws nothing.

So for the shipped `nether_cave`, whose multipliers, floor and `yScale` are all constants, a system consumes only its x, y and z draws, the room test, and the room radius and extra-tunnel draws when the room test passes.
· kind `formula` · confidence `verified(differential-harness)`
· widths: `hi − lo + 1` and the final sum are 32-bit signed. Anchors resolve to 32-bit values (Q13.3).
· fp-form: uniform float is single precision: `(max − min)` is computed first, then the product with the draw, then `+ min`.
· verify-by: already executed. Shipped `cave` (all uniform) and shipped `nether_cave` (constants with a uniform `y`) were both driven; draw sequences matched exactly.

> **Amendment to Q1.5: the uniform float's upper bound can be reached** (2026-11-09, run 05 Part B). The text above stands as first published. Its formula is right. Its parenthetical "with `max` exclusive" names the pack field (`max_exclusive`), but read as a bound it is **false**: the final single-precision sum can round up to exactly `max`. Implement the formula as written, and never clamp the result or assume it lies below `max`.
>
> Exhaustive evaluation of the formula over the top 4 096 draws, for every shipped uniform float provider:
> - shipped `cave` and `cave_extra_underground` `vertical_radius_multiplier` (0.8 to 1.3): **2** of the 2²⁴ draws give exactly `1.3`;
> - the canyon's `distance_factor` and `horizontal_radius_factor`: 2 each (Part B);
> - every other shipped uniform float provider: none.
>
> · kind `formula` · confidence `inferred-from-reading`. Inference: exhaustive evaluation of this claim's verified formula at the top draws.
> · widths: the draw is `next(24)`, a 24-bit integer, scaled exactly (A-Q1.2). No other integer operation.
> · fp-form: single precision: `(max − min)`, then the product with the draw, then `+ min`. Only the final rounding can reach `max`.
> · verify-by: already executed (the exhaustive evaluation above). Correction category: a false bound in a prose relation; the formula is unchanged.

---

## Q2 — Walk randomness

**Q2.1** Each tunnel walks with its **own** legacy generator, `g`, seeded (Q1.2 seeding) with the 64-bit value `Lg()` drawn last for that tunnel in Q1.4. Each fork (Q6.1) is seeded with a long drawn from its parent tunnel's `g`. No walk draws from the start generator or from any positional derivation.
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed, as part of Q1.4's bitwise walk-state comparison.

**Q2.2** Consequently every walk, its forks, and every ellipsoid it produces (Q3, Q4) are pure functions of `W`, the start chunk, `k`, the configured carver and its pack fields. The walk does not depend on the target chunk. The target chunk affects only which ellipsoid cells are decided (Q4), and through the reach test (Q7.1), at which step a tunnel stops.
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed. The same starts, evaluated for different targets in the driver, produced identical walk-state sequences up to each target's reach-test exit.

---

## Q3 — The walk

**Q3.1** Tunnel state and initial values.
- **State:**
  - position `(x, y, z)`, double;
  - heading `θ` and pitch `φ`, float;
  - heading and pitch velocities `δθ` and `δφ`, float, both starting at `0`;
  - thickness `τ`, float;
  - step index `i`, int;
  - length `L`, int.
- **Root tunnels** start at the system origin (Q1.4), not offset, with `i₀ = 0`, the `θ`, `φ`, `τ` and `L` of Q1.4, and vertical scale `λ = 1` (`cave`) or `λ = 5` (`nether_cave`).
- **Forks** start as Q6.1 states.
- **Before the first step**, `g` draws the fork index `b = I(⌊L/2⌋) + ⌊L/4⌋`, then the steepness flag `steep = (I(6) = 0)`.
· kind `formula` · confidence `verified(differential-harness)`
· widths: `⌊L/2⌋` and `⌊L/4⌋` are 32-bit signed division, truncating; `L ≥ 85`, so both are positive. The sum is 32-bit.
· fp-form: none beyond Q1.4 (no new floating-point operation).
· verify-by: already executed (bitwise walk state, Q1.4).

**Q3.2** One step. For `i = i₀, …, L − 1` the state is updated as follows. Every quantity on the right-hand side is the value before this step, unless it is marked as updated.
- **Radii:** `h_i = 1.5 + f32(S(f32(f32(3.1415927·i)/L))·τ)` and `v_i = h_i·λ`.
- **Position:** with `c = C(φ)`, `x += f32(C(θ)·c)`, `y += S(φ)`, `z += f32(S(θ)·c)`.
- **Pitch and heading:** `φ ← f32(f32(φ·(steep ? 0.92 : 0.7)) + f32(δφ·0.1))`; then `θ ← f32(θ + f32(δθ·0.1))`.
- **Velocities, decayed:** `δφ ← f32(δφ·0.9)`, then `δθ ← f32(δθ·0.75)`.
- **Velocities, kicked:** `δφ ← f32(δφ + f32(f32(f32(F() − F())·F())·2))`, then `δθ ← f32(δθ + f32(f32(f32(F() − F())·F())·4))`. In each, the three draws are taken left to right, all `δφ` draws before any `δθ` draw.

The fork check (Q6.1) follows, using the **updated** position, heading and pitch. Then the carve check (Q3.3). `h_i` and `v_i` use the step index **before** any update and are not changed by the step's motion.
· kind `formula` · confidence `verified(differential-harness)`
· widths: `i` and `L` are 32-bit; each converts exactly to float for the multiplication and division. `i ≤ 111`, so the conversion is exact.
· fp-form:
  - The argument of `S` is single precision: `f32(3.1415927·i)`, then divided by `L` in single precision. `3.1415927` is π rounded to float.
  - `S(·)·τ` is a single-precision product, widened exactly and added to the double `1.5`. `h_i·λ` is double.
  - The position increments are single-precision products, widened exactly and added to the double coordinates. `S(φ)` is widened exactly.
  - All angle, velocity and constant arithmetic is single precision. `0.92`, `0.7`, `0.1`, `0.9`, `0.75`, `2` and `4` are float literals.
  - Order: as written, left to right within each expression.
· verify-by: already executed. All five cross-section arguments (Q4.1), compared bit for bit at every one of 179 699 carve calls, with 0 mismatches.

**Q3.3** Carve check, after the fork check of the same step. The tunnel draws `I(4)`.
- When it is `0`, the step carves nothing, and **no reach test is made**.
- Otherwise the reach test (Q7.1) is applied. If it fails, the tunnel ends immediately. If it passes, an ellipsoid (Q4.1) is carved at `(x, y, z)` with horizontal radius `h_i·h_m` and vertical radius `v_i·v_m`. These are double products, using the system's multipliers from Q1.4.
· kind `formula` · confidence `verified(differential-harness)`
· widths: none (no integer arithmetic beyond the draw).
· fp-form: `h_i·h_m` and `v_i·v_m` are double products of double operands. The multipliers are float provider values, widened exactly.
· verify-by: already executed (bitwise radii at every carve call, Q3.2).

**Q3.4** The room (start shape). When a system's room test passes (Q1.4), one ellipsoid is carved before its tunnels. Its centre is `(x + 1, y, z)` (double), from the system origin. Its horizontal radius is `h_R = 1.5 + f32(S(1.5707964)·r)` and its vertical radius `h_R·ys`, where `ys` is the drawn `yScale`.

The room does **not** apply `horizontal_radius_multiplier` or `vertical_radius_multiplier`, and makes no reach test. It does apply `floor_level` (Q4.3).
· kind `formula` · confidence `verified(differential-harness)`
· widths: none.
· fp-form: `1.5707964` is π/2 rounded to float, widened exactly before the table lookup. `S(·)·r` is a single-precision product, widened and added to the double `1.5`. `h_R·ys` is a double product, with `ys` widened from float. `x + 1` is double.
· verify-by: already executed (room calls are among the 179 699 bitwise carve calls).

**Q3.5** `nether_cave` differs from `cave` in the walk only in:
- the system bound `β = 10` (Q1.4);
- the thickness formula (Q1.4);
- the root tunnels' vertical scale `λ = 5`. Forks use `λ = 1` in both carvers (Q6.1).

Everything else in Q3 is shared.
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed (nether scenario of the bitwise driver).

---

## Q4 — Cross-section

**Q4.1** An ellipsoid is carved at centre `(cx, cy, cz)` with horizontal radius `ρ` and vertical radius `ν`, against target chunk `T`.
- **Early-out:** if `|cx − Xm| > 16 + 2ρ` or `|cz − Zm| > 16 + 2ρ`, nothing is decided. This test is speed-only (Q8.2), but implement it exactly.
- **Bounds:** otherwise the decided region is the box below. All bounds are inclusive except `y_lo`.
  - local x from `max(⌊cx − ρ⌋ − X₀ − 1, 0)` to `min(⌊cx + ρ⌋ − X₀, 15)`;
  - local z from `max(⌊cz − ρ⌋ − Z₀ − 1, 0)` to `min(⌊cz + ρ⌋ − Z₀, 15)`;
  - y from `y_hi = min(⌊cy + ν⌋ + 1, y_min + H − 1 − m)` **down to** `y_lo + 1`, where `y_lo = max(⌊cy − ν⌋ − 1, y_min + 1)`.
  - The top margin is `m = 7`, except in a chunk being upgraded from an older world format, where `m = 0` (Q13.6).
· kind `formula` · confidence `verified(differential-harness)`
· widths: `⌊·⌋` is the floor of a double to a 32-bit signed integer. Every bound expression is 32-bit signed arithmetic. `X₀`, `Z₀` and the margins are 32-bit.
· fp-form: `cx ± ρ`, `cy ± ν` and `16 + 2ρ` are double. `|cx − Xm|` is computed in double, with `Xm` exactly converted.
· verify-by: already executed. Every cell and mask bit of 105 target-chunk driver runs, plus the carving-stage acceptance (Q11.6), with 0 differences.

**Q4.2** Column test. For a column at world coordinates `(X, Z)` inside the bounds, define `u = (X + 0.5 − cx)/ρ` and `w = (Z + 0.5 − cz)/ρ`. The column is considered iff `u² + w² < 1` (strict).
· kind `formula` · confidence `verified(differential-harness)`
· widths: `X` and `Z` are 32-bit, converted exactly to double.
· fp-form: double precision, in the written order: `(X + 0.5) − cx`, then the division. `u·u + w·w` is summed left to right.
· verify-by: already executed (as Q4.1).

**Q4.3** Cell test and `floor_level`. For a considered column and each `y` in Q4.1's range, define `t = (y − 0.5 − cy)/ν`. The cell is excluded iff `t ≤ f_l` (the system's sampled `floor_level`) or `u² + t² + w² ≥ 1`. So a cell is included iff `t > f_l` and `u² + t² + w² < 1`, both strict.
· kind `formula` · confidence `verified(differential-harness)`
· widths: `y` is 32-bit, converted exactly to double.
· fp-form: double precision. `(y − 0.5) − cy`, then the division. The sum is `(u·u + t·t) + w·w`, left to right. `f_l` is the float provider value widened exactly. This order matters for the bits at the boundary, so reproduce it.
· verify-by: already executed (as Q4.1).

**Q4.4** Mask. Each target chunk has one carving mask, a set of cells, initially empty. It is shared by every start, carver, system, room and tunnel carving into that chunk (Q11.4). An included cell already in the mask is **not** decided again. An included cell not yet in the mask is added to it and then decided (Q12). A cell enters the mask when it is reached, **before** the replaceability and aquifer tests, so it enters the mask even when it ends up unchanged, and is then never decided again.
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed (mask bits compared in the driver; mask effects end to end in Q11.6).

**Q4.5** Decision order within one ellipsoid: x ascending, then z ascending, then y **descending**. REVIEW-21 §1 requires this order, because fill marks depend on decision history (Q12.4).
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed. A mutant with a permuted within-ellipsoid order changed post-processing marks in 11 of 35 target chunks. The unpermuted order matched in all of them.

---

## Q5 — Discontinuity

**Q5.1** A tunnel's carving is the union of its step ellipsoids (Q3.3, Q4) at successive centres, which lie about one block apart (Q3.2). Carving that is not contiguous with the rest of a tunnel arises only through:
- **(a)** steps that carve nothing (Q3.3's one-in-four skip), when the neighbouring carved ellipsoids are too small to overlap across the gap;
- **(b)** the floor cut (Q4.3), which removes the lower part of every ellipsoid;
- **(c)** the generation-range clamp (Q4.1);
- **(d)** cells left unchanged inside the shape: non-replaceable blocks, and aquifer barrier verdicts (Q12.2), which leave islands;
- **(e)** the room, which is offset by `+1` in x from the tunnels' common origin (Q3.4).

There is no other jump: the position update is continuous, and forks start at their parent's position (Q6.1).
· kind `contract` · confidence `inferred-from-reading`. Inference: the union-of-ellipsoids model is verified cell for cell (Q4, Q11.6). The list above enumerates every way the verified model can leave a gap in that union. No separate mechanism produces non-contiguous carving.
· verify-by: covered by the verified model. A cheap direct probe: in a driver run, take each tunnel's carved cells and confirm that every face-disconnected component is explained by (a)–(e).

---

## Q6 — Branching

**Q6.1** Fork. At the step whose index `i` equals the fork index `b` (Q3.1), after that step's state update, the tunnel forks iff `τ > 1`, compared in single precision. It then:
- draws from its own generator, in order: seed₁ `= Lg()`, thickness `τ₁ = F()·0.5 + 0.5`;
- runs fork 1 to completion;
- draws seed₂ `= Lg()`, `τ₂ = F()·0.5 + 0.5`;
- runs fork 2;
- **ends**. The parent neither carves at step `b` nor continues.

Each fork starts at the parent's updated `(x, y, z)`, with start index `i₀ = b`, the parent's `L`, and vertical scale `λ = 1`. It inherits the system's `h_m`, `v_m` and `f_l`. Its heading is `θ − 1.5707964` (fork 1) or `θ + 1.5707964` (fork 2), and its pitch `φ/3`. Its velocities start at `0`, and it draws its own `b` and `steep` (Q3.1).
· kind `formula` · confidence `verified(differential-harness)`
· widths: none beyond Q3.1.
· fp-form: all single precision. `1.5707964` is π/2 rounded to float; `0.5` and `3` are float literals.
· verify-by: already executed (bitwise walk state, Q1.4, includes every fork).

**Q6.2** Forks never fork. A fork's thickness is at most `f32(f32((1 − 2⁻²⁴)·0.5) + 0.5) = 1.0`: the largest draw, halved, plus `0.5`, rounds to exactly `1.0` under round-half-to-even. So the fork condition `τ > 1` is always false for a fork. Branching is at most one level deep, and only from root tunnels with `τ > 1`.
· kind `contract` · confidence `inferred-from-reading`. Inference: from Q6.1's formula and single-precision rounding at the largest possible draw.
· verify-by: already executed: `f32(f32(0.99999994·0.5) + 0.5)` evaluates to `1.0` under IEEE-754 round-half-to-even.

**Q6.3** Systems and rooms.
- A system with a room has `1 + I(4)` root tunnels, so 1 to 4. A system without one has 1.
- Every root tunnel of a system starts at the system origin `(x, y, z)`. The room's centre is `(x + 1, y, z)`.
- The room and its tunnels share the system's `f_l`. Only the tunnels use `h_m` and `v_m` (Q3.4).
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed (as Q1.4).

---

## Q7 — Termination and reach

**Q7.1** Reach test. This applies only at carving steps (Q3.3), against target `T`. With `dx = x − Xm`, `dz = z − Zm`, `r = L − i` and `q = f32(f32(τ + 2) + 16)`, the test passes iff `dx² + dz² − r² ≤ q²`. A failure **ends the tunnel**: no later step and no fork happens for this target.

The test is behavioural, not merely a speed-up: it can end a tunnel that would still have carved cells in the target. Reproduce it exactly.
· kind `formula` · confidence `verified(differential-harness)`
· widths: `L − i` is 32-bit, converted exactly to double. `Xm` and `Zm` are 32-bit, converted exactly.
· fp-form: `q` is single precision (`τ + 2`, then `+ 16`) widened exactly. The rest is double, in the order `(dx·dx + dz·dz) − r·r ≤ q·q`.
· verify-by: already executed. Bitwise in the driver. A mutant ignoring the test changed carved cells in 1 of 35 target chunks.

**Q7.2** A tunnel ends at exactly one of:
- its last step `i = L − 1`;
- its fork step (Q6.1);
- a failed reach test (Q7.1).

Skipped carve steps (Q3.3) do not end it, and nothing else skips steps.
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed (as Q1.4).

**Q7.3** Distance bound. Target `T` is carved only by starts with `max(|Sx − Tx|, |Sz − Tz|) ≤ 8`. A walk that would reach beyond that window from its start chunk never carves there. This hard positional cut-off is independent of geometry.
· kind `constant` · confidence `verified(differential-harness)`
· verify-by: already executed. The carving-stage acceptance ran the real pipeline with exactly this window and matched cell for cell (Q11.6).

---

## Q8 — Chunk boundaries

**Q8.1** Each target chunk is computed by **re-running in full**, from their start draws, every start in its 17×17 window (Q7.3). Nothing is cached or shared between targets except the chunk's own mask (Q4.4). Only cells inside the target are decided.
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed (Q11.6).

**Q8.2** The ellipsoid early-out (Q4.1) is speed-only, by argument.
- If `cx − Xm > 16 + 2ρ`, then `⌊cx − ρ⌋ − X₀ − 1 ≥ 23`, which exceeds the upper bound `15`.
- If `Xm − cx > 16 + 2ρ`, then `⌊cx + ρ⌋ − X₀ < −8`, which is below the lower bound `0`.
- Likewise in z.

So whenever it fires, the column range is already empty. Implement it exactly anyway (REVIEW-21 §2). The reach test, unlike it, is behavioural (Q7.1).
· kind `contract` · confidence `inferred-from-reading`. Inference: the inequalities above. Also observed: a mutant without the early-out changed nothing in 35 of 35 target chunks.
· verify-by: already executed (mutant, 35 chunks).

---

## Q9 — Numerics

**Q9.1** The sine table has 65 536 single-precision entries. Entry `j` is the single-precision rounding of the sine of `j/10430.378350470453`.
- `S(a)` is the entry at index `trunc₆₄(a·10430.378350470453) & 65535`.
- `C(a)` is the entry at index `trunc₆₄(a·10430.378350470453 + 16384) & 65535`.
- Arguments are doubles; float angles are widened exactly first.
- Truncation is toward zero, not floor: a floor-based index differs on every negative non-integral product.
· kind `formula` · confidence `verified(differential-harness)`
· widths: `trunc₆₄` converts a double to a 64-bit signed integer, toward zero. `& 65535` is a 64-bit bitwise and, and the result indexes the table.
· fp-form: the product, and the `+ 16384` for `C`, are double, in that order. The table constant `10430.378350470453` is a double literal.
· verify-by: already executed. The table and its indexing matched the jar on every sampled argument. The table digest is in Q9.2.

**Q9.2** The table is built with the platform's sine, which Java does not pin bit-exactly. On the verified JVM it equals the single-precision rounding of the **strict** (correctly specified) sine at all 65 536 entries. Its SHA-256 digest, over the entries as big-endian IEEE-754 single-precision values in index order, is `87a67a39fff59636acb04edcda4ef622831037291f11fb61586c57596771dac2`. A platform can check its own table against that digest (REVIEW-10 §3 platform-math case).
· kind `constant` · confidence `verified(differential-harness)`
· verify-by: already executed (all 65 536 entries).

**Q9.3** Precision of walk state.
- **Double:** position; radii `h_i`, `v_i`, `ρ`, `ν`; ellipsoid normalized offsets `u`, `t`, `w`; the system origin.
- **Float:** heading, pitch and their velocities; thickness; room radius draw `r`; every provider value. The provider values are widened when used.
- **32-bit integer:** step index and length.

Every float→double conversion point is stated in its formula's `fp-form`. There is no double→float conversion anywhere except where marked `f32` and in the table entries.
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed (bitwise walk state, Q3.2).

---

## Q11 — Composition

**Q11.1** Order of starts. For a target chunk, start chunks are visited with `Sx − Tx` from `−8` to `8` as the **outer** order, and `Sz − Tz` from `−8` to `8` as the inner order. Within a start chunk, configured carvers run in carver-list order `k = 0, 1, …`. Each is seeded by its own `k` (Q1.1).

Order affects output only through the aquifer-flag history (Q12.4) and the shared mask (Q4.4). Implement it exactly. Mutants that transposed the start order, or reversed the list execution order (seeds unchanged), were **inert** at the sizes run: 64 chunks, measurement pack and q11 variant (Q11.6).
· kind `contract` · confidence `read-directly`. Implement exactly; a verifying mutant was inert at 64 chunks.
· verify-by: a larger mutant run (2 304 chunks, about 4 minutes) on a pack whose lists hold several carvers that reach the lava height.

**Q11.2** Which list applies. The carver list for start chunk `(Sx, Sz)` is the `carvers` list of the biome given by the dimension's **biome source itself**, sampled at quarter-resolution coordinates `(4·Sx, 0, 4·Sz)`, that is at block `(16·Sx, 0, 16·Sz)`. This is not the zoomed or jittered biome used for block-level queries. It is taken once per start chunk, regardless of which target is being carved.
· kind `contract` · confidence `verified(differential-harness)` for the horizontal position; `read-directly` for the quarter-y `0`.
· verify-by: already executed.
  - On a variant pack whose overworld biomes carry different lists, sampling at the chunk's middle instead changed 4 007 cells and 3 chunks' marks.
  - Sampling at quarter-y 16 instead of 0 was **inert** in 64 chunks, so the height is from reading. Implement it exactly. A site with varied cave biomes would discriminate it.

**Q11.3** List position `k` counts every entry of the list, including carvers of other types. A list `[cave, extra]` and a list `[extra, cave]` seed differently.
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed. The q11 variant included both orders and passed the carving-stage acceptance (0 fatal, 64 chunks).

**Q11.4** Shared state between carvers.
- **(a)** The per-target-chunk mask (Q4.4).
- **(b)** The aquifer's most-recent-decision flag (Q12.4).
- Nothing else. Each tunnel's generator is private (Q2.1), and the grass flag is per column per ellipsoid (Q12.3).

The mask's only observable effect is through (b): re-deciding an already-carved water cell (water is replaceable, Q12.2) would yield the same state and mark, but would refresh the flag history. Implement the mask exactly. A mutant resetting the mask for each start was inert at 64 chunks.
· kind `contract` · confidence `read-directly` for the effect argument; `verified(differential-harness)` for the mask bits (Q4.4).
· verify-by: as Q11.1.

**Q11.5** The Part A carvers are the types `minecraft:cave` (configured as `minecraft:cave` and `minecraft:cave_extra_underground`) and `minecraft:nether_cave`. Two configured carvers of the same type differ only in their pack fields. The shipped values are in `carvers-a-constants.json`.
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed (both overworld configured carvers in every overworld acceptance run).

**Q11.6** End-to-end acceptance at the carving stage. The real generation steps (biomes, noise fill with aquifer, surface, carving) were run on fresh chunks, against the same pre-carving state carved by an independent implementation of this spec. They were compared under the four-channel comparator with blocks read as stored, and no tolerated class:

| square | dimension | chunks | carved cells | differences |
|---|---|---|---|---|
| near spawn | overworld | 64 | 49 345 | 0 |
| near spawn | nether | 64 | 115 252 | 0 |
| 65+ chunks out | overworld | 2 304 | 1 447 417 | 0 |
| 65+ chunks out | nether | 2 304 | 3 616 496 | 0 |

The real pipeline's pre-carving state reproduced the server's saved worlds exactly on the near square. Its far-square differences from the saved worlds were all promotion effects of marked fluids (Q12.4; aquifer Q8.1).
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed (the table above).

---

## Q12 — Fill

**Q12.1** Overworld cave: what a decided cell becomes. Let `s` be the cell's current state and `y` its height.
- If `s` is not in `replaceable`, the cell is unchanged.
- Otherwise:
  - if `y ≤ y_lava`, the cell becomes a **lava source**. `y_lava` is `lava_level` resolved (Q13.3); shipped, `y_min + 8 = −56`. The aquifer is **not** consulted.
  - otherwise, the aquifer is consulted at the cell's position with density **`0.0`** (aquifer Q2.2–Q2.4 and Q6). Its answer is the new state: air, a water source, a lava source, or "solid".
  - On "solid" (the barrier verdict, aquifer Q6.6), the cell is **unchanged**.

The `lava_level` cut-off takes precedence over the aquifer.
· kind `formula` · confidence `verified(differential-harness)`
· widths: `y` and `y_lava` are 32-bit, compared as signed integers, non-strict.
· fp-form: the density argument is the double `0.0`. Aquifer arithmetic is as in `aquifer-spec.md`.
· verify-by: already executed. Water and lava outcomes, real noise-aquifer barriers, and the lava cut-off were all exercised. Driver scenarios S1–S2 plus the carving-stage acceptance: 0 differences.

**Q12.2** Replaceable blocks are the pack's `replaceable` block set. Shipped, these are `#minecraft:overworld_carver_replaceables` (`cave`, `cave_extra_underground`) and `#minecraft:nether_carver_replaceables` (`nether_cave`). They resolve in the vanilla data to 52 and 25 blocks; the lists are in the constants file.
- **Water** is replaceable in the overworld set, so an already-carved water cell is replaceable if decided again (Q11.4).
- **Air, cave air and lava** are not.
· kind `constant` · confidence `verified(differential-harness)`
· verify-by: already executed. The real pipeline bound these tags from the jar's own data, matching the resolved lists' sizes, and matched cell for cell.

**Q12.3** Overworld cave: the only change to an **undecided** cell.
- **The column flag.** Within one ellipsoid, each column carries a flag that starts false. It becomes true when a decided cell's **prior** state is `minecraft:grass_block` or `minecraft:mycelium`, whether or not that cell is then replaced. It stays true for the rest of that column in that ellipsoid, at lower `y`.
- **The rewrite.** After a decided cell is **replaced**, if the flag is true and the cell directly below currently holds `minecraft:dirt`, that cell is set to the surface rules' **top material** for its position. The top material is computed by the surface-rule system, outside this spec's scope, with "fluid above" set iff the replacement holds fluid. If the surface rules yield nothing, the cell stays dirt. If the top material holds fluid, its position is marked **unconditionally**, with no flag test (contrast Q12.4).

Nothing else changes a cell that is not itself decided.
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed. Disabling the rewrite changed 165 cells in 64 chunks. With it, 0 differences (Q11.6).

**Q12.4** Post-processing marks and their history dependence. After a cell is replaced with a state that holds fluid, it is **marked** iff the aquifer's flag (aquifer Q8.1–Q8.4) is true **as of the aquifer's most recent decision**. That is its most recent consultation in this target chunk's carving, in the order fixed by:
- Q11.1: start chunks, then list order;
- Q1.4: systems, room before tunnels;
- Q3 and Q6: steps; fork 1's whole walk before fork 2's;
- Q4.5: cells.

A lava-height cell (Q12.1) consults nothing, so its mark is decided by whichever decision came last. A barrier verdict is a decision.

The history starts at the noise fill's **last** decision, at local `(15, y_min, 15)`, which is in the bottom layer. Wherever that layer lies below the global lava level (aquifer Q1.2), which includes every shipped overworld preset, that decision returns early (aquifer Q2.2 or Q2.4) with its flag false (aquifer Q8.2). The inherited flag is then **false**.
· kind `contract` · confidence `verified(differential-harness)` for the order and marks; `inferred-from-reading` for the inherited flag. The inheritance inference is from the aquifer claims cited.
· verify-by: already executed.
  - Marks matched in all four acceptance squares, in all 4 736 chunks.
  - The within-ellipsoid order mutant changed marks in 11 of 35 chunks.
  - A prediction with a **fresh** aquifer (flag false) instead of the post-fill one was inert in 64 chunks, as the inference predicts.

**Q12.5** A mark's consequence at promotion is aquifer Q8.1 (as amended): the fluid's tick is **executed** when the chunk is promoted to full, so a stored full chunk differs from carving output next to every marked fluid that can flow. Carving itself places no ticks: block and fluid tick lists are unchanged by carving.
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed (tick channels were equal at the carving stage in all four acceptance squares; promotion per aquifer Q8.1).

**Q12.6** Nether cave fill.
- A decided cell whose state is in `replaceable` becomes a **lava source** if `y ≤ y_min + 31` (shipped: `31`), otherwise **cave air**.
- A non-replaceable cell is unchanged.
- The aquifer is never consulted and no cell is ever marked.
- There is no grass flag or top-material rewrite.
- The `lava_level` field is **not read** (Q13.2).
· kind `formula` · confidence `verified(differential-harness)`
· widths: `y_min + 31` is a 32-bit sum, compared with `y` as signed integers, non-strict.
· fp-form: none (no floating-point operation).
· verify-by: already executed (nether scenario of the driver; both nether acceptance squares; 0 marks in both).

**Q12.7** The carver writes **cave air**, and that is the carver's output. Separately, for the Implementer and outside carver scope: a chunk section that holds **only** air-like blocks (cave air counts as air-like) reads back as plain **air** at every cell through the game's own block read, although the section stores cave air. Carving can empty whole sections this way, in large nether cavities above the lava height. The stored state and the read state then differ.
· kind `contract` · confidence `verified(differential-harness)` for the recorded case; `read-directly` for the read rule.
· verify-by: already executed. In a 2 304-chunk nether square, 964 carved cells read as air while storing cave air, every one in a section holding no non-air block. A recording chunk showed the carver's write was cave air.

**Q12.8** Per-chunk state that makes a result depend on carving history: the mask (Q4.4), the aquifer flag (Q12.4), and per-column, per-ellipsoid, the grass flag (Q12.3). The cells' current states also matter, because an earlier carving changes what a later decision sees. All of these are confined to one target chunk's carving.
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed (Q11.6).

---

## Q13 — Configuration

**Q13.1** The fields shared by all carver types. "Placement" means it can change carved cells or marks.

| field | role | default | affects placement |
|---|---|---|---|
| `probability` | start test (Q1.3); float in `[0, 1]` | required | yes |
| `y` | height provider for system origins (Q1.4, Q1.5) | required | yes |
| `yScale` | float provider: the room's vertical scale (Q3.4) | required | yes |
| `lava_level` | vertical anchor: `cave` lava cut-off (Q12.1) | required | yes for `cave`; **no** for `nether_cave` (Q13.2) |
| `replaceable` | block set (Q12.2) | required | yes |
| `debug_settings` | debug states (Q13.4) | all defaults | only when debug is on (Q13.4) |

· kind `contract` · confidence `read-directly` for the roles; `verified(differential-harness)` for the shipped values' effects.
· verify-by: already executed (the shipped configurations drove the real pipeline in Q11.6).

**Q13.2** The fields specific to the cave types, shared by `cave` and `nether_cave`.

| field | role | default | affects placement |
|---|---|---|---|
| `horizontal_radius_multiplier` | float provider: tunnel ellipsoid horizontal radius (Q3.3); not applied to the room | required | yes |
| `vertical_radius_multiplier` | float provider: tunnel ellipsoid vertical radius (Q3.3); not applied to the room | required | yes |
| `floor_level` | float provider in `[−1, 1]`: the floor cut (Q4.3), room and tunnels | required | yes |

`nether_cave` never reads `lava_level`. Its lava height is fixed at `y_min + 31` (Q12.6).
· kind `contract` · confidence `read-directly` for the roles; `verified(differential-harness)` for the shipped values.
· verify-by: already executed (as Q13.1).

**Q13.3** Anchor resolution for `y` and `lava_level`, against the generation context (Notation):
- `absolute a` gives `a`;
- `above_bottom a` gives `y_min + a`;
- `below_top a` gives `y_min + H − 1 − a`.

Shipped:
- `cave`: `y` is uniform from `above_bottom 8` to `absolute 180`, so `[−56, 180]`; `lava_level` is `above_bottom 8`, so `−56`.
- `cave_extra_underground`: `y` is uniform from `[−56, 47]`; otherwise as `cave`.
- `nether_cave`: `y` is uniform from `absolute 0` to `below_top 1`, so `[0, 126]`.
· kind `formula` · confidence `verified(differential-harness)`
· widths: every resolution is a 32-bit signed sum or difference.
· fp-form: none (no floating-point operation).
· verify-by: already executed (system origins matched draw for draw, Q1.4).

**Q13.4** `debug_settings` holds `debug_mode` (default false), and `air_state`, `water_state`, `lava_state` and `barrier_state`. When the field is omitted entirely, the default states are a button, a candle, orange stained glass and glass. When only an individual state is omitted inside a present `debug_settings`, **each** of the four falls back to the **default air state**, a button, not to its own default.

With debug on, for the cave type:
- the mask is ignored, so cells are re-decided;
- replaceability is ignored;
- the aquifer's answers are replaced: air by `air_state`; water by `water_state` (waterlogged, if that state has the property); lava by `lava_state`. Cells at or below the lava height (Q12.1) still become plain lava;
- a barrier verdict places `barrier_state`.

For `nether_cave`, debug ignores only the mask.

No shipped configured carver sets `debug_mode`, so it is false. But `debug_mode` **is** a pack field, so a custom pack can turn it on.
· kind `open-question` · confidence `read-directly`. This is a live path, **not dead code**: a pack can enable it, though shipped packs do not. Whether Stratum supports pack-enabled carver debug output is the owner's decision (rule 17).
· verify-by: drive a pack with `debug_mode: true` and compare one target chunk.

> **Amendment to Q13.4: ruled, and verified** (2026-11-09, REVIEW-26 §2; run 05 Part B). The text above stands as first published. Ruling: parity applies, so pack-enabled carver debug output is reproduced. The open question is closed.
>
> Its verify-by was executed. Packs whose every shipped configured carver was inlined with `debug_mode: true` were driven through the real pipeline and compared with a reference implementing this claim's reading, with stored reads and no tolerated class, over 64 chunks per dimension:
> - **Overworld (`cave`, `cave_extra_underground`, `canyon`): 0 differences** over 106 902 decided-and-changed cells. Without debug, the same square changes 69 915 cells. Debug states and marks matched.
> - **Nether (`nether_cave`): 0 differences, and identical to the run without debug.** As read, nether debug only lifts the mask. A re-decided nether cell holds cave air or lava, which is not replaceable, so lifting the mask has no observable effect.
>
> · kind `contract` · confidence `verified(differential-harness)` for the cave types; `inferred-from-reading` for the nether's null effect (measured identical; the inference is stated above).
> · verify-by: already executed (above).

**Q13.5** Launch-time debug switches. Separately from the pack field, the game's carver debug, a whole-carver disable, and two void-terrain tests (which would suppress carving in some chunks) are read once at game start from JVM system properties. They are false unless those properties are set at launch. No pack, world or seed input reaches them.
· kind `contract` · confidence `verified(bytecode+harness)`. **DO-NOT-IMPLEMENT**. Reachability: only JVM launch properties, which are outside the pack, seed and position inputs of this spec.
· verify-by: already executed. The flags are not compile-time constants in the bytecode. Read from the unmodified jar, they are false under a default launch, and the carver debug flag is true with the two launch properties set.

**Q13.6** Upgraded chunks. In a chunk being upgraded from an older, shorter world format (one that carries below-zero retrogeneration data), the top margin in Q4.1 is `0` instead of `7`. In such chunks an additional carving filter near old chunks also applies. Neither occurs in freshly generated chunks.
· kind `open-question` · confidence `read-directly`. This is a live path, **not dead code**: it is reached when an old world is upgraded, and not otherwise. Whether Stratum supports world upgrading is the owner's decision.
· verify-by: upgrade a pre-extended-height world and compare one carved chunk.

---

## Open questions and deliberate omissions

- **Q13.4:** pack-enabled carver debug output. A live path, owner's decision.
- **Q13.6:** upgraded-chunk margin and the old-chunk filter. A live path, owner's decision.
- **Q11.1, Q11.2 (height), Q11.4:** implement exactly; the verifying mutants were inert at the sizes run (REVIEW-25 §3).
- **Omitted:** `minecraft:canyon` and Q10 (Part B). The surface-rule top material (Q12.3) and the aquifer's own internals are out of scope; they are cited, not derived.
