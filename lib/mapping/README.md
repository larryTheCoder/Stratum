# `lib/mapping/` — Java → Bedrock mapping layer

Milestone **M5** (SPEC §9). Empty by design until then.

This component is deliberately **downstream of the conformance boundary**:
Tier-A parity is diffed in *Java block space* (SPEC §7) before anything is
mapped to Bedrock. Nothing in here may influence generation.

When it lands it owns, with its own tests:

- Java block state → Bedrock runtime state, from maintained mapping data
  (GeyserMC mappings / pmmp upgrade schemas as reference inputs).
  Unmappable states resolve through an explicit, configurable fallback
  table — never a crash, never a silent stone substitution without a log.
- Biome mapping: custom/datapack biomes fall back to the nearest vanilla
  Bedrock biome for client-side fog/colour/music, while the engine's
  internal biome identity is preserved for generation.
