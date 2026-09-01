# Stratum

A C++20 world generation engine for [PocketMine-MP](https://pmmp.io) that
executes **vanilla Java Edition's data-driven worldgen JSON** — density
functions, noise settings, multi-noise biome sources and surface rules — so
that Java-ecosystem datapacks and authoring tools (Misode's generators,
Snowcapped, Spyglass) work as content for a Bedrock server.

- **Schema pin:** Java Edition **1.21.11**, data pack format **94.1**
- **Parity goal:** bit-exact terrain, biome and surface output versus vanilla
  Java for the same JSON and seed (Tier A, see below)
- **Status:** Milestone **M0** — scaffolding. The engine does not generate
  anything yet; every CLI subcommand fails loudly with the milestone that
  owns it.

`SPEC.md` is the single source of truth for scope and architecture;
`CLAUDE.md` defines how changes are made. Read both before contributing.

## Build and test

```bash
cmake --preset dev            # configure (Debug; no generator is pinned)
cmake --build --preset dev    # build lib/, cli/ and the test suites
ctest --preset dev            # unit + repo-policy lint suites
```

Conformance runs separately, because it needs fixtures that are generated
locally and never committed:

```bash
tools/fetch-vanilla --version 1.21.11   # Milestone M1 — not implemented yet
ctest --preset conformance              # skips loudly while fixtures are absent
```

| Preset | Purpose |
|--------|---------|
| `dev` | Debug, warnings visible but not fatal |
| `release` | RelWithDebInfo; identical determinism flags to `dev` |
| `ci-debug` / `ci-release` | As above with `-Werror` / `/WX` |

Requirements: CMake ≥ 3.24, a C++20 compiler (GCC 13+, Clang 16+, MSVC 2022),
and network access on first configure so Catch2 v3 can be fetched (or install
Catch2 ≥ 3 yourself and it will be used instead).

Lint locally the way CI does:

```bash
tools/lint/check-determinism.sh   # also runs as the ctest `lint.determinism` case
tools/lint/format.sh --fix        # clang-format 18.1.8, pinned
```

## Repository layout

```
lib/       core static library — pure C++, never includes PHP/zend headers
  mapping/ Java -> Bedrock block-state and biome mapping (M5, own tests)
cli/       render / generate / diff / validate, built on lib/
ext/       zend binding for PocketMine-MP (M5; marshaling only)
tools/     lint, fixture fetching, schema sync, CI glue
tests/     Catch2 v3 unit suites; conformance driven via `cli diff` + CTest
```

## Capability matrix (v1)

This matrix is part of the public contract (SPEC §8). A pack that
half-loads silently is a bug of the highest severity class: anything outside
the supported column is a **hard error at load, naming the registry or node
that was rejected**.

| Registry / feature | v1 | Notes |
|---|---|---|
| `worldgen/density_function` | Supported | Including all cache node types |
| `worldgen/noise` | Supported | |
| `worldgen/noise_settings` | Supported | Noise router, surface rules, spawn targets |
| Multi-noise biome source | Supported | |
| `dimension`, `world_preset` | Partial | Only what is needed to select noise settings |
| Tags and namespaced references | Supported | Within the registries above |
| `configured_feature`, `placed_feature` | **Rejected** | v2 (SPEC §10, M6) |
| `configured_carver` | **Rejected** | v2 |
| `structure`, `structure_set` | **Rejected** | v2 |
| `template_pool`, `processor_list` | **Rejected** | v2 |
| `flat_level_generator_preset` | **Rejected** | Not planned for v1 |

### Parity tiers

| Tier | Scope | Bar |
|---|---|---|
| A | Density functions, noise, terrain shape, aquifer fill decision, multi-noise biome assignment, surface rules | Bit-exact vs vanilla Java, block-for-block, in Java block space |
| B (v2) | Carvers, features, structures | Statistical parity; exact placement may differ |
| U | Everything outside the matrix | Hard error at pack load, never a silent skip |

## Determinism

Same (pipeline, seed, chunk) must produce identical bytes on every supported
platform, architecture and compiler (SPEC §5). In practice that means: all
seed arithmetic in `uint64_t` with explicit wrapping, Java-semantics
`floorDiv`/`floorMod`/`>>>` helpers instead of raw `%` and `>>`, only the
project's Java LCG and Xoroshiro128++ seeded per `(worldSeed, position,
salt)`, `-ffp-contract=off` / `/fp:precise`, and never `-ffast-math`.

CI enforces this on x86-64 **and** ARM64 across Linux, Windows and macOS; a
cross-architecture divergence is a build failure, not noise.
`tools/lint/check-determinism.sh` catches the parts a compiler cannot see.

## Provenance

This repository ships **no Mojang data and no Mojang source code**, in any
form (SPEC §12). Vanilla worldgen definitions and conformance fixtures are
produced on your machine by `tools/fetch-vanilla` from an official jar you
download yourself; they are gitignored and never redistributed here.
Behaviour is implemented from minecraft.wiki / datapack.wiki documentation,
mcdoc schemas, licensed references (cubiomes, MIT; Cuberite, Apache-2.0) and
the observed input/output of the vanilla server.

## Licence

Apache License 2.0 — see [LICENSE](LICENSE) and [NOTICE](NOTICE).

"Minecraft" is a trademark of Mojang AB. This project is not affiliated with,
endorsed by, or associated with Mojang AB or Microsoft.
