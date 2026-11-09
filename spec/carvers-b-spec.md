# Configured carvers, Part B (`canyon`) — clean-room specification (run 05)

- **Provenance:** see `PROVENANCE.md`, run-05 sections (the same 1.21.11 artifacts; digests re-verified at the start of every session, before any reading). Every claim is grounded in this run's reading of the recorded decompiled tree, the shipped bytecode, or differential measurement against the unmodified remapped jar. Nothing is from recollection.
- **Date:** 2026-11-09 · **Brief:** `RESEARCH-RUN-05.md` §3 question order; split per REVIEW-19 §1, with Part B begun under REVIEW-26 §3. Part B answers Q10, and the canyon's deltas to Q1, Q3, Q4, Q7 and Q13. It also carries the Q11 and Q13 pack-format and debug findings that the canyon runs produced.
- **Scope:** `minecraft:canyon`, from (world seed, target chunk, pack configuration) to the state of every cell it can affect, at the moment carving completes. Every shared mechanism is cited by its Part A claim id, written `A-Qn.m` (meaning `spec/carvers-a-spec.md` claim `Qn.m`), and is not restated. The aquifer is cited from `spec/aquifer-spec.md`.
- **Status:** hypothesis document, verified by default. `verify-by` reads "already executed" with counts where the experiment ran.
- **Context note:** the Implementer's model, measurements and lab design were withheld, and nothing here refers to them.

## Notation

Part A's notation applies throughout:

- `W`: the world seed.
- `(Tx, Tz)`, `X₀`, `Z₀`, `Xm`, `Zm`: the target chunk, its minimum block coordinates, and its middle.
- `(Sx, Sz)`: the start chunk.
- `k`: the list position.
- `y_min` and `H`: the generation floor and generation height.
- `F()`, `I(n)`, `Lg()`: the legacy generator's float, bounded-int and long draws.
- `S`, `C`: the table sine and cosine.
- `f32`: rounding to single precision.

New here:

- `T[·]`: a tunnel's **width table** (Q10.5).
- Pack field names of the canyon: `vertical_rotation`, `shape`, `distance_factor`, `thickness`, `width_smoothness`, `horizontal_radius_factor`, `vertical_radius_default_factor`, `vertical_radius_center_factor`. With the trapezoid provider's fields `min`, `max` and `plateau`, these are runtime data, and permitted.

---

## Q1 — Start (canyon deltas)

**Q1.1** Seeding and the start test are exactly A-Q1.1–A-Q1.3, with the canyon's own list position `k` and its `probability`. In every shipped overworld biome list the canyon is third, so `k = 2`.
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed. The real canyon's start test agreed with the reference at every start of the bitwise runs (Q10.9). The carving-stage acceptance agreed at every start (Q10.10).

**Q1.2** A passing canyon start makes **one tunnel**. It has no system count, no room, no forks, no radius multipliers and no `floor_level`. Its draw sequence from the re-seeded start generator, in order:
- `x = 16·Sx + I(16)`;
- `y` from the `y` provider (A-Q1.5), as an integer converted exactly to double;
- `z = 16·Sz + I(16)`;
- heading `θ = F()·6.2831855` (float);
- initial pitch `φ` from `vertical_rotation` (a float provider);
- `ys` from `yScale` (a float provider, widened to double);
- thickness `τ` from `shape.thickness` (a float provider; shipped, a trapezoid, Q1.3);
- length `L = trunc₃₂(f32(112·d))`, with `d` from `shape.distance_factor`;
- the tunnel seed `Lg()`.
· kind `formula` · confidence `verified(differential-harness)`
· widths: `16·Sx + I(16)` and `16·Sz + I(16)` are 32-bit, converted exactly to double. `trunc₃₂` converts the single-precision product to a 32-bit signed integer, toward zero. Shipped, `L ∈ [84, 112]`. `112` is reached because `d` itself can round up to exactly `1.0` (A-Q1.5 as amended; 2 of 2²⁴ draws).
· fp-form: `θ`, `φ` and `τ` are single precision. `112·d` is a single-precision product: `112` converts exactly to float, then is multiplied by the float `d`, then truncated.
· verify-by: already executed. Bitwise equality of every cross-section call, 4 382 calls on the near square plus the far-square run (Q10.9), would fail on any misaligned draw. A mutant swapping two per-step draws produced 8 764 bitwise mismatches.

