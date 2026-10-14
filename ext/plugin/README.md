# `ext/plugin/` — the PocketMine-MP plugin

Milestone **M5** (SPEC §4.3). Registers Stratum as a PocketMine-MP
generator, so a world generates vanilla Java Edition terrain.

Everything expensive is in the engine; this is the thin PHP layer that
PocketMine-MP itself can call.

## What it does

- `Main` registers the generator under the name `stratum`, in `onLoad()` —
  worlds are loaded between the STARTUP and POSTWORLD plugin phases, and a
  world naming an unregistered generator does not merely fail to load, it
  **aborts server startup**. Just before, it gives the extension the
  server's data path (`Stratum\setDataPath`), which a world's relative blob
  path is read against on every thread.
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
  `{"blob":"worlds/<name>/stratum-pipeline.blob","settings":"minecraft:overworld","biomes":"minecraft:overworld"}`.
  A generator receives nothing else — not the server, not the plugin, not a
  config — so the path to the frozen pipeline travels in here: relative to
  the server's data path, so the world moves with its server directory, or
  absolute, as worlds made before that have it, which ties the world to
  where it was made. `biomes` is
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
  Nether, a dimension outside PocketMine-MP's 24 sub-chunks, a list naming
  a biome with no Bedrock id, and a block state the pack names (the
  settings' `default_block` or `default_fluid`, a surface rule's
  `result_state`) that the mapping table cannot place are all refused at
  load, with the engine's reason. `GeneratorOptions`' doc lists exactly what is accepted.
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
  file. A pin that fails — the same full open a generator makes, so the
  world would fail on its first worker and take the server down — is
  logged by name and never thrown out of the event: the world is held off
  generation instead. Every chunk within eight of its spawn is locked, so
  the spawn area `WorldManager::generateWorld()` orders right after the
  event is queued rather than handed to a worker, and on the next tick the
  world is force-unloaded, which rejects all of it; a default world stops
  the server instead, by name. (Unloading inside the event is not safe:
  `generateWorld()` goes on to order populations on that same World, whose
  provider an unload has closed.)
- `FluidUpdateScheduler` makes aquifer water and lava flow. A Java server
  ticks every position in a generated chunk's `PostProcessing` lists once
  the chunk loads; PocketMine-MP has no such list, so once it has fired
  `ChunkPopulateEvent` for a chunk of a Stratum world, and the chunk has
  settled (below), this takes the chunk's positions from the engine
  (`Stratum\takeFluidUpdates`) and schedules a block update at each that
  still holds a liquid, the liquid's own tick rate after the population
  (what is left of it, if the chunk settled later) — the delay
  `Liquid::onNearbyBlockChange()` schedules itself. Population rather than
  first load, because a populated chunk's neighbours all exist; once it has
  settled, every chunk within three of it exists, so a flow reaches
  ungenerated terrain — where `World::setBlockAt()` throws, inside a
  scheduled update — only by running on past all of them. And never where
  a population is in flight or still to come: a population task locks its
  3x3 (`World::lockChunk`), and any block set there while it runs makes
  PocketMine-MP discard its result and populate again. So a populated chunk
  is held — its coordinates and the tick, its positions left in the
  extension — until none of its 3x3 is locked and every chunk within two
  of it, every chunk whose population could lock that 3x3, is loaded and
  populated: PocketMine-MP never populates those again, so the chunk has
  settled for good. A chunk at the edge of what is populated is held until
  the chunks beyond it are. `Main` runs the release once a tick from its
  task scheduler, which PocketMine-MP ticks before it collects finished
  population tasks and before the worlds tick. Registered in `onEnable()`,
  which at `load: STARTUP` is still before any world loads.
