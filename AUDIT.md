# AUDIT — aquifer clean-room specification

Self-audit per `RESEARCH.md` §6, performed against the filtration contract (§3) after the spec was complete. Crosses the wall alongside `spec/aquifer-spec.md` and `spec/aquifer-constants.json`.

- **Date:** 2026-09-06
- **Audited artifacts:** `spec/aquifer-spec.md` (56 numbered claims plus one coverage-budget entry, 57 field-bearing items), `spec/aquifer-constants.json`
- **Re-audited:** after empirical verification of Q4.4 (§8), after adversarial review (§10), and after the anchoring cycle (§11)
- **Provenance:** `PROVENANCE.md` — 1.21.11 release, all integrity checks passed

---

## 1. Identifier check

Mechanical search of the spec for camelCase and package-like (dotted) tokens:

| Pattern | Hits in spec | Hits in constants JSON |
|---|---|---|
| camelCase | **0** | 1 |
| dotted / package-like | **0** | 0 |

**The one hit, justified:** `firstOctave` in the constants file. This is a field name in the *data pack's own noise definition files* — runtime data observable in any pack, not a source identifier. §3 permits registry keys and runtime data explicitly. It is retained because the Implementer's harness reads those pack files directly and needs the key to match. The spec prose itself avoids it.

Registry keys present, all permitted as runtime data under §3: `minecraft:aquifer` (the RNG salt, required for seed derivation) and the four noise keys `minecraft:aquifer_{barrier, fluid_level_floodedness, fluid_level_spread, lava}`.

All other names are either pack-schema names required by §5 (`sea_level`, `default_fluid`, `default_block`, `aquifers_enabled`, `min_y`, `height`) or neutral math symbols chosen here (`d₁…d₄`, `s₁₂`, `θ_hi`, `θ_lo`, `θ_flow`, `φ`, `Π`, `i, j, k`, `u, v, w`, `S_min`, `S_max`, `y_skip`, `never`).

## 2. Code and pseudocode check

Fenced code blocks in the spec: **0**. No language keywords, no indentation-as-structure, no step-numbered operations.

Two places required care and are recorded as judgement calls:

- **Q5.3** describes a first-match-in-a-fixed-order selection. This is order-dependent, but the order is *fixed data* (the Q5.1 table), so it is stated as a definition — "let `o*` be the first offset satisfying …" — rather than as a loop. No traversal is narrated.
- **Q6.4** and **Q6.6** are case-split function definitions. Case splits over disjoint predicates are mathematical, not control flow; each is written as a set of guarded values, not as branches taken in sequence.

I judged both to fall on the permitted side. Both are flagged here so the owner can disagree without having to hunt for them.

**CLOSED — owner-accepted.** Both rulings were reviewed and accepted as permitted: a fixed table with significant order is data, and "the first element satisfying P" is a definition over that data rather than a narrated traversal; disjoint guarded cases are how mathematics writes piecewise functions. The filtration line sits at reproducing the source's expressive choices, and neither does. No future session should treat these as pending.

## 3. Organization check

Section order is exactly §4's question order: Q1 global picker, Q2 applicability, Q3 lattice, Q4 neighbourhood & selection, Q5 per-point fluid status, Q6 blending & barrier, Q7 completeness, Q8 interactions. No section mirrors a source boundary. Facts that live together in the source are split across questions where §4 asks for them separately (notably: the substance decision is distributed across Q2, Q4, Q6 and Q8), and facts the source separates are merged where §4 asks one question (notably Q5, which draws on material the source keeps apart).

## 4. Claim-field check

| Field | Present | Expected |
|---|---|---|
| Numbered claims `Qn.n` | 56 | — |
| Coverage-budget entry (unnumbered, same fields) | 1 | — |
| `kind` | 57 | 57 |
| `confidence` | 57 | 57 |
| `verify-by` | 57 | 57 |

Every constant and every formula carries a proposed confirmation. Seven claims carry no exercisable confirmation: **five** marked `none` (deliberate non-implementations — debug-switch behaviour and the dead constant of Q3.6, where nothing should be built) and **two** marked `n/a` (Q4.6 and Q4.8, which *are* verification and correction records rather than claims needing proof).

