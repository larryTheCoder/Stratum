# `tools/`

Non-C++ helper scripts: fixture fetching, schema sync, CI glue, repo lint.

| Tool | Milestone | Purpose |
|------|-----------|---------|
| `lint/check-determinism.sh` | M0 (present) | Repo policy checks a compiler cannot make: banned RNGs, banned float flags, raw `%` / `>>` on signed values |
| `lint/format.sh` | M0 (present) | `clang-format` check / apply over first-party sources |
| `fetch-vanilla` | M1 | Downloads the official server jar via Mojang's piston-meta manifest, extracts `data/minecraft/worldgen/**`, runs the vanilla data generator, and produces region files for the fixed test seed set by running the server headlessly |
| `mcdoc-sync` | M2 | Vendors the mcdoc (Spyglass) schema snapshot matching the pinned version, for load-time validation |

## Provenance

Nothing these tools download or generate may be committed (SPEC §12). The
repo ships scripts; the user supplies the jar. See `.gitignore`, which
excludes `.fixtures/`, `*.jar`, `*.mca` and extracted worldgen JSON.
