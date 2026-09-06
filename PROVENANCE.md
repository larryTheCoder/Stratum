# PROVENANCE — Aquifer clean-room research run

Recorded per `RESEARCH.md` §1. This file is the acquisition record the spec header references. It documents **artifacts and toolchain only** — it contains no source-derived content.

- **Session date:** 2026-09-06
- **Target version:** Minecraft: Java Edition **1.21.11** — `release`
- **Brief version:** `RESEARCH.md` as of this session
- **Scope of run:** aquifer system only (`RESEARCH.md` §2)

## 1. Version selection

Resolved through Mojang's official `piston-meta` version manifest. The `1.21.11` entry used is typed `release`, published `2025-12-09T12:23:30+00:00`.

Nine `1.21.11*` entries exist in the manifest; the eight typed `snapshot` (`-pre1`…`-pre5`, `-rc1`…`-rc3`) were **rejected** per §1's prohibition on pre-release and release-candidate jars. Only the `release` entry was downloaded.

| | |
|---|---|
| Manifest | `https://piston-meta.mojang.com/mc/game/version_manifest_v2.json` |
| Manifest SHA1 (as fetched) | `ee50ac4121cc1af4bf7a1180dee625be687bdaa1` |
| Version JSON | `https://piston-meta.mojang.com/v1/packages/a8fbc2763ec360d88eb92a791db7a6f79cdda346/1.21.11.json` |
| Version JSON SHA1 | `a8fbc2763ec360d88eb92a791db7a6f79cdda346` — **verified against manifest** |
| Declared Java requirement | `java-runtime-delta`, major 21 |
| Embedded `version.json` | `world_version` 4671 · `protocol_version` 774 · `data_major` 94 / `data_minor` 1 · built `2025-12-09T12:20:42+00:00` |

## 2. Downloaded artifacts

All SHA1s and byte sizes verified against the values declared in the version JSON. Every check passed.

| Artifact | Source | Size (B) | SHA1 | Verified |
|---|---|---|---|---|
| `server.jar` (bundler) | `piston-data.mojang.com/v1/objects/64bb6d76…/server.jar` | 56 327 581 | `64bb6d763bed0a9f1d632ec347938594144943ed` | ✅ |
| `server-mappings.txt` (official obfuscation mappings) | `piston-data.mojang.com/v1/objects/5621e925…/server.txt` | 8 678 606 | `5621e9253f05fd57872bbe7f8ddf5f9a7d525955` | ✅ |

The 1.21 server download is a bundler. The inner jar was extracted and verified against the SHA-256 the bundler itself declares in `META-INF/versions.list`:

| Inner artifact | Size (B) | SHA-256 | Verified |
|---|---|---|---|
| `META-INF/versions/1.21.11/server-1.21.11.jar` | 19 781 420 | `ec47239a8de246335e1d54f6ac319bd35641778eb4b6a6da06372840d02fcebc` | ✅ |

Class-file major version of the inner jar: **65** (Java 21), consistent with the manifest's declared runtime.

Mappings format: ProGuard, oriented **named → obfuscated**; remapping therefore applied them in reverse.

## 3. Toolchain

Both tools retrieved from Maven Central and verified against the repository's published `.sha1` sidecars. Neither exposes a `--version` flag; the Maven coordinate plus artifact SHA1 is the version pin.

| Step | Tool | Coordinate | SHA1 | Verified |
|---|---|---|---|---|
| Remap | AutoRenamingTool | `net.neoforged:AutoRenamingTool:2.0.4:all` | `12da27cc4caf32e429af9a91f79e1f66a965e667` | ✅ |
| Decompile | Vineflower | `org.vineflower:vineflower:1.11.1` | `77767cdbc76d116c6d5e1565ec387e531085b9f4` | ✅ |

Host JDK: OpenJDK **25.0.4** (2026-07-21), Ubuntu build `25.0.4+7-0ubuntu1-26.04`.

**Note:** MCP-Reborn's own Gradle `setup` was deliberately **not** used. It applies MCP-style community names, whereas §1 requires the official mappings; using it would also have layered a second body of third-party naming over the tree. The `src/` folder remains unpopulated and this run is independent of it.

