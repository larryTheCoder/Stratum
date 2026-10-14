// Stratum — Nukkit's entry point into the engine.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
package stratum.nukkit;

import cn.nukkit.level.ChunkManager;
import cn.nukkit.level.Level;
import cn.nukkit.level.format.generic.BaseFullChunk;
import cn.nukkit.level.generator.Generator;
import cn.nukkit.math.NukkitRandom;
import cn.nukkit.math.Vector3;

import java.util.Map;

/**
 * A Nukkit {@code Generator} filling chunks from a world's frozen Stratum
 * pipeline (CloudburstMC/Nukkit, {@code cn.nukkit.level.generator}).
 *
 * <p>How Nukkit drives a generator, read from its {@code Level}: one
 * instance per thread, from a {@code ThreadLocal}, constructed with the
 * world's options map. On the server thread {@code init} is called twice,
 * first with the {@code Level} itself and then with a fresh
 * {@code PopChunkManager}; on a generation worker once, with a
 * {@code PopChunkManager} that carries the seed and nothing else. So the
 * world is opened on the server thread's first call — the only one that
 * knows which world it is — and every later call finds it by seed and
 * options ({@link WorldPipeline}).
 *
 * <p>What Nukkit does with a refusal thrown from {@code init} is not a clean
 * failure ({@code b3caad67d}): {@code Level}'s per-thread generator logs the
 * exception, naming the refusal, and stores {@code null}, and
 * {@code Level.initLevel} then throws a {@code NullPointerException}.
 * {@code Server.generateLevel} catches it and logs a generation error — after
 * {@code LevelDBProvider.generate} has already written the new world's
 * {@code level.dat} — while {@code Server.loadLevel} does not, so an
 * existing refused world stops the server as it starts. That is why
 * {@link StratumPlugin} makes every check for the worlds Nukkit loads or
 * creates at startup before Nukkit touches them, and stops the server on a
 * refusal; this path is left to worlds a command or another plugin creates
 * or loads later.
 *
 * <p>{@code generateChunk} runs on a worker, inside Nukkit's lock on its
 * chunk manager, with the new chunk set into that manager. Terrain only:
 * {@link #populateChunk} is a deliberate no-op while features are out of
 * scope (SPEC's M6).
 */
public final class StratumGenerator extends Generator {
    /** The name worlds use: {@code level-type=stratum}, or {@code generator: stratum:...}. */
    public static final String NAME = "stratum";

    /**
     * The generator type this class is registered under. Not one of
     * Nukkit's own: Nukkit maps a {@code level.dat} that has a type and no
     * generator name to the class registered under that type, so taking
     * {@code TYPE_INFINITE} would hand every such old world to Stratum.
     */
    public static final int TYPE = 0x5354;

    private final Map<String, Object> options;
    private ChunkManager chunkManager;
    private WorldPipeline world;

    public StratumGenerator(Map<String, Object> options) {
        this.options = options;
    }

    @Override
    public int getId() {
        return TYPE;
    }

    @Override
    public void init(ChunkManager level, NukkitRandom random) {
        this.chunkManager = level;
        StratumPlugin.requireNative();
        final Object preset = options.get("preset");
        final String text = preset == null ? "" : preset.toString();
        if (level instanceof Level) {
            this.world = WorldPipeline.open((Level) level, text);
            return;
        }
        this.world = WorldPipeline.find(WorldPipeline.keyOf(level.getSeed(), text));
        if (this.world == null) {
            throw new StratumGenerationException("stratum: a generation thread found no open world "
                + "for seed " + level.getSeed() + " and options " + text + ": Stratum worlds are "
                + "opened on the server thread as they load, and this one was not");
        }
    }

    @Override
    public void generateChunk(int chunkX, int chunkZ) {
        final BaseFullChunk chunk = chunkManager.getChunk(chunkX, chunkZ);
        if (chunk == null) {
            throw new StratumGenerationException("stratum: Nukkit asked for chunk " + chunkX + ", "
                + chunkZ + " without handing the generator that chunk");
        }
        final int height = world.height;
        final int minY = world.minY;
        final int[] fullBlockIds = new int[16 * 16 * height];
        final int[] biomeIds = new int[16 * (height / 4)];
        final int[] fluids = NativePipeline.fill(world.bound, chunkX, chunkZ, fullBlockIds, biomeIds);

        // A chunk Nukkit hands a generator is new, so it is air already;
        // writing air would only allocate storage for empty sections.
        for (int ly = 0; ly < height; ly++) {
            final int y = minY + ly;
            for (int lz = 0; lz < 16; lz++) {
                for (int lx = 0; lx < 16; lx++) {
                    final int fullId = fullBlockIds[(ly * 16 + lz) * 16 + lx];
                    if (fullId != world.airFullId) {
                        chunk.setFullBlockId(lx, y, lz, fullId);
                    }
                }
            }
        }
        // Nukkit keeps a biome per block (LevelDBChunk.setBiomeId: section
        // y >> 4, block-local x, y & 15, z); the engine gives one per 4x4x4
        // quart, which is what each of its 64 blocks gets.
        for (int ly = 0; ly < height; ly++) {
            final int y = minY + ly;
            final int quartRow = (ly >> 2) * 16;
            for (int lz = 0; lz < 16; lz++) {
                for (int lx = 0; lx < 16; lx++) {
                    chunk.setBiomeId(lx, y, lz, biomeIds[quartRow + (lz >> 2) * 4 + (lx >> 2)]);
                }
            }
        }
        world.recordGeneration(chunkX, chunkZ, fluids);
    }

    @Override
    public void populateChunk(int chunkX, int chunkZ) {
        // Deliberately nothing: no features until M6.
    }

    @Override
    public Map<String, Object> getSettings() {
        return options;
    }

    @Override
    public String getName() {
        return NAME;
    }

    @Override
    public Vector3 getSpawn() {
        // A fixed point, not vanilla's spawn search (unimplemented).
        return new Vector3(0.5, 256, 0.5);
    }

    @Override
    public ChunkManager getChunkManager() {
        return chunkManager;
    }
}
