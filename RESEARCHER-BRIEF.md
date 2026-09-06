# RESEARCHER BRIEF — Aquifer clean-room specification

Status: proposed · Owner: Potato · Consumes: de-obfuscated Minecraft source
· Produces: `spec/aquifer-spec.md` + `spec/aquifer-constants.json`

You are the **Researcher** in a two-role clean-room process. You run in an
isolated repository with no Stratum code, no Stratum conversation context,
and no shared sessions with the **Implementer** (the Stratum codebase and
its Claude Code sessions). Your entire purpose is to read Mojang's world
generation source for **Java Edition 1.21.11** and produce a *filtered
specification* answering the questions in §4. The specification is the
ONLY artifact that crosses to the Implementer. Everything else you
produce — notes, scratch files, decompiled output — stays in this
repository and is never shared, quoted, or summarized elsewhere.

The legal theory this process relies on: methods, procedures, and
mathematical formulas are not copyrightable; protected *expression*
(code, structure, sequence, organization, naming) is. Your job is to move
the former across the wall and none of the latter. When in doubt about
which side of that line something falls on, leave it out and record the
omission in §6's open-questions list.

---

## 1. Source acquisition (do this first, record everything)

- Download the **final release** 1.21.11 server jar via Mojang's
  piston-meta version manifest. Do NOT use pre-release or release-candidate
  jars: the unobfuscated experimental builds from the 1.21.11 development
  cycle are not the pinned artifact, and near-identical is not identical
  when extracting constants.
- Download the official obfuscation mappings for the same version from the
  same manifest (licensed for development purposes).
- Decompile with a standard toolchain of your choice, applying the
  official mappings.
- Record in `PROVENANCE.md`: jar SHA, mappings SHA, decompiler + version,
  date. The spec's header references this file.
- The decompiled tree, and this entire repository, are **private and never
  published**. Add a README line stating the repo contains Mojang-derived
  material and must not be made public or redistributed.

## 2. Scope

The aquifer system only: everything that decides, for a generated block
below/around the preliminary surface, whether it is air, the default
fluid, lava, or a solid barrier — including the supporting lattice, the
per-cell fluid status computation, the auxiliary noise sampling, the
preliminary-surface interaction, and the global fluid fallback.

Out of scope for this run (each gets its own run of this brief later if
the owner chooses): ore veins, carvers, features, anything else. Do not
"while I'm here" additional systems — scope creep makes the filtration
audit unreviewable.

## 3. Filtration contract (the hard rules)

**Permitted in the output spec:**

- Numeric constants, with type, units, and the role they play.
- Formulas and mappings in mathematical notation (LaTeX-ish or plain
  math), expressed over *our* variable names (§5), never source names.
- Coordinate transforms and quantizations: "noise N is sampled at
  (⌊x/a⌋, ⌊y/b⌋, ⌊z/c⌋)" is math and is permitted.
- RNG derivation facts: derivation style (positional fork, hash-of-string
  seeding), salt strings, and consumption counts *where they affect
  output*.
- Behavioral contracts: "given inputs …, the result satisfies …".
- Data dependencies: which inputs a computed value mathematically depends
  on.

**Forbidden in the output spec:**

- Code or pseudocode in any language. If you are writing something with
  control-flow keywords, indentation-as-structure, or step-numbered
  operations, you are writing pseudocode. Stop and restate it as a
  function definition or a contract.
- Any identifier from the source or the mappings: class, method, field,
  local, or resource-location-as-name — with the single exception of salt
  strings and registry keys that are *runtime data* observable in packs
  or required for RNG derivation.
- The source's decomposition: its class boundaries, method boundaries,
  helper structure, or file organization. Your spec's organization comes
  from §4's question list, full stop.
- Narrated call sequences ("first it computes A, then it calls B"). See
  the purity rule below for the one escape hatch.
- Comments, Javadoc, or any prose from the source.
- Screenshots, excerpts, or attachments of source in any form.

**The purity rule.** Express everything as pure functions of
(world seed, position, pack inputs). Vanilla's caching, laziness, and
memoization are implementation details that are unobservable when the
underlying computations are position-determined — so they must not appear
in the spec. If you find genuinely order-dependent or stateful behavior
that cannot be expressed as a pure function of position and seed, do not
narrate the mechanism: record it in §6 open questions as "output at P
depends on evaluation history via …" with the observable consequence
only, and flag it for experimental confirmation.

**Confidence discipline.** You will be tempted to fill gaps with training
recollection of Minecraft internals. Do not. Every claim in the spec must
be grounded in the decompiled 1.21.11 tree you are reading in this
session. If you did not verify it in this tree, it does not go in the
spec.

## 4. Questions to answer

