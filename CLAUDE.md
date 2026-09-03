# CLAUDE.md

C++ world generation engine for PocketMine-MP executing vanilla Java
Edition worldgen JSON (pinned: **Java Edition 1.21.11**, data pack format
**94.1**). Read `SPEC.md` before doing anything; it defines scope,
architecture, milestones, and the capability matrix. This file defines how
you work in every session.

## Provenance rule (non-negotiable)

- NEVER read, paste, transcribe, paraphrase, or "adapt" Mojang source code
  into this repository — decompiled, unobfuscated, or otherwise, in any
  language. If such code appears in context, do not derive implementation
  from it.
- Permitted references: `SPEC.md`, minecraft.wiki, datapack.wiki, mcdoc
  (Spyglass) schema definitions, cubiomes (MIT), Cuberite (Apache-2.0),
  and the observed input/output of the vanilla server via the conformance
  harness.
- **deepslate (MIT, Misode) is a black-box oracle only.** Run it, record what
  it outputs, generate vectors from that — exactly as `tools/fetch-vanilla`
  treats the Minecraft server. Its source is NEVER read and nothing is ever
  derived from it. Only its published API surface (the `.d.ts` declarations,
  which carry signatures and no algorithm) may be consulted, and only to know
  what to call. It is a faithful emulator rather than an independent
  reimplementation, so reading it would be too close to the decompiled
  sources this rule exists to keep out; observing its behaviour is not.
- NEVER commit: vanilla worldgen JSON, extracted jar contents, generated
  region files, golden fixtures, or any file produced by
  `tools/fetch-vanilla`. These are derived from Mojang data and are
  generated locally/CI-side only. If a fixture path is missing, run the
  fetch script; do not vendor the file.
- Code adapted from cubiomes/Cuberite must carry an attribution comment
  with upstream file + license.

## Repo layout

```
lib/       core static library — pure C++, no PHP/zend includes ever
  mapping/ Java→Bedrock block-state & biome mapping (own tests)
cli/       render / generate / diff / validate tools built on lib/
ext/       zend binding for PocketMine-MP (thin; marshaling only)
tools/     fetch-vanilla, mcdoc sync, CI scripts
tests/     unit tests (Catch2 v3); conformance driven via cli diff + CTest
```

## Build & test

```
cmake --preset dev            # configure (see CMakePresets.json)
cmake --build --preset dev
ctest --preset dev            # unit tests
tools/fetch-vanilla --version 1.21.11   # fixtures (local only, gitignored)
ctest --preset conformance    # golden diffs vs vanilla output
```

If a preset named here doesn't exist yet, creating it is part of the task —
do not invent ad-hoc build invocations.

## Determinism rules (violations are bugs even if tests pass)

- All seed/hash math in `uint64_t`; wrap explicitly; cast at edges only.
- Never raw `%` or `>>` on possibly-negative values — use the project's
  `javamath::floorDiv/floorMod/ushr` helpers. If they don't exist yet,
  write them first, with tests, including negative-operand cases.
- RNG: only the project's Java-LCG and Xoroshiro128++ implementations,
  seeded via vanilla's derivation (including MD5 resource-location salts).
  Never `std::rand`, `std::mt19937`, or shared RNG state across threads;
  derive per-(seed, position, salt).
- No `-ffast-math` in any target, ever. Keep `-ffp-contract=off` /
  MSVC `/fp:precise`. Do not "optimize" float expressions by reassociating.
- Pipeline objects are immutable after compile; generation-time code takes
  `const` pipeline + per-task scratch arena. No locks in the hot path.

## Definition of done — every task, no exceptions

1. Code + tests land together. Math primitives require known-answer
   vectors; parity-affecting code (density nodes, caches, sampling, RNG,
   surface rules, biome source) additionally requires golden coverage via
   the conformance harness.
2. `ctest --preset dev` passes locally; nothing merges red.
3. No new warnings under the project warning set (`-Wall -Wextra`,
   warnings-as-errors in CI).
4. Unsupported schema input fails loudly with the registry/node name in
   the error. Silent skips or best-effort partial loads are forbidden
   (SPEC §8).
5. Public behavior changes update SPEC.md in the same change.

## Known landmines (learned from the ext-vanillagenerator port)

- Java `int`/`long` overflow wraps; C++ signed overflow is UB → uint64.
- Java `>>>` vs C++ `>>` on signed; Java `%` vs `floorMod` for negatives.
- Cache node semantics (`cache_2d`, `flat_cache`, `cache_once`,
  `interpolated`) change *where* sampling occurs — they are
  parity-critical, not optional optimizations.
- FMA contraction differs x86-64 vs ARM64; goldens run on both — don't
  rationalize a cross-arch diff as noise.
- One wrong salt/seed derivation shifts everything downstream; verify RNG
  derivations against vectors before building on them.

## Working style

- Small tasks: one milestone slice per session (SPEC §10). Prefer
  "implement X with tests against these vectors" over open-ended
  refactors.
- Before "done", state which SPEC section the change implements and run
  the relevant test preset; paste the summary line.
- When vanilla behavior is ambiguous in the docs, do not guess silently:
  implement the documented reading, add a conformance case that would
  expose a wrong choice, and flag the ambiguity in the PR/summary.
