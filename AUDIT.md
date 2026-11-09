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

---

## 12. Statement

No content in the spec reproduces expression from the source; all claims are methods, facts, or contracts. The spec contains no code or pseudocode, no source or mapping identifiers, no source comments or prose, and no reflection of the source's class, method, or file organization. Its organization derives solely from the question list in `RESEARCH.md` §4, plus a coverage-budget section added by owner instruction.

Every claim is grounded in the 1.21.11 artifacts recorded in `PROVENANCE.md` — in the decompiled tree, in the shipped bytecode, or in measurement against a server built from those artifacts and anchored against the untouched shipped jar. Nothing was supplied from prior recollection of the subject matter. Where the tree and the bytecode disagreed (§9), the bytecode governed. Where a claim and a measurement disagreed (§10), the measurement governed, and the error is recorded rather than quietly corrected.

Four of fifty-seven claims are verified. The rest are hypotheses with priors, and this document should be read as one.

This statement is placed last so that it covers §8–§11, which were added after the initial audit. It supersedes the statements previously standing at §8 and at the earlier §11.

— Researcher session, 2026-09-07 (re-signed after the anchoring cycle)

---

## 13. Run 02 audit — blended noise

Self-audit of `spec/blended-noise-spec.md` and `spec/blended-noise-constants.json`, per `RESEARCH-RUN-02.md` section 5's method (filtration contract, purity rule, confidence discipline unchanged from `RESEARCH.md`; bytecode precedent and verified-by-default added). This is a separate spec from run 01's aquifer spec; the checks below are re-run against it independently, not inherited.

**Identifier check.** Mechanical search for camelCase and dotted tokens found two real hits on the first pass, both fixed before this audit: three source method names [names removed, REVIEW-15 §2] (replaced with plain coordinate wording) and one further source method name [name removed, REVIEW-15 §2] (renamed to the neutral `clamp_lerp` and defined inline in the notation). After the fix: zero camelCase, zero dotted tokens, in both the spec and the constants JSON. The one permitted registry-key-shaped token is the salt string `minecraft:terrain` in Q5.1, runtime data required for seed derivation, matching the exception the filtration contract carves out.

**Code and pseudocode check.** Zero fenced code blocks. Q1.4's combination formula and Q2.2's fold/cap formula are both written as closed-form expressions with explicit summation notation, not as loops or step-numbered procedures, consistent with the run 01 precedent on this exact question (Q5.3/Q6.4/Q6.6 in the aquifer spec, ratified as permitted by the owner in REVIEW-01 section 1).

**Organization check.** Section order matches `RESEARCH-RUN-02.md` section 4's question order (Q1 structure, Q2 fold/cap, Q3 conditional behavior, Q4 initialization, Q5 completeness) plus a coverage note and open questions, mirroring run 01's shape. No section boundary reproduces the source's own class or method layout — the mechanism's own source file [name removed, REVIEW-15 §2] and the shared per-octave primitive's are two separate source files, but their content is redistributed across all five questions as the brief's question list demands, not kept in file-shaped chunks.

**Claim-field check.** 21 claims, all carrying `kind`, `confidence`, and `verify-by`. Confidence distribution: 7 `verified(differential-harness)`, 13 `read-directly`, 1 `inferred-from-reading` (Q3.1, a universal-negative proof from codec bounds that no finite sweep can establish by itself). This is a markedly higher verified fraction than run 01's initial delivery managed on a first pass — a direct consequence of `RESEARCH-RUN-02.md` section 5's raised bar, which asked for verification by default rather than as a later remediation.

**Purity check.** All claims are stated as pure functions of (seed, position, the five pack parameters). The one implementation-detail-shaped observation (Q1.5, the selector-saturation summation skip) is recorded specifically to explain why it does *not* appear as a formula branch — its omission is itself the purity-consistent choice, not an oversight.

**A genuine harness-authoring error, caught before anything crossed the wall.** The first version of the differential harness's independent reimplementation of Q2.2's interpolation step swapped which axis fed the outer blend weight, and separately used the *folded* y-fraction for that weight where the real mechanism uses the *unfolded* one. This produced near-total mismatch (19986 of 20000 baseline trials, 5976 of 6048 fold-mechanism trials) on the first run. The error was in the harness's own reimplementation, not in the source reading that preceded it — the source was read correctly; translating that reading into fresh Java code introduced the bug. It was caught immediately by the differential test itself (which is the entire point of building one), diagnosed by re-deriving the real call's argument order term by term against the already-correctly-read source, fixed, and the full suite re-run to zero mismatches across all eight sections before any claim was written with a `verified` tag. Nothing resting on the wrong version was ever published.

**Bytecode precedent.** `RESEARCH-RUN-02.md` section 5 asked that any claim resting on decompiled text be spot-checked against disassembly where the text looks reconstructed. No such passage was encountered in this run's three source files [file names removed, REVIEW-15 §2] — all decompiled cleanly, with none of the variable-collision or brace-mismatch symptoms that flagged the aquifer tree's one defect (`AUDIT.md` section 9). No bytecode spot-check was therefore performed; instead, every mechanism claim was verified behaviorally against the running unmodified jar, which is strictly stronger evidence than a bytecode read would have been on its own.

**Scope check.** The run covered `minecraft:old_blended_noise`'s sampling path only, per `RESEARCH-RUN-02.md` section 2. Two adjacent items were read far enough to place them out of scope with a specific reason rather than a blanket exclusion: a derivative-computing noise variant (confirmed entirely unreferenced in the whole tree, Q5.3) and the real engine's own seed-wiring for this mechanism (read one level up, at the wiring site, only as far as needed for Q5.1's salt-string fact — not investigated further). The legacy random source and the Implementer's own code remained closed to this session throughout, per section 2 and section 3 respectively; nothing about either was requested, read, or reasoned about.

**Correction after owner review (REVIEW-06).** Q3.1's topic sentence named the wrong branch as dead — it opened "the `d ≠ 0` branch... is dead for every reachable input" when the proof that follows it, the DO-NOT-IMPLEMENT clause at the end of the same claim, and `blended-noise-constants.json`'s `dead_branches.zero_divisor_fallback` entry all establish the opposite: the `d = 0` fallback is dead, and only the `d ≠ 0` formula is ever implemented. A DO-NOT-IMPLEMENT claim whose own topic sentence names the live branch as the dead one is worse than merely wrong — a skim-reader acting on the first sentence alone would delete the only path that actually runs. The topic sentence has been corrected to read "the `d = 0` fallback ... is dead"; nothing else in Q3.1, or anywhere else in the document, needed to change — the proof body, the DO-NOT-IMPLEMENT clause, and the constants JSON were all already correct. Checked for recurrence: this was the only place the inversion appeared.

**Statement note.** Section 12 above is run 01's own closing statement, left in place as a record of that run. It is superseded, not deleted, by the unified statement below, which covers both runs and is placed last.

---

## 14. Statement

No content in either spec reproduces expression from the source; all claims are methods, facts, or contracts. Neither spec contains code or pseudocode, source or mapping identifiers (beyond permitted runtime salts and registry keys), source comments or prose, or a reflection of the source's class, method, or file organization. `aquifer-spec.md`'s organization derives from `RESEARCH.md` §4 plus a coverage-budget section added by owner instruction; `blended-noise-spec.md`'s derives from `RESEARCH-RUN-02.md` §4.

Every claim in both specs is grounded in the 1.21.11 artifacts recorded in `PROVENANCE.md` — in the decompiled tree, in the shipped bytecode, or in measurement against a server or driver built from those artifacts. Nothing was supplied from prior recollection of the subject matter. Where the tree and the bytecode disagreed (§9), the bytecode governed. Where a claim and a measurement disagreed (§10), the measurement governed, and the error is recorded rather than quietly corrected. Where a harness's own first-draft reimplementation and the already-correctly-read source disagreed (§13), the source governed, the harness was fixed, and the error is recorded the same way rather than silently patched out of the history. Where a claim's own topic sentence disagreed with its own proof body (§13, Q3.1, caught on owner review rather than by this session), the proof body and the DO-NOT-IMPLEMENT clause governed — both were already correct — and only the misstated sentence was rewritten.

Four of fifty-seven aquifer claims are verified. Seven of the blended-noise spec's twenty-one claims are `verified(differential-harness)` outright, and the remaining fourteen carry `read-directly` or `inferred-from-reading` with no stronger exercisable check available (Q3.1's universal-negative proof; Q5.3/Q5.4's out-of-scope dead-code notes). The rest of both documents are hypotheses with priors, and both should be read as such.

This statement is placed last so that it covers §13, added after run 01's closure and reopening. It supersedes the run-01-only statement at §12 for anything spanning both runs, without erasing §12's own historical record.

— Researcher session, 2026-09-09 (re-signed after run 02; re-affirmed after the REVIEW-06 Q3.1 correction, same session)

---

## 15. Run 03 audit — bandlands surface rule

Self-audit of `spec/bandlands-spec.md` and `spec/bandlands-constants.json`, per `RESEARCH-RUN-03.md` section 5's method (filtration contract, purity rule, confidence discipline per `RESEARCH.md` sections 3/5, with the run-02 section 5 additions — bytecode precedent, verified-by-default — cited by reference, not restated). A third, independent spec; the checks below are re-run against it fresh, not inherited from either prior run.

**Identifier check — corrected after owner review (REVIEW-08 §2).** The sweep as first reported here was inaccurate: it covered `bandlands-spec.md` only and asserted the constants JSON was also clean without having actually run the check against it. The true sweep, run against both final artifacts:

| pattern | hits in spec | hits in constants JSON |
|---|---|---|
| camelCase | 0 (after one fix) | 1 (justified: `firstOctave`) |
| dotted / package-like | 0 | 0 (two `"...spec.md"`/`"...constants.json"` filename substrings excluded — see below) |

Spec: one camelCase hit on the first pass, a source method name [name removed, REVIEW-15 §2] in Q6.2's prose. Fixed by referencing "Q4's read path" instead.

Constants JSON, two hits, neither caught before this correction: this driver's own method name [name removed, REVIEW-15 §2], embedded inside a `corrections_recorded` string — fixed by rephrasing to "the read-path check," a neutral description of what the correction concerned rather than the name of the code that concerned it; and `firstOctave`, a field name from the noise pack's own definition files, justified the same way run 01 justified the identical field name (`AUDIT.md` §1) — runtime data the Implementer's harness reads directly from the pack, retained because the key must match, with no occurrence in the spec's own prose.

Two dotted-token matches in the constants JSON are the filenames `bandlands-spec.md` and `PROVENANCE.md` inside `_meta`'s own self-reference fields — the same self-reference pattern every constants file in this project uses (e.g. `blended-noise-constants.json`'s `_meta.source_spec`), not a source identifier; the regex matches the `.md` extension as if it were a method call, a false positive worth naming rather than silently passing over.

Coverage note, extended per REVIEW-08 §2: this sweep, and every sweep in this document from here forward, runs against the raw text of both artifacts in full — including JSON string *values*, not only keys or the spec's own prose. The two misses above happened because the constants JSON was never swept at all on its final version, not because string values specifically were skipped; recorded as the actual cause rather than deflected to a more flattering one.

