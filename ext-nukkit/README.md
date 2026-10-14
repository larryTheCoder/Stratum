# `ext-nukkit/` — CloudburstMC/Nukkit binding

Milestone **M5** (SPEC §9, §11's M5-Nukkit entries). A JNI binding: Nukkit's
floor is Java 8 (its `build.gradle.kts`), which rules out the Foreign
Function & Memory API, and Nukkit already ships a native library
(`leveldbjni`). Everything about Nukkit below was read from
CloudburstMC/Nukkit `master` at `b3caad67d` (2026-09-19, Bedrock 26.50), a
permitted M5 reference (CLAUDE.md).

**Status.** The C++ pipeline and the JNI surface are built and tested, the
JNI surface through real JNI in a JVM. The plugin has been type-checked only
against hand-written stubs of the Nukkit API it calls (scratch, never
committed), and **has never run inside Nukkit**: no Nukkit server jar exists
on this machine. The one approved download is a jar from CloudburstMC's own
CI, and that CI (`ci.opencollab.dev`) answered every request with HTTP 502
throughout 2026-10-15, so `tools/nukkit-dev` still pins no build. The
headless smoke test that runs the plugin in a real server is written, has
been exercised against a stand-in server, and skips until a build is pinned.
What that run is expected to settle is listed under "Open".

## Layout

| Part | Needs | Tested by |
|---|---|---|
| `stratum_nukkit_pipeline` (`include/`, `src/pipeline.cpp`) — plain C++ | nothing but `lib/` | `tests/pipeline_test.cpp`, in `stratum_unit_tests` (`ctest --preset dev`), every platform |
| `stratum_nukkit` (`src/jni_bridge.cpp`) and `java/core/` — the JNI shim and the Java classes with no Nukkit type in them | a JDK (javac, `jni.h`, `ct.sym`), `-DSTRATUM_BUILD_EXT_NUKKIT=ON` | `ext-nukkit.jni`: `java/test/…/NativePipelineCheck.java` in a bare JVM |
| `java/plugin/` — the Nukkit plugin (`stratum-nukkit.jar`) | the pinned Nukkit server jar, `STRATUM_NUKKIT_JAR` | `ext-nukkit.plugin`, `ext-nukkit.smoke` |
| `java/smoke/` — the smoke run's driver plugin | the same | `ext-nukkit.smoke` |
| `cmake/VerifyNukkitJar.cmake` — refuses a jar that is not the pinned one | `tools/nukkit-dev`'s pin | configure, and every javac against the jar |

Build and test:

```
JAVA_HOME=$(tools/nukkit-dev --print-java-home) \
STRATUM_NUKKIT_JAR=$(tools/nukkit-dev --print-jar) cmake --preset nukkit
cmake --build --preset nukkit -j 3 && ctest --preset nukkit
```

Without a jar, `--print-jar` fails, `STRATUM_NUKKIT_JAR` is empty, and
`ext-nukkit.plugin` and `ext-nukkit.smoke` skip, saying so. A jar that is
named and is not, byte for byte, the one `tools/nukkit-dev` pins (or any jar
while nothing is pinned) stops the configure; the same check runs again
before each javac against it, so a jar replaced after configuring is caught.

Every class is compiled `javac --release 8 -proc:none`. `-proc:none`
because javac up to JDK 22 runs any annotation processor a class path jar
registers, and Nukkit's server jar bundles log4j with its processor's
service file: without it, compiling against the download would run code
from it. Locally the JDK is Azul Zulu OpenJDK 25.0.4 (`~/.jdks/azul-25.0.4`),
the newest local JDK with headers and `ct.sym` (the system OpenJDK 25 is a
JRE: no `javac`, no `jni.h`). javac warns that 8 is obsolete and still
accepts it.

## How Nukkit drives a generator

- A world names its generator in `level.dat` (`generatorName`) and carries
  one options string (`generatorOptions`). A new default world takes them
  from `server.properties`' `level-type` and `generator-settings`; a new
  `nukkit.yml` world from `worlds.<name>.generator: "stratum:<options>"`
  (with no colon, `generator-settings` again). An existing world goes by its
  own `level.dat`, whatever the configuration says. Nukkit gives a world
  whose generator is not registered its own `Normal`, silently.
- `Level` keeps one `Generator` per thread in a `ThreadLocal`, constructed
  with the options map. On the server thread `init` is called twice, first
  with the `Level` itself, then with a fresh `PopChunkManager`; on an async
  worker once, with a `PopChunkManager` that carries the seed and nothing
  else.
- **An exception from `init` is not a clean failure.** The `ThreadLocal`
  logs it and stores `null`, and `Level.initLevel()` then throws a
  `NullPointerException`. For a new world, `Server.generateLevel` catches
  that and logs a generation error, but `LevelDBProvider.generate` has
  already written the world's `level.dat`; `Server.loadLevel` does not catch
  it, so for an existing world (the next start, say) it escapes Nukkit's
  startup and the server stops. Nukkit's `main` exits 0 either way.
- `GenerationTask`/`PopulationTask` call `generateChunk` on a worker, inside
  a lock on the chunk manager, with the new chunk set into it. Workers are
  `nukkit.yml`'s `settings.async-workers` (`ServerScheduler.WORKERS`).

So `StratumGenerator.init` opens the world on the server thread's first
call, the only one that knows which world it is (`WorldPipeline.open`), and
every later call finds it by seed and options string. And because a refusal
from `init` is what it is, the worlds Nukkit loads or creates at startup are
refused before Nukkit touches them (next section).

## Refusals happen at startup, before Nukkit touches a world

The plugin loads at `STARTUP`: Nukkit calls its `onLoad` after opening its
network interface and its player database, and before it loads or creates
any world. There, `StratumPlugin.onLoad`:

1. registers the generator (first, and whatever else fails: an unregistered
   generator would mean a silent `Normal` world);
2. loads the native library;
3. lists the Stratum worlds Nukkit is about to load or create, reading the
   configuration and each existing world's `level.dat` the way Nukkit's
   `Server` constructor does (`StartupWorlds`);
4. for each: freezes a new world's pipeline from its `"pack"` (below), then
   makes every check opening the world would (`WorldPipeline.validate`: the
   options, the frozen pipeline, the engine's own refusals, every block state
   resolved through Nukkit and bound — none of it depends on the seed, so it
   compiles for seed 0 and releases);
5. if any world is refused, logs one `REFUSED stratum: world '<name>': <why>`
   line per world and one `STOPPING the server …` line, calls Nukkit's
   `forceShutdown` and exits with status 1. Nothing is written for a refused
   world.

`WorldPipeline.open` makes the same checks again when Nukkit loads the
world, for its real seed. A Stratum world a command or another plugin
creates or loads later, after startup, is refused there, through Nukkit's
own path above (a log line naming the refusal, then the
`NullPointerException`, and for a new world a `level.dat` left behind).
Existing Anvil worlds are not read at startup; opening one refuses it.

## Block states (SPEC §9)

`lib/mapping/`'s table resolves a Java block state to a Bedrock
`{name, states, version}`; Nukkit resolves that to its own full id
(`legacyId << 13 | meta`, what `FullChunk.setFullBlockId` takes). Both
happen once, when the world opens (and once more at startup, to check):

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
   `runtimeToFullId` on the legacy mapper (never a snapshot's
   `getLegacyId`, which also falls back); and the full id is checked to come
   back as the same state through `legacyToRuntime`, which is how a chunk
   section stores it.
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
offers no world-creation hook, so the plugin takes one: at `STARTUP`, before
Nukkit creates the world, it freezes `"pack"` into `"blob"` for every new
Stratum world (the default world and `nukkit.yml`'s). A world that exists
without its blob is refused, never refrozen from a pack that may have
changed. Freezing refuses to replace a file and writes through a rename.

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
its 64 blocks. Read from source, not yet observed: the smoke run checks it
in memory.

Two things read from Nukkit that the smoke run is built to measure:

- **Nukkit's biome registry lacks four overworld biomes.** Its
  `biome_id_map.json` at `b3caad67d` knows 50 of the 54 the overworld can
  return; `cherry_grove` (192), `deep_dark` (190), `mangrove_swamp` (191)
  and `pale_garden` (193) are missing, and its network serializer sends
  those to clients as ocean (`Biome.getBiomeIdOrCorrect`). The world stores
  the true ids; the plugin logs the four by name when the world opens.
  Refusing instead would refuse every overworld — an owner decision, flagged.
- **Nukkit's biome serializers look off by four sections.**
  `Data3dSerializer.serialize` and `NetworkChunkSerializer` pass the array
  index `i` to `getBiomeStorage`, which adds the section offset (4 for the
  overworld) — so slot `i` gets section `i + 4`'s biomes and the top four
  get the bottom one's, on disk and on the wire. Invisible to Nukkit's own
  2D-biome generators. The smoke run counts disk mismatches explained by
  exactly this apart from any other. If every mismatch is that offset the
  smoke exits **3**, not 0: everything Stratum controls passed, the worlds'
  stored and sent biomes are still wrong wherever biomes vary with height,
  and CTest fails it until the owner rules on the defect.

## Fluid updates

Nukkit schedules block updates with `Level.scheduleUpdate(block, delay)`, on
the server thread. The generator keeps each chunk's fluid updates (packed by
`fill`); the plugin schedules each liquid among them after its own tick rate
when Nukkit fires `ChunkPopulateEvent` for that chunk (all eight neighbours
generated), as `ext/`'s PocketMine-MP plugin does, and counts them (the
smoke report's `fluid_updates_scheduled`). Written, never run. Not carried:
a chunk generated in one server run and populated in another.

## Loading the native library

`NativePipeline.load` tries, in order: `-Dstratum.nukkit.library=<absolute
path to libstratum_nukkit.so>`; `plugins/Stratum/libstratum_nukkit.so` (the
plugin's data folder); `java.library.path`. If it fails the generator is
still registered, and every Stratum world configured at startup is refused,
saying why, so the server stops rather than generating `Normal` terrain. On
JDK 24+ run Nukkit with `--enable-native-access=ALL-UNNAMED`.

## Headless smoke (`tools/nukkit-dev smoke`)

Writes a server directory (127.0.0.1, port 19232, query/RCON/bStats off,
whitelist on and empty, synthetic auth caches) with the plugin, the smoke
driver and a world whose options name the fixtures' version root, and runs
the jar three times in a network namespace holding only loopback
(`unshare -rn`), `-Xmx2G`:

0. a world with no frozen pipeline and no pack: the plugin must refuse it
   by name and stop the server with status 1, leaving no `worlds/refused`;
1. creates the real world, has Nukkit generate every chunk within 4 of spawn
   on its async workers, records the wall time and the generation threads,
   compares each chunk in memory with a single-threaded `fill`, saves, stops;
2. loads the world, reads the same chunks back from disk (generating none),
   compares again, stops.

**Network.** Nukkit's `Server` constructor fetches Microsoft's Bedrock auth
discovery document and the OpenID configuration it names on every start,
whatever its configuration (`EncryptionUtils`), and stops when both the
fetch and a cached copy fail. So `server` writes synthetic
`discovery-cache.json` and `openid-cache.json` naming only `auth.invalid`;
inside the namespace both fetches fail, and those two `Failed to fetch`
errors, each with its `UnknownHostException` trace, are the only errors the
log check accepts (counted in the summary). A directory written by
`tools/nukkit-dev server` and started by hand, outside a namespace, is
**not** hermetic: Nukkit reaches those hosts and overwrites the caches.

The server lives in `$STRATUM_NUKKIT_DIR/servers/smoke`;
`STRATUM_NUKKIT_SERVER_DIR` or `--dir` moves it. Each run empties it first,
and smoke only ever empties a directory that is empty or carries the
`.stratum-nukkit-smoke` marker it writes. The default disk (`~/stratum-work`)
is exFAT: no symlinks or Unix permissions, so nothing is linked there, and
should Nukkit's LevelDB misbehave on it, `~/.cache/stratum-nukkit/smoke` is
the place to move it to.

Exit 0 (PASS) needs: run 0 refused as above; `level.dat` naming `stratum`;
every chunk on disk and filled by Stratum; blocks matching everywhere;
biomes matching in memory and on disk; no log line in any run that is an
error, a stack trace or an exception, beyond the expected ones; and
Nukkit's LevelDB log showing no substituted block state. Nukkit's own `main`
exits 0 even after its startup threw, so the reports and the logs are the
evidence, not the status. Exit 3 is the biome offset above; 1 is anything
else; 77 a skip (no fixtures). The report also carries the census (every
emittable state's Nukkit id, how many of all 29,671 Java states resolve,
which biomes Nukkit's registry lacks), the fluid updates scheduled, and the
generation time and threads. `tools/nukkit-dev scan-log <log>` runs the log
check on one file.

## Open

- **Run it.** No Nukkit jar has been fetched: CloudburstMC's CI answered
  HTTP 502 to every request on 2026-10-15, so `tools/nukkit-dev` pins no
  build. Pinning (`tools/nukkit-dev pin <build>`, preferably the build of
  `b3caad67d`) is the owner's step. Until then the plugin, the startup
  refusal, the frozen-blob hook, the id resolution against Nukkit, the biome
  convention, the serializer reading and the fluid scheduling are unverified
  in Nukkit.
- **Keep the verified jar.** Nukkit's Jenkinsfile keeps the artifacts of the
  newest five builds only (`logRotator(artifactNumToKeepStr: '5')`), so a
  pinned build's jar disappears from the CI five builds later; `fetch` then
  says so (HTTP 404). The copy under `$STRATUM_NUKKIT_DIR/jars` (or
  `quarantine/`) is the durable one: keep it, or a backup, and `fetch`
  accepts it wherever it came from as long as its SHA-256 is the pin.
- The biome serializer defect and the four biomes Nukkit's registry lacks
  both await the owner's ruling.
- The Nether and the End (Nukkit's 0..127 and 0..255), and Anvil worlds.
- Vanilla's spawn search (`getSpawn` is a fixed point) and features
  (`populateChunk` is a no-op until M6).
- A world's generation threads keep its native handle after Nukkit unloads
  it: Nukkit gives no point after which no worker can still be filling.
- CI's clang-tidy run (`tools/lint/tidy.sh`'s list) does not include
  `ext-nukkit/`, and `jni_bridge.cpp` needs a compile database configured
  with `STRATUM_BUILD_EXT_NUKKIT=ON` (`-p build/nukkit`) to be analysed at
  all — a change for the lint scripts, outside this binding.
