// Stratum — the Nukkit plugin: registers the generator, freezes new worlds.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
package stratum.nukkit;

import cn.nukkit.Server;
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
import java.util.HashMap;
import java.util.Map;

/**
 * Loads at {@code STARTUP}, so everything here happens before Nukkit loads
 * or creates a world:
 * <ul>
 * <li>registers {@link StratumGenerator} as {@code stratum}. Registration
 * comes first and never depends on anything else succeeding: Nukkit gives a
 * world whose generator is not registered its own {@code Normal} generator,
 * silently, so a Stratum world must find Stratum registered even when the
 * native library is missing — and then fail to open, saying why;
 * <li>loads the native library ({@link NativePipeline#load}: the
 * {@code stratum.nukkit.library} property, then this plugin's data folder,
 * then {@code java.library.path});
 * <li>writes the frozen pipeline of every Stratum world that does not exist
 * yet, from its options' {@code "pack"} — the world-creation hook Nukkit
 * does not offer, taken before Nukkit creates the world. A world that
 * already exists is never refrozen.
 * </ul>
 * Nothing here throws: a plugin that fails to load leaves its worlds to
 * {@code Normal}. Every problem is logged, and the world it concerns then
 * refuses to open, by name.
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
        } catch (Throwable e) {
            nativeFailure = e;
            getLogger().error("cannot load the Stratum native library; every Stratum world will "
                + "refuse to open: " + e.getMessage());
            return;
        }
        prepareWorlds(getServer());
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

    /** Freezes the pipeline of each configured Stratum world Nukkit has not created yet. */
    private void prepareWorlds(Server server) {
        if (StratumGenerator.NAME.equalsIgnoreCase(server.getPropertyString("level-type", "default"))) {
            prepare(server, server.getPropertyString("level-name", "world"),
                server.getPropertyString("generator-settings", ""));
        }
        final Map<String, Object> worlds = server.getConfig("worlds", new HashMap<String, Object>());
        for (String name : worlds.keySet()) {
            // Nukkit's own reading of the entry (Server's constructor):
            // "<generator>:<options>", the options being everything after
            // the first colon.
            final String generator = String.valueOf(server.getConfig("worlds." + name + ".generator", ""));
            final int colon = generator.indexOf(':');
            final String type = colon < 0 ? generator : generator.substring(0, colon);
            if (StratumGenerator.NAME.equalsIgnoreCase(type)) {
                prepare(server, name, colon < 0 ? "" : generator.substring(colon + 1));
            }
        }
    }

    private void prepare(Server server, String world, String options) {
        if (server.isLevelGenerated(world)) {
            return;
        }
        try {
            final GeneratorOptions parsed = GeneratorOptions.parse(options);
            final File blob = parsed.blobFile();
            if (blob.isFile()) {
                getLogger().info("world '" + world + "' will generate from the existing frozen "
                    + "pipeline " + blob);
                return;
            }
            if (parsed.pack == null) {
                getLogger().error("world '" + world + "' has no frozen pipeline at " + blob
                    + " and its options name no \"pack\" to write one from; it will refuse to open");
                return;
            }
            NativePipeline.freeze(new File(parsed.pack).getAbsolutePath(), blob.getPath());
            getLogger().info("world '" + world + "': froze " + parsed.pack + " into " + blob);
        } catch (Throwable e) {
            getLogger().error("world '" + world + "': " + e.getMessage()
                + "; it will refuse to open");
        }
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
        for (int packed : fluids) {
            final Block block = level.getBlock(baseX + (packed & 15), world.minY + (packed >>> 8),
                baseZ + ((packed >>> 4) & 15));
            // What a liquid does on a normal update: schedule itself after
            // its own tick rate (BlockLiquid.onUpdate).
            if (block instanceof BlockLiquid) {
                level.scheduleUpdate(block, block.tickRate());
            }
        }
    }

    @EventHandler(priority = EventPriority.MONITOR, ignoreCancelled = true)
    public void onLevelUnload(LevelUnloadEvent event) {
        WorldPipeline.close(event.getLevel());
    }
}