**Code and pseudocode check — one judgment call flagged here, and later denied on appeal (REVIEW-08 §1).** Zero fenced code blocks. Q2.2 and Q2.4 each describe a loop's exact step-by-step advance rule, written as closed-form advance formulas and termination conditions rather than "first do X, then do Y" procedure narration. As first delivered, Q2.2 additionally explained why the advance is `+2` by naming the source's own realization of it. Flagged here as a judgment call at the time. On review the call was denied: that explanation was source-structure content with no behavioral content of its own — an implementation with a single combined `+2` advance is behaviorally indistinguishable from any other realization summing to the same value, so the explanation's entire content was the source's own expressive choice, not a fact about the mechanism. A sentence describing how the source realizes the `+2` across its loop structure was removed as source-structure content, from the spec claim, the constants file's note, and this paragraph itself; replaced with a plain recurrence (`indexₖ₊₁ = indexₖ + draw + 2`) that states the value and its consequence without describing how the source arrives at it. The value, the mis-derivation warning (a naive `+1` diverges immediately under full-array comparison), and the differential evidence all survive unchanged — nothing about *what was verified* changed, only how one sentence explained *why* the number is what it is.

**Organization check.** Section order matches `RESEARCH-RUN-03.md` section 4's question order (Q1 structure, Q2 construction, Q3 randomness, Q4 read path, Q5 conditional behavior, Q6 completeness). No section reproduces the source's own layout — the single source file backing this mechanism interleaves construction and read-path code with several unrelated per-dimension features (frozen-ocean icebergs, eroded-badlands pillars) that share the same class purely for code-organization reasons on the source side; none of that unrelated material appears in the spec at all, redistributed or otherwise, because it is out of scope, not because it was reorganized.

**Claim-field check.** 20 claims, all carrying `kind`, `confidence`, and `verify-by`. Confidence distribution: 10 `verified(differential-harness)`, 10 `read-directly`, 0 `inferred-from-reading`. Every `read-directly` claim in this spec carries an explicit "verify-by: none stronger available" with a stated reason (a wiring/lifetime fact, or an absence claim) rather than an unexplained gap — matching the infeasibility-must-be-stated requirement of `RESEARCH-RUN-03.md` section 5.