**Q1.3** The trapezoid float provider. Define `span = max − min`, `slope = (span − plateau)/2` and `flat = span − slope`. Its value is `min + F()·flat + F()·slope`: **two** draws, summed left to right. Shipped canyon thickness (`min 0`, `max 6`, `plateau 2`) gives `0 + F()·4 + F()·2`.
· kind `formula` · confidence `verified(differential-harness)`
· widths: none (no integer operation).
· fp-form: single precision throughout, in the written order: `span`, then `slope` (the division by `2` is single precision), then `flat`, then `(min + F()·flat) + F()·slope`.
· verify-by: already executed. Bitwise radii depend on `τ` at every step (Q10.9).

---

## Q3 — The walk (canyon deltas)

**Q3.1** The canyon's tunnel walks with its own generator `g`, seeded with the tunnel seed (A-Q2.1). Before any step, `g` builds the width table (Q10.5). There is no fork index and no steepness draw. Heading and pitch velocities start at `0`. Steps run `i = 0, …, L − 1`.
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed (Q10.9).

**Q3.2** One canyon step. Every quantity on the right-hand side is the value before this step, unless it is marked as updated. The draws are taken in this order: the horizontal factor's draw(s), the vertical factor's draw, the six velocity-kick draws (all `δφ` draws before any `δθ` draw), then `I(4)`.
- **Base radius:** `h⁰ = 1.5 + f32(S(f32(f32(i·3.1415927)/L))·τ)`. The vertical base is `v⁰ = h⁰·ys`.
- **Horizontal radius:** `h = h⁰·hf`, with `hf` drawn from `shape.horizontal_radius_factor`.
- **Vertical radius:** `v = (m·v⁰)·q`, where:
  - `c = f32(1 − f32(|f32(0.5 − f32(i/L))|·2))`, with `i` and `L` converted to float;
  - `m = f32(vertical_radius_default_factor + f32(vertical_radius_center_factor·c))`;
  - `q = f32(f32(F()·0.25) + 0.75)`.

  Shipped (`1.0` and `0.0`), `m = 1` exactly.
- **Position:** with `cp = C(φ)` and `sp = S(φ)`: `x += f32(C(θ)·cp)`, `y += sp`, `z += f32(S(θ)·cp)`.
- **Pitch and heading:** `φ ← f32(f32(φ·0.7) + f32(δφ·0.05))`; then `θ ← f32(θ + f32(δθ·0.05))`.
- **Velocities, decayed:** `δφ ← f32(δφ·0.8)`, then `δθ ← f32(δθ·0.5)`.
- **Velocities, kicked:** `δφ ← f32(δφ + f32(f32(f32(F() − F())·F())·2))`, then `δθ ← f32(δθ + f32(f32(f32(F() − F())·F())·4))`.
- **Carve check:** `I(4) ≠ 0`; then the reach test (Q7.1); then an ellipsoid with radii `(h, v)` at the updated `(x, y, z)` (Q4.1).

There is no steep flag: the pitch damping is always `0.7`.
· kind `formula` · confidence `verified(differential-harness)`
· widths: `i` and `L` are 32-bit, each converted exactly to float. `i ≤ 111`.
· fp-form:
  - The table-sine argument is single precision: `f32(i·3.1415927)`, then divided by `L`. `3.1415927` is π rounded to float.
  - `S(·)·τ` is a single-precision product, widened exactly and added to the double `1.5`. `h⁰·ys` is double.
  - `h⁰·hf` is double, with `hf` widened.
  - `m·v⁰` is double, with `m` widened, and is then multiplied in double by `q` widened. Evaluate the product as `(m·v⁰)·q`, in that order.
  - `c`, `m` and `q` are single precision. The literals `0.5`, `2`, `0.25`, `0.75`, `0.7`, `0.05`, `0.8` and `4` are float.
  - The position increments are single-precision products, widened and added in double.
· verify-by: already executed (all five cross-section arguments, bit for bit, Q10.9).

---

## Q4 — Cross-section (canyon deltas)

**Q4.1** The canyon's ellipsoid uses A-Q4.1 (early-out, bounds, margins), the column test A-Q4.2, the mask A-Q4.4 and the decision order A-Q4.5 unchanged. Only the **cell test** differs, and there is **no floor cut**. For a considered column and each `y` in A-Q4.1's range, with `u`, `w` as in A-Q4.2 and `t = (y − 0.5 − cy)/ν`, the cell is excluded iff `(u² + w²)·T[y − y_min − 1] + t²/6 ≥ 1`.
· kind `formula` · confidence `verified(differential-harness)`
· widths: `y − y_min − 1` is a 32-bit signed difference used as the table index. It is always within `[1, H − 9]`, given A-Q4.1's range.
· fp-form: double precision: `((u·u + w·w)·T) + (t·t)/6`, in that order. `T[·]` is a float table entry widened exactly. `6` is the double literal `6.0`.
· verify-by: already executed. Cells matched at the carving stage (Q10.10). A mutant indexing the table at `y − y_min` instead changed 1 541 cells and 8 chunks' marks on the near square.

