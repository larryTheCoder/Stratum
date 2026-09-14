# `ext-nukkit/` — CloudburstMC/Nukkit JNI binding

Milestone **M5** (SPEC §9). Interface defined and compiling; not yet a
working generator — see "What is not here yet" below.

## Why JNI, and why a real second target rather than a stub

Nukkit targets **Java 8** (`build.gradle.kts`: `JavaLanguageVersion.of(8)`,
confirmed by reading it directly) — Java's modern Foreign Function & Memory
API needs JDK 22+, so it is not available here. JNI is the only real option
at that floor, and it is a proven-safe one specifically for this codebase:
Nukkit already ships `leveldbjni` as a production dependency, so this is not
asking the project to adopt something foreign.

Unlike `ext/` (still a pure stub — PHP/zend headers are not available here
and are not this binding's concern), `ext-nukkit/` is a real, compiling,
tested CMake target today, split in two:

- `stratum_nukkit_pipeline` — plain C++ (`include/stratum_nukkit/pipeline.hpp`,
  `src/pipeline.cpp`). No JVM, no JNI, no `jni.h` needed to build or test it;
  `tests/unit/pipeline_test.cpp` (wired into `stratum_unit_tests` when this
  target exists) exercises it directly.
- `stratum_nukkit` — the thin JNI shim (`src/jni_bridge.cpp`) `StratumGenerator
  .java`'s native methods actually load. Needs a JDK's `jni.h`
  (`find_package(JNI REQUIRED)`); set `JAVA_HOME` if CMake cannot find one on
  its own.

Build with `-DSTRATUM_BUILD_EXT_NUKKIT=ON`. This also turns
`CMAKE_POSITION_INDEPENDENT_CODE` on globally (`stratum_nukkit` is a shared
library; everything it statically links, `stratum_core` included, needs
`-fPIC` to be linkable into one) — harmless for every other target, but
worth knowing if you are diffing build flags.

## What is landed

- **The `Generator` contract**, confirmed against Nukkit's own source
  directly rather than assumed: `init`/`generateChunk`/`populateChunk`/
  `getSettings`/`getName`/`getSpawn`/`getChunkManager`/`getId`
  (`StratumGenerator.java`'s own header cites the exact file and method
  signatures read).
- **One compiled pipeline per world, not per generation thread.** Nukkit
  gives every async worker its own `Generator` instance via a
  `ThreadLocal` — confirmed, not assumed — so `StratumGenerator` shares one
  native handle across all of them via a `ConcurrentHashMap` keyed by
  `ChunkManager` identity, matching `stratum_nukkit::Pipeline`'s own
  documented immutable-after-compile invariant (the same one
  `terrain::ChunkFiller` already relies on).
- **Biome writing.** `lib::mapping::bedrockBiomeId()` — already built and
  shipped for the PocketMine-MP path — is reused verbatim: Nukkit's own
  `EnumBiome` IDs are confirmed to be Bedrock's own network biome IDs
  directly, no Nukkit-specific scheme in between.
- **A real bug, found by testing rather than assumed absent.** `ChunkFiller
  ::compile()` documents that it stores raw pointers into whatever
  `surfaceRules`/`biomeParameters`/`biomeTemperatures` it is given — an
  early version of `Pipeline::compile()` built those as LOCAL variables and
  moved them into their final storage afterward, leaving `filler` holding
  pointers into a stack frame that no longer existed once `compile()`
  returned. It ran some of the time; `tests/pipeline_test.cpp`'s own
  "wrong-sized output span" case caught it as an out-of-bounds read inside
  `Interpreter::Scope::has`, not as a clean thrown exception. `pipeline.cpp`
  now builds every such member in place, in declaration order, so nothing
  `ChunkFiller::compile` takes a pointer to is ever a value about to be
  moved out from under it — see `Pipeline::Impl`'s own header comment for
  the full account.

## What is not here yet

- **Block state mapping.** `resolveNukkitFullId()` always throws
  `NukkitError`, so `fill()` cannot fill a real chunk yet; what it CAN do
  today is refuse correctly, which is what `tests/pipeline_test.cpp` pins.
  The planned shape (SPEC §9): `lib/mapping/`'s shared Java → Bedrock
  blockstate table, resolved through Nukkit's own `BlockStateMapping`
  (`updateState()` → `BlockStateSnapshot.getLegacyId()/getLegacyData()`) —
  no Nukkit-specific table. That resolver is Java-side, so the JNI boundary
  will likely change: Java builds the id lookup once at `init`, and native
  `fill()` only indexes it. Misses must be detected via `getStateUnsafe()`
  returning null — `getState()` silently substitutes `info_update`.
  Nukkit's palette is keyed by the full `NbtMap` including `version` and is
  at 1.21.30.7, older than the table's 1.21.60.33, so the lookup must match
  on name and states with Nukkit's own version stamped. Measured that way,
  every state vanilla's noise settings emit is present (SPEC §11).
- **The "nearest vanilla Bedrock biome" fallback** for custom/datapack
  biomes outside the generated table (same gap `lib/mapping/`'s own README
  names for the PocketMine-MP path — this binding inherits it rather than
  duplicating the problem).
- **`getBiomeStorage`'s exact per-section coordinate convention** —
  `StratumGenerator.java`'s own header flags this as the one Java-side
  detail not confirmed against a real Nukkit build (no Nukkit dependency
  exists in this repo yet).
- **A real Nukkit dependency and a Gradle/Maven build for the Java side.**
  `StratumGenerator.java` is written to match Nukkit's confirmed API
  precisely, but has never been compiled against real Nukkit classes.
- **Packaging.** Nukkit's own `leveldbjni` precedent turned out to delegate
  native-library loading to an external library rather than showing a
  pattern in Nukkit's own source, so it gave no concrete template to
  follow. Standard JNI packaging (a per-platform native library on
  `java.library.path`) applies instead, unimplemented so far.
- **`populateChunk` is a deliberate, permanent no-op** for as long as M6
  (features/decorations) stays out of scope — not a gap specific to this
  binding.
