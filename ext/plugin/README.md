# `ext/plugin/` — the PocketMine-MP plugin

Milestone **M5** (SPEC §4.3). Registers Stratum as a PocketMine-MP
generator, so a world generates vanilla Java Edition terrain.

Everything expensive is in the engine; this is the thin PHP layer that
PocketMine-MP itself can call.

## What it does

- `Main` registers the generator under the name `stratum`, in `onLoad()` —
  worlds are loaded between the STARTUP and POSTWORLD plugin phases, and a
  world naming an unregistered generator does not merely fail to load, it
  **aborts server startup**.
- `StratumGenerator` is constructed once per generation worker thread with
  the seed and the world's options string — again whenever PocketMine-MP
  restarts a worker it shut down while idle. It opens the world (the state
  the main thread pinned at world load; unpinned, the frozen pipeline,
  compiled once and shared by every worker that opens the same blob bytes,
  noise settings, biome source and seed) and, per chunk, hands each
  sub-chunk's already-packed layers to `PalettedBlockArray::fromData()`.
  The only PHP work per chunk is translating a handful of palette entries;
  there is no per-block PHP.
- `BlockStateTranslator` turns Java block state ids into PocketMine-MP's own
  state ids through **PocketMine-MP's own** upgrader and deserializer — the
  same path its world loader takes for every palette entry on disk — cached
  per worker.
- `GeneratorOptions` is the world's options string:
  `{"blob":"…/stratum-pipeline.blob","settings":"minecraft:overworld","biomes":"minecraft:overworld"}`.
  A generator receives nothing else — not the server, not the plugin, not a
  config — so the path to the frozen pipeline travels in here. `biomes` is
  the dimension's biome source: a multi-noise parameter list, or
  `minecraft:the_end` for the End (`"settings":"minecraft:end",
  "biomes":"minecraft:the_end"`). `settings` and `biomes` come together or
  not at all — with neither, both are the overworld's — since neither says
  which of the other goes with it. **Every refusal is made on the main
  thread**, where PocketMine-MP validates a world's options as it loads it
  (`GeneratorOptions::validate`, the validator `Main` registers): there a
  failure fails that world, while an exception on a generation worker takes
  the whole server down. The options are refused for a key other than these
  three, for either of the pair alone, for either not a string (a JSON
  `null` is a key given, not one left out), and for a biome source the
  engine's own `BiomeSource::parse` refuses (`minecraft:fixed`,
  `minecraft:checkerboard`, a bare `minecraft:multi_noise`, or no
  identifier at all), with its reason; and then the world is opened in full
  (`Stratum\checkDimension`) and nothing kept, so a blob that cannot be
  read, one frozen by another pipeline engine version or failing its
  content hash, settings or a parameter list the pipeline lacks, the legacy
  Nether, a dimension outside PocketMine-MP's 24 sub-chunks, and a list
  naming a biome with no Bedrock id are all refused at load, with the
  engine's reason. `GeneratorOptions`' doc lists exactly what is accepted.
  A pairing vanilla does not make, such as the End's settings with the
  overworld's list, is taken as asked: a datapack may make it.
- `StratumWorlds` pins each Stratum world's engine state — its compiled
  dimension and its held fluid updates — on the main thread from
  `WorldLoadEvent` to `WorldUnloadEvent` (`Stratum\pinWorld`). Unpinned,
  that state lives only as long as the world's generators, and
  PocketMine-MP's memory manager shuts idle workers down: the fluid updates
  of every chunk generated but not yet populated went with them, and a
  restarted worker read the blob afresh, whatever it held by then. Pinned,
  every generator gets the state the world loaded with, without reading the
  file. A pin that fails is logged by name, never thrown out of the event.
- `FluidUpdateScheduler` makes aquifer water and lava flow. A Java server
  ticks every position in a generated chunk's `PostProcessing` lists once
  the chunk loads; PocketMine-MP has no such list, so when it fires
  `ChunkPopulateEvent` for a Stratum world this takes the chunk's positions
  from the engine (`Stratum\takeFluidUpdates`) and schedules a block update
  at each that still holds a liquid, after the liquid's own tick rate —
  what `Liquid::onNearbyBlockChange()` does itself. Population rather than
  first load, because a populated chunk's neighbours all exist and the flow
  cannot reach ungenerated terrain. Registered in `onEnable()`, which at
  `load: STARTUP` is still before any world loads.
