# `ext/` — PocketMine-MP zend binding

Milestone **M5** (SPEC §4.3, §9; §11's M5-PMMP entry has the boundary as
read from PocketMine-MP and chunkutils2 source, and PROGRESS.md the slices).

**Landed: `stratum_pmmp_core`** (`include/stratum_pmmp/chunk_encoder.hpp`),
plain C++ with no PHP in it, built and tested on every CI platform.
`encodeChunk()` generates a chunk through `world::CompiledDimension` and
packs all 24 of PocketMine-MP's sub-chunks — those outside a shorter
dimension's height, such as the End's y 0..127, as air with the biome
source's biomes read at their own y, never left to PocketMine-MP's
all-ocean default — into `PalettedBlockArray::fromData`'s own arguments
for chunkutils2 0.3.x: host-endian `uint32` words in XZY order, the
smallest width chunkutils2 accepts ({0,1,2,3,4,5,6,8,16}) for a
deduplicated palette, and no block layer for an all-air sub-chunk. Block
palettes hold Java state ids for the PHP side to translate; biome palettes
hold Bedrock biome ids. `tests/chunk_encoder_test.cpp` pins the byte
lengths chunkutils2's own tests assert; `tests/vanilla_chunk_encoder_test.cpp`
decodes real overworld and End chunks back to exactly what was generated.

**Landed: the zend module** (`zend/php_stratum.cpp`; `zend/stratum.stub.php`
documents its PHP surface, SPEC §4.3). Marshaling only: it freezes a
pipeline, opens a dimension from a frozen blob — one compiled dimension per
process for every worker thread that opens the same world — and returns
`encodeChunk()`'s layers and `bedrockBlockState()`'s triples as PHP arrays.
`Stratum\parseBiomeSource()` reads a biome source as opening a dimension
will, and opens nothing (`zend/tests/006_biome_source.phpt`);
`Stratum\checkDimension()` is the whole of opening one — read, every check
the blob's header and content hash are held to, compile, and what
PocketMine-MP cannot hold (a dimension outside its 24 sub-chunks, a
parameter list naming a biome with no Bedrock id, a block state the pack
names — `default_block`, `default_fluid`, a surface rule's `result_state` —
that the mapping table cannot place) — with no seed and nothing kept, so a
caller can refuse on the main thread everything a worker would otherwise
die on, as the plugin's world-load validation does
(`zend/tests/008_pins.phpt`, `plugin/tests/003_world_load_validation.phpt`). Compiled dimensions
are shared by the blob's every byte, never by the header's payload hash, so
a blob restamped with another engine version is refused even while its
twin is open (`zend/tests/007_frozen_header.phpt`). `Stratum\pinWorld()`
holds a world's state on the main thread from world load to unload, so a
worker PocketMine-MP shuts down while idle takes neither its held fluid
updates nor its compiled state with it. The registry, the pins and the
outboxes are plain C++ in `stratum_pmmp_core`
(`include/stratum_pmmp/dimension_registry.hpp`, `worlds.hpp`), held to
`tests/worlds_test.cpp` under threads with a pipeline the test freezes
itself; `Stratum\freezePipeline()` writes whole or not at all — beside
the target, then renamed over it; on POSIX synced to disk before the rename
and the directory after, so a power cut leaves one blob or the other — and
first removes the `<blob>.partial-*` a write killed part-way left.
A blob path may be relative: `Stratum\setDataPath()` gives the process the
server's data path to read it against — once, on the main thread, before
any world loads; PocketMine-MP's worker threads share it — so a world whose
options name its blob relatively moves with its server directory, and a
relative path and the absolute one it names are one world
(`zend/tests/009_data_path.phpt`). An absolute path is read as it is.
C++ exceptions never cross into the engine; each becomes a
`Stratum\GenerationException`. Build it against a thread-safe PHP:

```
tools/php-dev                                       # PHP 8.2.30 ZTS + chunkutils2 0.3.5, ~1 min
STRATUM_PHP_CONFIG=$(tools/php-dev --print-config) cmake --preset ext
cmake --build --preset ext --target stratum_php
ctest --preset ext                                  # PHPT, with the real chunkutils2 loaded
```

`zend/tests/` hands every sub-chunk of real vanilla chunks to chunkutils2's
own `PalettedBlockArray::fromData`, which validates the width, the word
length, the palette and every offset.

**Landed: fluid updates** (`include/stratum_pmmp/fluid_updates.hpp`). The
positions a Java server keeps in a chunk's `PostProcessing` lists and ticks
once it loads — what makes aquifer water and lava flow — come back from
`encodeChunk()` beside the sub-chunks. A generator's worker cannot hand them
to PocketMine-MP with the chunk (only terrain crosses back), so the zend
module leaves them in a per-world outbox and `Stratum\takeFluidUpdates()`
gives them to the main thread, once, by the world's options and seed.
`tests/fluid_updates_test.cpp` covers the hand-over, threads included;
`zend/tests/004_fluid_updates.phpt` the PHP function.

