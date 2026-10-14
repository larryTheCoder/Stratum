// Stratum — the driver of the Nukkit binding's headless smoke run.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
package stratum.nukkit.smoke;

import cn.nukkit.level.ChunkLoader;
import cn.nukkit.level.Level;
import cn.nukkit.level.Position;
import cn.nukkit.level.biome.Biome;
import cn.nukkit.level.format.FullChunk;
import cn.nukkit.math.Vector3;
import cn.nukkit.plugin.PluginBase;
import cn.nukkit.scheduler.ServerScheduler;
import cn.nukkit.scheduler.TaskHandler;
import stratum.nukkit.BedrockState;
import stratum.nukkit.NativePipeline;
import stratum.nukkit.NukkitBlockResolver;
import stratum.nukkit.WorldPipeline;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.Properties;
import java.util.TreeSet;

/**
 * Drives one run of {@code tools/nukkit-dev smoke} inside a real Nukkit
 * server, on the server thread, a step per tick, and writes what it saw to
 * the properties file {@code -Dstratum.smoke.report} names. Inert without
 * {@code -Dstratum.smoke.mode}.
 *
 * <ul>
 * <li>{@code generate}: asks Nukkit to generate every chunk within
 * {@code -Dstratum.smoke.radius} chunks of the default world's spawn — on its
 * own async workers, through the registered generator — and times it; then
 * compares every one of those chunks, as Nukkit holds it in memory, with a
 * fresh single-threaded fill of the same chunk; then saves and stops.
 * <li>{@code readback}: in a second server run over the same directory,
 * loads the same chunks from disk without generating any, and compares them
 * the same way. The pass/fail reading of the report is
 * {@code tools/nukkit-dev}'s.
 * </ul>
 *
 * <p>Biomes are compared per block, the way Nukkit stores them
 * ({@code getBiomeId(x, y, z)}), against each block's quart. A mismatch is
 * also tested against one specific reading: Nukkit's LevelDB serializer
 * writing section {@code i + 4}'s biomes into slot {@code i} (its
 * {@code Data3dSerializer} calls {@code getBiomeStorage(i)}, which takes a
 * section y, with the array index), which after a reload puts every
 * section's biomes 64 blocks low and the bottom section's into the top four.
 * The report counts that reading separately, so it is confirmed or refuted
 * rather than assumed.
 */
public final class SmokePlugin extends PluginBase {
    private static final long TIMEOUT_NANOS = 30L * 60L * 1_000_000_000L;

    private final Properties report = new Properties();
    private String mode;
    private File reportFile;
    private int radius;
    private TaskHandler task;

    private Level level;
    private WorldPipeline world;
    private final List<int[]> area = new ArrayList<>();
    private int next;
    private int phase;
    private long startNanos;
    private long generatedAtStart;

    private long blockMismatches;
    private long biomeMismatches;
    private long biomeMismatchesOffset;
    private long chunksCompared;
    private long chunksMissing;
    private final List<String> examples = new ArrayList<>();

    @Override
    public void onEnable() {
        mode = System.getProperty("stratum.smoke.mode");
        if (mode == null) {
            getLogger().info("inert: no -Dstratum.smoke.mode");
            return;
        }
        reportFile = new File(System.getProperty("stratum.smoke.report", "smoke-report.properties"))
            .getAbsoluteFile();
        radius = Integer.getInteger("stratum.smoke.radius", 4);
        task = getServer().getScheduler().scheduleRepeatingTask(this, this::step, 1);
    }

    private void step() {
        try {
            stepOnce();
        } catch (Throwable e) {
            getLogger().error("smoke: " + e, e);
            finish("error: " + e);
        }
    }

    private void stepOnce() {
        switch (phase) {
            case 0:
                begin();
                break;
            case 1:
                if (allGenerated()) {
                    final long wallNanos = System.nanoTime() - startNanos;
                    report.setProperty("spawn_generation_ms", String.valueOf(wallNanos / 1_000_000L));
                    report.setProperty("chunks_generated_by_stratum",
                        String.valueOf(world.chunksGenerated() - generatedAtStart));
                    report.setProperty("generation_threads", String.valueOf(world.generationThreads().size()));
                    report.setProperty("generation_thread_names",
                        String.valueOf(new TreeSet<>(world.generationThreads())));
                    phase = 2;
                    next = 0;
                } else if (System.nanoTime() - startNanos > TIMEOUT_NANOS) {
                    finish("timed out waiting for generation");
                }
                break;
            case 2:
                compareNext("memory");
                if (next == area.size()) {
                    record("memory");
                    report.setProperty("saved", String.valueOf(level.save(true)));
                    finish("ok");
                }
                break;
            case 10:
                compareNext("disk");
                if (next == area.size()) {
                    record("disk");
                    report.setProperty("chunks_generated_during_readback",
                        String.valueOf(world.chunksGenerated() - generatedAtStart));
                    report.setProperty("chunks_missing_on_disk", String.valueOf(chunksMissing));
                    finish("ok");
                }
                break;
            default:
                finish("unknown phase " + phase);
                break;
        }
    }