---

## Q7 — Termination and reach (canyon deltas)

**Q7.1** The canyon uses A-Q7.1's reach test unchanged, with its own `τ`, `L` and `i`, at its carve checks only. A tunnel ends at its last step or at a failed reach test. There are no forks, so A-Q7.2's fork ending does not apply. The distance bound is A-Q7.3 (the ±8-chunk start window).
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed (Q10.9, Q10.10).

---

## Q10 — Canyon

**Q10.1** The walk is Q1.2 and Q3.1–Q3.2. The cross-section is Q4.1. Reach and termination are Q7.1. Fill is Q10.8.
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed (Q10.9, Q10.10).

**Q10.2** The role of each `shape` field:
- `distance_factor` sets the tunnel length (Q1.2).
- `thickness` sets `τ`, the amplitude of the sine radius profile (Q3.2).
- `width_smoothness` sets how often the width table changes value (Q10.5).
- `horizontal_radius_factor` is a per-step random multiplier on the horizontal radius (Q3.2).
- `vertical_radius_default_factor` and `vertical_radius_center_factor` form the vertical multiplier `m = default + center·c`. Here `c` is a triangle profile over the tunnel: `0` at the ends and `1` at the middle (Q3.2).
· kind `contract` · confidence `verified(differential-harness)` for the shipped values; `read-directly` for the roles of `vertical_radius_center_factor` at values other than `0`.
· verify-by: already executed for the shipped values. For a non-zero centre factor, a driver run on a pack with that change is a cheap probe.

**Q10.3** `vertical_rotation` is the provider of the tunnel's **initial pitch** `φ` (Q1.2). Shipped, it is uniform on `[−0.125, 0.125)`, one draw.
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed (Q10.9).

**Q10.4** `yScale`, for the canyon, scales the tunnel's vertical base radius at **every** step: `v⁰ = h⁰·ys` (Q3.2). For the cave types it scales only the room (A-Q3.4). Shipped, the canyon's is the constant `3.0`, with no draw.
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed (Q10.9).

**Q10.5** The width table `T` has `H` single-precision entries, one per generation layer. It is drawn from the tunnel's generator `g` before the first step, as a pure function of the tunnel seed:
- `w₀ = 1 + f32(F()·F())`;
- for `j ≥ 1`, `g` draws `I(width_smoothness)`. If that is `0`, `w_j = 1 + f32(F()·F())` (two fresh draws). Otherwise `w_j = w_{j−1}`, with no further draw.
- Then `T[j] = f32(w_j·w_j)`.

So the table is piecewise constant over runs of layers, and each layer after the first starts a new run with probability `1/width_smoothness`. Each run's value lies in `[1, 4)`. Layer `j` corresponds to height `y = y_min + 1 + j` (Q4.1).
· kind `formula` · confidence `verified(differential-harness)`
· widths: `j` and `width_smoothness` are 32-bit. `width_smoothness` is a positive integer field.
· fp-form: single precision: `F()·F()`, then `1 +`, then the square.
· verify-by: already executed. The real carver's table and the reference's were compared bit for bit, entry for entry, for every canyon tunnel in the bitwise runs (Q10.9).

**Q10.6** The complete canyon draw sequence. From the start generator:
- the start test `F()`;
- `x`: `I(16)`;
- `y`: one `I(·)` (A-Q1.5);
- `z`: `I(16)`;
- `θ`: `F()`;
- `vertical_rotation`: one `F()` if uniform;
- `yScale`: none if constant;
- `thickness`: two `F()` if trapezoid;
- `distance_factor`: one `F()` if uniform;
- the tunnel seed `Lg()`.

From the tunnel generator: the width table (Q10.5); then per step, in Q3.2's order, `horizontal_radius_factor` (one `F()` if uniform), the vertical factor `F()`, six kick draws, and `I(4)`. A reach-test failure ends all further draws for that target.
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed (Q10.9: any misaligned draw changes the next bitwise argument).

**Q10.7** Numerics: A-Q9.1–A-Q9.3 apply, adding:
- the float width table;
- the float `c`, `m` and `q` of Q3.2;
- the float-to-integer truncation of `L` (Q1.2).

There is no other conversion point.
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed (Q10.9).