**Purity check.** All claims are stated as pure functions of (world seed, position, the mechanism's own pack data). Q1.2's "built once per dimension, held for the dimension's lifetime" is a lifetime/caching fact, recorded exactly as the purity rule prescribes: as an observable when-it-happens fact, not as a narrated construction sequence, since the *value* built does not depend on when construction happens, only that it happens once and is reused.

**A genuine finding, not a genuine error, and the distinction matters.** Two harness bugs were caught and fixed in the course of this run (below), but the run's most significant result is not a bug at all: Q5.1 documents that the real, unmodified method fails with an array-index-out-of-range error [runtime class name removed, REVIEW-15 §2] for reachable inputs — reproduced directly against the shipped jar, not inferred. This is recorded as an `open-question`-kind claim per house convention for load-bearing uncertainty (REVIEW-06 section 2's ruling on that tag, applied here to a live hazard rather than dead code — the tag's job is to mark "requires a decision downstream," and a policy question is exactly that, whichever direction the uncertainty runs). It is explicitly flagged in the spec as *not* a DO-NOT-IMPLEMENT dead branch, the usual pattern from runs 01–02, precisely so a skim-reader does not mistake this run's one `open-question` for the others' shape.

**Two harness bugs, both recorded rather than silently fixed.** First: the harness's first draft of the read-path wrapper used a safer, always-nonnegative reduction formula than the one the source actually uses, so it silently disagreed with the real method's crash behavior instead of reproducing it — caught immediately because the real method threw on a hand-picked corner before the harness's own comparison logic ever ran, which is a stronger catch than a silent value mismatch would have been. Fixed by matching the source's exact single-add reduction shape; re-run to exact agreement, including exact agreement on which inputs throw. Second, cosmetic: an embedded double-quote inside a Java string literal broke compilation on the first fix attempt (a copy-paste of the source's own hyphenated phrasing into a string that was already delimited by the same character) — rephrased, no logic was ever affected, and this one is recorded only for completeness since `RESEARCH-RUN-03.md` section 5 asks deviations to be surfaced rather than the bar being "nothing ever went wrong."

**Bytecode precedent.** No passage in the mechanism's source file [name removed, REVIEW-15 §2] showed the reconstruction symptoms that flagged the aquifer tree's one defect (section 9) or that would have warranted a spot-check per `RESEARCH-RUN-03.md` section 5's citation of that precedent. The file decompiled cleanly throughout. As in run 02, every mechanism claim was instead verified behaviorally against the running unmodified jar, which is the stronger check regardless.

**Scope check.** The run covered the `minecraft:bandlands` mechanism's own construction and read path only, per `RESEARCH-RUN-03.md` section 2. Two adjacent pieces of code sharing the same source file were read far enough to place them out of scope with a specific reason, not a blanket one: the eroded-badlands and frozen-ocean extension methods (confirmed to be separate per-column features, invoked from the same surrounding loop but never called by or sharing state with this mechanism, Q1.3) and the surface-rule tree that decides *when* this mechanism is even consulted (out of scope per section 2's "rest of the surface-rule system," and specifically named in Q5.1 as the likely location of whatever keeps the reachable crash from firing in real play — not investigated). The Implementer's behavioral model and ground-truth captures remained closed throughout, per section 3; nothing about either was requested, read, or reasoned about, and every verification corner in the harness was chosen from reading the mechanism fresh, not from any external hint about where to look.

---

## 16. Statement

No content in any of the three specs reproduces expression from the source; all claims are methods, facts, or contracts. None contains code or pseudocode, source or mapping identifiers (beyond permitted runtime salts and registry keys), source comments or prose, or a reflection of the source's class, method, or file organization. `aquifer-spec.md`'s organization derives from `RESEARCH.md` §4 plus a coverage-budget section added by owner instruction; `blended-noise-spec.md`'s derives from `RESEARCH-RUN-02.md` §4; `bandlands-spec.md`'s derives from `RESEARCH-RUN-03.md` §4.

Every claim in all three specs is grounded in the 1.21.11 artifacts recorded in `PROVENANCE.md` — in the decompiled tree, in the shipped bytecode, or in measurement against a server or driver built from those artifacts. Nothing was supplied from prior recollection of the subject matter. Where the tree and the bytecode disagreed (§9), the bytecode governed. Where a claim and a measurement disagreed (§10), the measurement governed, and the error is recorded rather than quietly corrected. Where a harness's own first-draft reimplementation and the already-correctly-read source disagreed (§13, §15), the source governed, the harness was fixed, and the error is recorded the same way rather than silently patched out of the history. Where a claim's own topic sentence disagreed with its own proof body (§13, Q3.1, caught on owner review), the proof body governed and only the misstated sentence was rewritten. Run 03 additionally recorded a claim that is neither a correction nor a dead branch but a reproduced, load-bearing hazard in the shipped mechanism itself (§15, Q5.1) — a new category of finding for this project, handled the same way as everything else here: measured directly against the real, unmodified jar, and reported as found rather than smoothed over.

Two further corrections to §15, both from owner review (REVIEW-08) rather than caught in-session, and both recorded in §15 itself rather than silently applied. First: a sentence describing how the source realizes Q2.2's `+2` across its loop structure was removed as source-structure content — denied on appeal, since a single combined `+2` advance is behaviorally indistinguishable from any other realization summing to the same value; scrubbed from the spec, the constants file, and §15's own restatement, replaced with a plain recurrence stating the value and its consequence without describing the source's realization of it. Second, more serious: §15 as first delivered asserted zero camelCase identifiers in *both* the spec and the constants JSON, having only actually swept the spec — the constants JSON contained two uncaught hits, one of them this project's own driver method name embedded in a correction note, the other a justified-and-retained pack-data field name that the identifier table now counts rather than zeroes. Both hits are resolved (one scrubbed, one justified), the sweep has actually been run against both final artifacts, and §15 reports true counts with per-hit justification rather than an unchecked assertion. Every sweep in this document, from here forward, covers JSON string values as a matter of course, not only keys or prose. Standing rule from this correction forward: a correction names the category and reason of what was removed, never the removed expression itself.

Four of fifty-seven aquifer claims are verified. Seven of the blended-noise spec's twenty-one claims are `verified(differential-harness)` outright, with the remaining fourteen carrying `read-directly` or `inferred-from-reading` and no stronger exercisable check available. Ten of the bandlands spec's twenty claims are `verified(differential-harness)`, and the other ten are `read-directly` with an explicitly stated reason each time no stronger check was available — zero `inferred-from-reading` in that spec, the first of the three with none. The rest of all three documents are hypotheses with priors, and all three should be read as such.

This statement is placed last so that it covers §15, added after run 02's closure and a second reopening. It supersedes the two-run statement at §14 for anything spanning all three runs, without erasing §12's or §14's own historical record — each remains exactly what it said at the time it was signed.

— Researcher session, 2026-09-10 (re-signed after run 03; re-affirmed after the REVIEW-08 Q2.2/Q2.4 scrub and the identifier-count correction; re-affirmed again after the REVIEW-09 leak rephrasing and the justified-hit table fix, same session)

---

## 17. Run 04 audit — named noises in legacy-random-source dimensions

Artifacts audited: `spec/legacy-named-noise-spec.md` (33 claims) and
`spec/legacy-named-noise-constants.json`, both in their final versions.

### Identifier check

The sweep was run mechanically on the final raw text of both files. JSON
string values were included, because the sweep reads the raw file text.

| pattern | spec | constants JSON |
|---|---|---|
| camelCase | 10 (justified: `firstOctave`, pack field name) | 15 (justified: `firstOctave`) |
| PascalCase compound | 0 | 0 |
| package-like (`a.b.c`) | 0 | 0 |
| source class/method names (explicit list, case-insensitive) | 0 true hits | 0 true hits |
| this run's driver identifiers (explicit list, case-insensitive) | 0 true hits | 0 true hits |

False positives of the case-insensitive list, named here and kept:

- `named` / `Named` (23): the brief's own term "named noise";
- `noises` (8): the English plural, the brief's vocabulary;
- `stack` / `Stack` (6) and `lattice` / `Lattice` (2): ordinary mathematical
  words;
- JSON: `named` (1).

None of these is an identifier in this run's driver; see the corrections
below.

### Code and pseudocode check

- 0 code fences.
- 0 control-flow constructs. The one pattern hit is the English "for (a)
  and for (b)" in Q5.1.
- 39 lines end in `;`. All of them are punctuation in prose lists, not
  statements.
- No step-numbered procedures. Draw orders are stated as ordered contracts
  (Q1.5–Q1.7, Q2.7), and the octave and lattice computations as formulas
  (Q2.2–Q2.6).

### Organization check

Sections follow `RESEARCH-RUN-04.md` §3 exactly: Q1 construction, Q2 shape,
Q3 uniformity, Q4 read path, Q5 completeness. The material is drawn from at
least eight source files across three packages. None of their boundaries is
reflected. For example, the generator, its positional derivation, the
octave stack and the surface consumers are interleaved by question rather
than grouped by where they live.

### Claim-field check

33 claims, each carrying `kind`, `confidence` and `verify-by`. Counted mechanically from each claim's confidence field.

| confidence | count |
|---|---|
| `verified(differential-harness)` | 24 |
| `verified(bytecode+harness)` | 2 (Q1.8, Q4.5) |
| mixed, split per part | 4 (Q1.9, Q4.7, Q5.1, Q5.2; the `read-directly` part is stated in each) |
| `read-directly` | 3 (Q4.1, Q5.3, Q5.5) |
| `inferred-from-reading` | 0 |

Every `read-directly` part states why it was not measured (out of scope, or
a failure-mode policy question) and gives a costed confirmation.

### Purity check

Every claim is a pure function of the world seed, the key text, the key's
pack fields and position. The engine builds each noise on first request
and reuses it afterwards. That is stated only as its observable
consequence, request-order independence (Q1.4), which harness C measures.
No order-dependent or stateful behavior was found.

### Bytecode precedent

Rule 4 was applied at three places where the decompiled text set a bit
width:

1. **The generator's double draw.** The decompiled text renders the
   multiplier as a single-precision literal, which, if taken literally,
   implies a single-precision product. The bytecode holds the double
   constant 2⁻⁵³ and multiplies in double precision. **The decompiled text
   is wrong here, and the bytecode governs.** This is the project's second
   recorded decompiled-text defect (the first is §9), and Q1.8(d) states
   the warning.
2. **The positional seed's 32-bit and 64-bit products.** Text and bytecode
   agree.
3. **The sign extension of the string hash, and the signed add assembling
   the 64-bit draw.** Text and bytecode agree.

Each point was then confirmed behaviorally, by a mutant reference that
the jar rejects (harness B2).

### Scope check

Read beyond the brief only far enough to place items out of scope
(CLAUDE.md rule 18):

- **The router's built-in construction for three keys.** It is climate and
  terrain, so it is out of scope. It is named in Q5.1c and Q5.2, and the
  construction is not specified.
- **The aquifer's seed derivation.** One sentence (Q5.3). Aquifer open
  question 7 stays closed.
- **The surface-rule conditions that consume surface depth.** Read only to
  list how the integer is used (Q4.7). Only `stone_depth` was harnessed.
- **The iceberg column randomness in the surface system.** Read only to
  confirm it is not a named noise. It is not mentioned in the spec.

No Implementer material was requested, read or reasoned about.

### Corrections, recorded rather than applied silently

1. **Driver indexing fault.** Harness sections G and H indexed past the end
   of the position table. H halted. G's catch-all reported the resulting
   exception as if the jar had thrown during construction. Both were fixed
   by bounding the index. G was also restructured so that only the jar's
   own construction is guarded, and driver faults propagate. The suite was
   then re-run in full. No spec claim rests on the faulty run, and the five
   "threw" outcomes it printed were artifacts.
2. **Unexercised branch.** The first run of harness K exercised no negative
   secondary term. Targeted columns were added (540 negative cases), and
   only then was Q4.8 marked `verified`.
3. **The sweep's own error.** The first identifier sweep printed the wrong
   field of the match output, so its counts were meaningless. It was
   corrected and re-run before any result was used.
4. **Spec wording.** Two kinds of token were removed from the spec and the
   constants file:
   - a notation abbreviation that collided, case-insensitively, with a
     driver identifier;
   - two standard-library method names used to describe adjacent doubles.

   Both were replaced with neutral notation. To keep the remaining ordinary
   words free of collisions, four driver classes were then renamed and the
   full suite re-run. The result was unchanged (843 535 comparisons, 0
   mismatches), and the golden table was byte-identical.
5. **Claim counts in this section.** The first draft of this section gave
   36 claims and tier counts that did not add up. They had been estimated,
   not counted. All counts were then taken mechanically from the final
   spec (33 claims), and the section and §18 were corrected before
   signing.

### Open questions and deliberate omissions

These are listed at the end of the spec:

- Q5.2: a router versus named-path key collision. This is a live hazard,
  not dead code.
- Q5.5: a missing registry key.
- Q3.3: the validity of edge-shaped noise definitions as pack input.
- Q3.4 is the run's one `DO-NOT-IMPLEMENT`, with its reachability argument
  and a measurement.

---

## 18. Statement

No content in any of the four specs reproduces expression from the source;
all claims are methods, facts, or contracts. None contains code or
pseudocode, source or mapping identifiers (beyond permitted runtime salts,
registry keys and pack field names), source comments or prose, or a
reflection of the source's class, method, or file organization. Each
spec's organization derives from its own brief:

- `aquifer-spec.md` from `RESEARCH.md` §4, plus the owner-instructed
  coverage section;
- `blended-noise-spec.md` from `RESEARCH-RUN-02.md` §4;
- `bandlands-spec.md` from `RESEARCH-RUN-03.md` §4;
- `legacy-named-noise-spec.md` from `RESEARCH-RUN-04.md` §3.

Run 04's claims are grounded in the cloud artifacts, which were gated per
file against `records/tree-listing.sha256` in the same session, before any
reading. Its continuity with runs 01–03 is by evidence chain, as recorded
in `PROVENANCE.md`.

Where the decompiled text and the bytecode disagreed (§9 and §17), the
bytecode governed. Where a harness or a sweep was wrong (§13, §15 and §17),
the error was recorded and the run repeated, not patched out of the
history.

Of the 33 run-04 claims, 26 are wholly `verified`, 4 are split between a
verified part and a `read-directly` part, and 3 are `read-directly` with
the reason stated. Like the other three documents, the spec should be read
as a hypothesis with strong priors.

This statement supersedes §16 for anything spanning the four runs. §12,
§14 and §16 remain as signed at the time.

— Researcher session, 2026-11-09 (signed after run 04)

---

## 19. Aquifer Q3.4 width amendment (REVIEW-13 §2)

Artifacts changed: `spec/aquifer-spec.md` and `spec/aquifer-constants.json`.

Both changes are **additions only** (the diff has 0 deleted lines). The
original Q3.4 text, its confidence tag and the existing `rng.seed_mix`
fields stand as first published:

- the spec carries the amendment as a dated block directly below Q3.4;
- the constants file gains a widths entry under `rng` / `seed_mix`, and a
  `_changelog` entry.

### What the original lacked

Q3.4 published the mix's multipliers without integer widths. This was an
omission, not an error: the multipliers, the order of operations and the
shift amount are all correct. But it permitted implementations that agree
with the jar only on part of the index space. The amendment states the
regions:

- a 64-bit x product agrees exactly for `|i| ≤ 686`;
- a 32-bit z product agrees exactly for `|k| ≤ 18`;
- a zero-extended `j` agrees exactly for `j ≥ 0`;
- a logical shift agrees only where the pre-shift value is non-negative.

**Correction to REVIEW-13's framing, recorded per rule 20.** REVIEW-13 §2
names the x-product boundary (about 686 cells) as the case that existing
evidence cannot see. That is right for x. The z-product boundary is far
narrower, at 18 cells (about 300 blocks), so a 32-bit z implementation
fails much closer to the origin. Whether existing goldens would have caught
it depends on their coordinates. Those are Implementer material, which
this session does not see, so no conclusion is drawn.

### Evidence

- **Bytecode,** read this session:
  - the aquifer's single cell-positioning call, with its argument order;
  - the non-legacy positional factory's combination step;
  - the mix function itself.

  Each width in the amendment is read from those instructions.
- **Differential harness:**
  - 19 845 128-bit state comparisons against a reference that does the mix
    in arbitrary-precision arithmetic, reducing explicitly at each stated
    width so as not to inherit the host language's integer semantics;
  - 59 535 checks of the aquifer's three bounded draws.

  Both gave 0 mismatches. The index sets straddle every boundary on both
  signs, and include the reachable extremes, across 9 seeds.
- **Mutant references:** each wrong-width mutant agreed on 100% of samples
  inside its stated region and disagreed on 100% outside it. The amendment's
  boundary statements are therefore measured, not inferred.

### Scope check

The work covered the mix's arithmetic and its combination with the stream
seed. That is the "combined with the stream seed" phrase of Q3.4 itself.

Read only to place them:

- the stream's generator construction, including a zero-state substitution
  in the generator. This was not examined; it is not part of the mix;
- the legacy positional factory, which is aquifer open question 7 and
  stays closed.

The amendment states explicitly that it applies to the non-legacy source.

### Identifier check

Run mechanically on the final raw text of both full files, and separately
on the added lines alone:

| pattern | added lines | `aquifer-spec.md` (full) | `aquifer-constants.json` (full) |
|---|---|---|---|
| camelCase | 0 | 0 | 4 (justified: `firstOctave`; pre-existing, untouched by this amendment) |
| PascalCase compound | 0 | 0 | 0 |
| package-like | 0 | 0 | 0 |
| source names / this task's driver names (explicit list, case-insensitive) | 0 true hits | 0 true hits | 0 true hits |

False positive, named: `wraps` (1 in each file, the English verb). After a
rename it collides with no driver identifier; see the corrections.

There are no code fences and no step-numbered procedures. The amendment
states widths as formulas, and the agreement regions as contracts.

### Corrections, recorded rather than applied silently

1. **The first sweep of the added lines** found two hits:
   - a package-like token: a dotted key path naming a location in our own
     constants file. It was rephrased to a non-dotted form;
   - a case-insensitive collision between an English word and a driver
     function name. The driver function was renamed, and the harness
     re-run with identical results.

   No source content was involved in either.

---

## 20. Statement

No content in any of the four specs reproduces expression from the source;
all claims are methods, facts, or contracts. None contains code or
pseudocode, source or mapping identifiers (beyond permitted runtime salts,
registry keys and pack field names), source comments or prose, or a
reflection of the source's class, method, or file organization. Each
spec's organization derives from its own brief:

- `aquifer-spec.md` from `RESEARCH.md` §4, plus the owner-instructed
  coverage section, with its Q3.4 amendment placed under the claim it
  amends;
- `blended-noise-spec.md` from `RESEARCH-RUN-02.md` §4;
- `bandlands-spec.md` from `RESEARCH-RUN-03.md` §4;
- `legacy-named-noise-spec.md` from `RESEARCH-RUN-04.md` §3.

The Q3.4 amendment (§19) is grounded in the cloud artifacts, which were
gated per file in the same session before any reading. It adds widths and
leaves every published word of the claim in place. Where an amendment adds
precision to an earlier claim, the earlier claim stays visible, so the
history of what was published, and when, can be read directly.

Where the decompiled text and the bytecode disagreed (§9, §17), the
bytecode governed. In §19, every width was taken from the bytecode, not the
text. Where a harness, a sweep or a reviewer's framing was wrong or
incomplete (§13, §15, §17 and §19), the error is recorded rather than
patched out of the history.

This statement supersedes §18 for anything spanning the four specs and the
Q3.4 amendment. §12, §14, §16 and §18 remain as signed at the time.

— Researcher session, 2026-11-09 (re-signed after the aquifer Q3.4 width amendment)

---

## 21. Integer-width audit of all published specs (REVIEW-14 §1, CLAUDE.md rule 14a)

The audit covers every crossed spec: aquifer, blended noise, bandlands and
legacy named noise. Every amendment is an addition placed directly under
the claim it amends, headed "Width amendment (2026-11-09, REVIEW-14 §1)".
No existing line of any spec or constants file was changed or removed
(diff: 0 deleted lines in all eight files). Each constants file gains an
`integer_widths` entry and a `_changelog` entry.

### Evidence base

- **Bytecode,** read this session, for every row.
- **Driver** (private; recorded in `PROVENANCE.md`), run against the unmodified jar:
  431 444 comparisons of real methods with width-explicit references, 0
  mismatches. Sections:
  - A: cell-index and cell-origin maps, similarity;
  - B: pressure;
  - C: contraction, quantization, the preliminary-surface conversion and
    snap, lattice extent, skip height;
  - D: lattice-point retention, end to end;
  - E: legacy octave number.
- **Mutants:** one per wrong width or wrong rounding. Each agrees inside its
  stated region and disagrees outside it, as recorded per row.
- **Run 02–04 harnesses:** they already verified the values of several
  formulas whose widths are now stated. The table names them where they
  are part of the evidence.

### Table

"Widths stated" refers to the original publication: **yes**, **partial**
(rounding direction or language semantics stated, but not width or
signedness) or **no**.

| spec | claim | formula (neutral notation) | widths stated | action | evidence |
|---|---|---|---|---|---|
| aquifer | Q1.1 | `A_void` level `= 2·min_y_limit` | no | amended | bytecode; constant, no boundary |
| aquifer | Q1.4 | `never = 16·min_y_limit`; field widths 26/12/26 | no | amended | bytecode |
| aquifer | Q2.5 | `y_skip = 12·(⌊(S_max+20)/12⌋+1)+10`; `S_max` over stride-4 columns | partial | amended | driver C: 294 set-ups |
| aquifer | Q3.2 | `i = ⌊(x−5)/16⌋`, `j = ⌊(y+1)/12⌋`, `k = ⌊(z−5)/16⌋` | partial | amended | driver A; truncation mutants 55/55 & 50/50, 52/52 & 53/53 |
| aquifer | Q3.3 | point `(16i+u, 12j+v, 16k+w)`; **y retained mod a signed 12-bit field** | no | amended, plus **`open-question`** | driver D: 4 201-value sweep; end to end, top 6/63 and bottom 30/63 wrapped; selection changed for 77/62 720 and 106/37 632 blocks |
| aquifer | Q3.4 | cell-position mix | yes (REVIEW-13 amendment) | spot-checked | bytecode this session |
| aquifer | Q3.5 | lattice extent bounds | partial | amended | driver C: 294 set-ups |
| aquifer | Q4.1 | candidate cells `(i+a, j+b, k+c)` | no | amended | bytecode |
| aquifer | Q4.2 | `dx²+dy²+dz²` on retained coordinates | no | amended | bytecode |
| aquifer | Q5.1 | probe columns `q + 16·o` | no | amended | bytecode |
| aquifer | Q5.2 | `Prelim = ⌊value⌋` at `4·⌊x/4⌋`; `a = Prelim + 8` | no | amended | driver C; floor mutant 210/210 & 105/105; snap mutant 216/216 & 99/99 |
| aquifer | Q5.3 | `qy − 12`, `qy + 12` | no | amended | bytecode (found by the 14a sweep) |
| aquifer | Q5.6 | `d = S_min + 8 − qy`, converted exactly | no | amended | bytecode |
| aquifer | Q5.7 | `⌊q/16⌋`, `⌊qy/40⌋`; `40·yi+20`; `3·⌊(10s)/3⌋`; `min` | partial | amended | driver C; quantization mutant 10 080/10 080 & 5 600/5 600 |
| aquifer | Q5.8 | `⌊q/64⌋`, `⌊qy/40⌋` | partial | amended | driver C; mutant 342/342 & 778/778 |
| aquifer | Q5.10 | integer sample contexts converted exactly | no | amended | driver C (captured contexts); bytecode (14a sweep) |
| aquifer | Q6.1 | `1 − (q − p)/25.0` | no | amended | driver A: 322 524 pairs; integer-division mutant 13 020/13 020 & 309 504/309 504 |
| aquifer | Q6.4 | `Δ = \|L_A−L_B\|`; `m = 0.5·(L_A+L_B)`; `r = Δ/2.0` | no | amended | driver B: 14 739; single-halving mutants 6 528/6 528 & 7 344/7 344 |
| aquifer | Q8.3 | `1 − 44/25.0 = −0.76` | partial | amended | driver A: bitwise equal to the literal |
| blended | Q1.4 | block coordinates converted exactly; `scale(n) = 2⁻ⁿ` | no | amended | bytecode |
| blended | Q1.6 | `wrap`, with a 64-bit floor | yes | spot-checked | bytecode (64-bit floor, then exact conversion) |
| blended | Q2.2 | `y0 = ⌊y+yo⌋`; `fold = ⌊c/d + 10⁻⁷⌋·d` | partial | amended | bytecode; saturation bound from the codec ranges; values by run 02 D–H |
| blended | Q2.3 | `10⁻⁷` as a single-precision literal | yes | spot-checked | bytecode (`1.0000000116860974E-7` widened) |
| bandlands | Q2.2 | `index₊ = index + d + 2` | no | amended | bytecode (32-bit adds only) |
| bandlands | Q2.3 | runs, widths and starts in `[0, 191]` | no | amended | bytecode |
| bandlands | Q2.4 | index `±1`, advance in `[4, 19]` | no | amended | bytecode |
| bandlands | Q4.2 | `(x, 0, z)` converted exactly | no | amended | bytecode (14a sweep) |
| bandlands | Q4.4 | `round(4·v)`: 64-bit, narrowed to 32 | partial | amended | bytecode; run 03 C/D |
| bandlands | Q4.5 | `(y + offset + 192)` with a truncated remainder by 192 | partial | amended | bytecode; run 03 D |
| bandlands | Q5.1 | crash reach of the Q4.5 reduction | — | none (covered by Q4.5) | — |
| legacy | notation | `h(s)` mod 2³², signed | yes | spot-checked | bytecode this session |
| legacy | Q1.3 | `sx(h) ⊕ F` | yes | spot-checked | bytecode (sign extension before the XOR) |
| legacy | notation | seeding `(σ ⊕ M) mod 2⁴⁸`; step mod 2⁴⁸ | yes | spot-checked | bytecode |
| legacy | notation | `b(k)`, top k bits, signed 32 | yes | spot-checked | bytecode |
| legacy | notation | `L = b·2³² + sx(b)` | yes | spot-checked | bytecode (signed add) |
| legacy | notation | `D = (b26·2²⁷ + b27)·2⁻⁵³` | yes | spot-checked | bytecode (double multiply) |
| legacy | notation | `I(n)`, both paths | yes | spot-checked | bytecode (power-of-two path; rejection on 32-bit overflow) |
| legacy | Q1.2 | `F = L` of `R₄₈(W)` | yes | spot-checked | run 04 A/B |
| legacy | Q1.6 | `k_i = firstOctave + i`, **32-bit, wraps** | no | amended | driver E: 45 comparisons; 64-bit mutant 9/9 on overflow |
| legacy | Q1.7 | `256 − t`, `t + j` | no | amended | bytecode (14a sweep) |
| legacy | Q1.8 | the bit-width set (a)–(d) | yes | spot-checked | bytecode |
| legacy | Q2.3 | `Δ`, `Δ + 1` | partial | amended | bytecode |
| legacy | Q2.4 | input factor `2^{firstOctave}·2^i`; weights `λ·2⁻ⁱ` | partial | amended | bytecode; the underflow edge `firstOctave < −1074` was not driven (cost stated) |
| legacy | Q2.5 | `wrap`, with a 64-bit floor | yes | spot-checked | bytecode |
| legacy | Q2.6 | 32-bit lattice floor; `π[m mod 256]` | yes | spot-checked | bytecode |
| legacy | Q4.2 | `(x, 0, z)` converted exactly | no | amended | bytecode (14a sweep) |
| legacy | Q4.5 | `d` truncation; `ρ` products 32/64-bit | yes | spot-checked | bytecode |
| legacy | Q4.7 | `d·m` 32-bit; `⌊B⌋ + d − 8`; single-precision weights | no | amended | bytecode |
| legacy | Q4.8 | `e = trunc(((N+1)/2)·r)`, 32-bit; integer bound sum | partial | amended | bytecode; run 04 K |

**Totals** (counted mechanically from the action column): 49 rows, of
which 33 were amended, 15 spot-checked and 1 covered by another row. One
amendment, Q3.3, carries a new `open-question`.

### Mechanical completeness check (rule 14a)

A pattern sweep over all four specs flagged every claim containing an
integer-operation pattern: floor or truncation, `mod`, shifts, XOR, sign
extension, powers of two, integer products, and the words "index" and
"integer". It then checked each flagged claim, amendments included, for
width wording. Result after the amendments:

- 58 claims flagged; 43 state widths.
- The remaining 15 are:
  - 14 false positives: measurement tables or records (aquifer Q4.5–Q4.7,
    Q8.7), reasoning or contracts with no integer formula (blended Q1.3,
    Q3.1; bandlands Q2.5, Q3.1, Q6.1, Q6.3; legacy Q1.4, Q3.1, Q3.2), and a
    single-precision literal whose precision is already stated (blended
    Q2.3);
  - 1 covered by another claim: bandlands Q5.1, which discusses Q4.5's
    reduction.

**The sweep found six rows that the manual inventory had missed:** aquifer
Q5.3 and Q5.10, bandlands Q4.2, and legacy Q1.7, Q4.2 and Q4.8. All six were
amended before signing.

### Findings

1. **Aquifer Q3.3, a live hazard.** The lattice point's y coordinate is
   retained in a signed 12-bit field, so it wraps at the top cell of a
   dimension topped at 2028 or higher, and at the bottom cell when a pack's
   `sea_level` sits below the bottom. Neither edge is reached by any shipped
   preset. Both were measured as reachable by pack data, and both change
   the nearest-four selection for some blocks. This is published as an
   `open-question`, and the policy is the owner's.
2. **Aquifer Q3.2, Q5.2, Q5.7 and Q5.8: rounding was only partly stated.**
   Every division is floor, and the preliminary-surface value is floored,
   not truncated. A truncating implementation agrees only on non-negative
   or exactly divisible inputs. The published text already said "floordiv"
   in places, but never gave widths or the preliminary-surface conversion.
3. **Aquifer Q6.1 and Q6.4: real versus integer division.** The similarity
   uses a double division. The pressure midpoint and half-difference are
   exact real halves. Integer variants agree only on multiples of 25, or on
   even sums and differences, so they diverge throughout normal output.
4. **Legacy Q1.6.** The octave number wraps at 32 bits for extreme
   pack-supplied `firstOctave` values.

### Outside REVIEW-14's integer scope: recorded, not amended (rule 18)

- **Aquifer Q5.6 floodedness thresholds: a floating-point evaluation
  order.** The published `θ_hi = 0.8 − 1.1φ` and `θ_lo = 0.4 − 1.2φ` are
  algebraically equal to the engine's thresholds, but not bit-identical.
  Over all 65 reachable `φ` values (`d ∈ [0, 64]`), they differ in bits for
  44 (`θ_hi`) and 63 (`θ_lo`). Example: at `φ = 1` the engine's `θ_hi` is
  `−0.3`, while the published form gives `−0.30000000000000004`. Decisions
  flip only for a floodedness value inside a one-unit-in-the-last-place gap,
  so this is practically unobservable, but it is not bit-exact. Measured by
  the driver (section F). **Ruling requested:** amend Q5.6 to the engine's
  evaluation form, and order an audit of floating-point evaluation order of
  the same kind as this one.
- **Identifiers found in earlier crossed specs** by this section's sweep.
  These are not introduced by this audit, which added none, but the
  earlier statements missed them:
  - `blended-noise-spec.md`: **mapping class names**, two distinct tokens,
    6 occurrences (in Q1.4, Q2.2, Q4.2 and Q5.1).
    `blended-noise-constants.json`: one of them, 2 occurrences. The
    filtration contract forbids these.
  - `bandlands-spec.md`: a JDK runtime class name, 1 occurrence (Q5.1).
    `bandlands-constants.json`: the same token, 2 occurrences. It is not a
    Mojang identifier, but it is still an identifier.
  - `aquifer-constants.json`: one key and one value that match a source
    method name and a source constant name (the flow-threshold entries),
    1 occurrence each.

  **Correction to §14, §16, §18 and §20 (rule 20).** Each of those
  statements asserts that no spec contains source or mapping identifiers.
  For `blended-noise-spec.md` that was false when signed, including the two
  re-signings by this cloud session (§18, §20), whose sweeps covered only
  the files changed in their own runs. **Ruling requested:** scrub these
  tokens from the two runs' artifacts under the REVIEW-08 rule (category and
  reason cited, removed content not restated).

