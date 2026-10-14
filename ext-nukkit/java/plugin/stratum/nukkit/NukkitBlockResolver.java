// Stratum — a Bedrock blockstate to Nukkit's own full block id.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
package stratum.nukkit;

import cn.nukkit.block.Block;
import cn.nukkit.level.format.leveldb.BlockStateMapping;
import cn.nukkit.level.format.leveldb.LegacyStateMapper;
import cn.nukkit.level.format.leveldb.structure.BlockStateSnapshot;
import org.cloudburstmc.nbt.NbtMap;
import org.cloudburstmc.nbt.NbtMapBuilder;

/**
 * Resolves the engine's Bedrock blockstates through Nukkit's own
 * {@code BlockStateMapping} (SPEC §9): no second table, and no Nukkit
 * fallback. Nukkit's lookups substitute {@code info_update} for what they
 * cannot find ({@code getState(NbtMap)}, {@code updateState}, a snapshot's
 * {@code getLegacyId}); every step here uses the variant that reports a miss
 * instead, so a miss reaches the caller with its reason.
 *
 * <p>Three ways a state can miss, each checked:
 * <ol>
 * <li>Not in Nukkit's block palette by name and states. Nukkit keys the
 * palette by the whole NbtMap, {@code version} included, and its palette is
 * older than the engine's table, so the key carries Nukkit's own version
 * (read off its palette's default state), not the table's.
 * <li>In the palette, but with no legacy id:meta. Nukkit's chunks hold
 * legacy ids in memory.
 * <li>A legacy id:meta Nukkit would store back as a different state: a chunk
 * section turns a full id into a palette state through {@code
 * legacyToRuntime}, and two states sharing one legacy id cannot both survive
 * that.
 * </ol>
 * Reads only immutable tables Nukkit builds at startup.
 */
public final class NukkitBlockResolver {
    /** One answer: a full id ({@code legacyId << 13 | meta}), or why there is none. */
    public static final class Resolution {
        public final int fullId;
        public final String miss;

        private Resolution(int fullId, String miss) {
            this.fullId = fullId;
            this.miss = miss;
        }
    }

    private final BlockStateMapping mapping = BlockStateMapping.get();
    private final LegacyStateMapper legacy = mapping.getLegacyMapper();
    private final int paletteVersion = mapping.getDefaultState().getVanillaState().getInt("version");

    /** The blockstate version Nukkit's palette is at (1.21.30.7 at the pinned commit). */
    public int paletteVersion() {
        return paletteVersion;
    }

    public Resolution resolve(BedrockState state) {
        final NbtMapBuilder states = NbtMap.builder();
        for (int i = 0; i < state.keys.length; i++) {
            switch (state.types[i]) {
                case NativePipeline.TAG_BYTE:
                    states.putByte(state.keys[i], (byte) state.integers[i]);
                    break;
                case NativePipeline.TAG_INT:
                    states.putInt(state.keys[i], state.integers[i]);
                    break;
                case NativePipeline.TAG_STRING:
                    states.putString(state.keys[i], state.strings[i]);
                    break;
                default:
                    return miss("its state " + state.keys[i] + " has NBT tag type " + state.types[i]
                        + ", which no Bedrock blockstate uses");
            }
        }
        final NbtMap key = NbtMap.builder()
            .putString("name", state.name)
            .putCompound("states", states.build())
            .putInt("version", paletteVersion)
            .build();

        final BlockStateSnapshot snapshot = mapping.getStateUnsafe(key);
        if (snapshot == null) {
            return miss("Nukkit's block palette (blockstate version "
                + NativePipeline.formatVersion(paletteVersion) + ") has no state of that name and "
                + "states");
        }
        final int fullId = legacy.runtimeToFullId(snapshot.getRuntimeId());
        if (fullId < 0) {
            return miss("it is in Nukkit's block palette as runtime id " + snapshot.getRuntimeId()
                + ", which Nukkit has no legacy id:meta for");
        }
        final int back = legacy.legacyToRuntime(fullId >> Block.DATA_BITS, fullId & Block.DATA_MASK);
        if (back != snapshot.getRuntimeId()) {
            final BlockStateSnapshot stored = back < 0 ? null : mapping.getState(back);
            if (stored == null || !key.equals(stored.getVanillaState())) {
                return miss("its legacy id " + (fullId >> Block.DATA_BITS) + ":"
                    + (fullId & Block.DATA_MASK) + " is stored back by Nukkit as "
                    + (stored == null ? "nothing" : String.valueOf(stored.getVanillaState()))
                        .replace('\n', ' '));
            }
        }
        return new Resolution(fullId, null);
    }

    private static Resolution miss(String why) {
        return new Resolution(-1, why);
    }
}
