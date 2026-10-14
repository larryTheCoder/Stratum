// Stratum — the Nukkit binding's JNI surface, exercised in a bare JVM.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
package stratum.nukkit;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.Callable;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;

/**
 * Drives {@code libstratum_nukkit} through real JNI from a JVM with no
 * Nukkit in it: what the Nukkit plugin does at world load and per chunk,
 * with a synthetic id table in place of Nukkit's {@code BlockStateMapping}.
 * Covers the marshaling, the refusals, and that fills on several JVM
 * threads at once match a single-threaded fill exactly.
 *
 * <p>Usage: {@code NativePipelineCheck <version root> <scratch dir>}. Exits
 * 0 when every check passes, 1 on a failure, and 77 (CTest's skip code)
 * when the version root is absent.
 */
public final class NativePipelineCheck {
    private static final int SKIP = 77;
    private static int failures;
    private static int passes;

    private NativePipelineCheck() {
    }

    public static void main(String[] args) throws Exception {
        if (args.length != 2) {
            System.err.println("usage: NativePipelineCheck <version root> <scratch dir>");
            System.exit(2);
        }
        final File versionRoot = new File(args[0]);
        if (!new File(versionRoot, "worldgen").isDirectory()) {
            System.out.println("SKIP: no worldgen fixtures under " + versionRoot
                + "; run tools/fetch-vanilla first");
            System.exit(SKIP);
        }
        final File scratch = new File(args[1]);
        deleteTree(scratch);
        if (!scratch.mkdirs()) {
            throw new IllegalStateException("cannot create " + scratch);
        }
        System.out.println("native library: " + NativePipeline.load(null));

        checkOptions();
        final File blob = new File(scratch, "stratum-pipeline.blob");
        checkFreeze(versionRoot, blob);
        checkRefusals(blob);
        checkWorld(blob);

        System.out.println(passes + " checks passed, " + failures + " failed");
        System.exit(failures == 0 ? 0 : 1);
    }

    private static void checkOptions() {
        final GeneratorOptions plain = GeneratorOptions.parse("{\"blob\":\"w/b.blob\"}");
        check("options default to the overworld", plain.settings.equals("minecraft:overworld")
            && plain.biomes.equals("minecraft:overworld") && plain.pack == null);
        final GeneratorOptions full = GeneratorOptions.parse(
            " { \"blob\" : \"a\\\"b\", \"pack\":\"/p\", \"settings\":\"minecraft:amplified\","
                + "\"biomes\":\"minecraft:overworld\" } ");
        check("options read every key and escape", full.blob.equals("a\"b") && full.pack.equals("/p")
            && full.settings.equals("minecraft:amplified"));
        check("options round-trip through encode", GeneratorOptions.parse(GeneratorOptions.encode(
            "x\\y\"z", "/p", "minecraft:end", "minecraft:the_end")).blob.equals("x\\y\"z"));
        refuses("options with no blob", () -> GeneratorOptions.parse("{\"pack\":\"/p\"}"), "\"blob\"");
        refuses("empty options", () -> GeneratorOptions.parse(""), "generator options");
        refuses("an unknown key", () -> GeneratorOptions.parse("{\"blob\":\"b\",\"biome\":\"x\"}"),
            "\"biome\" would be ignored");
        refuses("settings without biomes",
            () -> GeneratorOptions.parse("{\"blob\":\"b\",\"settings\":\"minecraft:end\"}"),
            "without \"biomes\"");
        refuses("a null value", () -> GeneratorOptions.parse("{\"blob\":null}"), "not a string");
        refuses("a repeated key", () -> GeneratorOptions.parse("{\"blob\":\"a\",\"blob\":\"b\"}"),
            "given twice");
        refuses("trailing text", () -> GeneratorOptions.parse("{\"blob\":\"a\"} x"), "after the closing");
    }