### Identifier check (rule 14), run on all eight final artifacts

| file | camelCase | other identifier hits |
|---|---|---|
| `aquifer-spec.md` | 0 | 0 true. False positives: the spec's own floor-division notation symbol (16), and two English words containing a listed substring |
| `aquifer-constants.json` | 4 (justified: `firstOctave`) | 2 pre-existing (the flow-threshold key and value; see above) |
| `blended-noise-spec.md` | 0 | **6 pre-existing** (see above) |
| `blended-noise-constants.json` | 0 | **2 pre-existing** |
| `bandlands-spec.md` | 0 | **1 pre-existing** (JDK) |
| `bandlands-constants.json` | 2: `firstOctave` (justified) and `camelCase` (the English word in a changelog) | **2 pre-existing** (JDK) |
| `legacy-named-noise-spec.md` | 21 (justified: `firstOctave`) | 0 |
| `legacy-named-noise-constants.json` | 18 (justified: `firstOctave`) | 0 |

There are no code fences in any file. The added lines contain no source,
mapping or driver identifier.

### Corrections, recorded rather than applied silently

1. **Reachability of the bottom wrap was first argued unreachable,** on the
   grounds that those blocks are always below the global lava level. That is
   false when a pack lowers `sea_level` below the bottom, because the lava
   level is `min(−54, sea_level)`. It was measured reachable (30/63 cells)
   before anything was written.
