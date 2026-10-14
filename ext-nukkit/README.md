# `ext-nukkit/` — CloudburstMC/Nukkit binding

Milestone **M5** (SPEC §9, §11's M5-Nukkit entries). A JNI binding: Nukkit's
floor is Java 8 (its `build.gradle.kts`), which rules out the Foreign
Function & Memory API, and Nukkit already ships a native library
(`leveldbjni`). Everything about Nukkit below was read from
CloudburstMC/Nukkit `master` at `b3caad67d` (2026-09-19, Bedrock 26.50), a
permitted M5 reference (CLAUDE.md).

**Status.** The C++ pipeline and the JNI surface are built and tested,
the JNI surface through real JNI in a JVM. The plugin has been compiled only
against hand-written stubs of the Nukkit API it calls (not committed), and
**has never run inside Nukkit**: no Nukkit server jar exists on this machine
yet. `tools/nukkit-dev` fetches the one approved jar once a build is pinned
in it; the headless smoke test that runs the plugin in a real server is
written and skips until then. What that run is expected to settle is listed
under "Open".

## Layout

| Part | Needs | Tested by |
|---|---|---|
| `stratum_nukkit_pipeline` (`include/`, `src/pipeline.cpp`) — plain C++ | nothing but `lib/` | `tests/pipeline_test.cpp`, in `stratum_unit_tests` (`ctest --preset dev`), every platform |
| `stratum_nukkit` (`src/jni_bridge.cpp`) and `java/core/` — the JNI shim and the Java classes with no Nukkit type in them | a JDK (javac, `jni.h`, `ct.sym`), `-DSTRATUM_BUILD_EXT_NUKKIT=ON` | `ext-nukkit.jni`: `java/test/…/NativePipelineCheck.java` in a bare JVM |
| `java/plugin/` — the Nukkit plugin (`stratum-nukkit.jar`) | a Nukkit server jar, `STRATUM_NUKKIT_JAR` | `ext-nukkit.plugin`, `ext-nukkit.smoke` |
| `java/smoke/` — the smoke run's driver plugin | the same | `ext-nukkit.smoke` |

Build and test:

```
JAVA_HOME=$(tools/nukkit-dev --print-java-home) \
STRATUM_NUKKIT_JAR=$(tools/nukkit-dev --print-jar) cmake --preset nukkit
cmake --build --preset nukkit -j 3 && ctest --preset nukkit
```

Without a jar, `--print-jar` fails, `STRATUM_NUKKIT_JAR` is empty, and
`ext-nukkit.plugin` and `ext-nukkit.smoke` skip, saying so. Every class is
compiled `javac --release 8`; locally that is Azul Zulu OpenJDK 25.0.4
(`~/.jdks/azul-25.0.4`), the newest local JDK with headers and `ct.sym` (the
system OpenJDK 25 is a JRE: no `javac`, no `jni.h`). javac warns that 8 is
obsolete and still accepts it.

## How Nukkit drives a generator

- A world names its generator in `level.dat` (`generatorName`) and carries
  one options string (`generatorOptions`). The default world takes them from
  `server.properties`' `level-type` and `generator-settings`; others from
  `nukkit.yml`'s `worlds.<name>.generator: "stratum:<options>"`. Nukkit gives
  a world whose generator is not registered its own `Normal`, silently.
- `Level` keeps one `Generator` per thread in a `ThreadLocal`, constructed
  with the options map. On the server thread `init` is called twice, first
  with the `Level` itself, then with a fresh `PopChunkManager`; on an async
  worker once, with a `PopChunkManager` that carries the seed and nothing
  else. An exception from `init` is logged and the world does not load.
- `GenerationTask`/`PopulationTask` call `generateChunk` on a worker, inside
  a lock on the chunk manager, with the new chunk set into it. Workers are
  `nukkit.yml`'s `settings.async-workers` (`ServerScheduler.WORKERS`).

So `StratumGenerator.init` opens the world on the server thread's first
call, the only one that knows which world it is (`WorldPipeline.open`), and
every later call finds it by seed and options string. Opening is where every
refusal happens, by name, on the thread that loads the world.

## Block states (SPEC §9)

`lib/mapping/`'s table resolves a Java block state to a Bedrock
`{name, states, version}`; Nukkit resolves that to its own full id
(`legacyId << 13 | meta`, what `FullChunk.setFullBlockId` takes). Both
happen once, when the world opens:

1. Native code lists every Java state the world can place
   (`emittableBlockStates`: air, `lava[level=0]`, the default block and
   fluid, the ore-vein blocks when veins are on, every `block` surface rule,
   and the seven terracottas when the rules use `bandlands`) — 35 for the
   overworld, the 35 SPEC §11 measured.
2. Java resolves each through Nukkit's `BlockStateMapping`
   (`NukkitBlockResolver`): the key is the state's name and typed states
   with **Nukkit's own palette version** stamped (read off its default
   state; 1.21.30.7 at `b3caad67d`, where the table is at 1.21.60.33);
   `getStateUnsafe` (never `getState`, which substitutes `info_update`);
   `runtimeToFullId` (never a snapshot's `getLegacyId`, which also falls
   back); and the full id is checked to come back as the same state through
   `legacyToRuntime`, which is how a chunk section stores it.