    private static void checkFreeze(File versionRoot, File blob) {
        NativePipeline.freeze(versionRoot.getPath(), blob.getPath());
        check("freeze writes the blob", blob.isFile() && blob.length() > 0);
        check("freeze leaves no partial file", !new File(blob.getPath() + ".partial").exists());
        refuses("freezing over an existing blob",
            () -> NativePipeline.freeze(versionRoot.getPath(), blob.getPath()), "refusing to replace");
        refuses("freezing a directory that is no version root",
            () -> NativePipeline.freeze(blob.getParent(), new File(blob.getParent(), "x.blob").getPath()),
            "not a version root");
    }

    private static void checkRefusals(File blob) {
        final String path = blob.getPath();
        refuses("the Nether's settings", () -> NativePipeline.open(path, "minecraft:nether",
            "minecraft:nether", 1L), "'minecraft:nether' span y 0..127");
        refuses("the End's biome source", () -> NativePipeline.open(path, "minecraft:end",
            "minecraft:the_end", 1L), "minecraft:the_end");
        refuses("settings the pipeline does not carry", () -> NativePipeline.open(path,
            "minecraft:nowhere", "minecraft:overworld", 1L), "minecraft:nowhere");
        refuses("a missing blob", () -> NativePipeline.open(path + ".missing", "minecraft:overworld",
            "minecraft:overworld", 1L), "cannot read the frozen pipeline");
        refuses("a released handle", () -> NativePipeline.height(0L), "released or never-opened");
    }

    private static void checkWorld(File blob) throws Exception {
        final long pipeline = NativePipeline.open(blob.getPath(), "minecraft:overworld",
            "minecraft:overworld", 1234L);
        final int minY = NativePipeline.minY(pipeline);
        final int height = NativePipeline.height(pipeline);
        check("the overworld spans Nukkit's y -64..319", minY == -64 && height == 384);

        final int[] ids = NativePipeline.emittableStateIds(pipeline);
        final List<String> names = Arrays.asList(NativePipeline.emittableStateNames(pipeline));
        check("state ids and names pair up", ids.length == names.size() && ids.length > 20);
        check("the emittable states include the terrain's own",
            names.containsAll(Arrays.asList("minecraft:air", "minecraft:stone", "minecraft:water[level=0]",
                "minecraft:lava[level=0]", "minecraft:deepslate[axis=y]", "minecraft:powder_snow",
                "minecraft:raw_iron_block", "minecraft:white_terracotta")));
        System.out.println("  " + ids.length + " emittable states: " + names);
        final int[] biomeIds = NativePipeline.emittableBiomeIds(pipeline);
        final String[] biomeNames = NativePipeline.emittableBiomeNames(pipeline);
        check("the overworld's 54 biomes, each with a Bedrock id",
            biomeIds.length == 54 && biomeNames.length == 54);
        check("the table's blockstate version is 1.21.60.33",
            NativePipeline.formatVersion(NativePipeline.bedrockBlockStateVersion()).equals("1.21.60.33"));

        for (int id : ids) {
            final BedrockState state = BedrockState.ofJavaState(id);
            if (!state.name.startsWith("minecraft:")) {
                check("Bedrock state of Java state " + id + " is namespaced", false);
            }
        }
        final BedrockState snow = BedrockState.ofJavaState(ids[names.indexOf("minecraft:powder_snow")]);
        check("powder snow's Bedrock state", snow.name.equals("minecraft:powder_snow") && snow.keys.length == 0);

        final int[] partial = Arrays.copyOf(ids, ids.length - 1);
        refuses("an id table missing a state", () -> NativePipeline.bind(pipeline, partial,
            new int[partial.length]), names.get(names.size() - 1));
        refuses("a negative full id", () -> NativePipeline.bind(pipeline, new int[] {ids[0]},
            new int[] {-1}), "negative");

        final int[] fullIds = new int[ids.length];
        final Set<Integer> table = new HashSet<>();
        for (int i = 0; i < ids.length; i++) {
            fullIds[i] = 1000 + i;
            table.add(fullIds[i]);
        }
        final long bound = NativePipeline.bind(pipeline, ids, fullIds);
        NativePipeline.release(pipeline);
        final int airFullId = 1000 + names.indexOf("minecraft:air");

        final int[] blocks = new int[16 * 16 * height];
        final int[] biomes = new int[16 * (height / 4)];
        final int[] fluids = NativePipeline.fill(bound, 0, 0, blocks, biomes);
        boolean inTable = true;
        for (int block : blocks) {
            inTable &= table.contains(block);
        }
        check("every filled block is a table id", inTable);
        check("the top of the column is air", blocks[blocks.length - 1] == airFullId);
        check("the bottom of the column is not air", blocks[0] != airFullId);
        final Set<Integer> known = new HashSet<>();
        for (int id : biomeIds) {
            known.add(id);
        }
        boolean biomesKnown = true;
        for (int biome : biomes) {
            biomesKnown &= known.contains(biome);
        }
        check("every quart's biome is one the source can return", biomesKnown);
        boolean fluidsInRange = true;
        for (int packed : fluids) {
            fluidsInRange &= packed >= 0 && (packed >>> 8) < height;
        }
        check("fluid updates are packed in range", fluidsInRange);

        final int[] wrong = new int[7];
        Arrays.fill(wrong, 42);
        refuses("a wrongly sized block array", () -> NativePipeline.fill(bound, 0, 0, wrong, biomes),
            "disagree on geometry");
        check("a refused fill leaves the arrays alone", wrong[0] == 42 && wrong[6] == 42);

        checkConcurrency(bound, height);
        NativePipeline.releaseBound(bound);
    }

