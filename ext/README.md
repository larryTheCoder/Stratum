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
length, the palette and every offset. **Not yet:** the PocketMine-MP plugin
that registers a `Generator` and translates palettes (PROGRESS.md).

Scope when it lands — **marshaling only**, no generation logic:

- World load: PHP hands over the world's stored pipeline blob (SPEC §6) plus
  the seed; the binding compiles it and registers a generator instance.
- Per chunk: `generateChunk(cx, cz)` on the compiled pipeline, returning this
  engine's own Java block state per position — the zend side does not do
  Bedrock translation itself.
- **Block state resolution — the last step only** (SPEC §9; §11's "returns
  to `lib/mapping/`" entry has the measured reasoning). `lib/mapping/`
  supplies Java block state → Bedrock blockstate `{name, states, version}`.
  This binding turns each distinct one into PMMP's own internal state id
  through PMMP's own code — `GlobalBlockStateHandlers::getUpgrader()`'s
  `BlockStateUpgrader`, then `getDeserializer()->deserialize()`, the same
  path PMMP's LevelDB loader takes for every palette entry on disk. It does
  this once per distinct state when the generator starts, not per block,
  and writes the resulting ids through `Chunk::setBlockStateId()` like
  PMMP's own `Normal`/`Flat` generators do. No per-protocol table: a
  generator never sees which Bedrock version will connect. That translation
  is PMMP's networking layer's job (`TypeConverter`, at chunk send).
  - Catch `UnsupportedBlockStateException` here so SPEC §9's explicit
    fallback table applies — PMMP's own loader silently substitutes
    `info_update`.
  - Log, at world load, any difference between `lib/mapping/`'s blockstate
    version and the running PMMP's `BlockStateData::CURRENT_VERSION` (equal
    today: both 1.21.60.33). PMMP's upgrader only moves states forward, so
    a newer table's changed states surface as per-state misses.
  - Ship a fallback entry for `minecraft:powder_snow` from the start: PMMP
    `stable` has no powder snow block, and the overworld's surface rules
    emit it.
- Optional main-thread post-population hooks for plugins, outside the parity
  contract.

PHP never executes inside chunk generation: PMMP worker threads do not have
plugin code loaded. The reverse direction is enforced by lint —
`tools/lint/check-determinism.sh` fails if anything under `lib/` includes a
PHP or zend header.
