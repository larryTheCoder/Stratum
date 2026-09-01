# `ext/` — PocketMine-MP zend binding

Milestone **M5** (SPEC §4.3). Empty by design until then; configuring with
`-DSTRATUM_BUILD_EXT=ON` is a hard configure error rather than a silent
no-op.

Scope when it lands — **marshaling only**, no generation logic:

- World load: PHP hands over the world's stored pipeline blob (SPEC §6) plus
  the seed; the binding compiles it and registers a generator instance.
- Per chunk: `generateChunk(cx, cz)` on the compiled pipeline, returning
  populated `PalettedBlockArray` sub-chunk storages and biome arrays.
- Optional main-thread post-population hooks for plugins, outside the parity
  contract.

PHP never executes inside chunk generation: PMMP worker threads do not have
plugin code loaded. The reverse direction is enforced by lint —
`tools/lint/check-determinism.sh` fails if anything under `lib/` includes a
PHP or zend header.