### Exact invocations

Remap (reverse-applying the official mappings):

    AutoRenamingTool --input <inner server jar> --output <mapped jar>
                     --map <official mappings> --reverse

Decompile:

    vineflower -dgs=1 -din=1 -rsy=1 -asc=1 -jrt=current -log=WARN
               <mapped jar> <output dir>

Decompilation exited **0**. Warnings emitted were limited to inconsistent inner-class attribute entries and one duplicate-lambda notice — expected noise for a remapped obfuscated jar, and none in scope for this run.

### 3a. Verification toolchain (added after §1)

Empirical verification of Q4.4–Q4.8 required compiling and running the target. Recorded for reproducibility:

| Purpose | Tool |
|---|---|
| Compile / disassemble | Amazon Corretto **21.0.12.1** (`javac --release 21`, `javap`) |
| Run | the remapped jar of §4, with signature files stripped on a **copy** |
| Libraries | the 39 jars bundled in `META-INF/libraries` of the downloaded `server.jar` |

The remapped jar inherits Mojang's signature manifest, which fails verification once class bytes change; signatures were therefore removed from a working copy only. The artifact recorded in §4 is untouched and still hashes to `213239d7…3074e6`.

Server runs used a fixed seed (`4004`), 4 096 force-loaded chunks, and a patched build of the aquifer class placed ahead of the jar on the classpath.

**A defect in the decompiled tree.** Compilation surfaced a decompiler fault: in one branch of the aquifer path, two distinct variables were emitted under the same identifier, so that code neither compiles nor says what the class does. It was repaired **in the private build copy only**; the recorded tree and its digest are unchanged. The affected claim (Q2.1) was re-verified against bytecode rather than the faulty text. This is the concrete reason no claim in the spec should rest on decompiled text alone.

### 3b. Reproducibility baseline (REVIEW-01 Task A, REVIEW-02 §2/§3)

An attempt to anchor the differential harness to the shipped engine by generating the same 4 096 chunks and comparing worlds. Comparison was per chunk: block arrays block-for-block (palette-order independent) and post-processing entries as position sets. Byte comparison of region files is invalid and was not used.

**The controlling result: vanilla is not run-to-run reproducible at full generation.** Two runs of the *untouched* official jar, same seed, same properties, same chunks, differ from each other:

| comparison | chunks differing | differing cells | water/lava cells | pair types |
|---|---|---|---|---|
| official run1 vs official run2 | 1 770 / 4 096 | 101 792 (252.8 ppm) | 967 | 274 |
| official vs §3a patched build | 1 800 / 4 096 | 116 818 (290.1 ppm) | 913 | 260 |

Same character, same magnitude; the patched build sits inside the noise the official jar generates against itself. Both are dominated by decoration-stage pairs (`air↔spruce_leaves`, `air↔snow`, `andesite↔granite↔diorite`, `dirt↔grass_block`). The cause is order-sensitivity of the decoration stage under concurrent chunk generation.

**Shape test** on the water/lava-involving cells, applied to both comparisons. A shifted aquifer fluid level would appear as connected horizontal sheets; a flipped barrier as lattice-aligned walls.

| | official vs patched | official vs official |
|---|---|---|
| clusters (26-connectivity) | 162 | 154 |
| largest cluster | 205 | 205 |
| single-y sheets | 8 | 7 |
| cells adjacent to a feature diff | 91.3% | 82.2% |

Seven of the eight sheets in the first column are `water↔ice` at the water surface (y=62, y=55) — ice is not an aquifer output. The eighth, a 12-cell `water↔stone` sheet at y=15, was the one aquifer-shaped candidate; **the same 12-cell sheet, at the same y and the same lattice cells, appears in the official-versus-official control.** It is ambient, not a harness artifact.

**Conclusion.** The acceptance criterion "zero block differences at full generation" is unachievable by any build, including the official jar against itself; the comparison scope was invalid. Recorded per REVIEW-02 §1 as an owner-side design error, not an execution fault. The differential harness is neither confirmed nor impeached by this measurement — it is not resolved by it either way. Anchoring is retried under §3c.

### 3c. Task A' — anchoring on a feature-stripped pack (REVIEW-02 §4)

