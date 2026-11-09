# Named noises in legacy-random-source dimensions — clean-room specification (run 04)

- **Provenance:** see `PROVENANCE.md`, run-04 section. The 1.21.11 artifacts and the per-file tree gate were re-derived and passed in this session before any reading.
- **Grounding:** every claim rests on one of the following, never on recollection:
  - this session's reading of the recorded tree;
  - the shipped bytecode;
  - bit-exact differential measurement against the unmodified remapped jar, using an external driver.
- **Date:** 2026-11-09
- **Brief:** `RESEARCH-RUN-04.md`, in its §3 question order.
- **Scope:** how a dimension whose noise settings set `legacy_random_source: true` obtains a named noise (a `worldgen/noise` registry entry referenced by resource location), and how that noise's value is produced and read. Terrain density, climate, blended noise and the aquifer stay out of scope (brief §1). Where those were touched, it was only to place them.
- **Status:** a hypothesis document under CLAUDE.md §4. Every `verified` claim names the harness section that exercised it. Harness totals: 843 535 bit-exact comparisons, 0 mismatches. Seeds and positions were chosen from this run's own reading only.
- **Context note:** none of the Implementer's material was requested, read or reasoned about (brief §2).

## Notation

- `W`: the world seed, a signed 64-bit integer.
- `⊕`: bitwise XOR on 64-bit two's-complement integers. Arithmetic marked "mod 2⁶⁴" wraps.
- `sx(v)`: the sign extension of a signed 32-bit value `v` to 64 bits.
- `h(s)`: the 32-bit string hash. For `s` with UTF-16 code units `c₀ … c_{m−1}`, `h(s) = Σ cᵢ·31^{m−1−i}` mod 2³², read as a signed 32-bit value.
- `R₄₈(σ)`: a generator with a 48-bit state, defined as follows.
  - Seeding: `s₀ = (σ ⊕ 25214903917) mod 2⁴⁸`.
  - Step: `s ← (s·25214903917 + 11) mod 2⁴⁸`.
  - A **k-bit draw** `b(k)`, for 1 ≤ k ≤ 32, steps once and yields `⌊s/2^{48−k}⌋`, read as a signed 32-bit value. So `b(32)` can be negative, and `b(k)` for `k < 32` cannot.
- Derived draws, each consuming the bit draws shown, in order:
  - `L` (64-bit): `b(32)·2³² + sx(b(32))` mod 2⁶⁴. The first bit draw is the high word. The second is **sign-extended and added**, not inserted bitwise.
  - `D` (double in [0,1)): `(b(26)·2²⁷ + b(27)) · 2⁻⁵³`, with one double-precision multiply by exactly `2⁻⁵³`.
  - `I(n)` (integer in [0,n), for n ≥ 1):
    - if `n` is a power of two, `⌊n·b(31) / 2³¹⌋`, using one `b(31)`;
    - otherwise, draw `v = b(31)` and set `r = v mod n`. Redraw while `v − r + n − 1 > 2³¹ − 1`, and yield `r`.
- `k_i`: the absolute octave number of amplitude-list index `i`: `k_i = firstOctave + i`.
- Registry keys (`minecraft:gravel_layer`, …), salt strings (`octave_<k>`) and pack field names (`firstOctave`, `amplitudes`, `legacy_random_source`, `min_threshold`, `max_threshold`, `add_surface_depth`, `secondary_depth_range`, `surface_depth_multiplier`, `offset`) are runtime data and are permitted.

---

## Q1 — Construction

**Q1.1** In a legacy dimension, every named noise requested through the surface-rule system is driven only by the 48-bit `R₄₈` family of the notation section. This covers the 13 brief-named keys, and any other key the same paths request (Q5.1). No other generator family takes part.
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed. Harness A rebuilt every octave layer's offsets and permutation table for all 13 names, in both stacks, across 3 settings × 20 seeds: 677 440 comparisons, 0 mismatches. Harness B compared values (Q2) over the same matrix at 198 positions: 79 200 comparisons, 0 mismatches.

**Q1.2** **Dimension factory seed.** `F(W) = L`, the first 64-bit draw of `R₄₈(W)`. `F` is fixed for the dimension's lifetime. No named-noise derivation advances or changes it.
· kind `formula` · confidence `verified(differential-harness)`
· verify-by: already executed (harness A/B). Every named noise reproduces only if `F` is exactly this value. The golden table in the constants file records `F` for six seeds.