    private void begin() {
        level = getServer().getDefaultLevel();
        world = level == null ? null : WorldPipeline.of(level);
        if (world == null) {
            finish("the default world is not an open Stratum world");
            return;
        }
        report.setProperty("mode", mode);
        report.setProperty("level", level.getFolderName());
        report.setProperty("level_dat_generator", level.getProvider().getGenerator());
        report.setProperty("seed", String.valueOf(level.getSeed()));
        report.setProperty("async_workers", String.valueOf(ServerScheduler.WORKERS));
        report.setProperty("min_y", String.valueOf(world.minY()));
        report.setProperty("height", String.valueOf(world.height()));
        census();

        final int centerX = level.getSpawnLocation().getChunkX();
        final int centerZ = level.getSpawnLocation().getChunkZ();
        for (int x = centerX - radius; x <= centerX + radius; x++) {
            for (int z = centerZ - radius; z <= centerZ + radius; z++) {
                area.add(new int[] {x, z});
            }
        }
        report.setProperty("area", (2 * radius + 1) + "x" + (2 * radius + 1) + " chunks around chunk "
            + centerX + "," + centerZ);
        report.setProperty("area_chunks", String.valueOf(area.size()));
        generatedAtStart = world.chunksGenerated();
        startNanos = System.nanoTime();
        if (mode.equals("generate")) {
            // Held loaded until the server stops, so the in-memory
            // comparison reads what generation left in memory and not a copy
            // Nukkit unloaded (30 s unused) and read back from disk.
            final AreaLoader loader = new AreaLoader();
            for (int[] chunk : area) {
                level.registerChunkLoader(loader, chunk[0], chunk[1], false);
            }
            for (int[] chunk : area) {
                level.generateChunk(chunk[0], chunk[1], true);
            }
            phase = 1;
        } else if (mode.equals("readback")) {
            phase = 10;
        } else {
            finish("unknown mode " + mode);
        }
    }

    /** The census: every state the world can place, and every Java state, through Nukkit. */
    private void census() {
        final String[] names = world.stateNames();
        final int[] fullIds = world.stateFullIds();
        final StringBuilder resolved = new StringBuilder();
        for (int i = 0; i < names.length; i++) {
            resolved.append(i == 0 ? "" : ", ").append(names[i]).append('=').append(fullIds[i] >> 13)
                .append(':').append(fullIds[i] & 0x1fff);
        }
        report.setProperty("census_emittable_states", String.valueOf(names.length));
        report.setProperty("census_emittable_resolved", resolved.toString());

        final NukkitBlockResolver resolver = new NukkitBlockResolver();
        final int total = NativePipeline.javaBlockStateCount();
        int hits = 0;
        for (int id = 0; id < total; id++) {
            if (resolver.resolve(BedrockState.ofJavaState(id)).miss == null) {
                hits++;
            }
        }
        report.setProperty("census_all_java_states", hits + " of " + total + " resolve");
        report.setProperty("nukkit_palette_version", NativePipeline.formatVersion(resolver.paletteVersion()));
        report.setProperty("engine_table_version",
            NativePipeline.formatVersion(NativePipeline.bedrockBlockStateVersion()));

        final String[] biomes = world.biomeNames();
        final int[] biomeIds = world.biomeIds();
        final StringBuilder unknown = new StringBuilder();
        int known = 0;
        for (int i = 0; i < biomes.length; i++) {
            if (Biome.getBiomeNameFromId(biomeIds[i]) != null) {
                known++;
            } else {
                unknown.append(unknown.length() == 0 ? "" : ", ").append(biomes[i]).append('=')
                    .append(biomeIds[i]);
            }
        }
        report.setProperty("census_biomes", known + " of " + biomes.length + " known to Nukkit");
        report.setProperty("census_biomes_unknown_to_nukkit", unknown.toString());
    }

    private boolean allGenerated() {
        for (int[] chunk : area) {
            if (!level.isChunkGenerated(chunk[0], chunk[1])) {
                return false;
            }
        }
        return true;
    }