Derived datapack at `research/verify/pack/nofeatures` (private, never leaves this repository): all 65 vanilla biomes with every placed-feature step emptied (2 641 placed features removed, step counts preserved), **carvers retained untouched**, no other field altered. `pack_format` 94 with matching `min_format`/`max_format`. A first attempt used a `pack.mcmeta` lacking `min_format`/`max_format`, which 1.21.11 rejects; that run was aborted before any comparison and the pack corrected.

Strip verified by census against a full-vanilla world: `spruce_leaves`, `coal_ore`, `iron_ore`, `diorite`, `andesite`, `snow`, `short_grass`, `clay` all fall to **0**; `bedrock` is identical (884 742); water and lava remain. Residual granite / logs / moss are structure-placed, not feature-placed, and appear on both sides. Noise-stage fill is therefore intact.

Same 4 096 chunks, seed 4004, one run each on the untouched official jar and on the §3a patched build, run sequentially to avoid CPU contention.

**Result: differences remain. Acceptance not met.**

| | value |
|---|---|
| chunks with a block difference | 1 837 / 4 096 (44.8%) |
| differing cells | 32 439 (80.6 ppm) |
| distinct pair types | 22 (was 260 unstripped) |
| post-processing set differences | 2 |

Complete pair list, by count: `ice↔water` 28 272; `dirt↔grass_block` 3 766; `air↔water` 200; `water↔water` (state) 89; `sand↔water` 27; `cave_air↔stone` 24; `air↔sand` 11; `cave_air↔cobweb` 10; then 14 further pairs of ≤8 occurrences, all mineshaft/structure blocks (`spawner`, `rail`, `wall_torch`, doors, fences).

Observations, offered as observations only: stripping features removed ~72% of the differing cells and 92% of the pair types; 99.4% of what remains lies in section Y=3 (the y≈48–63 band containing the water surface); `ice↔water` and `dirt↔grass_block` alone are 98.8% of it; and no difference appears in the deep terrain skeleton — no `stone↔water`, no `deepslate↔water`, no barrier-shaped pair anywhere in the list. Whether the residual is itself ambient was **not** measured: a stripped-official-versus-stripped-official control was not run, per the no-variation rule. That control is the decisive next experiment, ~14 minutes.

**Across the aquifer's entire observable domain, the patched build matched the untouched official jar exactly in Task A'.** Every residual difference class is a post-generation runtime mutation (random-tick freezing and grass spread, scheduled fluid ticks, gravity, and mineshaft blocks from structures — which were never stripped, structures being a separate registry from features), not a generation-stage output. This is the substantive anchor, pending the formal one.

Q4.6/Q4.7 remain unanchored pending that formal anchor. No confidence tier was changed on the strength of this run.

### 3d. Task A'' — anchoring on pure generation output (REVIEW-03 §3)

Pack v2 at `research/verify/pack/nofeatures2` (private): pack v1 plus all 20 structure sets emptied (34 structure entries removed), carvers retained. Loads without error.

**Method note (ratified by REVIEW-04 §1 — a ruling, not a violation).** REVIEW-03 §3 requires draining pending ticks to empty and forbids comparing non-quiescent worlds. That is not reachable on this build: unfreezing re-queues scheduled fluids into pending, so the drain does not converge (measured 211 -> 207 pending fluid ticks across two 80-second cycles, with an earlier fixed 180-second drain leaving 190 across 37 chunks). The runs therefore stayed **frozen from before generation through to save**, applying zero simulation, so the saved state is pure generation output and the tick lists are non-empty by design. To avoid sweeping that under the rug, `block_ticks` and `fluid_ticks` were **added to the comparison** as position sets (kind, x, y, z, delay, priority) rather than being drained away. The worlds compared are *not* quiescent in the §3 sense. REVIEW-04 §1 adopted this frozen regime as the criterion, retroactively for A'' and for all future anchoring runs: it compares generation itself rather than the endpoint of post-generation evolution, which is the thing being anchored.