**Q1.3** **Per-noise seed. The resource location is a hash salt.** For a named noise with key `id`, where `id` is the full resource-location text `namespace:path` (for example `minecraft:gravel_layer`), the noise's own generator is `R₄₈(σ_id)` with `σ_id = sx(h(id)) ⊕ F(W)`. The resource location plays no other role (it is not a lookup into built-in values; see Q2.1). The salt is the **full text, including the namespace and the colon**.
· kind `formula` · confidence `verified(differential-harness)`
· verify-by: already executed (harness A/B). Sensitivity check B2: a reference that zero-extends `h` instead of sign-extending it disagrees with the jar on 2 000 of 5 200 samples. That is exactly the samples belonging to the 5 names whose `h(id)` is negative (Q1.8).

**Q1.4** **Independent generators, with no shared stream between noises.** Each named noise's generator is a function of `F(W)` and its own `id` only. Consequences:
- request order does not affect any noise;
- neither does the time of first request;
- nor which other noises are requested;
- nor which consumers (surface rule, surface system) request a key.

A key requested by several consumers denotes one and the same noise.
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed.
- Harness C requested all 13 names in forward order in one dimension state and in reverse order in another, across 3 settings × 3 seeds: 3 528 comparisons, 0 mismatches.
- C also confirmed that the surface-depth consumer and a surface-rule request of `minecraft:surface` receive the identical noise.
- Harness J: identical bits across all three settings (Q3.1).

**Q1.5** **Within one noise: two octave stacks from one short stream.** `R₄₈(σ_id)` yields exactly two 64-bit draws `P₁ = L` and then `P₂ = L` (four 32-bit bit draws in all), and nothing else is ever drawn from it. `P₁` seeds the octave layers of stack 1, and `P₂` those of stack 2 (Q2.2).
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed. Harness A matched both stacks' layers separately. Swapping or sharing `P₁`/`P₂` would desynchronize every layer.

**Q1.6** **Per-octave layer seed.** For amplitude-list index `i` with `amplitudes[i] ≠ 0`, stack `j`'s layer `i` has its own generator `R₄₈(sx(h("octave_" + dec(k_i))) ⊕ P_j)`.
- `dec(k)` is the base-10 numeral of the absolute octave number `k_i = firstOctave + i`: a leading `-` for negative values, no `+` sign, and no padding. Examples: `octave_-8`, `octave_0`, `octave_1`.
- The salt uses the absolute octave number, not the list index `i`.
- A zero-amplitude index has **no layer and consumes nothing**. No stream runs across layers, so there is nothing to skip.

· kind `formula` · confidence `verified(differential-harness)`
· verify-by: already executed. Harness A checked the null-layer pattern against the zero-amplitude pattern, and every present layer's state. Harness G repeated this under redefined parameters with interior zeros and positive octaves (Q3.3).

> **Width amendment** (2026-11-09, REVIEW-14 §1). The text above stands as first published; this block adds integer widths and rounding semantics only.
>
> The absolute octave number `k_i = firstOctave + i` is a **32-bit signed sum with wraparound**. `firstOctave` is an unbounded pack integer, so for `firstOctave` near `2³¹ − 1` the salt of a later index uses the wrapped value, for example `octave_-2147483648`. A 64-bit (non-wrapping) octave number agrees exactly when `firstOctave + i` stays within the 32-bit range.
>
> · kind `formula` · confidence `verified(bytecode+harness)`
> · verify-by: already executed. The 32-bit add was read in the disassembly. The jar's layers were compared for `firstOctave ∈ {2³¹ − 1, 2³¹ − 2, −2³¹, −8, 0}` against layers seeded from the wrapped salt: 45 comparisons, 0 mismatches. The 64-bit mutant disagreed on 9 of 9 overflowing indices.

**Q1.7** **Layer state from its generator.** Each layer's draws come in this order:
- three `D` draws, giving offsets `xo = 256·D`, `yo = 256·D` and `zo = 256·D`;
- 256 integer draws that permute a table `π`, which starts as the identity `π[t] = t` for `t ∈ [0,255]`. For `t = 0, 1, …, 255` in ascending order, the draw is `j_t = I(256 − t)`, then `π[t]` and `π[t + j_t]` are exchanged.

Each layer consumes 6 bit draws for the offsets, plus the bit draws of its 256 integer draws.
· kind `formula` · confidence `verified(differential-harness)`
· verify-by: already executed (harness A). Exact equality of all 256 table entries and all three offsets, for every layer.

