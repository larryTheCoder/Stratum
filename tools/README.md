# `tools/`

Non-C++ helper scripts: fixture fetching, schema sync, CI glue, repo lint.

| Tool | Milestone | Purpose |
|------|-----------|---------|
| `lint/check-determinism.sh` | M0 (present) | Repo policy checks a compiler cannot make: banned RNGs, banned float flags, raw `%` / `>>` on signed values |
| `lint/format.sh` | M0 (present) | `clang-format` check / apply over first-party sources |
| `vectors/JavaMathVectors.java` | M1 (present) | Emits `tests/unit/javamath_vectors.inc`: known-answer vectors for `stratum::javamath`, observed from a real JVM. CI re-runs it and fails on any diff |
| `vectors/JavaRandomVectors.java` | M1 (present) | Emits `tests/unit/java_random_vectors.inc`: known-answer vectors for `stratum::rng::JavaRandom` from `java.util.Random`, floats as raw bit patterns |
| `vectors/StrictMathVectors.java` | M1 (present) | Emits `tests/unit/fdlibm_vectors.inc`: `StrictMath.log` known-answer vectors, the oracle for the vendored fdlibm port |
| `vectors/XoroshiroVectors.java` | M1 (present) | Emits `tests/unit/xoroshiro_vectors.inc` from two JDK oracles: `jdk.internal.random.Xoroshiro128PlusPlus` seeded to an exact state, and `RandomSupport.mixStafford13`. Declares its own `--add-exports` flags on a `// JAVA_FLAGS:` line |
| `vectors/generate-md5-vectors.sh` | M1 (present) | Emits `tests/unit/md5_vectors.inc` from `md5sum`, covering RFC 1321's suite, the padding boundaries, and the `octave_*` salts vanilla seeds noise with |
| `vectors/generate-noise-vectors.sh` + `noise_vectors.c` | M1 (present) | Emits `tests/unit/noise_vectors.inc` from cubiomes (MIT) at a pinned commit — fetched, never vendored. Covers the bounded draw, Perlin init and sampling, simplex, octave and normal noise |
| `vectors/generate-climate-vectors.sh` + `climate_vectors.c` | M2 (present) | Emits `tests/conformance/climate_vectors.inc` from the same pinned cubiomes commit, one layer up: the world-seed-to-named-noise derivation, and the overworld's 2D density chain — shift_a/shift_b, flat_cache, shifted_noise and the offset spline. Carries a float-throughout spline reading alongside cubiomes' own, because the two disagree |
| `fetch-vanilla` | M1 (present) | Resolves and verifies the pinned version through Mojang's piston-meta manifest (manifest → metadata SHA-1 → published jar SHA-1 → nested jar SHA-256), downloads or accepts a server jar, and extracts `data/minecraft/worldgen/**`. `--with-structures` also extracts the 1202 vanilla structure `.nbt` files. `--generate-regions --accept-eula` runs the vanilla server headlessly to produce golden region files for the fixed seed set — frozen, with carvers and features stripped, and with a margin so every chunk reaches full status (SPEC §7) |
| `mcdoc-sync` | M2 (present) | Fetches SpyglassMC/vanilla-mcdoc (MIT) at a pinned commit, reads the density-function and noise-settings schemas, filters them to the pinned Minecraft version, and generates the C++ tables the loader validates against. Fetched, never vendored. The reader is strict: an mcdoc construct it cannot map raises rather than being skipped, because a silently incomplete table would reject datapacks vanilla accepts. Deriving the noise router's field list is not ceremony — at 1.21.9 `initial_density_without_jaggedness` became `preliminary_surface_level`, and a hand-written list would have refused every vanilla settings file |

## Provenance

Nothing these tools download or generate may be committed (SPEC §12). The
repo ships scripts; the user supplies the jar. See `.gitignore`, which
excludes `.fixtures/`, `*.jar`, `*.mca` and extracted worldgen JSON.