Tick discipline, applied identically in both runs before any force-load and verified in each console log (1.21.11 moved game rules to a registry with snake_case ids; the old camelCase names are silently rejected as "Incorrect argument for command", which cost one invalid run):

    gamerule random_tick_speed 0
    gamerule advance_weather false
    gamerule fire_spread_radius_around_player 0     (no boolean doFireTick exists)
    weather clear 1000000
    tick freeze                                     -> "The game is frozen"

`max-tick-time=-1` in both `server.properties`; the server logs "Can't keep up!" under 4 096 force-loaded chunks, so the watchdog would otherwise kill the run.

**Result: 0 block differences; 1 post-processing set difference.**

| channel | differences |
|---|---|
| block arrays, all 4 096 chunks | **0** |
| `fluid_ticks` sets | **0** |
| `block_ticks` sets | **0** |
| post-processing sets | **1 chunk** |

The single difference is chunk (17, -16): 109 entries, distributed {section 3: 93, section 4: 13, section 6: 3}, present in the official run and absent in the patched run. Both chunks are status `full`.

**The same chunk, the same 109 entries, and the same per-section distribution appear in the official-versus-official control** (REVIEW-03 §2), there present in run 2 and absent in run 1. The list is all-or-nothing and flips between runs of the untouched jar.

Acceptance ("zero block differences and zero post-processing set differences") is **not met**. Stopped and reported; no re-patch, no variation. Q4.6/Q4.7 remain unanchored and no confidence tier was changed.

### 3e. The frozen anchoring regime (ratified) and its measured noise floor

REVIEW-04 §1 ratified the regime below as the anchoring criterion. Recorded in full so it can be reimplemented without reference to this session.

**Recipe.**

1. `server.properties`: `level-seed=4004`, `pause-when-empty-seconds=0`, **`max-tick-time=-1`** — the server logs "Can't keep up!" under 4 096 force-loaded chunks and the watchdog would otherwise kill the run.
2. Datapack in `<world>/datapacks/` before first boot. Pack v2 = all 65 biomes with every placed-feature step emptied (step counts preserved) and all 20 structure sets emptied (34 entries), carvers retained untouched. `pack.mcmeta` needs `pack_format`, `min_format` **and** `max_format` (94); omitting the latter two is rejected by 1.21.11.
3. After boot, before any force-load, issue the discipline commands and **verify each acknowledgement in the log**; abort the run on any rejection. 1.21.11 moved game rules into a registry with snake_case ids and silently rejects the old camelCase names as "Incorrect argument for command":

       gamerule random_tick_speed 0                  -> "Gamerule ... set to: 0"
       gamerule advance_weather false
       gamerule fire_spread_radius_around_player 0   (kept; no boolean doFireTick
                                                      exists in 1.21.11. Freeze
                                                      already covers it - this is
                                                      belt-and-suspenders.)
       weather clear 1000000
       tick freeze                                   -> "The game is frozen"

4. Force-load in batches of at most 256 chunks (a 256x256 block square); larger requests are refused.
5. **Never unfreeze.** Unfreezing re-queues scheduled fluids into pending, so a drain-to-empty does not converge (measured 211 -> 207 over two cycles).
6. `save-all flush`, then `stop`.
7. Compare four channels, all as generation output: block arrays block-for-block (palette-order independent); `block_ticks` and `fluid_ticks` as position sets; post-processing as position sets. Byte-comparison of region files is invalid.

**Comparator contract — two tolerance classes, both final.**

*Consumption race (REVIEW-04 §2).* A per-chunk post-processing difference in which one side's entire list is empty and the other's is non-empty. Logged with chunk and count; does not fail acceptance. Any partial set difference is a hard failure.

*Flow-state race (REVIEW-05 §1).* A block-channel difference where both sides are the same fluid differing **only** in the `level` property, accompanied by a `fluid_ticks` set difference at that exact position. Logged with position and both states; does not fail acceptance, and the paired tick difference is folded into the class rather than counted separately — counting it twice would make the class unusable, since the pairing is what defines it. Everything outside that predicate on the block channel is fatal: a level-only difference *without* its paired tick entry, any substance difference, any air-fluid difference. The mechanism is deliberately not investigated; the rate is measured on the untouched jar against itself under matched discipline, which is all the classification needs. Future frozen runs accumulate the log, and any drift in rate or signature reopens it.