    /** Several JVM threads at once, in a different order, against one thread. */
    private static void checkConcurrency(long bound, int height) throws Exception {
        final List<int[]> chunks = new ArrayList<>();
        for (int x = -3; x <= 2; x++) {
            for (int z = -1; z <= 0; z++) {
                chunks.add(new int[] {x * 7, z * 11});
            }
        }
        final List<int[][]> serial = new ArrayList<>();
        for (int[] chunk : chunks) {
            serial.add(fillOnce(bound, chunk, height));
        }
        final List<Integer> order = new ArrayList<>();
        for (int i = 0; i < chunks.size(); i++) {
            order.add(i);
        }
        Collections.reverse(order);
        final ExecutorService pool = Executors.newFixedThreadPool(4);
        try {
            final List<Future<int[][]>> parallel = new ArrayList<>();
            for (int i = 0; i < chunks.size(); i++) {
                parallel.add(null);
            }
            for (int index : order) {
                final int[] chunk = chunks.get(index);
                parallel.set(index, pool.submit((Callable<int[][]>) () -> fillOnce(bound, chunk, height)));
            }
            boolean same = true;
            for (int i = 0; i < chunks.size(); i++) {
                final int[][] got = parallel.get(i).get();
                for (int part = 0; part < 3; part++) {
                    same &= Arrays.equals(serial.get(i)[part], got[part]);
                }
            }
            check(chunks.size() + " chunks filled on 4 threads match one thread, blocks, biomes "
                + "and fluid updates", same);
        } finally {
            pool.shutdown();
        }
    }

    private static int[][] fillOnce(long bound, int[] chunk, int height) {
        final int[] blocks = new int[16 * 16 * height];
        final int[] biomes = new int[16 * (height / 4)];
        final int[] fluids = NativePipeline.fill(bound, chunk[0], chunk[1], blocks, biomes);
        return new int[][] {blocks, biomes, fluids};
    }

    private interface Action {
        void run() throws Exception;
    }

    private static void refuses(String what, Action action, String needle) {
        try {
            action.run();
            check(what + " is refused", false);
        } catch (StratumGenerationException e) {
            final boolean named = e.getMessage() != null && e.getMessage().contains(needle);
            if (!named) {
                System.out.println("  message was: " + e.getMessage());
            }
            check(what + " is refused, naming \"" + needle + "\"", named);
        } catch (Exception e) {
            System.out.println("  threw " + e);
            check(what + " is refused with StratumGenerationException", false);
        }
    }

    private static void check(String what, boolean ok) {
        if (ok) {
            passes++;
            System.out.println("PASS " + what);
        } else {
            failures++;
            System.out.println("FAIL " + what);
        }
    }

    private static void deleteTree(File file) {
        final File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) {
                deleteTree(child);
            }
        }
        if (file.exists() && !file.delete()) {
            throw new IllegalStateException("cannot delete " + file);
        }
    }
}
