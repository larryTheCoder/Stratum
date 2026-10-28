// Stratum — the Nukkit plugin: registers the generator, freezes and checks worlds.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
package stratum.nukkit;

import cn.nukkit.block.Block;
import cn.nukkit.block.BlockLiquid;
import cn.nukkit.event.EventHandler;
import cn.nukkit.event.EventPriority;
import cn.nukkit.event.Listener;
import cn.nukkit.event.level.ChunkPopulateEvent;
import cn.nukkit.event.level.LevelUnloadEvent;
import cn.nukkit.level.Level;
import cn.nukkit.level.format.FullChunk;
import cn.nukkit.level.generator.Generator;
import cn.nukkit.plugin.PluginBase;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

/**
 * Loads at {@code STARTUP}, so everything in {@link #onLoad} happens before
 * Nukkit loads or creates a world:
 * <ul>
 * <li>registers {@link StratumGenerator} as {@code stratum}. Registration
 * comes first and never depends on anything else succeeding: Nukkit gives a
 * world whose generator is not registered its own {@code Normal} generator,
 * silently;
 * <li>loads the native library ({@link NativePipeline#load}: the
 * {@code stratum.nukkit.library} property, then this plugin's data folder,
 * then {@code java.library.path});
 * <li>for every Stratum world Nukkit is about to load or create
 * ({@link StartupWorlds}): writes a new world's frozen pipeline from its
 * options' {@code "pack"} — the world-creation hook Nukkit does not offer,
 * taken before Nukkit creates the world; a world that exists is never
 * refrozen — and then makes every check opening it would
 * ({@link WorldPipeline#validate});
 * <li>if any of those worlds is refused, logs each refusal by name and stops
 * the server, exit status 1, before Nukkit loads or creates any world.
 * </ul>
 * Stopping is the only clean refusal Nukkit leaves a plugin
 * ({@code b3caad67d}): a generator whose {@code init} throws is logged and
 * replaced by {@code null} in {@code Level}'s per-thread generator, and
 * {@code Level.initLevel} then throws a {@code NullPointerException}. For a
 * new world, {@code Server.generateLevel} catches that, but only after
 * {@code LevelDBProvider.generate} has written the world's {@code level.dat};
 * for an existing one, {@code Server.loadLevel} does not, and from Nukkit's
 * startup the exception ends the server anyway.
 *
 * <p>While running, it schedules each generated chunk's fluid updates once
 * Nukkit populates the chunk (all eight neighbours generated, as
 * {@code ext/}'s PocketMine-MP plugin does), and forgets a world when Nukkit
 * unloads it.
 */
public final class StratumPlugin extends PluginBase implements Listener {
    private static volatile Throwable nativeFailure;
    private static volatile boolean nativeLoaded;

    @Override
    public void onLoad() {
        Generator.addGenerator(StratumGenerator.class, StratumGenerator.NAME, StratumGenerator.TYPE);
        try {
            final String from = NativePipeline.load(getDataFolder());
            nativeLoaded = true;
            getLogger().info("native library loaded from " + from + " (engine blockstates at "
                + NativePipeline.formatVersion(NativePipeline.bedrockBlockStateVersion()) + ")");
        } catch (RuntimeException | LinkageError e) {
            nativeFailure = e;
        }

        final StartupWorlds startup = StartupWorlds.read(getServer());
        for (String problem : startup.unreadable) {
            getLogger().warning(problem + "; if it is a Stratum world, it is checked only when "
                + "Nukkit opens it");
        }
        if (startup.worlds.isEmpty()) {
            if (nativeFailure != null) {
                getLogger().error("cannot load the Stratum native library, and no Stratum world is "
                    + "configured; any Stratum world created later will refuse to open: "
                    + nativeFailure.getMessage());
            }
            return;
        }
        final List<String> refused = new ArrayList<>();
        for (StartupWorlds.World world : startup.worlds) {
            try {
                requireNative();
                if (!world.exists) {
                    freezeNew(world);
                }
                WorldPipeline.validate(world.name, world.options);
                getLogger().info("world '" + world.name + "' checked: it opens, and every block "
                    + "state it can place resolves through Nukkit's BlockStateMapping");
            } catch (RuntimeException | LinkageError e) {
                refused.add(world.name);
                final String why = String.valueOf(e.getMessage());
                getLogger().error("REFUSED " + (why.contains("world '" + world.name + "'") ? why
                    : "stratum: world '" + world.name + "': " + why));
            }
        }
        if (!refused.isEmpty()) {
            stopBeforeWorlds(refused);
        }
    }