Evidence for that rule: across six independent world comparisons, **every** post-processing difference ever observed - 12 of them - was whole-list, and **none** was partial. The affected chunks cluster at x = 1 and x = 17, one chunk past the x = 0 and x = 16 force-load batch boundaries, consistent with a load-threshold crossing.

**Measured noise floor of the frozen regime** (two runs of the untouched official jar, pack v2, identical discipline):

| channel | differences |
|---|---|
| block arrays | 2 cells in 402 653 184 (0.005 ppm) |
| `fluid_ticks` sets | 2 chunks |
| `block_ticks` sets | 0 |
| post-processing | 2 chunks, both whole-list races |

The two block differences are single water blocks at **y = 62** (the water surface) reading `water[level=0]` against `water[level=1]`, each with a matching `water`/`flowing_water` scheduled tick - a surface flow-state race. **This class was not anticipated by REVIEW-04 and is new information: the frozen regime's noise floor is small but not zero.** It does not weaken A''`, which showed zero differences on both of those channels; it does bear on reuse of the regime downstream, and no classification rule has been agreed for it.

**Task A'' result under the amended contract: PASS.**

| channel | A'' (official vs patched) | control (official vs official) |
|---|---|---|
| block arrays | **0** | 2 |
| `block_ticks` | **0** | 0 |
| `fluid_ticks` | **0** | 2 |
| post-processing, partial | **0** | 0 |
| post-processing, whole-list races | 1, logged | 2, logged |

The patched build reproduced the untouched official jar's generation output more closely than the official jar reproduced itself.

## 4. Derived outputs (private — never leave this repository)

| Output | Digest |
|---|---|
| Remapped jar (23 021 198 B) | SHA-256 `213239d7c095891f8649a731db952196e3ff22b333c44a68b2c291f90a3074e6` |
| Decompiled tree — 4 660 `.java` files, 71 MiB | tree digest SHA-256 `1a324c8a513b2b4af6b7d46daf7118389a0fe90373b2fdff4fef033a385e873a` |

Tree digest is `sha256` over the sorted `sha256sum` listing of every file in the tree; recomputable, and stored at `research/decompiled.tree.sha256`.

Layout, all under `research/` (git-ignored):

    meta/       manifest + version JSON as fetched
    artifacts/  server.jar, official mappings, extracted inner jar
    tools/      remapper + decompiler
    work/       remapped jar, decompiled tree
    verify/     verification build and server run (§3a)

## 5. Containment

- `README.md` carries the private / Mojang-derived / do-not-publish notice required by §1.
- `research/` is git-ignored so no derived material can be committed.
- Per §1 and §6, nothing in `research/` — jar, mappings, decompiled tree, or working notes — is shared, quoted, or summarized outside this repository. Only `spec/aquifer-spec.md`, `spec/aquifer-constants.json`, and `AUDIT.md` cross the wall.

## 6. Status — run closed

**§1 complete** — all integrity checks passed.

**§2–§6 complete.** The aquifer path was read, the specification written (`spec/aquifer-spec.md`, 56 claims), the constants extracted (`spec/aquifer-constants.json`), and the filtration self-audit performed (`AUDIT.md`). Claims Q4.4–Q4.8 and Q8.7 were additionally verified empirically against a running server built from these artifacts; the remaining claims rest on reading and carry unexercised `verify-by` proposals.

The four source artifacts recorded above were re-checked after all verification work and remain **byte-identical** to the digests in §2 and §4.

**Run complete.** The specification, constants and audit crossed the wall at the digests recorded above; all four source artifacts remain byte-identical to §2 and §4. The repository is **archived, not deleted** — the recorded artifacts, the harnesses, the two derived packs, and the frozen-regime tooling together are the reproduction path for every claim that crossed. The repository is dormant.

**Reopen conditions, exhaustively.** A carver run of the brief. The legacy random source (open question 7). The next instrumented run's shopping list: the `A₄` four-way-junction rate, which the coverage budget currently warns is zero-coverage, and the rank-1–2 flag path, unreachable by sampling at one event per 4 096 chunks. Or a version bump, which re-runs the brief from §1 against new artifacts — nothing carries over on trust.
