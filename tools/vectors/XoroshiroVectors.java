// Stratum — known-answer vectors for stratum::rng::Xoroshiro128PlusPlus.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// JAVA_FLAGS: --add-exports java.base/jdk.internal.random=ALL-UNNAMED --add-exports java.base/jdk.internal.util.random=ALL-UNNAMED --add-exports jdk.random/jdk.random=ALL-UNNAMED
//
// Two independent oracles, both shipped with the JDK:
//
//   * jdk.internal.random.Xoroshiro128PlusPlus has a public (x0, x1)
//     constructor, so its state can be set exactly and its stream compared
//     against ours step for step. This is a real oracle for the algorithm.
//   * jdk.internal.util.random.RandomSupport.mixStafford13 is the same
//     mixing function vanilla uses to derive a 128-bit state from a seed.
//
// What these oracles do NOT establish is that Mojang composes them the way
// we think. The seed-upgrade vectors below are computed from the documented
// formula using the JDK's own mixStafford13, so they catch a porting mistake
// in our C++ — but agreement with vanilla itself is only settled when the
// density-function pipeline can be diffed against the goldens (M3).
//
// Regenerate with the flags above:
//     java --add-exports java.base/jdk.internal.random=ALL-UNNAMED \
//          --add-exports java.base/jdk.internal.util.random=ALL-UNNAMED \
//          tools/vectors/XoroshiroVectors.java > tests/unit/xoroshiro_vectors.inc

import java.lang.reflect.Constructor;
import java.util.Arrays;
import java.util.random.RandomGenerator;
import jdk.internal.util.random.RandomSupport;

public final class XoroshiroVectors {

    /// States worth covering: the degenerate low values, an all-ones word,
    /// and states that look like a mixed world seed.
    private static final long[][] STATES = {
        {1L, 0L},
        {0L, 1L},
        {1L, 2L},
        {-1L, -1L},
        {Long.MIN_VALUE, Long.MAX_VALUE},
        {0x6A09E667F3BCC909L, 0x9E3779B97F4A7C15L},
        {-4172144997902289642L, 2891948927356891L},
        {0x0123456789ABCDEFL, -0x0123456789ABCDEFL},
    };

    private static final long[] SEEDS = {
        0L, 1L, -1L, 42L, -42L, 123456789L,
        -4172144997902289642L, 2891948927356891L,
        Long.MAX_VALUE, Long.MIN_VALUE,
    };

    private static final int SEQUENCE_LENGTH = 8;

    // Vanilla derives a 128-bit state from a 64-bit seed with these two
    // constants — the 64-bit fractional parts of the silver and golden
    // ratios — and Stafford's variant 13 mix.
    private static final long SILVER_RATIO_64 = 0x6A09E667F3BCC909L;
    private static final long GOLDEN_RATIO_64 = 0x9E3779B97F4A7C15L;

    /// The JDK's Xoroshiro128PlusPlus has moved package between releases, so
    /// it is looked up by name rather than imported: an import that resolves
    /// on the machine that wrote the vectors and not on the one checking them
    /// turns a reproducibility check into a build error, which is exactly
    /// what happened on CI.
    private static final String[] CANDIDATE_CLASSES = {
        "jdk.internal.random.Xoroshiro128PlusPlus",
        "jdk.random.Xoroshiro128PlusPlus",
    };

    private static final Constructor<?> XOROSHIRO = findXoroshiro();

    private static Constructor<?> findXoroshiro() {
        for (String name : CANDIDATE_CLASSES) {
            try {
                return Class.forName(name).getConstructor(long.class, long.class);
            } catch (ReflectiveOperationException ignored) {
                // Try the next spelling.
            }
        }
        throw new IllegalStateException(
            "no Xoroshiro128PlusPlus with a (long, long) constructor on this JDK ("
            + System.getProperty("java.version") + "); tried "
            + Arrays.toString(CANDIDATE_CLASSES)
            + ". The exact-state constructor is what makes this an oracle at all.");
    }

    private static RandomGenerator newGenerator(long x0, long x1) {
        try {
            return (RandomGenerator) XOROSHIRO.newInstance(x0, x1);
        } catch (ReflectiveOperationException error) {
            throw new IllegalStateException("cannot construct the JDK generator", error);
        }
    }

    private static String i64(long v) {
        return v == Long.MIN_VALUE ? "(-9223372036854775807LL - 1)" : v + "LL";
    }

    private static String u64(long v) {
        return String.format("UINT64_C(0x%016X)", v);
    }

    public static void main(String[] args) {
        System.out.println("// GENERATED FILE — DO NOT EDIT BY HAND.");
        System.out.println("//");
        System.out.println("// Oracles: the JDK's own Xoroshiro128PlusPlus (seeded to an exact");
        System.out.println("// state) and RandomSupport.mixStafford13. See");
        System.out.println("// tools/vectors/XoroshiroVectors.java for what they do and do not");
        System.out.println("// establish. CI regenerates this file and fails if it differs.");
        System.out.println();
        System.out.println("#include <array>");
        System.out.println("#include <cstdint>");
        System.out.println();
        System.out.println("// clang-format off");
        System.out.println();

        emitCoreVectors();
        emitMixVectors();
        emitSeedUpgradeVectors();
        emitDerivedVectors();

        System.out.println("// clang-format on");
    }