> **Width amendment** (2026-11-09, REVIEW-14 §1). The text above stands as first published; this block adds integer widths and rounding semantics only.
>
> `256 − t` and `t + j_t` are 32-bit signed integers in `[0, 255]`. No overflow is possible.
>
> · kind `formula` · confidence `verified(bytecode)`
> · verify-by: already executed — read in the disassembly.

**Q1.8** **Bit-width behavior.** Each of the following is load-bearing:
- **(a) Only `W mod 2⁴⁸` matters.** Seeding discards the top 16 bits of `W`, so two world seeds that differ only in bits 48–63 give bit-identical named noises in a legacy dimension.
- **(b)** `h(id)` is sign-extended before the XOR. When `h(id) < 0`, bits 32–47 of the result are complemented, and they survive seeding. Among the 13 names, `h(id)` is negative for `minecraft:gravel_layer`, `minecraft:nether_state_selector`, `minecraft:patch`, `minecraft:calcite` and `minecraft:gravel`. Every octave salt `octave_<k>` for `k ∈ [−64, 32]` has a positive hash.
- **(c)** The low word of `L` is sign-extended and added. This propagates a borrow into the high word whenever the low draw is negative.
- **(d)** `D` uses a double-precision multiply by exactly `2⁻⁵³`. A single-precision rendering of that constant or of the multiply is a mis-derivation.

· kind `contract` · confidence `verified(bytecode+harness)`
· verify-by: already executed.
- (b), (c) and (d) were each confirmed in the disassembly (sign-extension before XOR, an add rather than an OR, and a double constant equal to 2⁻⁵³).
- Each was then confirmed by a mutant reference in B2, which disagrees with the jar on 2 000/5 200, 4 340/5 200 and 5 200/5 200 samples respectively.
- (a): harness D compared seed pairs `W` and `W ⊕ (m·2⁴⁸)` for m ∈ {1, 0x7FFF, −1}. All 18 720 legacy comparisons were bit-identical. The control, the same pairs in the non-legacy `minecraft:overworld` settings, differed on 420/420.
- The seed set deliberately included 0, ±1, the 64-bit extremes, `2⁴⁷`, `2⁴⁸`, `2⁴⁸ − 1`, the values at which the seeding XOR yields a zero state, and the 32-bit sign boundaries.

**Q1.9** **Mis-derivation warning.** Legacy dimensions do **not** build their named noises with the sequential, shared-stream octave construction that the blended-noise mechanism uses (blended-noise Q4.1–Q4.5). In particular:
- there is no stream running across octaves;
- there is no skipped-layer consumption;
- positive octaves are not restricted (Q3.3).

The "legacy" setting changes the generator family and its salt combination only. The construction structure is the same one non-legacy dimensions use: a per-noise salt, two stacks, and per-octave salts. That structure under the non-legacy generator family is **not** specified here.
· kind `contract` · confidence `verified(differential-harness)` (the legacy half); `read-directly` (that the structure is shared with non-legacy dimensions)
· verify-by: the legacy half was already executed (harness A, G). The shared-structure statement is not measured: the non-legacy generator family is out of this run's scope, and the overworld control in D shows only that its output differs.

---

## Q2 — Shape

**Q2.1** **The pack's octave parameters apply, and no built-in values exist for these names.** For each of the 13 keys, the noise uses the `firstOctave` and `amplitudes` of that key's registry entry. The shipped vanilla values (runtime data) are:

| key | `firstOctave` | `amplitudes` |
|---|---|---|
| `minecraft:gravel_layer` | −8 | [1, 1, 1, 1, 0, 0, 0, 0, 0.013333333333333334] |
| `minecraft:nether_state_selector` | −4 | [1] |
| `minecraft:nether_wart` | −3 | [1, 0, 0, 0.9] |
| `minecraft:netherrack` | −3 | [1, 0, 0, 0.35] |
| `minecraft:patch` | −5 | [1, 0, 0, 0, 0, 0.013333333333333334] |
| `minecraft:soul_sand_layer` | −8 | [1, 1, 1, 1, 0, 0, 0, 0, 0.013333333333333334] |
| `minecraft:calcite` | −9 | [1, 1, 1, 1] |
| `minecraft:gravel` | −8 | [1, 1, 1, 1] |
| `minecraft:ice` | −4 | [1, 1, 1, 1] |
| `minecraft:packed_ice` | −7 | [1, 1, 1, 1] |
| `minecraft:powder_snow` | −6 | [1, 1, 1, 1] |
| `minecraft:surface` | −6 | [1, 1, 1] |
| `minecraft:surface_swamp` | −2 | [1] |