Confidence distribution under the three-tier scheme (`read-directly` | `inferred-from-reading` | `verified(<method>)`, added to the brief by owner amendment):

| tier | count | claims |
|---|---|---|
| `verified(bytecode+harness)` | 1 | Q4.4 |
| `verified(differential-harness)` | 3 | Q4.5, Q4.7, Q8.7 |
| `read-directly` | 51 | the rest |
| `inferred-from-reading` | 2 | Q1.4; the `A₄` coverage entry |

Q3.4 carries a partial inference noted inline. Q4.6 and Q4.8 are records rather than claims needing a tier.

**Four of the fifty-seven are verified; the other fifty-three rest on reading** and carry `verify-by` text that has never been run. §10 records that at least one such proposal was unworkable when finally attempted, so the count of claims carrying a `verify-by` must not be read as a count of claims confirmed.

## 5. Purity check

All claims are stated as pure functions of (seed, position, pack inputs). Vanilla's caching and memoization were encountered and deliberately excluded: the per-block reuse of the barrier noise value is noted in **Q6.5** only to say that it is *unobservable* and imposes no ordering requirement, which is the purity rule's intended treatment.

No genuinely order-dependent or stateful behaviour was found in the aquifer path. The one order-sensitive element — the probe table of Q5.1 — is fixed data rather than evaluation history, so it did not need the §3 escape hatch.

## 6. Open questions and deliberate omissions

Carried in the spec's closing section; summarized here:

1. **Q4.4 tie-breaking** — **RESOLVED**, see §8. Verified against a running server and at bytecode level. Its earlier "measure-zero" characterization was **wrong** and is corrected by the measurements in Q4.5.
2. **Q3.6** — a declared distance constant, dead in this version. Recorded so the Implementer does not "restore" it. Must not be implemented.
3. **Debug switches** (Q1.5, Q6.8, Q7.4) — build-time constants, false in shipped builds. Not implementable behaviour.
4. **Q1.4** — engine bounds are arithmetic compositions of four separately-read definitions, not literals in the tree. Recompute rather than trusting the transcribed numbers.
5. **Q5.1** — the thirteen-offset table is asymmetric with no principle I could identify. Opaque data; do not normalize it.
6. **Not investigated** — the definitions of the preliminary-surface, erosion and depth inputs. Treated as opaque. Each needs its own run of this brief if not already verified downstream.
7. **Legacy random source** — presets selecting it derive the Q3.4 seed by a different combination step. Only the modern source was traced in full.
8. **`A₄` coverage rate** — Q8.4's fourth-nearest-point term was never instrumented, and no defensible bound exists from current data. Golden regions provide **zero** measured coverage of that path. On the next instrumented run's shopping list.
9. **Why rank 2–3 ties never change a block** — measured as exactly 0 in 792 338 ties, but the barrier predicate is not visibly symmetric under that swap. The mechanism is unidentified; the spec explicitly forbids encoding the zero as an invariant. Added when Q4.7's first draft was found to assert an unverified explanation — the same inference error as the one recorded in §10.

Two things I want to flag beyond the required list, because they are the most likely sources of a silently-wrong implementation:

- **Q7.5** — `fluid_level_spread` and `lava` are sampled at *contracted grid indices used directly as coordinates*; `barrier` and `fluid_level_floodedness` are sampled at true positions. Getting this backwards produces plausible-looking but wrong terrain.
- **Q7.3** — no biome or structure data is read anywhere in this path, despite one threshold pair being named for a biome. Worth stating because the natural assumption is the opposite.

## 7. Scope check

The run covered the aquifer system only, per §2. No adjacent system was investigated. Q8.5 notes that the carver system consumes the aquifer's fluid-update flag, but the carver side was deliberately not read; the note records only the contract the aquifer itself provides.

---

## 8. Empirical verification (added after the initial audit)

Q4.4 was the one claim carrying `inferred-from-reading` for a *behaviour*. It has since been verified two independent ways, neither of which trusts the decompiled source.

