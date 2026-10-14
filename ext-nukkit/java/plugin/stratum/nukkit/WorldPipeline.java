// Stratum — one open Stratum world: its compiled pipeline and Nukkit id table.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
package stratum.nukkit;

import cn.nukkit.Server;
import cn.nukkit.level.Level;
import cn.nukkit.level.biome.Biome;
import cn.nukkit.level.format.leveldb.LevelDBProvider;
import cn.nukkit.utils.MainLogger;

import java.io.File;
import java.util.Collections;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Everything a Stratum world generates through, opened once per world on
 * the thread that loads it and shared by every generation thread (SPEC
 * §4.1). Every refusal is decided here: unreadable options, no frozen
 * pipeline, settings or a biome source outside the binding's scope, a biome
 * with no Bedrock id, a block state Nukkit cannot place. Each is a
 * {@link StratumGenerationException} naming the world and what is wrong.
 *
 * <p>The plugin runs {@link #validate} for every configured Stratum world at
 * {@code STARTUP}, before Nukkit loads or creates any world, and stops the
 * server on a refusal; {@link #open} repeats the same checks when Nukkit
 * loads the world, for worlds that were not configured at startup (see
 * {@link StratumGenerator} for what Nukkit does with a refusal there).
 *
 * <p>Keyed by seed and options string, which is all a generation thread is
 * given to find it by. The options name the blob, so one key is one
 * pipeline.
 */
public final class WorldPipeline {
    private static final Map<String, WorldPipeline> OPEN = new ConcurrentHashMap<>();

    final String key;
    final long bound;
    final int minY;
    final int height;
    final int airFullId;
    final File blob;
    private final String[] stateNames;
    private final int[] stateFullIds;
    private final String[] biomeNames;
    private final int[] biomeIds;

    /** Fluid updates of chunks generated and not yet populated, by chunk. */
    private final Map<Long, int[]> fluidUpdates = new ConcurrentHashMap<>();

    private final AtomicLong chunksGenerated = new AtomicLong();
    private final AtomicLong fluidUpdatesScheduled = new AtomicLong();
    private final Set<String> generationThreads = ConcurrentHashMap.newKeySet();

    private WorldPipeline(String key, long bound, int minY, int height, int airFullId, File blob,
                          String[] stateNames, int[] stateFullIds, String[] biomeNames, int[] biomeIds) {
        this.key = key;
        this.bound = bound;
        this.minY = minY;
        this.height = height;
        this.airFullId = airFullId;
        this.blob = blob;
        this.stateNames = stateNames;
        this.stateFullIds = stateFullIds;
        this.biomeNames = biomeNames;
        this.biomeIds = biomeIds;
    }

    static String keyOf(long seed, String options) {
        return seed + "/" + options;
    }

    /** The open world for this key, or null. */
    static WorldPipeline find(String key) {
        return OPEN.get(key);
    }

    /** The open Stratum world behind {@code level}, or null if it is not one. */
    public static WorldPipeline of(Level level) {
        final Object options = level.getProvider().getGeneratorOptions().get("preset");
        return OPEN.get(keyOf(level.getSeed(), options == null ? "" : options.toString()));
    }

    /**
     * Every check {@link #open} makes that does not need Nukkit's
     * {@code Level}: the options, the frozen pipeline, the engine's own
     * refusals (settings and biome source in scope, every biome mapped), and
     * every block state the world can place resolved through Nukkit's
     * {@code BlockStateMapping} and bound. None of them depends on the seed,
     * so the pipeline is compiled for seed 0 and released again; opening
     * the world compiles it once more, for its own seed.
     */
    static void validate(String world, String options) {
        final GeneratorOptions parsed = parse(world, options);
        final File blob = requireBlob(world, parsed);
        final long pipeline = openNative(world, blob, parsed, 0L);
        try {
            final Resolved resolved = resolveStates(world, pipeline);
            NativePipeline.releaseBound(bindNative(world, pipeline, resolved));
        } finally {
            NativePipeline.release(pipeline);
        }
    }

    /**
     * Opens the world {@code level} is, once; a later call for the same key
     * returns the same pipeline. Synchronized: worlds load one at a time on
     * the server thread anyway, and this keeps two opens of one key from
     * racing.
     */
    static synchronized WorldPipeline open(Level level, String options) {
        final long seed = level.getSeed();
        final String key = keyOf(seed, options);
        final WorldPipeline existing = OPEN.get(key);
        if (existing != null) {
            return existing;
        }
        final MainLogger log = Server.getInstance().getLogger();
        final String world = level.getFolderName();
        if (!(level.getProvider() instanceof LevelDBProvider)) {
            // Anvil is Nukkit's legacy format, y 0..255 with 2D biomes: the
            // overworld's -64..319 cannot be stored in it.
            throw new StratumGenerationException("stratum: world '" + world + "' is stored as "
                + level.getProvider().getClass().getSimpleName() + ", and Stratum fills Nukkit's "
                + "LevelDB worlds only (y -64..319, a biome per block)");
        }
        final GeneratorOptions parsed = parse(world, options);
        final File blob = requireBlob(world, parsed);

        final long pipeline = openNative(world, blob, parsed, seed);
        try {
            final Resolved resolved = resolveStates(world, pipeline);
            final long bound = bindNative(world, pipeline, resolved);

            final int tableVersion = NativePipeline.bedrockBlockStateVersion();
            if (tableVersion != resolved.paletteVersion) {
                log.info("stratum: world '" + world + "': the engine's blockstates are at "
                    + NativePipeline.formatVersion(tableVersion) + " and Nukkit's palette at "
                    + NativePipeline.formatVersion(resolved.paletteVersion) + "; matched by name and "
                    + "states, all " + resolved.ids.length + " states it can place resolved");
            }
            final int[] biomeIds = NativePipeline.emittableBiomeIds(pipeline);
            final String[] biomeNames = NativePipeline.emittableBiomeNames(pipeline);
            final StringBuilder unknown = new StringBuilder();
            for (int i = 0; i < biomeIds.length; i++) {
                if (Biome.getBiomeNameFromId(biomeIds[i]) == null) {
                    unknown.append(unknown.length() == 0 ? "" : ", ").append(biomeNames[i])
                        .append(" (Bedrock ").append(biomeIds[i]).append(')');
                }
            }
            if (unknown.length() != 0) {
                log.warning("stratum: world '" + world + "': Nukkit's biome registry "
                    + "(biome_id_map.json) has no entry for " + unknown + ". The world stores "
                    + "these ids as generated; Nukkit itself sends them to clients as ocean "
                    + "(Biome.getBiomeIdOrCorrect in its network chunk serializer)");
            }

            final WorldPipeline opened = new WorldPipeline(key, bound, NativePipeline.minY(pipeline),
                NativePipeline.height(pipeline), resolved.airFullId, blob, resolved.names,
                resolved.fullIds, biomeNames, biomeIds);
            OPEN.put(key, opened);
            log.info("stratum: world '" + world + "' generates from " + blob + " (settings "
                + parsed.settings + ", biomes " + parsed.biomes + ", " + resolved.ids.length
                + " block states resolved through Nukkit's BlockStateMapping)");
            return opened;
        } finally {
            NativePipeline.release(pipeline);
        }
    }

    /** What resolving a world's block states through Nukkit gave, index for index. */
    private static final class Resolved {
        final int[] ids;
        final String[] names;
        final int[] fullIds;
        final int airFullId;
        final int paletteVersion;

        Resolved(int[] ids, String[] names, int[] fullIds, int airFullId, int paletteVersion) {
            this.ids = ids;
            this.names = names;
            this.fullIds = fullIds;
            this.airFullId = airFullId;
            this.paletteVersion = paletteVersion;
        }
    }

    private static GeneratorOptions parse(String world, String options) {
        try {
            return GeneratorOptions.parse(options);
        } catch (StratumGenerationException e) {
            throw new StratumGenerationException("stratum: world '" + world + "': " + e.getMessage(), e);
        }
    }

    private static File requireBlob(String world, GeneratorOptions parsed) {
        final File blob = parsed.blobFile();
        if (!blob.isFile()) {
            throw new StratumGenerationException("stratum: world '" + world + "' has no frozen "
                + "pipeline at " + blob + ". The Stratum plugin writes it from \"pack\" before the "
                + "world is created, and never afterwards: a world that exists without its "
                + "pipeline is refused rather than regenerated from a pack that may have changed");
        }
        return blob;
    }

    /** The engine's refusals (scope, unmapped biomes, an unreadable blob), naming the world. */
    private static long openNative(String world, File blob, GeneratorOptions parsed, long seed) {
        try {
            return NativePipeline.open(blob.getPath(), parsed.settings, parsed.biomes, seed);
        } catch (StratumGenerationException e) {
            throw new StratumGenerationException("stratum: world '" + world + "': " + e.getMessage(), e);
        }
    }

    private static long bindNative(String world, long pipeline, Resolved resolved) {
        try {
            return NativePipeline.bind(pipeline, resolved.ids, resolved.fullIds);
        } catch (StratumGenerationException e) {
            throw new StratumGenerationException("stratum: world '" + world + "': " + e.getMessage(), e);
        }
    }

    /** Every state the world can place, through Nukkit; any miss refuses the world, naming each. */
    private static Resolved resolveStates(String world, long pipeline) {
        final int[] ids = NativePipeline.emittableStateIds(pipeline);
        final String[] names = NativePipeline.emittableStateNames(pipeline);
        final NukkitBlockResolver resolver = new NukkitBlockResolver();
        final int[] fullIds = new int[ids.length];
        final StringBuilder misses = new StringBuilder();
        int missCount = 0;
        int airFullId = -1;
        for (int i = 0; i < ids.length; i++) {
            final BedrockState state = BedrockState.ofJavaState(ids[i]);
            final NukkitBlockResolver.Resolution resolution = resolver.resolve(state);
            if (resolution.miss != null) {
                misses.append(missCount++ == 0 ? "" : "; ").append(names[i]).append(" (Bedrock ")
                    .append(state).append("): ").append(resolution.miss);
                continue;
            }
            fullIds[i] = resolution.fullId;
            if (names[i].equals("minecraft:air")) {
                airFullId = resolution.fullId;
            }
        }
        if (missCount != 0) {
            throw new StratumGenerationException("stratum: world '" + world + "': " + missCount
                + " of the " + ids.length + " block states it can place have no Nukkit block id, "
                + "so it is refused rather than generated with substitutes: " + misses);
        }
        return new Resolved(ids, names, fullIds, airFullId, resolver.paletteVersion());
    }

    /** Forgets the world behind {@code level}, when Nukkit unloads it. */
    static void close(Level level) {
        final Object options = level.getProvider().getGeneratorOptions().get("preset");
        final WorldPipeline removed =
            OPEN.remove(keyOf(level.getSeed(), options == null ? "" : options.toString()));
        // The bound handle is not released: a generation task already
        // running on a worker may still be filling through it, and Nukkit
        // gives no point after which none can be.
        if (removed != null) {
            removed.fluidUpdates.clear();
        }
    }

    void recordGeneration(int chunkX, int chunkZ, int[] fluids) {
        chunksGenerated.incrementAndGet();
        generationThreads.add(Thread.currentThread().getName());
        if (fluids.length != 0) {
            fluidUpdates.put(Level.chunkHash(chunkX, chunkZ), fluids);
        }
    }

    /** The chunk's fluid updates, removed: each is scheduled once. */
    int[] takeFluidUpdates(int chunkX, int chunkZ) {
        return fluidUpdates.remove(Level.chunkHash(chunkX, chunkZ));
    }

    void recordFluidUpdatesScheduled(int count) {
        fluidUpdatesScheduled.addAndGet(count);
    }

    public int minY() {
        return minY;
    }

    public int height() {
        return height;
    }

    /** The block states this world can place, spelt as Java spells them. */
    public String[] stateNames() {
        return stateNames.clone();
    }

    /** The Nukkit full id each of {@link #stateNames} resolved to, index for index. */
    public int[] stateFullIds() {
        return stateFullIds.clone();
    }

    /** The biomes this world's biome source can return, by Java id. */
    public String[] biomeNames() {
        return biomeNames.clone();
    }

    /** Their Bedrock ids, index for index. */
    public int[] biomeIds() {
        return biomeIds.clone();
    }

    /** How many chunks this world's generator has filled since the world opened. */
    public long chunksGenerated() {
        return chunksGenerated.get();
    }

    /** How many liquid blocks the plugin has scheduled an update for, since the world opened. */
    public long fluidUpdatesScheduled() {
        return fluidUpdatesScheduled.get();
    }

    /** The names of the threads that filled them. */
    public Set<String> generationThreads() {
        return Collections.unmodifiableSet(generationThreads);
    }

    /**
     * Fills a chunk outside Nukkit's generation, on the calling thread:
     * exactly what the generator writes, for a caller comparing against
     * what Nukkit stored. Same layout as {@link NativePipeline#fill}.
     */
    public int[] referenceFill(int chunkX, int chunkZ, int[] fullBlockIds, int[] biomeIds) {
        return NativePipeline.fill(bound, chunkX, chunkZ, fullBlockIds, biomeIds);
    }
}
