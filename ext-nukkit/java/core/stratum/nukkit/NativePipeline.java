// Stratum — the native methods of the Nukkit binding.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
package stratum.nukkit;

import java.io.File;

/**
 * The JNI surface of {@code libstratum_nukkit} (ext-nukkit/src/jni_bridge.cpp).
 * Plain Java with no Nukkit type in it, so it loads and is tested in a bare
 * JVM (ext-nukkit/java/test). Every method raises
 * {@link StratumGenerationException} for what the engine refuses.
 *
 * <p>Handles are native objects owned by the caller until released: a
 * pipeline handle from {@link #open}, and a bound handle from {@link #bind},
 * which keeps its pipeline alive on its own, so the pipeline handle may be
 * released as soon as it is bound. Both are immutable and safe to use from
 * any number of threads at once (SPEC §4.1).
 */
public final class NativePipeline {
    /** System property naming the library file, an absolute path. */
    public static final String LIBRARY_PROPERTY = "stratum.nukkit.library";

    /** The library's base name: {@code libstratum_nukkit.so} on Linux. */
    public static final String LIBRARY_NAME = "stratum_nukkit";

    /** Bedrock NBT tag ids, as {@link #bedrockState} reports a value's type. */
    public static final int TAG_BYTE = 1;
    public static final int TAG_INT = 3;
    public static final int TAG_STRING = 8;

    private static String loadedFrom;

    private NativePipeline() {
    }

    /**
     * Loads the library once and says where from. Tried in order: the file
     * {@value #LIBRARY_PROPERTY} names; {@code directory}'s
     * {@code System.mapLibraryName("stratum_nukkit")} when {@code directory}
     * is not null (the plugin passes its data folder); then
     * {@code java.library.path}.
     */
    public static synchronized String load(File directory) {
        if (loadedFrom != null) {
            return loadedFrom;
        }
        final String named = System.getProperty(LIBRARY_PROPERTY);
        if (named != null && !named.isEmpty()) {
            final File file = new File(named).getAbsoluteFile();
            if (!file.isFile()) {
                throw new StratumGenerationException("stratum: -D" + LIBRARY_PROPERTY + " names "
                    + file + ", which is not a file");
            }
            System.load(file.getPath());
            loadedFrom = file.getPath();
            return loadedFrom;
        }
        if (directory != null) {
            final File file = new File(directory, System.mapLibraryName(LIBRARY_NAME)).getAbsoluteFile();
            if (file.isFile()) {
                System.load(file.getPath());
                loadedFrom = file.getPath();
                return loadedFrom;
            }
        }
        try {
            System.loadLibrary(LIBRARY_NAME);
        } catch (UnsatisfiedLinkError error) {
            throw new StratumGenerationException("stratum: cannot load the native library: set -D"
                + LIBRARY_PROPERTY + " to libstratum_nukkit's path"
                + (directory == null ? "" : ", put it in " + directory.getAbsolutePath())
                + ", or add its directory to java.library.path (" + error.getMessage() + ")", error);
        }
        loadedFrom = "java.library.path";
        return loadedFrom;
    }

    /** Freezes the version root's datapack into a new file (SPEC §6). Refuses to replace one. */
    public static native void freeze(String versionRoot, String blobPath);

    /** Opens a world's frozen pipeline for noise settings and a biome source. */
    public static native long open(String blobPath, String settings, String biomes, long seed);

    public static native void release(long pipeline);

    public static native int minY(long pipeline);

    public static native int height(long pipeline);

    /** Java state ids of every block state the world can place. */
    public static native int[] emittableStateIds(long pipeline);

    /** The same states, spelt as {@code minecraft:snow_layer[layers=1]}. */
    public static native String[] emittableStateNames(long pipeline);

    /** Bedrock ids of every biome the world's biome source can return. */
    public static native int[] emittableBiomeIds(long pipeline);

    /** The same biomes' Java ids. */
    public static native String[] emittableBiomeNames(long pipeline);

    public static native int javaBlockStateCount();

    /** The Bedrock blockstate version of the engine's mapping table. */
    public static native int bedrockBlockStateVersion();

    /**
     * The Bedrock blockstate for a Java state id: {@code {String name,
     * String[] keys, int[] types, int[] integers, String[] strings}}, the
     * states sorted by key; {@link BedrockState#of} reads it.
     */
    public static native Object[] bedrockState(int javaStateId);

    /**
     * Binds a pipeline to the Nukkit full ids the caller resolved:
     * {@code javaStateIds[i]} is {@code fullIds[i]}. Refuses, naming each,
     * if any state the world can place is missing.
     */
    public static native long bind(long pipeline, int[] javaStateIds, int[] fullIds);

    public static native void releaseBound(long bound);

    /**
     * Fills chunk ({@code chunkX}, {@code chunkZ}): {@code fullBlockIds}
     * at index {@code (ly * 16 + lz) * 16 + lx}, {@code ly} counted up from
     * {@link #minY}; {@code biomeIds} one per quart at {@code (qy * 4 + qz)
     * * 4 + qx}. Returns the chunk's fluid updates, each
     * {@code lx | lz << 4 | (y - minY) << 8}. On a refusal neither array is
     * written.
     */
    public static native int[] fill(long bound, int chunkX, int chunkZ, int[] fullBlockIds,
        int[] biomeIds);

    /** {@code 1.21.60.33} for a blockstate version int. */
    public static String formatVersion(int version) {
        return ((version >>> 24) & 0xff) + "." + ((version >>> 16) & 0xff) + "."
            + ((version >>> 8) & 0xff) + "." + (version & 0xff);
    }
}
