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
- **Block state mapping lives here, in PHP, not in the zend binding's C++**
  (SPEC §9's M5 entry has the full measured reasoning): Java block state →
  Bedrock blockstate NBT (`{name, states, version}`), one table per
  connected client's Bedrock protocol version — Bedrock's own block network
  runtime id has been a hash-sorted index over the whole registry since
  1.16.100, so a single table cannot serve more than one protocol version at
  once. PMMP's own `BlockStateDeserializer` already turns that NBT shape
  into a real `Block` object; this binding needs only produce the NBT, the
  way NetherGamesMC's `BlockTranslator.php` already does per-protocol
  selection for PMMP's own world loading. Populated `PalettedBlockArray`
  sub-chunk storages and biome arrays are the result of that step, not
  something the zend layer hands over pre-translated.
- Optional main-thread post-population hooks for plugins, outside the parity
  contract.

PHP never executes inside chunk generation: PMMP worker threads do not have
plugin code loaded. The reverse direction is enforced by lint —
`tools/lint/check-determinism.sh` fails if anything under `lib/` includes a
PHP or zend header.