- `WorldFactory::create()` makes one of four dimensions, as vanilla's world
  presets pair settings and biome source: `OVERWORLD` (the default),
  `LARGE_BIOMES` and `AMPLIFIED` (their settings, the overworld's parameter
  list), and `THE_END` (`minecraft:end` with the End's own source); any
  other is refused by name before anything is written. An End world spawns
  at (0, 128, 0), over the main island every End seed has at the origin, so
  the spawn area PocketMine-MP generates round it holds terrain — its own
  default spawn, (256, 70, 256), is over the void. It refuses a name that
  is already a world — loaded or not — before writing anything, returning
  `false` as `generateWorld()` would: freezing over an existing world's
  blob would change every chunk it generates from then on, a seam (SPEC
  §6). Otherwise it freezes the pipeline into the new world's own folder —
  whole or not at all: written beside it, synced to disk, then renamed into
  place — names it in the options relative to the server's data path
  (`worlds/<name>/stratum-pipeline.blob`), checks it in full on the main
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
registers no folder loader — so pack this directory first, into the
server's `plugins/` and never into this checkout:
`php -d phar.readonly=0 ext/pocketmine/build_plugin_phar.php ext/plugin
<server>/plugins/Stratum.phar` (`plugin.yml` at the root, `src/` beside
it).

```php
// Creating a Stratum world, e.g. from a command or another plugin:
use Stratum\PMMP\WorldFactory;
WorldFactory::create($server, "overworld", seed: 12345, versionRoot: "/path/to/1.21.11");
WorldFactory::create($server, "end", seed: 12345, versionRoot: "/path/to/1.21.11", dimension: WorldFactory::THE_END);
```

`versionRoot` is a version root as `tools/fetch-vanilla` leaves it —
`worldgen/` and `biome_parameters/` side by side.

A world can come from `pocketmine.yml` too, with no code: freeze the
pipeline into the world's folder first, as `WorldFactory` does
(`Stratum\freezePipeline($versionRoot, "<server>/worlds/<name>/stratum-pipeline.blob")`),
then name the generator and give the options as its preset — the blob
relative to the server directory, so the world moves with it:

```yaml
worlds:
  <name>:
    generator: stratum
    preset: '{"blob":"worlds/<name>/stratum-pipeline.blob","settings":"minecraft:overworld","biomes":"minecraft:overworld"}'
    seed: 12345
```

Such a world gets PocketMine-MP's default spawn, (256, 70, 256): a
`worlds:` entry cannot set one, so an End world made this way spawns over
the void; `WorldFactory::create()` with `THE_END` does not.

## Running on PocketMine-MP

`tools/pmmp-dev` fetches PocketMine-MP 5.44.3 and its own PHP
(pmmp/PHP-Binaries php-build-524), digest-checked, into
`~/.cache/stratum-pmmp`, and runs this plugin and the extension inside
them. `tools/pmmp-dev server <dir> --stratum-so <so> --version-root
<root>` writes a hermetic headless server directory — loopback only,
nothing that phones home, the plugin packed into `plugins/`, the world as
the `worlds:` entry above, its blob named relatively — and prints the
command that starts it. It refuses a `<dir>` inside this checkout outside
`build/`, where a frozen pipeline would be one commit away from being
published, and every server directory it writes ignores itself.
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
  `server.log` (the network interface on 127.0.0.1 and nowhere else, no
  Query listener, no error, and not one discarded population), crash
  dumps and `level.dat` checked, and then every chunk on disk (292) read
  back through PocketMine-MP's own LevelDB provider and compared, block by
  block and biome by biome, with what the engine generates for it from the
  world's own options and seed, each block state through
  `BlockStateTranslator` (`ext/pocketmine/inspect_world.php`). The biomes
  must match exactly. A block may differ only where PocketMine-MP 5.44.3's
  own liquid and falling-block rules, read from its `Liquid`, `Water`,
  `Lava` and `FallableTrait`, would change it once `FluidUpdateScheduler`
  sets the aquifer water and lava flowing — flowing or falling liquid only
  beside or under the liquid it came from, a water source only beside two
  others and leading back to the engine's own, stone only beside lava,
  hardened lava only beside water, a fallen gravel or sand block only over
  what it could fall into — judged block by block from what is round it,
  and no more changed blocks in all than four for each fluid position the
  engine marked in the populated chunks. Anything else fails: a plugin
  writing empty or shifted chunks, one placing water as stone, and one
  placing air as water were each tried and caught. The End's spawn area is
  the main island: the test-only evidence plugin (`ext/pocketmine`) sets
  the new world's spawn to 0 70 0 on its `WorldInitEvent`, and a spawn
  area of nothing but air fails, as does a generator that drops the End's
  terrain and keeps its biomes (tried, caught).
- `pmmp.observe`: the overworld smoke watched from inside the server by
  that evidence plugin, about seven minutes. The world's `WorldLoadEvent`
  must fire, with the world pinned by `StratumWorlds` and none of its
  chunks loaded, before its first chunk is requested; once every async
  worker has been idle the 300 s `AsyncPool::shutdownUnusedWorkers()` asks
  for, the console's `gc` (what the periodic garbage collection runs) must
  shut them all down; and the chunks the old workers generated beside the
  spawn area, populated on new workers together with every chunk within
  two of them (so that they settle), must get a block update at every
  fluid position the engine marks in them — updates only the pin held. A
  copy of the plugin that records a pin and never makes one fails it.

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
  (`StratumWorlds` across a worker shut down and restarted),
  `005_world_factory.phpt` (`WorldFactory` leaving an existing world's
  blob byte for byte, and each dimension's options, relative blob path and
  spawn), `006_moved_server.phpt` (a world it made loading, pinning and
  generating the same chunk after its server directory moved) and
  `007_failed_pin.phpt` (a world whose pin fails reaching no worker, and
  unloaded or the server stopped). The real-server checks need the cache,
  so they are local.
- What the pins are built on is observed in the real server
  (`pmmp.observe`): the world's `WorldLoadEvent` fires before its first
  chunk is requested, with none of its chunks loaded and the world already
  pinned; and once the async workers have been idle the 300 s
  `AsyncPool::shutdownUnusedWorkers()` asks for, the memory manager's
  garbage collection — run from the console's `gc`, the call its periodic
  run makes — shuts all three down, new workers register the generator
  again, and the
  chunks the old ones generated get every fluid update the engine marked
  in them (2637 of 2637) — from the pin, since a copy of the plugin that
  never pins delivers 0 of 2637. That an exception on a generation worker
  takes the server down has been observed too: a generator refusal
  injected into the smoke crashes its workers and the server.
- **A world whose pin fails** is held off generation and unloaded
  (`StratumWorlds`). Observed in the real server for a default world, with
  a pin failure injected into a scratch copy of the plugin: the error by
  name, the spawn area ordered and queued, no generator registered on any
  worker, then at the first tick the emergency by name and a clean stop,
  exit code 0, no crash dump. The non-default case — the world unloaded on
  the next tick — is `007_failed_pin.phpt`'s, against stand-ins. Not held
  back: a population some other code orders outside the locked spawn area
  between the failed pin and the next tick.
- **The fluid updates no longer cost generation work.** A block update
  scheduled at each aquifer liquid as its chunk was populated made that
  liquid flow while the spawn area was still generating, and a block
  change inside the 3x3 of a population task still in flight makes
  PocketMine-MP discard that task's result and populate again
  (`World::generateChunkCallback`): 3 to 56 discarded populations an
  overworld smoke. Holding a chunk's liquids only until its 3x3 was
  unlocked still lost 18 in one run; held until every chunk within two is
  populated as well, no overworld boot of this plugin has discarded one:
  241 to 462 blocks of air turned to flowing water in each of the plugin
  owner's runs, 264 to 369 in three CTest smokes (spawn area 16.5 to
  19.7 s), and 2792 after the observation's five minutes of flow, 32 of
  them the engine's lava hardened to obsidian — nothing else different
  from the engine in any. The CTest smokes and the observation now fail
  on a single discarded population. The price: a chunk at the edge of
  what is populated keeps its aquifer fluid still until the chunks beyond
  it are populated. Whether the flow is Java's is unobserved.