- `WorldFactory::create()` refuses a name that is already a world — loaded
  or not — before writing anything, returning `false` as `generateWorld()`
  would: freezing over an existing world's blob would change every chunk it
  generates from then on, a seam (SPEC §6). Otherwise it freezes the
  pipeline into the new world's own folder — whole or not at all: written
  beside it, then renamed into place — checks it in full on the main
  thread, and then creates the world from it, in that order: creating the
  world immediately registers the generator on a worker, which opens the
  blob.

## Installing

The engine is a PHP extension, not part of the plugin: build it with
`tools/php-dev` + `cmake --preset ext-release` (see `ext/README.md`) and
load `stratum.so` in the server's `php.ini` with an absolute `extension=`
line. `plugin.yml` declares `extensions: {stratum: "*"}`, so a server
without it refuses the plugin by name at load time instead of failing
inside a worker thread.

PocketMine-MP 5.44.3 loads plugins from phars only — without DevTools it
registers no folder loader — so pack this directory first:
`php -d phar.readonly=0 ext/pocketmine/build_plugin_phar.php ext/plugin
plugins/Stratum.phar` (`plugin.yml` at the root, `src/` beside it).

```php
// Creating a Stratum world, e.g. from a command or another plugin:
\Stratum\PMMP\WorldFactory::create($server, "overworld", seed: 12345, versionRoot: "/path/to/1.21.11");
```

`versionRoot` is a version root as `tools/fetch-vanilla` leaves it —
`worldgen/` and `biome_parameters/` side by side.

A world can come from `pocketmine.yml` too, with no code: freeze the
pipeline into the world's folder first, as `WorldFactory` does
(`Stratum\freezePipeline($versionRoot, "<server>/worlds/<name>/stratum-pipeline.blob")`),
then name the generator and give the options as its preset:

```yaml
worlds:
  <name>:
    generator: stratum
    preset: '{"blob":"<server>/worlds/<name>/stratum-pipeline.blob","settings":"minecraft:overworld","biomes":"minecraft:overworld"}'
    seed: 12345
```

## Running on PocketMine-MP

`tools/pmmp-dev` fetches PocketMine-MP 5.44.3 and its own PHP
(pmmp/PHP-Binaries php-build-524), digest-checked, into
`~/.cache/stratum-pmmp`, and runs this plugin and the extension inside
them. `tools/pmmp-dev server <dir> --stratum-so <so> --version-root
<root>` writes a hermetic headless server directory — loopback only,
nothing that phones home, the plugin packed into `plugins/`, the world as
the `worlds:` entry above — and prints the command that starts it.
`ctest --preset pmmp` is the evidence, local and not CI:

- `pmmp.load`: the module's build id and the ABI lines of both PHPs agree,
  every zend symbol it imports is exported, it loads beside pmmpthread,
  chunkutils2, leveldb, igbinary, morton and the rest, and PocketMine-MP's
  own startup gate accepts the environment.
- `pmmp.phpt`: `ext/zend/tests` and this directory's `tests/` under that
  PHP, driven by tools/php-dev's `run-tests.php` rather than the copy in
  PocketMine-MP's PHP tarball.
- `pmmp.census.blocks`: every Java block state through
  `BlockStateTranslator`, PocketMine-MP's own upgrader and deserializer;
  fails if a state v1 terrain places reaches `minecraft:info_update`.
- `pmmp.census.biomes`: every biome each vanilla world preset's dimension
  can emit, against `LegacyBiomeIdToStringIdMap` (what the wire asks) and
  the biome definitions clients are sent.
- `pmmp.threads`: four `pocketmine\thread\Thread`s opening one blob's
  Dimension and encoding at once, byte-identical to one thread's encode.
- `pmmp.smoke.overworld`, `pmmp.smoke.end`: a server boot that generates
  the spawn area (224 chunks populated) and stops from the console, with
  `server.log`, crash dumps and `level.dat` checked, and then every chunk
  on disk (292) read back through PocketMine-MP's own LevelDB provider and
  compared, block by block and biome by biome, with what the engine
  generates for it from the world's own options and seed, each block state
  through `BlockStateTranslator` (`ext/pocketmine/inspect_world.php`). The
  biomes must match exactly. A block may differ only as PocketMine-MP's own
  liquid rules change the terrain once `FluidUpdateScheduler` sets the
  aquifer water and lava flowing — air now water or lava, lava hardened to
  obsidian or cobblestone beside water, water or flowed water turned to
  stone where lava flowed into it — and each such change is counted.
  Anything else fails, which is what a plugin writing empty or shifted
  chunks does (both tried, both caught).