**Q10.8** Fill. The canyon decides cells exactly as the overworld cave does: A-Q12.1 (the `lava_level` cut-off, then the aquifer at density `0.0`, with barrier verdicts leaving the cell unchanged); A-Q12.2 (replaceable); A-Q12.3 (the grass and mycelium column flag and the dirt top-material rewrite); and A-Q12.4–A-Q12.5 (marks and their history, in the order of A-Q11.1 with the canyon at its list position). Its shipped `replaceable` is the overworld set, and its `lava_level` is `above_bottom 8`.
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed (Q10.10: cells and marks with canyons present, 0 differences).

**Q10.9** Bit-level verification. The unmodified canyon carver ran through a recording subclass, which records the cross-section arguments and then calls through unchanged (REVIEW-19 §2). Its five arguments at every call were compared bit for bit with an independent reference, and its width table was compared entry for entry, on the near and far squares:
- 7 991 canyon starts;
- 101 822 cross-section calls;
- **3 577 654 comparisons, 0 mismatches**.

Non-interference was measured: cells and mask of every target, with the recording subclass against the unmodified class, **0 differences** over 232 783 872 compared cell and mask positions.
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed (counts above). The test is sensitive: a reference mutant swapping two per-step draws produced 8 764 mismatches.

**Q10.10** End-to-end acceptance at the carving stage, by the method of A-Q11.6, with the canyon present. The pack keeps every shipped carver. Both sides were read as stored, rule 9 was unamended and no class was tolerated. Results:

| square | dimension | chunks | changed cells | differences |
|---|---|---|---|---|
| near spawn | overworld | 64 | 69 915 | 0 |
| near spawn | nether | 64 | 115 252 | 0 |
| 65+ chunks out | overworld | 2 304 | 1 676 769 | 0 |
| 65+ chunks out | nether | 2 304 | 3 616 496 | 0 |
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed (the table above).

---

## Q11 — Composition (pack-format finding)

**Q11.1** A biome's `carvers` field may be **either** a single configured-carver reference, written as a bare id string, or a list. A bare string is exactly a one-element list: `k = 0`. The shipped nether biomes use the bare-string form. Entries may also be inline configured-carver definitions instead of ids. An inline entry takes the list position it occupies, like any other.
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed. The shipped nether lists (bare strings) passed the nether acceptance squares. Inline definitions passed the debug-pack check (Q13.2).

---

## Q13 — Configuration (canyon deltas, and the debug check)

**Q13.1** The canyon's fields: the shared fields of A-Q13.1 (with `yScale` in the role of Q10.4), plus the following.

| field | role | default | affects placement |
|---|---|---|---|
| `vertical_rotation` | float provider: initial pitch (Q10.3) | required | yes |
| `shape.distance_factor` | float provider: length (Q1.2) | required | yes |
| `shape.thickness` | float provider: `τ` (Q1.2) | required | yes |
| `shape.width_smoothness` | positive integer: width-table run rate (Q10.5) | required | yes |
| `shape.horizontal_radius_factor` | float provider: per-step horizontal multiplier (Q3.2) | required | yes |
| `shape.vertical_radius_default_factor` | float: vertical multiplier at the tunnel ends (Q3.2) | required | yes |
| `shape.vertical_radius_center_factor` | float: added vertical multiplier at the tunnel middle (Q3.2) | required | yes |

The canyon has no `horizontal_radius_multiplier`, `vertical_radius_multiplier` or `floor_level`. Its `lava_level` is read (Q10.8). Shipped values are in `carvers-b-constants.json`.
· kind `contract` · confidence `read-directly` for the roles; `verified(differential-harness)` for the shipped values.
· verify-by: already executed for the shipped values (Q10.9, Q10.10).

**Q13.2** The debug check of REVIEW-26 §2, which supplies A-Q13.4's verify-by. Packs whose every configured carver (cave, cave extra underground and canyon in the overworld; nether cave in the nether) is the shipped definition, inlined with `debug_mode` set to true, were driven through the real pipeline. The results were compared with a reference implementing A-Q13.4's reading. The canyon uses the cave type's debug behaviour (A-Q13.4).
- **Overworld, 64 chunks: 0 differences** over 106 902 changed cells (69 915 without debug). Debug states and marks matched.
- **Nether, 64 chunks: 0 differences, identical to the run without debug.** As A-Q13.4's amendment explains, nether debug only lifts the mask, which has no observable effect there.
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed (above).

---

## Open questions and deliberate omissions

- **None new.** Part A's Q13.4 is ruled by REVIEW-26 §2 (parity) and amended in place with this verification. Q13.6 is ruled out of Stratum's scope, and stays as written.
- **Omitted:** the surface-rule top material and the aquifer's internals, which are cited, not derived.
