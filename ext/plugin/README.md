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
  the seed and the world's options string. It opens the world's frozen
  pipeline (the engine shares one compiled dimension across every worker
  that opens the same blob, settings, list and seed) and, per chunk, hands
  each sub-chunk's already-packed layers to
  `PalettedBlockArray::fromData()`. The only PHP work per chunk is
  translating a handful of palette entries; there is no per-block PHP.
- `BlockStateTranslator` turns Java block state ids into PocketMine-MP's own
  state ids through **PocketMine-MP's own** upgrader and deserializer — the
  same path its world loader takes for every palette entry on disk — cached
  per worker.
- `GeneratorOptions` is the world's options string:
  `{"blob":"…/stratum-pipeline.blob","settings":"minecraft:overworld","biomes":"minecraft:overworld"}`.
  A generator receives nothing else — not the server, not the plugin, not a
  config — so the path to the frozen pipeline travels in here.
- `WorldFactory::create()` freezes the pipeline into the new world's own
  folder and then creates the world from it, in that order: creating the
  world immediately registers the generator on a worker, which opens the
  blob.

## Installing

The engine is a PHP extension, not part of the plugin: build it with
`tools/php-dev` + `cmake --preset ext` (see `ext/README.md`) and load
`stratum.so` in the server's `php.ini`. `plugin.yml` declares
`extensions: {stratum: "*"}`, so a server without it refuses the plugin by
name at load time instead of failing inside a worker thread.

```php
// Creating a Stratum world, e.g. from a command or another plugin:
\Stratum\PMMP\WorldFactory::create($server, "overworld", seed: 12345, versionRoot: "/path/to/1.21.11");
```

`versionRoot` is a version root as `tools/fetch-vanilla` leaves it —
`worldgen/` and `biome_parameters/` side by side.

## What is not verified

**This plugin has never run.** PocketMine-MP cannot be installed here: its
own PHP releases ship no headers, and it needs a stack of extensions
(pmmpthread, leveldb, igbinary, morton, …) that `tools/php-dev` does not
build. So:

- Every API it calls was read from PocketMine-MP 5.44.4's source and
  independently re-checked (SPEC §11's M5-PMMP entry lists what that
  corrected), but nothing here has been executed against a real server.
- CI checks that every file parses (`php -l`) and runs
  `tests/001_generator_options.phpt`, which exercises the one class that is
  pure PHP against a faithful stand-in for the single PocketMine-MP class it
  touches.
- The rest — registration, chunk assembly, block translation — needs a real
  PocketMine-MP server to verify, and that is the next real step for this
  binding (PROGRESS.md).

## Traps this code is written around

Each of these produces a **wrong world rather than an error**, and each was
found by reading PocketMine-MP's source rather than by running it:

- A sub-chunk key `Chunk::__construct` does not find becomes air **and an
  all-ocean biome array**. Keys are signed indices (-4..19); a 0-based array
  would shift the world vertically with nothing to say so.
- An unmapped biome id is silently replaced with `OCEAN` **on the wire**
  (`ChunkSerializer`), with no exception and no log.
- A block PocketMine-MP does not implement (`minecraft:powder_snow` is a
  real, measured example that the overworld's surface rules place) would
  otherwise become PocketMine-MP's `info_update`; the fallback table names
  it, and every miss is logged once.