Built-in substitution happens only for three keys, and only when they are referenced from the terrain/climate router (Q5.1). None of the 13 is among them.
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed.
- Harness B drove the jar with parameters decoded, by the jar's own codec, from the shipped data files.
- Harness 0 found those values equal to the jar's built-in registry bootstrap for all 60 noise entries.
- Harness G showed that redefined values govern (Q3.2).

**Q2.2** **Composition: two stacks, summed and scaled.**
`N(x, y, z) = (S₁(x, y, z) + S₂(c·x, c·y, c·z)) · V`, where `c = 1.0181268882175227`. `S₁` and `S₂` are built from the same `firstOctave`/`amplitudes`, with layers seeded per Q1.5–Q1.6. Stack 2's input is the coordinate triple scaled by `c`, and nothing else differs.
· kind `formula` · confidence `verified(differential-harness)`
· verify-by: already executed (harness B, 79 200 values, bit-exact).

**Q2.3** **Value scaling.** `V = 0.16666666666666666 / (0.1 · (1 + 1/(Δ + 1)))`.
- `Δ` is the index of the last non-zero amplitude minus the index of the first non-zero amplitude.
- `1/(Δ+1)` is a real (floating-point) division of 1 by the integer `Δ + 1`.
- Leading and trailing zeros in the list do not affect `Δ`. Interior zeros do.
- `V` for the shipped values is listed per name in the constants file.

When no amplitude is non-zero, including an empty list, the noise is identically `0.0` (Q3.3), and `V` is unobservable.
· kind `formula` · confidence `verified(differential-harness)`
· verify-by: already executed (harness B; harness G with interior zeros, an all-zero list and an empty list).

> **Width amendment** (2026-11-09, REVIEW-14 §1). The text above stands as first published; this block adds integer widths and rounding semantics only.
>
> `Δ` and `Δ + 1` are 32-bit signed integers, bounded by the list length. No overflow is reachable when at least one amplitude is non-zero.
>
> · kind `formula` · confidence `verified(bytecode)`
> · verify-by: already executed — read in the disassembly.

**Q2.4** **One octave stack.** For a list of length `n`, zeros included:

`S(x, y, z) = Σ_{i ∈ [0,n), aᵢ ≠ 0} (aᵢ · Oᵢ(w(x·2^{k_i}), w(y·2^{k_i}), w(z·2^{k_i}))) · (λ · 2^{−i})`, where `λ = 2^{n−1} / (2^n − 1)` and `aᵢ = amplitudes[i]`.

- The input factor `2^{k_i}` and the weight `λ·2^{−i}` are each exact powers of two times `λ`.
- The finest octave has the largest `i`, and receives the smallest weight.
- **For bit-exact results:** each term is formed as `(aᵢ · Oᵢ) · (λ·2^{−i})`, and the terms are accumulated in ascending `i`, starting from `0.0`.

`n` counts zero entries, so trailing zeros change `λ` and therefore every term.
· kind `formula` · confidence `verified(differential-harness)`
· verify-by: already executed (harness B; harness G with a trailing-zero list).

> **Width amendment** (2026-11-09, REVIEW-14 §1). The text above stands as first published; this block adds integer widths and rounding semantics only.
>
> - `n` and `i` are 32-bit integers.
> - The input factor of index `i` is `2^{firstOctave}` (a double-precision power of two) times `2^i`, applied as successive exact doublings in double precision. The weight is `λ` times `2⁻ⁱ`, applied as successive exact halvings.
> - For `firstOctave ≥ −1074` the doublings are exact and equal `2^{k_i}`.
> - **For `firstOctave < −1074`, `2^{firstOctave}` is `0` in double precision, so every input factor is `0`** and each layer samples its lattice at `(0, 0, 0)`. A per-index power `2^{k_i}` would differ there. Only a pack with `firstOctave < −1074` reaches this, and the effect on the sum is at subnormal magnitudes unless the list is longer than about 1 000 entries.
>
> · kind `formula` · confidence `verified(bytecode)`
> · verify-by: already executed — the power-of-two base and the per-index doubling read in the disassembly. The normal range was verified bit-exact (harness B of run 04). The underflow edge was not driven. Cheapest confirmation: one redefinition with `firstOctave = −1100` and 1 100 amplitudes, about 10 minutes.