2. **A stale build was run.** One compile failed, and the following run
   executed the previous build's classes. That output was discarded, and the
   build directory is now cleared before every compile.
3. **Two mutant regions were first stated loosely.** One was a combined
   halving mutant whose two errors cancel. The other was a contraction
   region defined jointly over both axes instead of per axis. Both were
   split or redefined, and the sharp results are the ones recorded. The
   combined mutant's partial disagreement is kept as information.
4. **The manual inventory missed six rows,** found by the 14a sweep (above).
5. **This audit's own JSON contained a driver function name** in two
   `integer_widths` values. It was replaced with neutral notation before
   signing.
6. **A storage-allocation detail** was removed from the aquifer Q3.5
   amendment, as source-structure content (rule 12).
7. **The table totals were first written by estimate** (50 rows, 16
   spot-checked). Counted mechanically before signing: 49 and 15. This is
   the same class of error as §17 correction 5, and it recurred because the
   totals were again typed before they were counted.
8. **This section's first draft named the pre-existing identifiers
   verbatim, and the driver's directory,** inside a file that crosses the
   wall. That would have re-published the tokens it flags. It was rewritten
   to cite category, location and count (rule 13) before signing.

---

## 22. Statement

All four specs, with their width amendments, express methods, facts and
contracts. They contain no code or pseudocode, no source comments or
prose, and no reflection of the source's class, method or file
organization. Each spec's organization derives from its own brief, and
every amendment sits directly under the claim it amends.

**One exception, stated rather than signed over.** The blended-noise spec
and its constants file contain mapping class names. The bandlands spec and
its constants file contain a JDK class name. The aquifer constants file
contains a key and value that match source names (§21). These tokens
predate this section. The statements at §14, §16, §18 and §20 should not
have certified their absence, and this one does not. Their removal awaits
the owner's ruling.

**Widths (rule 14a).** Every formula in the four specs that combines
integers now states the width and signedness of each operation, and its
wrap, extension, and floor or truncation semantics. The evidence is the
table and the mechanical sweep of §21. Every width was read from the
bytecode this session, and every reachable boundary was exercised by a
driver against the unmodified jar, with a mutant per wrong width.

Where an earlier claim, an earlier audit statement or this session's own
first argument was wrong or incomplete, the error is recorded in §21, not
patched out of the history. Correction 1 is the one most worth noting: a
reachability argument of my own that a measurement overturned.

This statement supersedes §20 for anything spanning the four specs. §12,
§14, §16, §18 and §20 remain as signed at the time, with the correction
above.

— Researcher session, 2026-11-09 (signed after the REVIEW-14 integer-width audit)

---

## 23. Identifier scrub, aquifer Q5.6 bit-exact amendment, and a whole-set sweep (REVIEW-15 §2)

### Scrub

Each scrub follows the REVIEW-08 / rule-13 pattern: neutral wording
replaces the token, and the record cites category, location and reason,
never the token.

**The enumerated list (REVIEW-15 §2.1):**

- `blended-noise-spec.md`: mapping class names, 6 occurrences, in Q1.4,
  Q2.1, Q4.2 (2), Q4.3 and Q5.1. They are now "lattice-noise layer" or
  primitive, and "random stream".
- `blended-noise-constants.json`: 2 occurrences, in string values. No key
  changed. A `_changelog` entry was added.
- `bandlands-spec.md`: a JDK runtime class name, 1 occurrence (Q5.1). It is
  now "an array-index-out-of-range error".
- `bandlands-constants.json`: the same token, 2 occurrences, in string
  values. A `_changelog` entry was added.
- `aquifer-constants.json`: the three flow-threshold keys of Q8.3 are
  renamed to `flow_threshold`, `flow_threshold_formula` and
  `flow_threshold_claim`. The formula value now uses the spec's own `sim`
  notation. The values are unchanged. The `_changelog` entry gives the
  reason and the new keys, and tells a consumer of the earlier keys to
  locate them by claim id and value.

**Extension beyond the enumerated list, recorded so it can be overruled.**
The whole-set sweep (rule 14, REVIEW-15 §4) covered `AUDIT.md` for the
first time, and found more of the same class:

- `AUDIT.md` §13 and §15: 13 identifier tokens (source file, class and
  method names, one runtime class name, one driver method name) on 7
  lines. Each is replaced in place by a bracketed marker that names the
  category and this review, 8 markers in all, so the edit itself stays
  visible in the history. The rewritten lines carry no identifier except
  the justified pack field name.
- `aquifer-constants.json`: two position-encoding keys of Q1.4 (renamed to
  `y_field_bits` and `y_span`, values unchanged) reproduced source
  constant names.
- `legacy-named-noise-constants.json`: one `integer_widths` key **that the
  REVIEW-14 audit itself introduced** coincided with a source constant
  name. It is renamed to `octave_input_scale`.

Each rename has a `_changelog` entry.

### Aquifer Q5.6: bit-exact amendment

- The engine's evaluation form is now stated in an amendment block. The
  original expressions are kept visible and marked "not bit-exact".
- `aquifer-constants.json` gains `phi_formula_engine`,
  `theta_hi_formula_engine`, `theta_lo_formula_engine` and a note. The
  original formula entries stay, marked.
- **Driver** (private; recorded in `PROVENANCE.md`): the real level
  computation was driven with floodedness exactly at each threshold and one
  unit in the last place either side, for every `d ∈ [−3, 67]`, submerged
  and not. The engine form gave **852 decisions, 0 mismatches**. The
  published form disagreed with the jar on **332 of 852**.
- The `φ` ramp was already bit-exact: 0 of 142 inputs differ. Only the two
  threshold expressions were wrong: 44 and 63 of the 65 reachable values
  differ.

### Finding: a false relation in blended-noise Q2.1 (rule 20)

Writing the mandatory fields for Q2.1 required reading its operands
exactly. Q2.1 states that the divisor equals the cap bound times
`smear_scale_multiplier`, "exactly". **That holds only at block `y = 1`.**
The divisor does not depend on `y`, and the cap bound is proportional to it
(bytecode, this session). The operand formulas stated in Q2.1 and Q1.4 are
correct, and run 02's value verification used them, so no verified value
changes. But a crossed, `verified`-tagged claim carried a false sentence,
and the constants file carried the same relation as a value. Both are now
corrected: an amendment block under Q2.1, and the constants value corrected
with a `_changelog` entry citing category and reason.

### Mandatory fields (rule 14a), checked mechanically

Every claim touched by this task was checked from the diff:

| claim | kind | `widths:` | `fp-form:` | result |
|---|---|---|---|---|
| aquifer Q5.6 | formula | yes | yes | pass |
| blended Q1.4 | formula | yes | yes | pass |
| blended Q2.1 | formula | yes | yes | pass |
| blended Q2.2 | formula | yes | yes | pass (fields added because Q2.1 supplies its arguments) |
| blended Q4.2, Q4.3, Q5.1 | contract | — | — | pass (fields not required) |
| bandlands Q5.1 | open-question | — | — | pass (fields not required) |

0 touched claims fail.

### Whole-set identifier sweep (rule 14, REVIEW-15 §4)

The sweep covers all nine artifacts that have ever crossed. Tokens are
classified against three sets derived mechanically this session:

- every class simple name in the official mappings (5 746);
- every member name in the mappings (37 957);
- every class simple name in the JDK 21 runtime image (21 129).

Counts below are copied from the sweep's output.

| file | compound class name | JDK class name | camelCase member name | justified (`firstOctave`) | snake-case runtime data | single English word = class name | pattern word |
|---|---|---|---|---|---|---|---|
| `spec/aquifer-spec.md` | 0 | 0 | 0 | 0 | 14 | 11 | 0 |
| `spec/aquifer-constants.json` | 0 | 0 | 0 | 4 | 4 | 0 | 0 |
| `spec/blended-noise-spec.md` | 0 | 0 | 0 | 0 | 0 | 5 | 0 |
| `spec/blended-noise-constants.json` | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| `spec/bandlands-spec.md` | 0 | 0 | 0 | 0 | 2 | 7 | 0 |
| `spec/bandlands-constants.json` | 0 | 0 | 0 | 1 | 9 | 0 | 1 |
| `spec/legacy-named-noise-spec.md` | 0 | 0 | 0 | 21 | 25 | 9 | 0 |
| `spec/legacy-named-noise-constants.json` | 0 | 0 | 0 | 18 | 52 | 0 | 0 |
| `AUDIT.md` (final text, including this section) | 0 | 0 | 0 | 15 | 3 | 36 | 18 |

Column notes:

- **Snake-case runtime data:** registry keys (noise, noise-settings and
  block ids) and the pack field `min_y`. They match source constants only
  because the source names its constants after them. Permitted.
  - Exception named here: `default_color`, a key in the bandlands
    constants. It coincides with constants in three unrelated classes
    (items, dyes, maps), not with the mechanism it describes. Kept as a
    generic phrase.
  - Also named: two compound keys contain a source constant's name as a
    substring, a size key and a stack input-factor key. Neither whole
    token matches, so the whole-token rule does not flag them. Considered
    and kept.
- **Single English word = class name:** capitalized ordinary words, such as
  headings and sentence starts, that happen to coincide with a class's
  simple name. False positives.
- **Pattern word:** the words "camelCase" and "PascalCase" themselves. False
  positives.
- There are no code fences in any file.

**Limits of this sweep, stated:**

- Member names are checked only for camelCase tokens. A lowercase single
  word that is also a member name is ordinary vocabulary in almost every
  case, and a full check of those would be unreadable.
- Snake-case tokens are checked against upper-snake constant names.

A mixed case in a form the sweep does not test would pass it.

### Corrections, recorded rather than applied silently

1. **Rename changelogs first named the old keys.** That would have
   restated the source-matching tokens they remove (rule 13). They were
   rewritten, before commit, to cite claim id and value.
2. **AUDIT §21's location list for the blended-noise tokens was wrong.**
   It attributed one occurrence to Q2.2 instead of Q2.1, and omitted Q4.3.
   The count was right.
3. **This task's field block for Q2.2 was first written with a false
   header,** saying its wording had been touched by the scrub. Corrected
   before commit.