    /** One chunk per tick: a Debug fill takes most of a second. */
    private void compareNext(String where) {
        final int[] position = area.get(next++);
        final FullChunk chunk = level.getChunk(position[0], position[1], false);
        if (chunk == null || !chunk.isGenerated()) {
            chunksMissing++;
            example(where + ": chunk " + position[0] + "," + position[1] + " is "
                + (chunk == null ? "not stored" : "not generated"));
            return;
        }
        final int height = world.height();
        final int minY = world.minY();
        final int[] blocks = new int[16 * 16 * height];
        final int[] biomes = new int[16 * (height / 4)];
        world.referenceFill(position[0], position[1], blocks, biomes);
        for (int ly = 0; ly < height; ly++) {
            for (int lz = 0; lz < 16; lz++) {
                for (int lx = 0; lx < 16; lx++) {
                    final int want = blocks[(ly * 16 + lz) * 16 + lx];
                    final int got = chunk.getFullBlock(lx, minY + ly, lz);
                    if (got != want) {
                        blockMismatches++;
                        example(where + ": block " + lx + "," + (minY + ly) + "," + lz + " of chunk "
                            + position[0] + "," + position[1] + " is " + got + ", filled " + want);
                    }
                    final int wantBiome = quart(biomes, lx, ly, lz);
                    final int gotBiome = chunk.getBiomeId(lx, minY + ly, lz);
                    if (gotBiome != wantBiome) {
                        biomeMismatches++;
                        // Section s holding section s+4's biomes, the top
                        // four the bottom one's.
                        final int section = ly >> 4;
                        final int shiftedLy = section + 4 < height / 16 ? ly + 64 : ly & 15;
                        if (gotBiome == quart(biomes, lx, shiftedLy, lz)) {
                            biomeMismatchesOffset++;
                        }
                        example(where + ": biome at " + lx + "," + (minY + ly) + "," + lz + " of chunk "
                            + position[0] + "," + position[1] + " is " + gotBiome + ", filled " + wantBiome);
                    }
                }
            }
        }
        chunksCompared++;
    }

    private static int quart(int[] biomes, int lx, int ly, int lz) {
        return biomes[((ly >> 2) * 4 + (lz >> 2)) * 4 + (lx >> 2)];
    }

    private void example(String text) {
        if (examples.size() < 10) {
            examples.add(text);
        }
    }

    private void record(String where) {
        report.setProperty("chunks_compared_" + where, String.valueOf(chunksCompared));
        report.setProperty("block_mismatches_" + where, String.valueOf(blockMismatches));
        report.setProperty("biome_mismatches_" + where, String.valueOf(biomeMismatches));
        report.setProperty("biome_mismatches_" + where + "_explained_by_section_offset",
            String.valueOf(biomeMismatchesOffset));
        for (int i = 0; i < examples.size(); i++) {
            report.setProperty("example_" + where + "_" + i, examples.get(i));
        }
    }

    /** Keeps the smoke area in use, the way a player's view would. */
    private final class AreaLoader implements ChunkLoader {
        private int id;

        AreaLoader() {
            this.id = Level.generateChunkLoaderId(this);
        }

        @Override
        public int getLoaderId() {
            return id;
        }

        @Override
        public boolean isLoaderActive() {
            return true;
        }

        @Override
        public Position getPosition() {
            return level.getSpawnLocation();
        }

        @Override
        public double getX() {
            return level.getSpawnLocation().getX();
        }

        @Override
        public double getZ() {
            return level.getSpawnLocation().getZ();
        }

        @Override
        public Level getLevel() {
            return level;
        }

        @Override
        public void onChunkChanged(FullChunk chunk) {
        }

        @Override
        public void onChunkLoaded(FullChunk chunk) {
        }

        @Override
        public void onChunkUnloaded(FullChunk chunk) {
        }

        @Override
        public void onChunkPopulated(FullChunk chunk) {
        }

        @Override
        public void onBlockChanged(Vector3 block) {
        }
    }

    private void finish(String outcome) {
        if (task != null) {
            task.cancel();
            task = null;
        }
        report.setProperty("outcome", outcome);
        try (OutputStream out = new FileOutputStream(reportFile)) {
            report.store(out, "Stratum Nukkit smoke, " + mode);
        } catch (IOException e) {
            getLogger().error("smoke: cannot write " + reportFile + ": " + e);
        }
        getLogger().info("smoke " + mode + ": " + outcome + ", report in " + reportFile);
        getServer().shutdown();
    }
}