    @Override
    public void onEnable() {
        getServer().getPluginManager().registerEvents(this, this);
    }

    /** Throws the native library's load failure, if it failed. */
    static void requireNative() {
        if (!nativeLoaded) {
            final Throwable failure = nativeFailure;
            throw new StratumGenerationException("stratum: the native library is not loaded"
                + (failure == null ? " (is the Stratum plugin installed and loaded?)"
                    : ": " + failure.getMessage()), failure);
        }
    }

    /** Writes a new world's frozen pipeline from its "pack", unless the blob is there already. */
    private void freezeNew(StartupWorlds.World world) {
        final GeneratorOptions parsed = GeneratorOptions.parse(world.options);
        final File blob = parsed.blobFile();
        if (blob.isFile()) {
            getLogger().info("world '" + world.name + "' will be created from the existing frozen "
                + "pipeline " + blob);
            return;
        }
        if (parsed.pack == null) {
            // Left to validate(), which refuses a world with no blob.
            return;
        }
        NativePipeline.freeze(new File(parsed.pack).getAbsolutePath(), blob.getPath());
        getLogger().info("world '" + world.name + "': froze " + parsed.pack + " into " + blob);
    }

    /**
     * Stops the server, exit status 1. Called from {@link #onLoad}: Nukkit
     * has opened its network interface and its player database by then, and
     * no world, and nothing is written yet, so exiting loses nothing. Not
     * through {@code forceShutdown}: before the server's first tick its
     * scheduler's tick is -1, and the {@code mainThreadHeartbeat(Integer.MAX_VALUE)}
     * that {@code forceShutdown} makes overflows its own catch-up test and
     * counts an int up to {@code Integer.MAX_VALUE} inclusive, which never
     * ends (Nukkit b3caad67, ServerScheduler; observed: the JVM spun there).
     */
    private void stopBeforeWorlds(List<String> refused) {
        getLogger().error("STOPPING the server before Nukkit loads or creates any world: "
            + refused.size() + " Stratum world(s) refused, each named above: " + refused
            + ". Nukkit cannot fail one world cleanly (a refused new world would be left with a "
            + "level.dat, and an existing one would stop the server anyway), so fix or remove the "
            + "world and start again");
        System.exit(1);
    }

    @EventHandler(priority = EventPriority.MONITOR)
    public void onChunkPopulate(ChunkPopulateEvent event) {
        final Level level = event.getLevel();
        final WorldPipeline world = WorldPipeline.of(level);
        if (world == null) {
            return;
        }
        final FullChunk chunk = event.getChunk();
        final int[] fluids = world.takeFluidUpdates(chunk.getX(), chunk.getZ());
        if (fluids == null) {
            return;
        }
        final int baseX = chunk.getX() << 4;
        final int baseZ = chunk.getZ() << 4;
        int scheduled = 0;
        for (int packed : fluids) {
            final Block block = level.getBlock(baseX + (packed & 15), world.minY + (packed >>> 8),
                baseZ + ((packed >>> 4) & 15));
            // What a liquid does on a normal update: schedule itself after
            // its own tick rate (BlockLiquid.onUpdate).
            if (block instanceof BlockLiquid) {
                level.scheduleUpdate(block, block.tickRate());
                scheduled++;
            }
        }
        world.recordFluidUpdatesScheduled(scheduled);
    }

    @EventHandler(priority = EventPriority.MONITOR, ignoreCancelled = true)
    public void onLevelUnload(LevelUnloadEvent event) {
        WorldPipeline.close(event.getLevel());
    }
}