4. **The REVIEW-14 audit itself introduced a key** that coincided with a
   source constant name. It was caught by this sweep and renamed.

---

## 24. Statement

The four specs and their constants express methods, facts and contracts.
They contain no code or pseudocode, no source comments or prose, and no
reflection of the source's class, method or file organization. Each spec's
organization derives from its own brief, and every amendment sits directly
under the claim it amends.

**Identifiers.** The whole-set sweep of §23 was run on the final text of
all nine artifacts that have ever crossed, against identifier sets derived
mechanically from the official mappings and the JDK. It finds no compound
class name, no JDK class name and no camelCase member name in any of them.
Every remaining hit is a justified pack field, runtime data or a named
false positive. The sweep's limits are stated in §23. The exception
recorded at §22 is resolved by §23's scrub.

**Widths and floating point.** Integer widths stand as certified at §22.
Aquifer Q5.6 now gives the engine's bit-exact floating-point form, verified
by driving the real jar at every reachable threshold. Every formula claim
touched since REVIEW-15 carries the `widths:` and `fp-form:` fields. The
floating-point audit of the remaining formulas is ordered (REVIEW-15 §5)
and is not certified here.

Where an earlier claim or audit statement was wrong, the error is recorded
here and not patched out of the history: the Q2.1 relation, §21's
locations, and a key my own previous audit introduced. Every count in this
section was copied from a command's output.

This statement supersedes §22 for anything spanning the four specs. §12,
§14, §16, §18, §20 and §22 remain as signed at the time.

— Researcher session, 2026-11-09 (signed after the REVIEW-15 §2 scrub and Q5.6 amendment)

---

## 25. Floating-point evaluation-order audit of all published specs (REVIEW-15 §5, REVIEW-17 §2)

The audit covers every crossed spec. Its scope:

- evaluation order and association, wherever they change the bits;
- literal precision;
- conversions between float and double;
- any platform math function whose result the platform does not specify
  bit-exactly;
- per REVIEW-17, the **prose relations** of every `verified`
  floating-point claim, checked against the bytecode, not only its values.

Every amendment is an addition directly under the claim it amends, headed
"Floating-point amendment (2026-11-09, REVIEW-15 §5)". Each carries the
rule-14a fields.

### Evidence base

- **Bytecode,** read this session, for every row.
- **Driver** (private; recorded in `PROVENANCE.md`), run against the
  unmodified jar: 408 685 comparisons, 0 mismatches. Sections:
  - A: deep-dark thresholds;
  - B: barrier-predicate association, decided by the real aquifer;
  - C: the engine's clamped lerp at `t = 1`;
  - D: fade order;
  - E: the above-preliminary-surface interpolation, via the real context;
  - F: platform power of two.
- **The REVIEW-15 §2 driver** (Q5.6, 852 decisions), earlier the same day.

### Table