**Q2.5** **Coordinate wrap.** `w(v) = v − ⌊v / 33554432 + 0.5⌋ · 33554432`. It uses a 64-bit floor, applies to all three coordinates of every octave, and is applied after the octave's input factor.
· kind `formula` · confidence `verified(differential-harness)`
· verify-by: already executed. Harness B includes positions at ±16 777 216, ±29 999 999 and 32 954 048 (just below 2²⁵ / c), and random positions up to ±3·10⁷, where the wrap is active for octaves with `k_i ≥ 0` and in stack 2.

**Q2.6** **Octave layer function** `O(x, y, z)` for a layer with offsets `(xo, yo, zo)` and table `π`:
- **Lattice cell:** let `X = x + xo`, `Y = y + yo` and `Z = z + zo`, with lattice corners `(Xᵢ, Yᵢ, Zᵢ) = (⌊X⌋, ⌊Y⌋, ⌊Z⌋)` (a 32-bit floor) and fractions `(u, v, t) = (X − Xᵢ, Y − Yᵢ, Z − Zᵢ)`.
- **Table lookup:** `P(m) = π[m mod 256]`, where `mod` is the non-negative residue.
- **Corner hashes:**
  - `A = P(Xᵢ)` and `B = P(Xᵢ + 1)`;
  - `AA = P(A + Yᵢ)`, `AB = P(A + Yᵢ + 1)`, `BA = P(B + Yᵢ)` and `BB = P(B + Yᵢ + 1)`;
  - corner `(dx, dy, dz) ∈ {0,1}³` uses hash `P(H + Zᵢ + dz)`, where `H` is `AA, BA, AB, BB` for `(dx, dy) = (0,0), (1,0), (0,1), (1,1)`.
- **Corner values:** each corner's value is `g·(u − dx, v − dy, t − dz)`, with `g = G[hash mod 16]` from the gradient table of the constants file, evaluated as `g₀·p + g₁·q + g₂·r` left to right.
- **Fade:** `f(s) = s³(s(6s − 15) + 10)`.
- **Interpolation:** with `lerp(s, a, b) = a + s·(b − a)`, blend the corners first along x (weight `f(u)`), then y (weight `f(v)`), then z (weight `f(t)`).

· kind `formula` · confidence `verified(differential-harness)`
· verify-by: already executed (harness B). The reference implements exactly this and matches 79 200 values bit for bit.

> **Floating-point amendment** (2026-11-09, REVIEW-15 §5). The text above stands as first published; this block states the engine's floating-point form.
>
> **Fade, as evaluated:** `f(s) = ((s·s)·s)·((s·((s·6.0) − 15.0)) + 10.0)`, double precision. **Mis-derivation warning:** the expanded polynomial `6s⁵ − 15s⁴ + 10s³` differs from this in the last bits on most inputs. Implement the factored form exactly. The lerp is `a + t·(b − a)`, and the corner dot products are `g₀·p + g₁·q + g₂·r`, left to right, with integer gradient components converted exactly.
>
> · kind `formula` · confidence `verified(bytecode+harness)`
> · widths: the lattice floor is 32-bit, and the table index is the non-negative residue mod 256 (as above).
> · fp-form: double precision throughout, in the written order. Literals `6.0`, `15.0` and `10.0` are double.
> · verify-by: already executed. The engine's fade matched the factored form on 200 000 random inputs, while the expanded polynomial differed on 156 829 of them. The lerp and dot orders were read in the disassembly.

**Q2.7** **Draw order, summary.** The complete consumption contract is Q1.2 (one `L` from `R₄₈(W)`), Q1.5 (two `L` from each noise's stream) and Q1.7 (each layer's own stream). Nothing is skipped or discarded, and no draw is shared between layers, stacks or noises beyond what those claims state.
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed (harness A).

---

## Q3 — Uniformity

**Q3.1** `minecraft:nether`, `minecraft:caves` and `minecraft:floating_islands` all use the mechanism of Q1–Q2. For a given `W`, each of the 13 names is bit-identical in all three. The settings' other fields (sea level, default block, router, surface rule) do not enter any named noise.
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed. Harness J compared all 13 names across the three settings, at 3 seeds × 40 positions: 3 120 comparisons, 0 mismatches. All three ship with `legacy_random_source: true`.

**Q3.2** **A data pack that redefines one of these names changes what a legacy dimension gets.**
- Fields read: `firstOctave`, and `amplitudes` in full. All entries count, zeros included, because they fix `n`, `λ`, `Δ` and octave numbering.
- Also read: the entry's **key**, as the salt of Q1.3. Identical parameters under a different key give an unrelated noise.
- Ignored: nothing else. A noise entry carries no other fields.