- **Where a flow could still go wrong**, read from 5.44.3 and not
  observed: `World::getBlockAt()` answers air for a chunk that is not
  loaded, and `World::setBlockAt()` loads a generated chunk to write into
  it and throws for an ungenerated one. Every chunk within three of a
  settled chunk is generated, so only a flow running on past all of them
  would throw inside a scheduled update; one reaching a chunk unloaded in
  the meantime would read it as air and could overwrite what is there. No
  run has shown either.
- **Only a world whose options name its blob relatively can move.**
  `WorldFactory` writes `worlds/<name>/stratum-pipeline.blob`, read against
  the data path `Main` gives the extension, and such a world loads from a
  moved or renamed server directory: `006_moved_server.phpt` against
  stand-ins, and once in the real server (a scratch run, not a CTest
  case): a server whose default world names its blob relatively, with an
  End world `WorldFactory` made, moved whole to another path, booted
  again, loaded both, generated a new chunk in each, and every chunk on
  disk matched the engine's encode. Every CTest smoke now names its
  world's blob relatively too, so the real server reads a relative path on
  its main thread and its workers in each; the move itself is not a CTest
  case. A world whose options hold an absolute path — every world made
  before this — still loads only where it was made: moved, PocketMine-MP
  refuses it at load, and a default world so refused stops the server at
  startup (measured).
- **An End world generates, and is presented as the overworld.** The End
  smoke boots, generates and stops cleanly, every biome Bedrock's one End
  biome. But PocketMine-MP 5.44.3 names `DimensionIds::OVERWORLD` to every
  client whatever the world (`PreSpawnPacketHandler`, `NetworkSession`),
  so its sky, fog and light will be the overworld's. `WorldFactory::create()`
  with `THE_END` writes the End's options and spawns the world at
  (0, 128, 0), over the main island: in the real server (the scratch run
  above) its 224-chunk spawn area held end stone in 608 sub-chunk palettes,
  biome 9 throughout, every block and biome the engine's. A `worlds:`
  entry cannot set a spawn, so an End world made that way gets
  PocketMine-MP's default, (256, 70, 256), over the void between the main
  island and the outer ones, and its whole 224-chunk spawn area is air;
  `pmmp.smoke.end`'s world is such an entry, and the test-only evidence
  plugin, not this one, sets its spawn to (0, 70, 0) as it is created, so
  that the smoke compares the main island. Read from source and measured
  headless; no client has seen it.
- Fluid updates live in memory between a chunk's generation and their
  release once it has settled — held through worker restarts while the
  world is loaded and pinned, but not through an unload: a chunk generated
  as a neighbour, or populated at the edge of what is populated, in one
  server run (or one load of the world) and settled only in a later one
  gets none, and its aquifer fluid stays as generated until something
  beside it changes. PocketMine-MP saves no scheduled updates either, so a
  flow the server stops mid-way stops there.

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
