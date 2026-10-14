// Stratum — the Stratum worlds Nukkit will load or create as it starts.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
package stratum.nukkit;

import cn.nukkit.Server;
import cn.nukkit.level.format.anvil.Anvil;
import cn.nukkit.level.format.leveldb.LevelDBProvider;
import cn.nukkit.level.generator.Generator;
import cn.nukkit.nbt.stream.NBTInputStream;
import cn.nukkit.nbt.tag.CompoundTag;
import cn.nukkit.nbt.tag.Tag;

import java.io.DataInputStream;
import java.io.File;
import java.io.IOException;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * Which worlds Nukkit's {@code Server} constructor is about to load or
 * create once its {@code STARTUP} plugins are enabled, and which of them
 * generate through Stratum — read the way that constructor reads them
 * (CloudburstMC/Nukkit {@code b3caad67d}), so the plugin can refuse a world
 * before Nukkit touches it:
 * <ul>
 * <li>every name under {@code nukkit.yml}'s {@code worlds}, then the
 * default world, {@code server.properties}' {@code level-name};
 * <li>a world exists when Nukkit has a provider for its folder (a
 * {@code level.dat} beside {@code db/}, or beside Anvil's {@code region/});
 * an existing world generates through what its own {@code level.dat} names,
 * whatever the configuration says now;
 * <li>a new world generates through {@code worlds.<name>.generator}'s text
 * before the first colon, with the rest as its options (or, with no colon,
 * {@code server.properties}' {@code generator-settings}); the default world
 * through {@code level-type}, with {@code generator-settings}.
 * </ul>
 * Not {@code Server.isLevelGenerated}: it asks {@code LevelProviderManager},
 * which Nukkit fills only after its {@code STARTUP} plugins are enabled, so
 * at this point it calls every world new.
 */
final class StartupWorlds {
    /** One Stratum world Nukkit will load ({@link #exists}) or create. */
    static final class World {
        final String name;
        final File folder;
        final boolean exists;
        final String options;

        World(String name, File folder, boolean exists, String options) {
            this.name = name;
            this.folder = folder;
            this.exists = exists;
            this.options = options;
        }
    }

    /** What could not be read, each naming its world; Nukkit reads those itself. */
    final List<String> unreadable = new ArrayList<>();

    /** The Stratum worlds, in the order Nukkit will load or create them. */
    final List<World> worlds = new ArrayList<>();

    private StartupWorlds() {
    }

    static StartupWorlds read(Server server) {
        final StartupWorlds result = new StartupWorlds();
        final Set<String> names = new LinkedHashSet<>();
        final Map<String, Object> configured = server.getConfig("worlds", new HashMap<String, Object>());
        for (String name : configured.keySet()) {
            if (!names.add(name)) {
                continue;
            }
            final String generator = String.valueOf(server.getConfig("worlds." + name + ".generator",
                Generator.getGenerator("default").getSimpleName()));
            // Nukkit's own split: the generator, then everything after the
            // first colon, rejoined, as the options.
            final String[] parts = generator.split(":");
            final StringBuilder options = new StringBuilder();
            for (int i = 1; i < parts.length; i++) {
                options.append(i == 1 ? "" : ":").append(parts[i]);
            }
            result.add(server, name, Generator.getGenerator(parts[0]),
                parts.length > 1 ? options.toString() : server.getPropertyString("generator-settings", ""));
        }
        String defaultName = server.getPropertyString("level-name", "world");
        if (defaultName == null || defaultName.trim().isEmpty()) {
            defaultName = "world";
        }
        if (names.add(defaultName)) {
            result.add(server, defaultName,
                Generator.getGenerator(server.getPropertyString("level-type", "DEFAULT")),
                server.getPropertyString("generator-settings", ""));
        }
        return result;
    }

    private void add(Server server, String name, Class<? extends Generator> configuredGenerator,
                     String configuredOptions) {
        final String path = name.contains("/") || name.contains("\\") ? name
            : server.getDataPath() + "worlds/" + name + "/";
        final File folder = new File(path);
        if (LevelDBProvider.isValid(path)) {
            final CompoundTag levelData;
            try {
                levelData = readLevelDat(new File(folder, "level.dat"));
            } catch (IOException | RuntimeException e) {
                unreadable.add("world '" + name + "': cannot read " + new File(folder, "level.dat")
                    + " (" + e + ")");
                return;
            }
            // LevelDBProvider's reading: generatorName, or failing that the
            // generator registered under the numeric type.
            final Class<? extends Generator> generator;
            if (levelData.contains("generatorName")) {
                generator = Generator.getGenerator(levelData.getString("generatorName"));
            } else if (levelData.contains("Generator")) {
                generator = Generator.getGenerator(levelData.getInt("Generator"));
            } else {
                generator = Generator.getGenerator("DEFAULT");
            }
            if (generator == StratumGenerator.class) {
                worlds.add(new World(name, folder, true, levelData.contains("generatorOptions")
                    ? levelData.getString("generatorOptions") : ""));
            }
            return;
        }
        if (Anvil.isValid(path)) {
            // Stratum never creates Anvil worlds; reading Anvil's level.dat
            // is left to Nukkit, and opening one refuses it by name.
            return;
        }
        if (configuredGenerator == StratumGenerator.class) {
            worlds.add(new World(name, folder, false, configuredOptions));
        }
    }

    /** LevelDBProvider.loadLevelData's reading, without its fallback to level.dat_old. */
    private static CompoundTag readLevelDat(File file) throws IOException {
        try (NBTInputStream stream = new NBTInputStream(
            new DataInputStream(Files.newInputStream(file.toPath())), ByteOrder.LITTLE_ENDIAN)) {
            final int version = stream.readInt();
            if (version != 8 && version != 9 && version != 10) {
                throw new IOException("level.dat version " + version + " is not one Nukkit reads");
            }
            stream.readInt();
            final Tag tag = Tag.readNamedTag(stream);
            if (!(tag instanceof CompoundTag)) {
                throw new IOException("level.dat does not hold a compound tag");
            }
            return (CompoundTag) tag;
        }
    }
}