"Stated" refers to the published form before this audit: **yes** (the
engine's order is stated, or the published form is already bit-exact),
**ambiguous** (more than one reading, differing in bits) or **wrong**.

| spec | claim | formula (neutral notation) | stated | action | evidence |
|---|---|---|---|---|---|
| aquifer | Q5.6 | `φ` ramp, `θ` thresholds, `n − θ > 0` | yes (REVIEW-15 §2 amendment) | spot-checked | REVIEW-15 §2 driver: 852/852 |
| aquifer | Q5.7 | `F = ⌊(10.0·s)/3.0⌋` | yes (REVIEW-14 width amendment) | spot-checked | bytecode |
| aquifer | Q5.9 | deep-dark thresholds | **wrong** (literal precision) | amended | driver A: 64/64; the decimal doubles wrong on 24/64 |
| aquifer | Q5.10 | sample `(cx·xz_scale, cy·y_scale, cz·xz_scale)` | yes | spot-checked | bytecode |
| aquifer | Q6.1 | `1.0 − (q−p)/25.0` | yes | spot-checked | bytecode; REVIEW-14 driver A |
| aquifer | Q6.4 | pressure branches; `Π = 2.0·(b + u)` | yes | spot-checked | bytecode. The engine forms `0.0 + t` before dividing; that differs from `t` only in the sign of a zero, which cannot change `D + s·Π > 0` |
| aquifer | Q6.6 | `(s₁₂·s₁₃)·Π`, `(s₁₂·s₂₃)·Π`, then `D + x > 0` | **ambiguous** | amended | driver B: 374 boundary decisions by the jar, all left-to-right |
| aquifer | Q8.2, Q8.4 | `s ≥ θ_flow` comparisons | yes | spot-checked | bytecode; no arithmetic beyond Q6.1 |
| aquifer | Q8.3 | `θ_flow = −0.76` | yes | spot-checked | REVIEW-14 driver A (bitwise) |
| blended | notation | fade, lerp | **ambiguous** (the fade's expanded form differs) | amended | driver D: 200 000/200 000; expanded form wrong on 156 829 |
| blended | Q1.3 | `684.412·scale` | yes | spot-checked | bytecode (one commutative product) |
| blended | Q1.4 | the full composition | yes (REVIEW-15 §2 fields) | spot-checked | bytecode this session |
| blended | Q1.5 | the limit-sum skip | **wrong** (prose relation) | amended | bytecode; driver C: `clamp_lerp(1, a, b) ≠ b` for 8 157/100 000 |
| blended | Q1.6 | wrap | yes | spot-checked | bytecode |
| blended | Q2.1 | divisor and cap products | yes (REVIEW-15 §2 correction) | spot-checked | bytecode |
| blended | Q2.2 | fold, fraction, weight | yes (REVIEW-15 §2 fields) | spot-checked | bytecode |
| blended | Q2.3 | `10⁻⁷`, single-precision, widened | yes | spot-checked | bytecode |
| blended | Q3.2 | sign of the cap tracks the sign of `y` (prose) | yes | spot-checked | bytecode: every factor is positive |
| bandlands | Q4.4 | `round(v·4.0)`, ties toward +∞ | yes | spot-checked | bytecode; the platform round is exactly specified |
| legacy | notation | `D = (b26·2²⁷ + b27)·2⁻⁵³` | yes | spot-checked | bytecode this session |
| legacy | Q2.2 | `(S₁ + S₂(c·x, c·y, c·z))·V` | yes | spot-checked | bytecode this session |
| legacy | Q2.3 | `V = 0.1666…/(0.1·(1 + 1/(Δ+1)))` | yes | spot-checked | bytecode this session |
| legacy | Q2.4 | `(a·O)·w`; doubling and halving; `λ` via the power of two | yes | spot-checked | bytecode; driver F (power of two exact for all exponents tested) |
| legacy | Q2.5 | wrap | yes | spot-checked | bytecode |
| legacy | Q2.6 | fade, lerp, dot order | **ambiguous** (fade) | amended | driver D |
| legacy | Q4.4 | `min ≤ N ≤ max` | yes | spot-checked | run 04 harness F |
| legacy | Q4.5 | `((N·2.75) + 3.0) + J·0.25`, truncated | yes | spot-checked | bytecode this session |
| legacy | Q4.6 | `−1.0909… = −3/2.75` bound (prose) | yes | spot-checked | arithmetic from Q4.5 |
| legacy | Q4.7 | bilinear interpolation with single-precision weights | **unstated order** | spot-checked (no amendment needed) | driver E: 0/3 948 differ under swapped axis order or double weights. Integer corners and `k/16` weights keep every intermediate exact, so any order is bit-exact |
| legacy | Q4.8 | `((N + 1)/2)·r`, truncated | yes | spot-checked | run 04 harness K; engine form equal in bits |

**Totals** (counted from the action column by command): 30 rows, of which
5 were amended and 25 spot-checked.

### Whole-set identifier sweep (rule 14; `AUDIT.md` as its own row, per REVIEW-16)

Counts are copied from the sweep's output on the final text, using the method of §23.

| file | compound class name | JDK class name | camelCase member name | justified (`firstOctave`) | snake-case runtime data | single English word = class name | pattern word |
|---|---|---|---|---|---|---|---|
| `spec/aquifer-spec.md` | 0 | 0 | 0 | 0 | 14 | 11 | 0 |
| `spec/aquifer-constants.json` | 0 | 0 | 0 | 4 | 4 | 0 | 0 |
| `spec/blended-noise-spec.md` | 0 | 0 | 0 | 0 | 0 | 5 | 0 |
| `spec/blended-noise-constants.json` | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| `spec/bandlands-spec.md` | 0 | 0 | 0 | 0 | 2 | 7 | 0 |
| `spec/bandlands-constants.json` | 0 | 0 | 0 | 1 | 9 | 0 | 1 |
| `spec/legacy-named-noise-spec.md` | 0 | 0 | 0 | 21 | 25 | 9 | 0 |
| `spec/legacy-named-noise-constants.json` | 0 | 0 | 0 | 18 | 52 | 0 | 0 |
| `AUDIT.md` (final text, including this section) | 0 | 0 | 0 | 16 | 3 | 38 | 20 |

The classifications are as in §23. The aquifer constants' snake-case
count is again the four aquifer noise registry keys (see correction 2).

### Completeness against every formula claim (checked by command)

Every claim of kind `formula` in the four specs was matched against this
table and §21's table. Two formula claims appear in neither:

- aquifer Q1.2: an integer comparison with an integer `min`;
- aquifer Q5.4: an integer minimum.

Neither involves an operation in REVIEW-14's integer scope (no overflow,
rounding, shift, division or conversion is possible), and neither
involves floating point. Both operate on 32-bit signed values, already
stated through the Q5.2 width amendment that defines `Prelim`. They are
recorded here so that the two tables together account for every formula
claim. No action is needed.

### Platform math functions (REVIEW-10 §3)

The floating-point paths of all four specs use only:

- the power function, with integer arguments: Java's specification
  requires an exactly representable result to be exact. Driver F confirms
  this for every exponent from −1 100 to 1 100, against both the strict
  variant and exact scaling;
- integer and double absolute value, the round function, and min/max: all
  exactly specified.

**No claim depends on behavior specific to a JVM implementation.** No REVIEW-10
§3 flag is needed.

### Prose relations of verified floating-point claims (REVIEW-17 §2)

The verified claims checked against the bytecode for prose relations, not
only values:

- blended Q1.4, Q2.1 (corrected at REVIEW-15 §2), Q2.2 and Q3.2;
- legacy Q2.2–Q2.6, Q4.2–Q4.6 and Q4.8;
- bandlands Q4.4;
- aquifer Q5.6.

**No further false relation was found among them.** The one prose error
this audit found (blended Q1.5) is in a `read-directly` claim.

### Findings

1. **Aquifer Q5.9: wrong literal precision.** Both deep-dark thresholds are
   single-precision literals, widened. The constants values are corrected
   in place, with a `_changelog` entry. This is the project's third
   literal-precision finding, and the second as a published defect.
2. **Aquifer Q6.6: ambiguous association,** settled by the jar itself at 374
   boundary cases.
3. **Blended Q1.5: a false prose relation.** The limit-sum skip is
   behavioral at a selector of exactly `1.0`.
4. **The fade: the factored form is the engine's form.** The expanded
   polynomial is a likely mis-derivation, and the amendments say so.

### Corrections, recorded rather than applied silently

1. **A fade mutant was ill-designed.** The first "alternative association"
   mutant (`s·(s·s)` against `(s·s)·s`) is the same product with its
   operands swapped. It is bitwise identical by construction, so it tested
   nothing. The meaningful hazard, the expanded polynomial, is the result
   reported.
2. **A new constants key collided with a source constant name.** The first
   draft of the aquifer `fp_forms` entry used a key that the whole-set
   sweep flagged. It was renamed before commit.
3. **A completeness claim was first made without a check.** This
   audit's report first asserted that every formula claim appears in this
   table or in §21's. The mechanical check found two that appear in
   neither (above). Both are recorded, and the report is corrected.

---

## 26. Statement

The four specs and their constants express methods, facts and contracts.
They contain no code or pseudocode, no source comments or prose, and no
reflection of the source's class, method or file organization. Every
amendment sits directly under the claim it amends.

**Identifiers.** The whole-set sweep of §23's method, re-run on the final
text of all nine crossed artifacts, `AUDIT.md` included, finds no compound
class name, no JDK class name and no camelCase member name. The per-file
counts are in §25's sweep table.

**Widths and floating point.** Integer widths stand as certified at §22.
Every floating-point formula in the four specs has been audited for
evaluation order, association, literal precision, conversions and
platform-function exactness (§25). Each one now either states the engine's
form or is bit-exact as published. Every `verified` floating-point claim's
prose relations were checked against the bytecode. Every formula claim
amended since REVIEW-15 carries the `widths:` and `fp-form:` fields.

Errors found by this audit, in published claims and in its own drafts, are
recorded in §25, not patched out of the history. Every count in §25 and
§26 was copied from a command's output.

This statement supersedes §24 for anything spanning the four specs. §12,
§14, §16, §18, §20, §22 and §24 remain as signed at the time.

— Researcher session, 2026-11-09 (signed after the REVIEW-15 §5 floating-point audit)

---

## 27. Aquifer Q8.1 amendment: the mark's consequence at promotion (REVIEW-23 §2)

**What changed.** One amendment block, added under aquifer Q8.1. The
original text stands. No other claim changed.

`spec/aquifer-constants.json` did not change, because the amendment
changes no value or key. `grep` finds no key tied to Q8.1, so no changelog
entry is due.

**Why.** The published clause says the mark "causes a fluid tick to be
scheduled when the chunk is loaded". That understates it.

- Read: at promotion to full status, a marked fluid's tick is executed at
  once, and the list is then cleared.
- Measured: under the frozen regime, where no scheduled tick runs, marked
  sources flowed at promotion.

Correction category: an understated consequence in a contract claim.
Reason: the reading and a world-level measurement show the consequence is
immediate execution, not scheduling.

**Basis for the confidence tier.** The tier is
`verified(differential-harness)` for the fluid case. The method was:

1. an unmodified jar, under the frozen regime;
2. a 48×48-chunk square at least 65 chunks from the spawn chunk;
3. a check, from the run's own saved data, that the square lies outside the
   area loaded at startup;
4. a carved world and a no-carver world, compared with an independent
   prediction of generation output.

The marks exercised were the carver's, which carry Q8.1's flag (Q8.5). The
non-fluid case is `read-directly` and is stated as such in the block.

**Prose check of the verified relations (REVIEW-18).**

| relation stated | evidence | holds |
|---|---|---|
| a marked fluid's tick is executed at promotion | read; under a freeze, all 53 marked sources that had an open neighbour changed at least one neighbour | yes |
| this happens whether or not ticking is frozen | the measurement ran frozen from before any force-load | yes |
| the list is then cleared | 0 of 2 304 chunks in the far square, and 0 of 64 near spawn, were stored with a non-empty list. Predicted non-empty lists: 944 and 28. | yes |
| a one-step flow, or a new source by the fluid's conversion rule | 47 one-step flows. 6 new water sources, each with two horizontal sources beside it and a solid block below, as the conversion rule reads. | yes |
| further scheduled fluid ticks follow | every flowed cell and its source carry a stored fluid tick absent from the no-carver world | yes |

**The residue, resolved (REVIEW-24 §2).** In 3 cells, flowing lava
stands beside lava sources that carry no carver mark. At first this was
unresolved. The fill marks were then computed at driver level, with the
real noise fill and aquifer: all 3 sources carry the noise-fill mark, and
an adjacent control cell does not. The 3 cells are this claim's own
consequence, through fill marks rather than carver marks. The claim's note
was replaced accordingly. Correction category: an open residue in a crossed
claim's evidence, closed by measurement.

**Rule 14a.** Q8.1 is of kind `contract`, not `formula`, so the field check
does not apply. It was run anyway: 0 touched claims fail.

**Correction to my own record (rule 20).** The REVIEW-22 report said every
difference was accounted for. It classified the block and post-processing
channels, but not the 3 chunks whose fluid ticks differed. They are now
classified: all 6 entries are world-only ticks at the 3 flowed cells and
their 3 sources.

### Whole-set identifier sweep (rule 14; `AUDIT.md` as its own row)

All counts are copied from the sweep's output on the final text, using the
method of §23. The columns are as in §25.

| file | compound class name | JDK class name | camelCase member name | justified (`firstOctave`) | snake-case runtime data | single English word = class name | pattern word |
|---|---|---|---|---|---|---|---|
| `spec/aquifer-spec.md` | 0 | 0 | 0 | 0 | 14 | 11 | 0 |
| `spec/aquifer-constants.json` | 0 | 0 | 0 | 4 | 4 | 0 | 0 |
| `spec/blended-noise-spec.md` | 0 | 0 | 0 | 0 | 0 | 5 | 0 |
| `spec/blended-noise-constants.json` | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| `spec/bandlands-spec.md` | 0 | 0 | 0 | 0 | 2 | 7 | 0 |
| `spec/bandlands-constants.json` | 0 | 0 | 0 | 1 | 9 | 0 | 1 |
| `spec/legacy-named-noise-spec.md` | 0 | 0 | 0 | 21 | 25 | 9 | 0 |
| `spec/legacy-named-noise-constants.json` | 0 | 0 | 0 | 18 | 52 | 0 | 0 |
| `AUDIT.md` (final text, including this section) | 0 | 0 | 0 | 18 | 3 | 42 | 23 |

The rows for the eight spec files are unchanged from §25. Every change
this round is in `AUDIT.md`'s own row, and all of it is ordinary prose:

- the single-word column grew by 4: "Aquifer" twice and "Rule" twice
  (headings, and this note quoting them), all in that class already;
- the pattern-word and justified columns grew because this table's header
  repeats the words "camelCase" and `firstOctave`.

---

## 28. Statement

The four specs and their constants express methods, facts and contracts.
They contain no code or pseudocode, no source comments or prose, and no
reflection of the source's class, method or file organization. Every
amendment sits directly under the claim it amends.

**Identifiers.** The whole-set sweep of §23's method, re-run on the final
text of all nine crossed artifacts, `AUDIT.md` included, finds no compound
class name, no JDK class name and no camelCase member name. The counts are
in §27's table.

**This round.** One amendment: aquifer Q8.1 (§27). Its prose relations
were checked against the measurement and the reading. It is a `contract`
claim, so it needs no `widths:` or `fp-form:` field. The field check
confirms that no touched claim fails. Widths and floating point stand as
certified at §22 and §26.

Every count in §27 and §28 was copied from a command's output. This
statement supersedes §26 for anything spanning the four specs. §12, §14,
§16, §18, §20, §22, §24 and §26 remain as signed at the time.

— Researcher session, 2026-11-09 (signed after the REVIEW-23 §2 amendment)

---

## 29. Run 05 Part A: `carvers-a-spec.md` and `carvers-a-constants.json` (REVIEW-19 to REVIEW-25)

**What crosses with this set:**

- `spec/carvers-a-spec.md` (new; 49 claims);
- `spec/carvers-a-constants.json` (new);
- the held aquifer Q8.1 amendment, as updated under REVIEW-24 §2 (§27);
- this section and §30.

### Filtration (RESEARCH.md §3, §6)

- **Organization** follows the brief's §3 question order: Q1–Q9, then Q11–Q13. Q10 and the canyon are Part B (REVIEW-19 §1). No section mirrors the source's layout.
- **No code or pseudocode.** The new spec has 0 code fences and 0 numbered-procedure lines, counted by command. Walk dynamics are stated as recurrences: state after a step as a function of state before it.
- **Purity.** Every quantity is a function of the world seed, the target chunk, the start chunk, the list position and pack fields. There are two exceptions, both stated as observable contracts with the complete order needed to reproduce them (REVIEW-21 §1):
  - the mark history (Q12.4);
  - the mask's effect (Q11.4).
- **Every claim carries kind, confidence and verify-by:** 49 of 49.
  - kinds: 18 `formula`, 26 `contract`, 3 `constant`, 2 `open-question`;
  - all counts are from the field check's output.
- **Dead and live paths (rule 16):**
  - launch-time debug switches: `DO-NOT-IMPLEMENT`, with a reachability argument (Q13.5);
  - pack-enabled debug: `open-question`, live and not dead code (Q13.4);
  - upgraded-chunk behaviour: `open-question`, live and not dead code (Q13.6).
- **Downstream policy (rule 17):** the two `open-question` items go to the owner. The stored-versus-read fact in Q12.7 is stated as a fact, per REVIEW-25 §2, with no policy.
- **Implement-exactly guidance (REVIEW-21 §2, REVIEW-25 §3):**
  - the early-out: speed-only by a stated argument (Q8.2);
  - start and list execution order (Q11.1);
  - the sampling height (Q11.2);
  - the mask's effect (Q11.4).

  Each states that its verifying mutant was inert at the sizes run.

### Rule 14a field check

Run on the whole new spec, not only touched claims, because the file is
new. 18 of 18 `formula` claims carry both `widths:` and `fp-form:`, and 0
fail. Every formula combining integers states width, signedness and
truncation or wrap:

- Q1.1, Q1.2, Q1.4, Q1.5;
- Q3.1, Q3.2;
- Q4.1–Q4.3;
- Q7.1;
- Q9.1;
- Q12.1, Q12.6;
- Q13.3.

Formulas with no integer operation say so: Q1.3, Q3.3, Q3.4, Q6.1.

### Prose-relation check of every `verified` claim (REVIEW-18 §2)

43 claims carry a `verified` tag: 40 as their primary tag, and Q11.4, Q13.1
and Q13.2 as a secondary tag (counted by command). Each relation was
checked against what was executed.

| claims | evidence actually executed | relations checked |
|---|---|---|
| Q1.1–Q1.5, Q2.1, Q2.2, Q3.1–Q3.5, Q6.1, Q6.3, Q7.2, Q9.1–Q9.3 | the driver against the unmodified carvers: draw-for-draw and walk-state bitwise; 9 028 435 comparisons, 179 699 carve calls | the draw order, every conversion point, forks' draw interleaving and the room's missing multipliers, all read against the bytecode-level reference. Q2.2's "independent of target" holds because each target matched the same target-independent reference sequence. |
| Q4.1–Q4.5, Q7.1, Q8.1 | cells and mask bits of 105 driver targets; reach mutant (1 of 35); order mutant (11 of 35) | strictness of each test, the floor cut, the descending y, the exclusive lower y bound |
| Q7.3, Q11.2 (horizontal), Q11.3, Q11.5, Q11.6, Q12.1–Q12.6, Q12.8, Q13.1–Q13.3 | carving-stage acceptance, 4 squares, 4 736 chunks, 0 differences, with stored reads; grass mutant (165 cells); horizontal biome mutant (4 007 cells) | lava cut-off precedence, water and lava both exercised, barrier leaves the cell, unconditional top-material mark, no ticks at carving, the nether's unread `lava_level` |
| Q11.4 (mask bits only), Q12.7 (recorded case), Q13.5 | as cited in each claim | Q12.7 claims `verified` only for the recorded case; the read rule is `read-directly`. Q13.5: the flags are not compile-time constants (bytecode), and both launch states were measured. |

**Wording found and fixed by this check, before commit** (rule 13: category and reason only):

1. **Q4.3:** a sentence about the order of two side-effect-free exclusion tests. Category: source-structure narration with no behavioural content (rule 12). Removed.
2. **Q4.4:** a word that collided with the post-processing sense of "mark". Category: an ambiguous term in a verified relation. Reworded.

### Other corrections to my own drafts, before commit (rule 20)

1. **Q12.3:** a mark condition was stated wrongly as flag-gated; the top-material mark is unconditional. Found by a line-by-line check against the source.
2. **Q13.4:** lava at or below the lava height was stated as debug-replaced; it is not.
3. **A start count and a chunk count** were first derived by hand and were wrong. Both were recomputed by command (273 371 starts; 4 736 chunks).
4. **A constants key I wrote coincided with a source constant name.** The sweep caught it, and it was renamed before commit.
5. **A generation-height symbol** in the new spec collided with the aquifer spec's density symbol. Renamed.

### Whole-set identifier sweep (rule 14; `AUDIT.md` as its own row)

The method is §23's. The justified list now also holds `yScale`, a pack
field name: the literal key in the shipped configured carvers, permitted by
rule 15. Counts are copied from the sweep's output on the final text.

| file | compound class name | JDK class name | camelCase member name | justified (`firstOctave`, `yScale`) | snake-case runtime data | single English word = class name | pattern word |
|---|---|---|---|---|---|---|---|
| `spec/aquifer-spec.md` | 0 | 0 | 0 | 0 | 14 | 11 | 0 |
| `spec/aquifer-constants.json` | 0 | 0 | 0 | 4 | 4 | 0 | 0 |
| `spec/blended-noise-spec.md` | 0 | 0 | 0 | 0 | 0 | 5 | 0 |
| `spec/blended-noise-constants.json` | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| `spec/bandlands-spec.md` | 0 | 0 | 0 | 0 | 2 | 7 | 0 |
| `spec/bandlands-constants.json` | 0 | 0 | 0 | 1 | 9 | 0 | 1 |
| `spec/legacy-named-noise-spec.md` | 0 | 0 | 0 | 21 | 25 | 9 | 0 |
| `spec/legacy-named-noise-constants.json` | 0 | 0 | 0 | 18 | 52 | 0 | 0 |
| `spec/carvers-a-spec.md` | 0 | 0 | 0 | 5 | 27 | 18 | 1 |
| `spec/carvers-a-constants.json` | 0 | 0 | 0 | 3 | 64 | 3 | 1 |
| `AUDIT.md` (final text, including this section) | 0 | 0 | 0 | 21 | 3 | 49 | 28 |

- **Snake-case runtime data in the new files:** registry keys and block ids, tag names, and pack field names.
- **Pattern words in the new files:** one each, `xB`, a named false positive from the hexadecimal literal `0xB`.
- **Single English words in the new files:** prose words that are also class names, the same class as before.
- **`AUDIT.md`'s own row changed only through this section's prose:** the table header's words, this note's quotation of the `0xB` false positive, the two justified field names, and English words that are also class names.

---

## 30. Statement

No content in the five specs reproduces expression from the source. All
claims are methods, facts or contracts. They contain no code or
pseudocode, no source comments or prose, and no reflection of the source's
class, method or file organization. Every amendment sits directly under the
claim it amends.

**Identifiers.** The whole-set sweep of §23's method, re-run on the final
text of all eleven artifacts that cross, `AUDIT.md` included, finds no
compound class name, no JDK class name and no camelCase member name. The
hits that remain are justified pack field names, runtime data, or the named
false positive (§29).

**Fields and relations.** Every formula claim in `carvers-a-spec.md` carries
`widths:` and `fp-form:`. Every `verified` claim's prose relations were
checked against what was executed (§29). Widths and floating point in the
four earlier specs stand as certified at §22 and §26.

Every count in §29 and §30 was copied from a command's output. This
statement supersedes §28 for anything spanning the five specs. §12, §14,
§16, §18, §20, §22, §24, §26 and §28 remain as signed at the time.

— Researcher session, 2026-11-09 (signed after run 05 Part A)

---

## 31. Run 05 Part B: `carvers-b-spec.md` and `carvers-b-constants.json`; two Part A amendments (REVIEW-26)

**What crosses with this set:**

- `spec/carvers-b-spec.md` (new; 20 claims);
- `spec/carvers-b-constants.json` (new);
- **two amendments to `spec/carvers-a-spec.md`**, original text kept:
  - **A-Q1.5:** a false bound. "`max` exclusive" can be reached by rounding. Correction category: a false prose relation in an approved claim; the formula is unchanged. Found by my own Part B range check.
  - **A-Q13.4:** ruled (REVIEW-26 §2), and its verify-by executed.
- this section and §32.

### Filtration

- **Organization** follows the brief: the canyon deltas to Q1, Q3, Q4 and Q7, then Q10, then the Q11 and Q13 findings. Shared mechanisms are cited as `A-Qn.m`, not restated.
- **No code or pseudocode:** 0 code fences and 0 numbered-procedure lines, by command.
- **Purity:** every canyon quantity is a function of the world seed, the target, the start, the list position and pack fields. The canyon adds no history dependence beyond Part A's (A-Q12.4), through its fill (Q10.8).
- **Every claim carries kind, confidence and verify-by:** 20 of 20 (15 `contract`, 5 `formula`), by command.
- **No new open question.** Q13.4 is ruled and verified. Q13.6 is ruled out of scope and stays as written (REVIEW-26 §2).

### Rule 14a field check

- **Part B:** 5 of 5 formula claims carry `widths:` and `fp-form:`; 0 fail.
  - Integer-combining: Q1.2 (sums; float-to-int truncation), Q3.2 (index conversions), Q4.1 (table index), Q10.5 (layer index and draw bound).
  - No integer operation: Q1.3.
- **The A-Q1.5 amendment** (a formula) carries both fields.

### Bit-level and acceptance evidence (counts copied from the run logs by command)

- **Bitwise** (the recording subclass of the unmodified canyon, against the reference; near and far overworld squares):
  - 7 991 canyon starts;
  - 101 822 cross-section calls;
  - 3 577 654 comparisons (five arguments per call, plus every width-table entry);
  - **0 mismatches**.
- **Non-interference:** 232 783 872 cell and mask positions compared, **0 differences**.
- **Sensitivity:**
  - swapping two per-step draws: 8 764 bitwise mismatches and 2 018 cells;
  - a width-table index off by one: 1 541 cells and 8 chunks' marks.
- **Carving-stage acceptance**, with the pack keeping every shipped carver, stored reads and rule 9 unamended: 4 736 chunks, 281 362 starts, 5 478 432 changed cells, **0 fatal, 0 tolerated**.
- **Debug pack:** overworld 0 differences over 106 902 changed cells; nether 0 differences, identical to the non-debug run.
- **Regression:** Part A's near-square run, repeated with the extended reference, reproduced its counts exactly.

### Prose-relation check of every `verified` claim (REVIEW-18 §2)

20 claims carry a `verified` tag: 19 as their primary tag, plus Q13.1 as a secondary tag (counted by command). The relations were checked against what was executed:

| claims | evidence | relations checked |
|---|---|---|
| Q1.1–Q1.3, Q3.1, Q3.2, Q10.3–Q10.7, Q10.9 | bitwise canyon run | draw order within a step; the `(m·v⁰)·q` association; the float-to-int length truncation; `L ∈ [84, 112]` (112 reachable, computed); table layer correspondence and value range `[1, 4)` |
| Q4.1, Q7.1, Q10.1, Q10.2 (shipped), Q10.8, Q10.10 | carving-stage acceptance; index mutant | no floor cut; index range `[1, H − 9]`; fill identical to the overworld cave |
| Q11.1 | nether acceptance (bare-string lists); debug pack (inline entries) | a bare string is equivalent to a one-element list; inline entries keep their list position |
| Q13.1 (shipped values), Q13.2 | acceptance; debug pack | the nether's null debug effect is stated as measured-identical plus inference, not as a sensitive test |

**Wording fixed by this check, before commit:** none in Part B. In Part A, one false bound (A-Q1.5, above).

### Corrections to my own drafts, before commit (rule 20)

1. **The canyon length range was first written as `[84, 111]`, and the step bound as 110.** An exhaustive check showed that `distance_factor` rounds up to exactly `1.0` on 2 of the 2²⁴ draws. Corrected to `[84, 112]` and 111. The same check exposed A-Q1.5's false bound.
2. **The prediction's carver-list parser assumed a list.** The shipped data uses bare strings for single carvers. Fixed in the driver before any Part B result. It became claim Q11.1.

### Whole-set identifier sweep (rule 14; `AUDIT.md` as its own row)

The method is §23's, with the justified list as in §29. Counts are copied from the sweep's output on the final text.

| file | compound class name | JDK class name | camelCase member name | justified (`firstOctave`, `yScale`) | snake-case runtime data | single English word = class name | pattern word |
|---|---|---|---|---|---|---|---|
| `spec/aquifer-spec.md` | 0 | 0 | 0 | 0 | 14 | 11 | 0 |
| `spec/aquifer-constants.json` | 0 | 0 | 0 | 4 | 4 | 0 | 0 |
| `spec/blended-noise-spec.md` | 0 | 0 | 0 | 0 | 0 | 5 | 0 |
| `spec/blended-noise-constants.json` | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| `spec/bandlands-spec.md` | 0 | 0 | 0 | 0 | 2 | 7 | 0 |
| `spec/bandlands-constants.json` | 0 | 0 | 0 | 1 | 9 | 0 | 1 |
| `spec/legacy-named-noise-spec.md` | 0 | 0 | 0 | 21 | 25 | 9 | 0 |
| `spec/legacy-named-noise-constants.json` | 0 | 0 | 0 | 18 | 52 | 0 | 0 |
| `spec/carvers-a-spec.md` | 0 | 0 | 0 | 5 | 30 | 21 | 1 |
| `spec/carvers-a-constants.json` | 0 | 0 | 0 | 3 | 64 | 3 | 1 |
| `spec/carvers-b-spec.md` | 0 | 0 | 0 | 4 | 2 | 12 | 0 |
| `spec/carvers-b-constants.json` | 0 | 0 | 0 | 1 | 1 | 3 | 0 |
| `AUDIT.md` (final text, including this section) | 0 | 0 | 0 | 23 | 3 | 63 | 32 |

The remaining hits are of the classes named in §29: runtime data, justified pack field names, and English words. The named false positive is `xB`, from `0xB`.

---

## 32. Statement

No content in the six specs reproduces expression from the source. All
claims are methods, facts or contracts. They contain no code or
pseudocode, no source comments or prose, and no reflection of the source's
class, method or file organization. Every amendment sits directly under the
claim it amends.

**Identifiers.** The whole-set sweep of §23's method, re-run on the final
text of all thirteen artifacts that cross, `AUDIT.md` included, finds no
compound class name, no JDK class name and no camelCase member name (§31).

**Fields and relations.** Every formula claim in `carvers-b-spec.md`, and
the A-Q1.5 amendment, carries `widths:` and `fp-form:`. Every `verified`
claim's prose relations in Part B were checked against what was executed
(§31). Part A stands as certified at §30, as amended by the two amendments
recorded in §31.

Every count in §31 and §32 was copied from a command's output. This
statement supersedes §30 for anything spanning the six specs. §12, §14,
§16, §18, §20, §22, §24, §26, §28 and §30 remain as signed at the time.

— Researcher session, 2026-11-09 (signed after run 05 Part B)