Organize the spec as one section per question, in this order:

- **Q1 Global picker.** As pure functions of pack parameters (`sea_level`,
  `min_y`, `default_fluid`): what fluid and level apply where the local
  aquifer does not? Where does lava sit and what determines that depth?
- **Q2 Applicability.** Under exactly what conditions does the aquifer
  decide a block at all (density sign, height relative to preliminary
  surface, `aquifers_enabled`)? What happens everywhere else?
- **Q3 Lattice.** Grid cell dimensions in blocks; the per-cell point
  placement (offset ranges per axis); the RNG derivation producing those
  offsets (derivation style, salt string, draw order iff output-visible).
- **Q4 Neighborhood & selection.** Which cells are considered for a given
  block; how many nearest points participate; the distance metric.
- **Q5 Per-point fluid status.** The full pure function from
  (seed, point position, router noises, preliminary surface levels, pack
  params) to (fluid kind, fluid level), including: the floodedness
  mapping and its height dependence; the spread mapping and its level
  quantization; the lava rule; every sampling-coordinate quantization
  involved; how preliminary surface levels cap or modify the status, and
  at which sampled positions surface levels are taken.
- **Q6 Blending & barrier.** The similarity function over the selected
  points; the pressure computation; the barrier noise's role; the exact
  predicate deciding solid-barrier placement vs fluid vs air; the block
  used for the barrier.
- **Q7 Completeness.** Does anything in the aquifer path read state not
  enumerated above (other router fields, biome data, structure data,
  anything)? Enumerate every external input, even if it surprised you.
- **Q8 Interactions.** Anything the aquifer writes or schedules that
  affects other systems within Tier-A scope (e.g., post-placement fluid
  behavior relevant to the stored chunk), described as observable output
  only.

## 5. Output format

`spec/aquifer-spec.md`:

- Header: provenance reference, date, scope, this brief's version.
- One section per question. Each claim is a numbered item:
  - **statement** — math/contract form, using this naming scheme: our
    names, drawn from the pack schema (`fluid_level_floodedness`,
    `sea_level`, …) and neutral math symbols for internals (grid indices
    i,j,k; offsets; thresholds θ with subscripts).
  - **kind** — `constant` | `formula` | `contract` | `open-question`.
  - **confidence** — `read-directly` | `inferred-from-reading` (state the
    inference).
  - **verify-by** — the cheapest confirmation you can propose: an
    ablation experiment shape, a golden-region predicate, or a
    deepslate-oracle probe. Every constant and formula gets one. This
    field is what makes the spec a hypothesis document rather than an
    authority — downstream, nothing enters `lib/` on this spec's word
    alone.

`spec/aquifer-constants.json`: every numeric constant and salt string as
a flat map under our names, mirroring the prose. The Implementer's
harness will consume this file directly.

## 6. Self-audit (mandatory final pass before delivery)

Re-read the spec end to end against the filtration contract and answer in
an `AUDIT.md` (which crosses the wall alongside the spec):

- Zero source/mapping identifiers present (search the spec for camelCase
  and package-like tokens; justify any hit).
- Zero code/pseudocode blocks; zero step-numbered procedures.
- Organization matches §4's question order, not the source layout.
- Every claim carries kind, confidence, and verify-by.
- Open questions and deliberate omissions listed.
- A one-paragraph statement: "No content in the spec reproduces
  expression from the source; all claims are methods, facts, or
  contracts," signed with the session date.

The owner reviews the spec and AUDIT.md only — never this repository's
notes or decompiled tree — and may bounce the spec back with filtration
violations flagged. Fix and re-audit; do not argue edge cases into the
spec.

---

## Appendix A — Stratum amendments (owner commits BEFORE this brief runs)

**SPEC §12**, replace the second bullet with:

> - No Mojang source code — decompiled or unobfuscated — is read, pasted,
>   transcribed, or paraphrased in this repository or by Implementer
>   sessions. Since commit <hash>, behavior may additionally derive from
>   **clean-room specifications**: filtered, audited documents produced by
>   an isolated Researcher process under `RESEARCHER-BRIEF.md`, containing
>   methods, constants, and behavioral contracts only. Prior to that
>   commit, no Mojang source was read anywhere in this project. Clean-room
>   specs are hypotheses: every value they supply still enters `lib/` only
>   through golden verification (§7), and each adopted value's §11 entry
>   cites both the spec claim and the confirming evidence.

**CLAUDE.md provenance rule**, add:

> - Clean-room specs under `spec/` are permitted references. The
>   Researcher process and its repository are not: never request, read,
>   or reconstruct its notes or sources. If spec content appears to
>   contain code, pseudocode, or source identifiers, stop and flag it to
>   the owner instead of using it.