The settings' `legacy_random_source` selects the generator family (Q1) and has no effect on shape.
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed. Harness G redefined five keys (`nether_wart`, `ice`, `gravel`, `patch`, `surface`) with altered octaves, interior and trailing zeros, positive octaves, an all-zero list and an empty list, across 3 settings × 3 seeds: 1 125 comparisons, 0 mismatches.

**Q3.3** **Edge shapes a pack can supply**, all accepted:
- a positive `firstOctave` builds and evaluates normally, so legacy dimensions place no restriction on positive octaves here;
- an all-zero `amplitudes` list gives `N ≡ 0.0`;
- an empty `amplitudes` list gives `N ≡ 0.0`.

Whether those packs are valid input is a downstream policy question, not settled here.
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed (harness G; each shape matched the reference and constructed without error).

**Q3.4** A noise-positive-octave restriction exists in the generator codebase, but only on the sequential construction (Q1.9). That construction is never used for named noises, so for this mechanism the restriction is **`DO-NOT-IMPLEMENT`**. It is unreachable because no named-noise request reaches that construction. Measured: harness G built `firstOctave = 1` without error.
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed (harness G).

---

## Q4 — Read path

**Q4.1** Nothing in the read path differs between a legacy-built named noise and one built in a non-legacy dimension. Evaluation is Q2.2–Q2.6 in both cases, and the legacy setting affects construction only (Q1).
· kind `contract` · confidence `read-directly`
· verify-by: the legacy half is measured (harness B, F). The non-legacy half was not measured, because its generator family is out of scope. The cheapest confirmation is harness B with the non-legacy generator substituted in the reference, at about 1 hour of driver work.

**Q4.2** **Surface-rule sampling coordinates.** A surface rule's `noise_threshold` condition evaluates `N(x, 0.0, z)` at the column's integer block coordinates `x` and `z`. The second coordinate is the fixed value `0.0`, whatever the block's own `y`. The result is therefore constant within a column.
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed. Harness F drove the jar's own condition with randomized block `y` and matched `N(x, 0, z)` exactly: 3 settings × 3 seeds × each setting's names × 40 columns × 5 threshold cases = 12 000 trials, 0 mismatches.

> **Width amendment** (2026-11-09, REVIEW-14 §1). The text above stands as first published; this block adds integer widths and rounding semantics only.
>
> The column coordinates `x` and `z` are 32-bit integers, converted **exactly** to double. The middle coordinate is the double `0.0`.
>
> · kind `formula` · confidence `verified(bytecode)`
> · verify-by: already executed — read in the disassembly.

**Q4.3** **Scaling at read: none.** The raw `N` is compared.
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed (harness F, with thresholds set to the exact sampled value).

**Q4.4** **Comparison semantics.** The condition holds iff `min_threshold ≤ N(x, 0, z) ≤ max_threshold`, both inclusive, as double comparisons. When `min_threshold > max_threshold`, it never holds.
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed. Harness F cases, where `v↑` and `v↓` are the adjacent representable doubles above and below `v`:
- `[v, v]` holds;
- `[v↑, 10]` fails;
- `[−10, v↓]` fails;
- `[v − 10⁻³, v + 10⁻³]` holds;
- `[10, −10]` fails.

**Q4.5** **Surface depth.** The per-column integer `d(x, z) = trunc((N_surface(x, 0.0, z) · 2.75 + 3.0) + J(x, z) · 0.25)`, where:
- `trunc` rounds toward zero;
- `N_surface` is the named noise `minecraft:surface` (Q1–Q2);
- `J(x, z)` is the first `D` draw of `R₄₈(ρ(x, 0, z) ⊕ F(W))`, using the same `F` as Q1.2;
- `ρ(x, y, z)` is defined as follows. Let `u = sx((x · 3129871) mod 2³²) ⊕ ((z · 116129781) mod 2⁶⁴) ⊕ sx(y)`. Then `ρ = ((u · u · 42317861 + u · 11) mod 2⁶⁴) >> 16`, an arithmetic shift. **The x product wraps at 32 bits before sign extension. The z product is formed in 64 bits.**

· kind `formula` · confidence `verified(bytecode+harness)`
· verify-by: already executed.
- The 32-bit and 64-bit products were confirmed in the disassembly.
- Harness E compared the jar's own depth with the reference over 3 settings × 20 seeds × 400 columns (half within ±2 000, half within ±3·10⁷): 24 000 trials, 0 mismatches.

