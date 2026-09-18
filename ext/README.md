# `ext/` — PocketMine-MP zend binding

Milestone **M5** (SPEC §4.3, §9; §11's M5-PMMP entry has the boundary as
read from PocketMine-MP and chunkutils2 source, and PROGRESS.md the slices).

**Landed: `stratum_pmmp_core`** (`include/stratum_pmmp/chunk_encoder.hpp`),
plain C++ with no PHP in it, built and tested on every CI platform.
`encodeChunk()` generates a chunk through `world::CompiledDimension` and
packs every sub-chunk into `PalettedBlockArray::fromData`'s own arguments
for chunkutils2 0.3.x: host-endian `uint32` words in XZY order, the
smallest width chunkutils2 accepts ({0,1,2,3,4,5,6,8,16}) for a
deduplicated palette, and no block layer for an all-air sub-chunk. Block
palettes hold Java state ids for the PHP side to translate; biome palettes
hold Bedrock biome ids. `tests/chunk_encoder_test.cpp` pins the byte
lengths chunkutils2's own tests assert; `tests/vanilla_chunk_encoder_test.cpp`
decodes real overworld chunks back to exactly what was generated.

**Landed: the zend module** (`zend/php_stratum.cpp`; `zend/stratum.stub.php`
documents its PHP surface, SPEC §4.3). Marshaling only: it freezes a
pipeline, opens a dimension from a frozen blob — one compiled dimension per
process for every worker thread that opens the same world — and returns
`encodeChunk()`'s layers and `bedrockBlockState()`'s triples as PHP arrays.
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

**Landed: the PocketMine-MP plugin** (`plugin/`, its own README): registers
the generator, translates each sub-chunk's palette through PocketMine-MP's
own deserializer, and freezes a pipeline into a new world's folder. Written
against PocketMine-MP 5.44.4's source but **never run** — PocketMine-MP
cannot be installed here — so CI checks that it parses and tests the one
class that is pure PHP.

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
