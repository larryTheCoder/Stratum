// Stratum — a Bedrock blockstate as the engine's mapping table names it.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
package stratum.nukkit;

/**
 * {@code {name, states}} of one Bedrock blockstate, typed: a byte 0 and an
 * int 0 are different states to Nukkit, as to the client. Read from
 * {@link NativePipeline#bedrockState}; the version is the table's, not
 * carried per state.
 */
public final class BedrockState {
    public final String name;
    public final String[] keys;
    public final int[] types;
    public final int[] integers;
    public final String[] strings;

    private BedrockState(String name, String[] keys, int[] types, int[] integers, String[] strings) {
        this.name = name;
        this.keys = keys;
        this.types = types;
        this.integers = integers;
        this.strings = strings;
    }

    public static BedrockState of(Object[] native_) {
        if (native_ == null || native_.length != 5) {
            throw new StratumGenerationException("stratum: a Bedrock state from the native library "
                + "is not {name, keys, types, integers, strings}");
        }
        final BedrockState state = new BedrockState((String) native_[0], (String[]) native_[1],
            (int[]) native_[2], (int[]) native_[3], (String[]) native_[4]);
        final int count = state.keys.length;
        if (state.types.length != count || state.integers.length != count
            || state.strings.length != count) {
            throw new StratumGenerationException("stratum: Bedrock state " + state.name
                + " came back with arrays of different lengths");
        }
        return state;
    }

    public static BedrockState ofJavaState(int javaStateId) {
        return of(NativePipeline.bedrockState(javaStateId));
    }

    /** {@code minecraft:snow_layer{covered_bit:0b,height:0}}, NBT's own spelling. */
    @Override
    public String toString() {
        final StringBuilder text = new StringBuilder(name).append('{');
        for (int i = 0; i < keys.length; i++) {
            if (i > 0) {
                text.append(',');
            }
            text.append(keys[i]).append(':');
            switch (types[i]) {
                case NativePipeline.TAG_BYTE:
                    text.append(integers[i]).append('b');
                    break;
                case NativePipeline.TAG_INT:
                    text.append(integers[i]);
                    break;
                case NativePipeline.TAG_STRING:
                    text.append('"').append(strings[i]).append('"');
                    break;
                default:
                    text.append("<tag ").append(types[i]).append('>');
                    break;
            }
        }
        return text.append('}').toString();
    }
}