**Bytecode.** The decompiler's output was set aside and the shipped class was disassembled directly. All four rank comparisons compile to the branch form javac emits for a non-strict `≥`, confirming that an equal-distance candidate displaces the incumbent. The three loop bounds and the axis each loop variable accumulates into were likewise read as opcodes, confirming x-outermost / y-middle / z-innermost.

**Running server.** A differential harness was compiled against the remapped jar and run inside a real dedicated server generating 4 096 chunks. For every selection it captured all 12 candidates and ranked them by this specification's model and by a strict "earlier holds" alternative, comparing both against the engine's own choice.

| Measure | Value |
|---|---|
| Selection evaluations | 53 897 588 |
| Mismatches — this spec's model | **0** |
| Mismatches — strict alternative | 3 259 052 |
| Evaluations with duplicate distances | 15.40% |
| Rank-1 ties | 581 785 (1.08%) |

The two models were separately shown to disagree on 47% of randomized synthetic inputs, so the zero-mismatch result is not vacuous.

**Honest scope limit.** Candidates are captured in the engine's own traversal order, so the experiment tests the tie-break *rule* only, not the *iteration order*. The order rests on bytecode alone. This is stated in Q4.6 rather than being papered over.

## 9. A defect found in the decompiled tree

Compiling the aquifer source surfaced a genuine **decompiler defect**: in the aquifers-disabled path, two distinct variables were emitted with the same identifier, so the code as decompiled does not compile and, read literally, says something different from what the class actually does.

Consequences, all addressed:

- The artifact was repaired in the *private* build copy only. The recorded decompiled tree and its digest in `PROVENANCE.md` are untouched.
- Q2.1 depends on exactly that path, so it was re-verified against bytecode rather than the faulty text. It stands as written: the disabled path returns solid where density is positive and otherwise reads the global picker **at the block position**.
- This is the concrete justification for the brief's `verify-by` discipline. At least one place in the tree does not say what the class does, so no claim should rest on decompiled text alone.

I did not audit the rest of the tree for further artifacts; that was out of scope for this run. Claims outside the aquifer path are unaffected because there are none.

---

## 10. Correction after adversarial review (second re-audit)

An adversarial review of this verification found a real error in §8 as first written, and the correction is recorded here rather than quietly applied.

**What was wrong.** §8 reported "ties that change the emitted block = 56 092". That number was never measured. What was measured is that in 56 092 rank-1 ties the two tied points carry *different fluid statuses*; changed output was then **inferred** from that. The inference is invalid — the barrier predicate of Q6.6 usually collapses the difference before it reaches the block.

**What the measurement actually shows.** A second harness restructured the post-selection logic so it could be invoked twice per tied evaluation, once with the engine's ordering and once with the tied pair swapped, using the engine's own code rather than a reimplementation:

| tied pair | ties | substance changed | flag changed |
|---|---|---|---|
| ranks 1–2 | 581 785 | **47** | 1 |
| ranks 2–3 | 792 338 | 0 | 13 962 |
| ranks 3–4 | 941 739 | 1 649 | 3 494 |

So the true rank-1 figure is 47, not 56 092 — wrong by about three orders of magnitude. This is now Q4.8 in the spec, kept as an explicit correction claim.

**A second error, in the fidelity check.** I first tried to establish that the restructured harness was faithful by byte-comparing saved region files against a baseline. All 16 differed, which proves nothing: those files embed timestamps and independently compressed payloads, so two runs of identical logic never match byte-for-byte. The valid evidence is that both builds report identical totals to the digit — 53 897 588 evaluations and 581 785 rank-1 ties.

**What this changes about the system.** The tie-break's real consequence sits where none of my earlier reasoning looked: rank 3–4 ties change roughly 35× more blocks than rank 1–2 ties, and rank 2–3 ties change only the fluid-update flag. Captured as Q4.7 and Q8.7.

**Process note the owner should weigh.** The finding that surfaced this was produced by an adversarial reviewer and then *rejected* by two independent refuters who called it a category error. Re-measuring directly showed the reviewer was right and both refuters were wrong. Refuter consensus is therefore not evidence, and this document should not be read as if agreement between checkers were a quality signal — only direct measurement settled it.

