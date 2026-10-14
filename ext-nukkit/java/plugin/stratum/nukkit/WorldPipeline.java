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
 * §4.1). Opening is where every refusal happens: unreadable options, no
 * frozen pipeline, settings or a biome source outside the binding's scope,
 * a biome with no Bedrock id, a block state Nukkit cannot place. Each is a
 * {@link StratumGenerationException} naming what is wrong, raised before
 * the first chunk, on the thread loading the world.
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
        final GeneratorOptions parsed;
        try {
            parsed = GeneratorOptions.parse(options);
        } catch (StratumGenerationException e) {
            throw new StratumGenerationException("stratum: world '" + world + "': " + e.getMessage(), e);
        }
        final File blob = parsed.blobFile();
        if (!blob.isFile()) {
            throw new StratumGenerationException("stratum: world '" + world + "' has no frozen "
                + "pipeline at " + blob + ". The Stratum plugin writes it from \"pack\" before the "
                + "world is created, and never afterwards: a world that exists without its "
                + "pipeline is refused rather than regenerated from a pack that may have changed");
        }

        final long pipeline = NativePipeline.open(blob.getPath(), parsed.settings, parsed.biomes, seed);
        try {
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
            final long bound = NativePipeline.bind(pipeline, ids, fullIds);

            final int tableVersion = NativePipeline.bedrockBlockStateVersion();
            if (tableVersion != resolver.paletteVersion()) {
                log.info("stratum: world '" + world + "': the engine's blockstates are at "
                    + NativePipeline.formatVersion(tableVersion) + " and Nukkit's palette at "
                    + NativePipeline.formatVersion(resolver.paletteVersion()) + "; matched by name and "
                    + "states, all " + ids.length + " states it can place resolved");
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
                NativePipeline.height(pipeline), airFullId, blob, names, fullIds, biomeNames, biomeIds);
            OPEN.put(key, opened);
            log.info("stratum: world '" + world + "' generates from " + blob + " (settings "
                + parsed.settings + ", biomes " + parsed.biomes + ", " + ids.length
                + " block states resolved through Nukkit's BlockStateMapping)");
            return opened;
        } finally {
            NativePipeline.release(pipeline);
        }
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
