# `tools/`

Non-C++ helper scripts: fixture fetching, schema sync, CI glue, repo lint.

| Tool | Milestone | Purpose |
|------|-----------|---------|
| `lint/check-determinism.sh` | M0 (present) | Repo policy checks a compiler cannot make: banned RNGs, banned float flags, raw `%` / `>>` on signed values |
| `lint/format.sh` | M0 (present) | `clang-format` check / apply over first-party sources |
| `vectors/JavaMathVectors.java` | M1 (present) | Emits `tests/unit/javamath_vectors.inc`: known-answer vectors for `stratum::javamath`, observed from a real JVM. CI re-runs it and fails on any diff |
| `vectors/JavaRandomVectors.java` | M1 (present) | Emits `tests/unit/java_random_vectors.inc`: known-answer vectors for `stratum::rng::JavaRandom` from `java.util.Random`, floats as raw bit patterns |
| `vectors/StrictMathVectors.java` | M1 (present) | Emits `tests/unit/fdlibm_vectors.inc`: `StrictMath.log` known-answer vectors, the oracle for the vendored fdlibm port |
| `fetch-vanilla` | M1 (partial) | Resolves and verifies the pinned version through Mojang's piston-meta manifest (manifest → metadata SHA-1 → jar SHA-1), downloads or accepts a server jar, and extracts `data/minecraft/worldgen/**`. Golden region generation by running the server headlessly is still to come, and refuses loudly meanwhile |
| `mcdoc-sync` | M2 | Vendors the mcdoc (Spyglass) schema snapshot matching the pinned version, for load-time validation |

## Provenance

Nothing these tools download or generate may be committed (SPEC §12). The
repo ships scripts; the user supplies the jar. See `.gitignore`, which
excludes `.fixtures/`, `*.jar`, `*.mca` and extracted worldgen JSON.