**Consequence for the brief's `verify-by` discipline.** Q4.4's original `verify-by` ("construct a synthetic tie and compare") was unworkable as written: a randomly constructed rank-1 tie shows no difference over 99.99% of the time. Proposed confirmations in this document are hypotheses too, and at least one was wrong. They should be costed before being trusted as a verification route.

---

---

## 11. Anchoring the harness to the shipped engine

The §8 differential rested on a *patched, recompiled* build of a class this same run proved the decompiler can fumble (§9). Four cycles of owner review closed that gap; the full record is `PROVENANCE.md` §3b–§3e.

**What failed, and why it was the criterion rather than the harness.** Comparing whole generated worlds cannot work at full generation: two runs of the **untouched official jar** differ from each other in ~1 800 of 4 096 chunks (~250 ppm), because the decoration stage is order-sensitive under concurrent chunk generation. Stripping features cut that by ~72% but left a simulation residual — ice formation, grass spread, fluid ticks — because force-loaded chunks tick. The quiescence criterion proposed to drain that away is unreachable: unfreezing re-queues scheduled fluids, so the drain does not converge.

**What worked.** Freeze the world before generation and never unfreeze, strip features *and* structures, and compare generation output on four channels. Under that regime the patched build and the untouched official jar differ in **nothing** across 4 096 chunks — 0 block differences, 0 `block_ticks`, 0 `fluid_ticks`, 0 partial post-processing differences — with one whole-list post-processing consumption race that the identical comparison reproduces between two runs of the official jar against itself.

**Honest limits of the anchor, all three.**

1. It is measured on a feature- and structure-stripped pack. Aquifer-domain equivalence on the *unstripped* pack is shown separately and more weakly: zero differences anywhere in the deep terrain skeleton, with every residual difference class identified as post-generation simulation.
2. The frozen regime's own noise floor is small but **not zero**: two runs of the untouched jar differ in 2 cells of 402 653 184, both single water blocks at the water surface flipping flow state, each with a matching scheduled tick. The anchor certifies equivalence at that resolution, not exact identity. This class was not anticipated by the review that ratified the regime, and no classification rule has been agreed for it.
3. It anchors the harness that produced Q4.5/Q4.7's *measurements*. Q4.4's rule never depended on it — that came from shipped bytecode.

**Two tolerance classes, both owner-ruled and both scoped to an observed signature.** *Consumption race*: a whole-list post-processing difference, one side empty. *Flow-state race*: a same-fluid, level-only block difference paired with a `fluid_ticks` difference at that exact position. Anything outside those predicates stays fatal — including a level-only difference without its paired tick. Task A'' needed neither: it was 0 on the block and tick channels, and the classes exist because the *untouched jar against itself* exhibits both.

**A methodological result worth keeping.** Every post-processing difference observed across six independent world comparisons — twelve in total — was whole-list: one side holding the entire list, the other exactly zero. None was partial. The affected chunks cluster one chunk past force-load batch boundaries. That regularity is what justifies classifying whole-list differences as ambient while keeping any partial difference a hard failure.

## 12. Statement

No content in the spec reproduces expression from the source; all claims are methods, facts, or contracts. The spec contains no code or pseudocode, no source or mapping identifiers, no source comments or prose, and no reflection of the source's class, method, or file organization. Its organization derives solely from the question list in `RESEARCH.md` §4, plus a coverage-budget section added by owner instruction.

Every claim is grounded in the 1.21.11 artifacts recorded in `PROVENANCE.md` — in the decompiled tree, in the shipped bytecode, or in measurement against a server built from those artifacts and anchored against the untouched shipped jar. Nothing was supplied from prior recollection of the subject matter. Where the tree and the bytecode disagreed (§9), the bytecode governed. Where a claim and a measurement disagreed (§10), the measurement governed, and the error is recorded rather than quietly corrected.

Four of fifty-seven claims are verified. The rest are hypotheses with priors, and this document should be read as one.

This statement is placed last so that it covers §8–§11, which were added after the initial audit. It supersedes the statements previously standing at §8 and at the earlier §11.

— Researcher session, 2026-09-07 (re-signed after the anchoring cycle)