**Landed: the PocketMine-MP plugin** (`plugin/`, its own README): registers
the generator, translates each sub-chunk's palette through PocketMine-MP's
own deserializer, freezes a pipeline into a new world's folder, refuses on
the main thread every world that cannot generate, and pins each Stratum
world's state while it is loaded. Read from PocketMine-MP 5.44.4-dev's
source and 5.44.3's phar, and run in 5.44.3 (below); CI checks that it
parses and tests its classes against stand-ins for PocketMine-MP's, with
this extension loaded.

**Run inside a real PocketMine-MP 5.44.3, headless** (`tools/pmmp-dev`,
`ctest --preset pmmp`; local evidence, not CI). `cmake --preset
ext-release` builds the optimised stratum.so for it, against tools/php-dev's
PHP: PocketMine-MP's own PHP (pmmp/PHP-Binaries) is the same 8.2.30 ZTS
with the same build id, and the module loads into it beside pmmpthread,
chunkutils2, leveldb and the rest and passes every PHPT there.
`pocketmine/` holds the checks that run under PocketMine-MP's own classes:
the block state and biome censuses, a pmmpthread case, what a stopped
server left on disk (`inspect_world.php`), and what `tools/pmmp-dev
observe` saw (`check_observation.php`). It is also a test-only plugin,
never part of Stratum's (`pocketmine/plugin.yml`,
`src/EvidencePlugin.php`): packed beside it by `tools/pmmp-dev`, it sets
the End smoke's spawn over the main island and reports, from inside the
server, what the observation needs. `plugin/README.md` says what they
found.

**What the generation task costs through PHP** (`tools/pmmp-task-cost`;
`ctest --preset pmmp-task-cost` runs its check; local evidence, not CI).
`pocketmine/task_cost.php` plays a PocketMine-MP worker on a
`pocketmine\thread\Thread` (`src/TaskCostThread.php`): PocketMine-MP's own
`AsyncGeneratorRegisterTask` builds the plugin's `StratumGenerator`, and each
chunk is PocketMine-MP's own `PopulationTask`, timed per chunk in parts —
the zend call, the plugin's PHP (`src/TimedGenerator.php` times the two
halves of `generateChunk()` apart), PocketMine-MP's serialisation of the
result, and the whole — over the bench's own chunks, every one held to the
bench's dump (`src/ChunkDump.php`). The script's header says what is timed
and what is left out; `tools/README.md` says how it is run.

## How the pieces fit

1. `lib/` generates Java block states and biomes; `lib/mapping/` turns those
   into Bedrock blockstates and biome ids (SPEC §9).
2. `stratum_pmmp_core` packs them into chunkutils2's own layer format.
3. The zend module hands those layers to PHP, along with
   `bedrockBlockState()` for the palette entries.
4. `plugin/` turns each palette entry into a PocketMine-MP state id using
   **PocketMine-MP's own** upgrader and deserializer — the same path its
   world loader takes for every palette entry on disk — and builds the
   `SubChunk`s. A block PocketMine-MP does not implement goes through an
   explicit fallback with a log, never a silent substitution (SPEC §9).
5. Once PocketMine-MP has populated a chunk and it has settled — every
   chunk within two of it populated too, so no population can be under
   way or still to come in its 3x3, where its flow first sets blocks —
   `plugin/` takes its fluid updates and schedules a block update at each
   liquid, so aquifer water and lava flow by PocketMine-MP's own liquid
   rules, from where a Java server would set them flowing. A chunk waiting
   to settle is looked at only when a chunk within two of it is populated
   or loaded, let go when it unloads and held again when it is next
   loaded; its positions wait in the extension's outbox meanwhile, for as
   long as the world is loaded.

No per-protocol table appears anywhere in this: a generator never learns
which Bedrock version will connect. That translation belongs to
PocketMine-MP's networking layer (`TypeConverter`, at chunk send), which is
why SPEC §11 moved block state mapping back out of the bindings.

Plugin PHP *does* run on PocketMine-MP's generation worker threads — that is
where a `Generator` lives, and a phar plugin's classes are autoloadable
there. What never happens is per-block PHP: the engine hands over whole
packed sub-chunks, so each chunk costs PHP only a handful of palette
lookups.

The reverse direction is enforced by lint — `tools/lint/check-determinism.sh`
fails if anything under `lib/` includes a PHP or zend header.
