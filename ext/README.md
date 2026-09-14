# `ext/` — PocketMine-MP zend binding

Milestone **M5** (SPEC §4.3). Empty by design until then; configuring with
`-DSTRATUM_BUILD_EXT=ON` is a hard configure error rather than a silent
no-op.

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