## What is not verified

**It has run in a real, headless PocketMine-MP 5.44.3, never with a
client.** Every check above passes. What that leaves:

- Nothing has connected to it. Whether a Bedrock client renders the
  terrain, the biomes and the flowing fluids as a Java client would is the
  owner's check to make (SPEC §11, "M5: Stratum inside a real
  PocketMine-MP").
- CI still checks only that every file parses and runs the PHPT suites in
  `tests/`, each with the real extension loaded against stand-ins for the
  PocketMine-MP classes the code calls: `001_generator_options.phpt`
  (`GeneratorOptions`, fixture-free), `002_fluid_update_scheduler.phpt`
  (`FluidUpdateScheduler`), `003_world_load_validation.phpt` (the full
  open `validate()` makes at world load), `004_world_pins.phpt`
  (`StratumWorlds` across a worker shut down and restarted) and
  `005_world_factory.phpt` (`WorldFactory` leaving an existing world's
  blob byte for byte). The real-server checks need the cache, so they are
  local.
- Two facts about PocketMine-MP that the pins are built on come from the
  M5 review that asked for them and have not been observed in the real
  server: that the memory manager shuts idle workers down (and every
  generator with them), and that `WorldLoadEvent` fires before any of the
  world's chunks is requested. The third, that an exception on a
  generation worker takes the server down, has been: a generator refusal
  injected into the smoke crashes its workers and the server.
- **The fluid updates cost generation work.** A block update scheduled at
  each aquifer liquid when its chunk is populated makes that liquid flow
  while the spawn area is still generating, and a block change inside the
  3x3 of a population task still in flight makes PocketMine-MP discard
  that task's result and populate again (`World::generateChunkCallback`).
  Measured on the overworld smoke: 3 to 56 discarded populations a run,
  and slower spawn generation in each of three paired runs, against none
  with the scheduler left out. Whether the flow is Java's is unobserved.
- **A Stratum world cannot move.** Its generator options hold the frozen
  pipeline's absolute path, and PocketMine-MP validates them on every
  load: move or rename the server directory and the world no longer
  loads, and when it is the default world the server stops at startup.
  Measured, not inferred.
- **An End world generates, and is presented as the overworld.** The End
  smoke boots, generates and stops cleanly, every biome Bedrock's one End
  biome. But PocketMine-MP 5.44.3 names `DimensionIds::OVERWORLD` to every
  client whatever the world (`PreSpawnPacketHandler`, `NetworkSession`),
  so its sky, fog and light will be the overworld's; a new world gets
  PocketMine-MP's default spawn, (256, 70, 256), which in the End is over
  the void between the main island and the outer ones — the whole
  224-chunk spawn area is air; and `WorldFactory::create()` only ever
  writes the overworld's options, so an End world needs its options
  written by hand, as the smoke's `worlds:` entry does. Read from source
  and measured headless; no client has seen it.
- Fluid updates live in memory between a chunk's generation and its
  population — held through worker restarts while the world is loaded and
  pinned, but not through an unload: a chunk generated as a neighbour in
  one server run (or one load of the world) and first populated in a later
  one gets none, and its aquifer fluid stays as
  generated until something beside it changes. PocketMine-MP saves no
  scheduled updates either, so a flow the server stops mid-way stops there.

## Traps this code is written around

Each of these produces a **wrong world rather than an error**, and each was
found by reading PocketMine-MP's source rather than by running it:

- A sub-chunk key `Chunk::__construct` does not find becomes air **and an
  all-ocean biome array**. Keys are signed indices (-4..19); a 0-based array
  would shift the world vertically with nothing to say so. And all 24 must
  be given, not only those a dimension's height covers: the End generates
  y 0..127, so `encodeChunk()` returns the other sixteen as air with the
  End's own biomes (SPEC §4.3), and the plugin passes on every key it gets.
- An unmapped biome id is silently replaced with `OCEAN` **on the wire**
  (`ChunkSerializer`), with no exception and no log.
- A block PocketMine-MP does not implement (`minecraft:powder_snow` is a
  real, measured example that the overworld's surface rules place) would
  otherwise become PocketMine-MP's `info_update`; the fallback table names
  it, and every miss is logged once per worker thread (each has its own
  translator), so a server with three workers may say it three times.