    /// The algorithm itself, from an exactly known state.
    private static void emitCoreVectors() {
        System.out.println("struct XoroshiroSequence {");
        System.out.println("    std::uint64_t lo;");
        System.out.println("    std::uint64_t hi;");
        System.out.println("    std::array<std::int64_t, " + SEQUENCE_LENGTH + "> values;");
        System.out.println("};");
        System.out.println();
        System.out.println("constexpr auto kNextLongVectors = std::to_array<XoroshiroSequence>({");
        for (long[] state : STATES) {
            RandomGenerator generator = newGenerator(state[0], state[1]);
            StringBuilder line = new StringBuilder("    {" + u64(state[0]) + ", " + u64(state[1]) + ", {{");
            for (int i = 0; i < SEQUENCE_LENGTH; i++) {
                line.append(i == 0 ? "" : ", ").append(i64(generator.nextLong()));
            }
            System.out.println(line + "}}},");
        }
        System.out.println("});");
        System.out.println();
    }

    private static void emitMixVectors() {
        System.out.println("struct MixVector {");
        System.out.println("    std::uint64_t input;");
        System.out.println("    std::uint64_t mixed;");
        System.out.println("};");
        System.out.println();
        System.out.println("constexpr auto kMixStafford13Vectors = std::to_array<MixVector>({");
        for (long seed : SEEDS) {
            System.out.printf("    {%s, %s},%n", u64(seed), u64(RandomSupport.mixStafford13(seed)));
        }
        for (long[] state : STATES) {
            System.out.printf("    {%s, %s},%n", u64(state[0]),
                u64(RandomSupport.mixStafford13(state[0])));
        }
        System.out.println("});");
        System.out.println();
    }

    /// The documented seed upgrade, computed here with the JDK's mix so that
    /// a porting error in our C++ shows up. See the header for what this
    /// does not prove.
    private static void emitSeedUpgradeVectors() {
        System.out.println("struct SeedUpgradeVector {");
        System.out.println("    std::int64_t seed;");
        System.out.println("    std::uint64_t lo;");
        System.out.println("    std::uint64_t hi;");
        System.out.println("};");
        System.out.println();
        System.out.println("constexpr auto kSeedUpgradeVectors = std::to_array<SeedUpgradeVector>({");
        for (long seed : SEEDS) {
            long lo = seed ^ SILVER_RATIO_64;
            long hi = lo + GOLDEN_RATIO_64;
            System.out.printf("    {%s, %s, %s},%n", i64(seed),
                u64(RandomSupport.mixStafford13(lo)), u64(RandomSupport.mixStafford13(hi)));
        }
        System.out.println("});");
        System.out.println();
    }

    /// nextInt / nextDouble / nextFloat as the JDK derives them from
    /// nextLong. Emitted only for the states above, so our derivations are
    /// checked against an implementation that is not ours.
    private static void emitDerivedVectors() {
        System.out.println("struct DerivedVector {");
        System.out.println("    std::uint64_t lo;");
        System.out.println("    std::uint64_t hi;");
        System.out.println("    std::array<std::int32_t, 4> nextInt;");
        System.out.println("    std::array<std::uint64_t, 4> nextDoubleBits;");
        System.out.println("    std::array<std::uint32_t, 4> nextFloatBits;");
        System.out.println("    std::array<bool, 8> nextBoolean;");
        System.out.println("};");
        System.out.println();
        System.out.println("constexpr auto kDerivedVectors = std::to_array<DerivedVector>({");
        for (long[] state : STATES) {
            StringBuilder line = new StringBuilder("    {" + u64(state[0]) + ", " + u64(state[1]) + ", {{");

            RandomGenerator ints = newGenerator(state[0], state[1]);
            for (int i = 0; i < 4; i++) {
                line.append(i == 0 ? "" : ", ").append(ints.nextInt());
            }
            line.append("}}, {{");

            RandomGenerator doubles = newGenerator(state[0], state[1]);
            for (int i = 0; i < 4; i++) {
                line.append(i == 0 ? "" : ", ")
                    .append(u64(Double.doubleToRawLongBits(doubles.nextDouble())));
            }
            line.append("}}, {{");

            RandomGenerator floats = newGenerator(state[0], state[1]);
            for (int i = 0; i < 4; i++) {
                line.append(i == 0 ? "" : ", ")
                    .append(String.format("UINT32_C(0x%08X)",
                        Float.floatToRawIntBits(floats.nextFloat())));
            }
            line.append("}}, {{");

            RandomGenerator booleans = newGenerator(state[0], state[1]);
            for (int i = 0; i < 8; i++) {
                line.append(i == 0 ? "" : ", ").append(booleans.nextBoolean());
            }
            System.out.println(line + "}}},");
        }
        System.out.println("});");
        System.out.println();
    }

    private XoroshiroVectors() {}
}
