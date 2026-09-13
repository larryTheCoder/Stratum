package cn.nukkit.generator.stratum;

import cn.nukkit.level.ChunkManager;
import cn.nukkit.level.format.BaseFullChunk;
import cn.nukkit.level.generator.Generator;
import cn.nukkit.level.util.PalettedBlockStorage;
import cn.nukkit.math.NukkitRandom;
import cn.nukkit.math.Vector3;

import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

/**
 * Nukkit's own entry point into the Stratum engine.
 *
 * <p>Confirmed against Nukkit's own source directly, not assumed: the
 * abstract method list below is exactly {@code Generator}'s own
 * (github.com/CloudburstMC/Nukkit,
 * {@code src/main/java/cn/nukkit/level/generator/Generator.java}), and
 * block writes go through {@link ChunkManager#setBlockFullIdAt}, whose
 * signature is confirmed the same way. What is NOT yet confirmed against a
 * real build (no Nukkit dependency exists in this repo yet — see this
 * directory's own README): {@link PalettedBlockStorage#setBlock}'s exact
 * per-section coordinate convention for {@code getBiomeStorage(y)} — whether
 * {@code x}/{@code z} there are block-local (0-15) or quart-local (0-3).
 * This class assumes quart-local, matching the quart grid
 * {@code stratum_nukkit::Pipeline::fill} itself already produces, but that
 * assumption needs verifying before this compiles against real Nukkit
 * classes.
 *
 * <p>Marshaling only (the same rule {@code ext/}'s PocketMine-MP binding's
 * own README states): every block placed and every biome written comes
 * straight out of {@code nativeFill}'s two arrays. No decoration — trees,
 * ores, structures — happens here; {@link #populateChunk} is a deliberate
 * no-op, because Stratum does not implement features yet (SPEC's M6,
 * out of scope for v1). A caller expecting populated forests will not get
 * them from this generator today.
 */
public final class StratumGenerator extends Generator {
    static {
        // Standard JNI loading: a native library built as
        // libstratum_nukkit.{so,dylib} / stratum_nukkit.dll on
        // java.library.path. ext-nukkit/README.md names the build step;
        // this repo does not yet package or bundle it per platform.
        System.loadLibrary("stratum_nukkit");
    }

    /**
     * One compiled pipeline handle per world, not per generation thread.
     * Nukkit gives every async generation worker its own {@code Generator}
     * instance via a {@code ThreadLocal} (confirmed:
     * {@code Level.java}'s {@code ThreadLocal<Generator> generators}), so
     * without this map {@link #init} would compile a fresh, redundant
     * pipeline per worker thread instead of sharing the one immutable
     * pipeline every thread can safely read at once
     * (stratum_nukkit/pipeline.hpp's own doc explains why sharing is safe).
     * Keyed by the {@code ChunkManager} identity Nukkit hands {@link #init};
     * {@code computeIfAbsent} makes the compile-if-absent check atomic, so
     * two worker threads racing to initialize the same world compile it
     * exactly once between them.
     */
    private static final Map<ChunkManager, Long> PIPELINE_HANDLES = new ConcurrentHashMap<>();

    private final Map<String, Object> options;
    private ChunkManager chunkManager;
    private long pipelineHandle;
    private int height;

    public StratumGenerator(Map<String, Object> options) {
        this.options = options;
    }

    @Override
    public int getId() {
        // TYPE_INFINITE, by elimination against Generator's own constants
        // (TYPE_OLD, TYPE_FLAT, TYPE_NETHER, TYPE_THE_END, TYPE_VOID all
        // name something Stratum's overworld pipeline is not) — not
        // confirmed against a built-in generator's own getId() body, since
        // the one checked (Normal.java) was not found at the path expected.
        return Generator.TYPE_INFINITE;
    }

    @Override
    public void init(ChunkManager level, NukkitRandom random) {
        this.chunkManager = level;
        final String packDir = (String) options.get("packDir");
        if (packDir == null) {
            throw new StratumGenerationException(
                "stratum: generator options need a \"packDir\" — the datapack this pipeline "
                    + "compiles from");
        }
        final String dimension = (String) options.getOrDefault("dimension", "minecraft:overworld");
        final long seed = ((Number) options.getOrDefault("seed", 0L)).longValue();

        this.pipelineHandle =
            PIPELINE_HANDLES.computeIfAbsent(level, ignored -> nativeCompile(packDir, dimension, seed));
        this.height = nativeHeight(pipelineHandle);
    }

    @Override
    public void generateChunk(int chunkX, int chunkZ) {
        final int[] fullBlockIds = new int[16 * 16 * height];
        final int[] biomeIds = new int[4 * 4 * (height / 4)];
        nativeFill(pipelineHandle, chunkX, chunkZ, fullBlockIds, biomeIds);

        final BaseFullChunk chunk = chunkManager.getChunk(chunkX, chunkZ);
        final int minY = nativeMinY(pipelineHandle);
        for (int ly = 0; ly < height; ly++) {
            final int y = minY + ly;
            for (int lz = 0; lz < 16; lz++) {
                for (int lx = 0; lx < 16; lx++) {
                    final int index = (ly * 16 + lz) * 16 + lx;
                    chunkManager.setBlockFullIdAt(chunkX * 16 + lx, y, chunkZ * 16 + lz,
                        fullBlockIds[index]);
                }
            }
        }

        final int quartsPerColumn = height / 4;
        for (int qy = 0; qy < quartsPerColumn; qy++) {
            // getBiomeStorage's own section-index convention (block Y vs
            // section index vs quart index) is the one piece this class's
            // own header flags as unconfirmed.
            final PalettedBlockStorage biomeStorage = chunk.getBiomeStorage(minY + qy * 4);
            for (int qz = 0; qz < 4; qz++) {
                for (int qx = 0; qx < 4; qx++) {
                    final int index = (qy * 4 + qz) * 4 + qx;
                    biomeStorage.setBlock(qx, 0, qz, biomeIds[index]);
                }
            }
        }
    }

    @Override
    public void populateChunk(int chunkX, int chunkZ) {
        // Deliberate no-op — see this class's own header.
    }

    @Override
    public Map<String, Object> getSettings() {
        return options;
    }

    @Override
    public String getName() {
        return "stratum";
    }

    @Override
    public Vector3 getSpawn() {
        // Placeholder, not vanilla's own spawn search (unimplemented,
        // matching every other decoration/post-processing gap M6 leaves
        // open) — a fixed point rather than a guess dressed up as one.
        return new Vector3(0, 64, 0);
    }

    @Override
    public ChunkManager getChunkManager() {
        return chunkManager;
    }

    private static native long nativeCompile(String packDir, String dimension, long seed);

    private static native int nativeMinY(long handle);

    private static native int nativeHeight(long handle);

    private static native void nativeFill(long handle, int chunkX, int chunkZ, int[] fullBlockIds,
        int[] biomeIds);

    private static native void nativeDestroy(long handle);
}