**Q4.6** **Mis-derivation warning: truncation is live.** `N_surface` reaches below `−1.0909…` (minimum observed: −1.182719). The pre-cast sum is then negative, and on (−1, 0) truncation gives `d = 0` where a floor would give `−1`. This is reachable, **not dead code**: 42 such columns occurred within a 4 096 × 2 048 scan, and the jar returned `0` for all of them. An example is in the golden table: seed 0, column (−1415, −602), `d = 0`.
· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed (harness I: targeted scan of 2 039 808 columns, 42 cases, 0 mismatches).

**Q4.7** **How `d` is consumed.** `d` is an integer per column. Surface-rule conditions use it as follows:
- `stone_depth` with `add_surface_depth: true` adds `d` to its depth bound (Q4.8);
- `y_above` and `water` add `d · surface_depth_multiplier` to their reference height;
- the hole condition holds iff `d ≤ 0`;
- the above-preliminary-surface condition uses (interpolated preliminary surface) + `d − 8` as its minimum level.

· kind `contract` · confidence `verified(differential-harness)` for `stone_depth`; `read-directly` for the other three
· verify-by:

> **Width amendment** (2026-11-09, REVIEW-14 §1). The text above stands as first published; this block adds integer widths and rounding semantics only.
>
> - `d · surface_depth_multiplier` is a 32-bit product. The multiplier's codec range is `[−20, 20]`, so no overflow is reachable.
> - The above-preliminary-surface minimum level is `⌊B⌋ + d − 8`. Here `⌊·⌋` is the 32-bit floor, toward −∞, of a double-precision bilinear interpolation `B` of the four surrounding preliminary-surface values.
> - The interpolation weights are `(x mod 16)/16` and `(z mod 16)/16`, each computed in **single precision** (exact for these values), then widened.
> - The sums are 32-bit.
>
> · kind `formula` · confidence `verified(bytecode)`
> · verify-by: already executed — read in the disassembly. Not driven: these consumers remain `read-directly` in behavior. The cost is as stated above.
- `stone_depth`: harness K (Q4.8).
- The other three need a full surface-building context, including the preliminary surface. Assembling one is beyond this run's driver and outside the brief's scope (the rest of the surface-rule system).
- Cheapest confirmation: the same constructor-bypassed context technique as harness K, with the height fields set. Estimated at about 2 hours.

**Q4.8** **The secondary depth term**, the other noise-fed term of `stone_depth`. With `secondary_depth_range = r`:
- `r = 0` gives `e = 0`;
- `r > 0` gives `e = trunc(((N_ss(x, 0.0, z) + 1) / 2) · r)`, where `N_ss` is the named noise `minecraft:surface_secondary`. This is unclamped, and `e` can be negative.

The condition holds iff `stone depth ≤ 1 + offset + (d if add_surface_depth, else 0) + e`. In the shipped caves and floating-islands rules, `r ∈ {6, 30}` occurs, so this term is live in those dimensions.
· kind `formula` · confidence `verified(differential-harness)`
· verify-by: already executed. Harness K drove the jar's own `stone_depth` condition at each bound and one above it, over four (offset, add, r) configurations, 3 settings × 3 seeds × 300 random columns plus 30 targeted columns per run with `N_ss < −1 − 2/30`: 23 760 trials, 0 mismatches. That included 540 negative values of `e`.

> **Width amendment** (2026-11-09, REVIEW-14 §1). The text above stands as first published; this block adds integer widths and rounding semantics only.
>
> `r` is a 32-bit integer, converted exactly to double. `e` is the 32-bit truncation, toward zero, of the double product. The depth bound `1 + offset + d + e` is a 32-bit sum, compared as an integer. No overflow is reachable: `|e| ≤ r·1.1` with small `r`.
>
> · kind `formula` · confidence `verified(bytecode+harness)`
> · verify-by: already executed — the truncation and 32-bit sums were read in the disassembly, and behavior was verified by harness K of run 04, including 540 negative `e`.

---

## Q5 — Completeness