3. Any miss refuses the world, listing each state with its Bedrock form and
   why. Otherwise `NativePipeline.bind` hands the table to native code,
   which checks it is complete again.

`fill` then only indexes the table, per chunk palette entry; a state outside
it refuses the chunk by name, which is reachable only if `lib/` starts
placing a block `emittableBlockStates` does not list (the unit tests fill
real chunks to catch exactly that). `lib/` does not publish that list
itself, so this binding mirrors it — a gap reported to `lib/`.

## Worlds and their frozen pipeline (SPEC §6)

A world generates from its frozen pipeline, never from a live pack. Its
options (`GeneratorOptions`) are `ext/`'s keys plus one:

```
{"blob":"worlds/stratum/stratum-pipeline.blob","pack":"/path/to/.fixtures/1.21.11",
 "settings":"minecraft:overworld","biomes":"minecraft:overworld"}
```

`"blob"` is required (relative to the server's working directory);
`"settings"` and `"biomes"` come together or not at all (the overworld's);
any other key, a repeated key or a non-string value is refused. Nukkit
offers no world-creation hook, so the plugin takes one: loading at
`STARTUP`, before Nukkit loads or creates any world, it freezes `"pack"` into
`"blob"` for every Stratum world that does not exist yet (the default world
and `nukkit.yml`'s). A world that exists without its blob is refused, never
refrozen from a pack that may have changed. Freezing refuses to replace a
file and writes through a rename.

Scope: noise settings shaped like Nukkit's overworld (y −64..319: vanilla's
`overworld`, `amplified`, `large_biomes`) with a multi-noise biome source,
in a LevelDB world. The Nether's and the End's settings, the End's biome
source and Anvil worlds are refused by name; a biome the source can return
that has no Bedrock id is refused (no nearest-biome fallback in v1).

## Biomes

Nukkit stores a biome per block (`LevelDBChunk.setBiomeId(x, y, z, id)`:
`getBiomeStorage(y >> 4)`, then block-local x, `y & 15`, z — a section y,
not a block y or an array index, and block-local, not quart-local). The
engine gives one per 4×4×4 quart; the generator writes each quart's id to
its 64 blocks. Read from source; the smoke run checks it in memory.

Two things read from Nukkit that the smoke run is built to measure:

- **Nukkit's biome registry lacks four overworld biomes.** Its
  `biome_id_map.json` at `b3caad67d` knows 50 of the 54 the overworld can
  return; `cherry_grove` (192), `deep_dark` (190), `mangrove_swamp` (191)
  and `pale_garden` (193) are missing, and its network serializer sends
  those to clients as ocean (`Biome.getBiomeIdOrCorrect`). The world stores
  the true ids; the plugin logs the four by name when the world opens.
  Refusing instead would refuse every overworld — an owner decision, flagged.
- **Nukkit's LevelDB biome serializer looks off by four sections.**
  `Data3dSerializer.serialize` (and `NetworkChunkSerializer`) pass the array
  index `i` to `getBiomeStorage`, which adds the section offset (4 for
  the overworld) — so slot `i` gets section `i + 4`'s biomes and the top
  four get the bottom one's. Invisible to Nukkit's own 2D-biome generators.
  The smoke run counts disk mismatches explained by exactly this separately
  from any other; only those pass, loudly.

## Fluid updates

Nukkit schedules block updates with `Level.scheduleUpdate(block, delay)`, on
the server thread. The generator keeps each chunk's fluid updates (packed by
`fill`); the plugin schedules each liquid among them after its own tick rate
when Nukkit fires `ChunkPopulateEvent` for that chunk (all eight neighbours
generated), as `ext/`'s PocketMine-MP plugin does. Written, never run. Not
carried: a chunk generated in one server run and populated in another.

## Loading the native library

`NativePipeline.load` tries, in order: `-Dstratum.nukkit.library=<absolute
path to libstratum_nukkit.so>`; `plugins/Stratum/libstratum_nukkit.so` (the
plugin's data folder); `java.library.path`. If it fails the generator is
still registered, so a Stratum world refuses to open, saying why, rather
than falling to Nukkit's `Normal`. On JDK 24+ run Nukkit with
`--enable-native-access=ALL-UNNAMED`.

## Headless smoke (`tools/nukkit-dev smoke`)

Writes a hermetic server (127.0.0.1, port 19232, query/RCON/bStats off,
whitelist on and empty) with the plugin, the smoke driver and a world whose
options name the fixtures' version root, and runs it twice in a network
namespace holding only loopback (`unshare -rn`), `-Xmx2G`:

1. creates the world, has Nukkit generate every chunk within 4 of spawn on
   its async workers, records the wall time and the generation threads,
   compares each chunk in memory with a single-threaded `fill`, saves, stops;
2. loads the world, reads the same chunks back from disk (generating none),
   compares again, stops.

The server lives in `$STRATUM_NUKKIT_DIR/servers/smoke`, rebuilt from empty
each run; `STRATUM_NUKKIT_SERVER_DIR` moves it. The default disk
(`~/stratum-work`) is exFAT: no symlinks or Unix permissions, so nothing is
linked there, and should Nukkit's LevelDB misbehave on it,
`~/.cache/stratum-nukkit/smoke` is the place to move it to.

It passes when `level.dat` names `stratum`, every chunk is on disk and was
filled by Stratum, blocks match everywhere, biomes match in memory, disk
biome mismatches (if any) are all the serializer offset above, neither run
logged an exception or error, and Nukkit's LevelDB log shows no substituted
block state. The report also carries the census: every emittable state's
Nukkit id, how many of all 29,671 Java states resolve, and which biomes
Nukkit's registry lacks.

## Open

- **Run it.** No Nukkit jar has been fetched: `tools/nukkit-dev` has no
  pinned build yet, and CloudburstMC's CI answered HTTP 502 on 2026-10-15.
  Pinning (`tools/nukkit-dev pin <build>`) is the owner's step. Until then
  the plugin, the frozen-blob hook, the id resolution against Nukkit, the
  biome convention, the serializer reading and the fluid scheduling are
  unverified in Nukkit.
- The Nether and the End (Nukkit's 0..127 and 0..255), and Anvil worlds.
- Vanilla's spawn search (`getSpawn` is a fixed point) and features
  (`populateChunk` is a no-op until M6).
- A world's generation threads keep its native handle after Nukkit unloads
  it: Nukkit gives no point after which no worker can still be filling.