**Q5.1** **Every path in a legacy dimension that requests named noises:**
- **(a)** surface-rule `noise_threshold` conditions, for any key: the Q1–Q2 rule;
- **(b)** the surface system, which requests nine fixed keys for every dimension whatever its rules contain: `minecraft:surface`, `minecraft:surface_secondary`, `minecraft:clay_bands_offset`, `minecraft:badlands_pillar`, `minecraft:badlands_pillar_roof`, `minecraft:badlands_surface`, `minecraft:iceberg_pillar`, `minecraft:iceberg_pillar_roof` and `minecraft:iceberg_surface`. All nine follow the Q1–Q2 rule;
- **(c)** noise references in the terrain/climate router. These follow the Q1–Q2 rule **except** `minecraft:temperature`, `minecraft:vegetation` and `minecraft:offset`. In a legacy dimension, the router gives those three a different, built-in construction that ignores their pack values. That construction is climate and terrain, out of scope (brief §1). It was read only far enough to place it, and is not specified here.

· kind `contract` · confidence `verified(differential-harness)` for (a) and for (b)'s `surface`, `surface_secondary` and `clay_bands_offset`; `read-directly` for the remaining six keys of (b) and for (c)
· verify-by:
- Harness H requested `minecraft:temperature`, `minecraft:vegetation`, `minecraft:offset`, `minecraft:surface_secondary` and `minecraft:clay_bands_offset` through the named path: 600 trials, 0 mismatches.
- The remaining six keys of (b) use the same request path. The cheapest confirmation is to extend harness H, at about 5 minutes.

**Q5.2** **`open-question`: a key collision between the router and the named path. This is a live hazard, not dead code.** A data pack can reference `minecraft:temperature`, `minecraft:vegetation` or `minecraft:offset` from a surface-rule `noise_threshold`. In a legacy dimension, it then receives the Q1–Q2 noise (measured in harness H), which is a different noise from the router's own use of the same key (Q5.1c). An implementation that keeps one noise per key will be wrong for one of the two consumers. How to represent this is a downstream decision and is not made here.
· kind `open-question` · confidence `verified(differential-harness)` for the named-path side; `read-directly` for the router side
· verify-by: measure the router-side noise for these keys against the named-path noise at matched positions. This is out of this run's scope (climate). About 30 minutes, if a later run opens it.

**Q5.3** The aquifer's positional seed derivation in a legacy dimension passes through the same salt combination as Q1.3 (sign-extended string hash XOR `F`). Per the brief, that fact is recorded and not pursued, and aquifer open question 7 stays closed.
· kind `contract` · confidence `read-directly`
· verify-by: none in scope (brief §1).

**Q5.4** **The complete external input set** of a named noise in a legacy dimension:
- `W mod 2⁴⁸`;
- the settings' `legacy_random_source`;
- the key's resource-location text;
- that key's `firstOctave` and `amplitudes`;
- the sample position.

Surface rules supply `(x, 0, z)` (Q4.2). Surface depth additionally reads `F(W)` through `J` (Q4.5).

**Not read**, though its absence might surprise:
- the top 16 bits of `W`;
- sea level, default block, height or minimum `y`;
- the block's own `y` (for surface-rule reads);
- biome;
- the dimension's router, beyond Q5.1c;
- any other registry entry;
- the order or timing of requests;
- which consumers exist.

· kind `contract` · confidence `verified(differential-harness)`
· verify-by: already executed. Harness D covers the top 16 bits, C the order and the consumers, J the other settings fields, and F the block `y`. Biome and other registry entries rest on construction-state equality (harness A), which leaves no room for them.

**Q5.5** **`open-question`: a key missing from the registry.** If a surface rule (or the surface system's fixed list, Q5.1b) names a key with no registry entry, the request is a hard failure, not a zero noise. Whether to model that, or to reject such packs earlier, is downstream policy.
· kind `open-question` · confidence `read-directly`
· verify-by: drive a dimension state with one of the nine fixed keys removed from the noise getter, at about 5 minutes. Not executed, because the observable is a failure mode whose policy is the owner's.

---

## Open questions and deliberate omissions

1. **Q5.2:** a router versus named-path key collision for `minecraft:temperature`, `minecraft:vegetation` and `minecraft:offset`. This is a live hazard.
2. **Q5.5:** a missing registry key is a hard failure. Policy is downstream.
3. **Q3.3:** whether edge-shaped noise definitions (positive octaves, empty or all-zero lists) are valid pack input is a policy question. Their behavior is specified.
4. **Omitted, out of scope (brief §1):**
   - the router's built-in construction for the three keys in Q5.1c;
   - the non-legacy generator family;
   - terrain density, climate, blended noise and the aquifer.
5. **Not measured:** the three `d` consumers of Q4.7 other than `stone_depth`, and the six surface-system keys in Q5.1b beyond those harnessed. Costs are given in those claims.
