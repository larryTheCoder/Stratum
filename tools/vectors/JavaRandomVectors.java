// Stratum — known-answer vector generator for stratum::rng::JavaRandom.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Records what java.util.Random actually produces, so our reimplementation
// is checked against observed behaviour rather than against a reading of the
// specification. Gaussians are covered too: stratum::fdlibm::log
// reproduces StrictMath.log bit-for-bit (see tools/vectors/StrictMathVectors.java). java.util.Random's algorithm is published in its javadoc
// (the linear congruential generator, its constants, and the contract of
// each next* method); this generator uses only the public API.
//
// Floating point values are emitted as raw bit patterns, never as decimal
// text: parity here is bit-exact or it is nothing.
//
// Regenerate with:
//     java tools/vectors/JavaRandomVectors.java > tests/unit/java_random_vectors.inc

import java.util.Random;

public final class JavaRandomVectors {

    // Seeds: the degenerate ones, the LCG multiplier itself (a scrambled
    // seed of zero), and a couple that look like world seeds.
    private static final long[] SEEDS = {
        0L, 1L, -1L, 42L, -42L,
        0x5DEECE66DL,
        123456789L, -987654321L,
        2891948927356891L, -4172144997902289642L,
        Long.MAX_VALUE, Long.MIN_VALUE,
    };

    // Bounds worth covering: 1 (always 0), powers of two (the fast path),
    // non-powers of two (the rejection loop), and the extremes.
    private static final int[] BOUNDS = {
        1, 2, 16, 17, 100, 1 << 30, Integer.MAX_VALUE, Integer.MAX_VALUE - 1,
    };

    private static final int SEQUENCE_LENGTH = 8;

    private static String i64(long v) {
        return v == Long.MIN_VALUE ? "(-9223372036854775807LL - 1)" : v + "LL";
    }

    private static String i32(int v) {
        return v == Integer.MIN_VALUE ? "(-2147483647 - 1)" : Integer.toString(v);
    }

    public static void main(String[] args) {
        System.out.println("// GENERATED FILE — DO NOT EDIT BY HAND.");
        System.out.println("//");
        System.out.println("// Known-answer vectors observed from java.util.Random on a real JVM.");
        System.out.println("// Floating point values are raw bit patterns: parity is bit-exact.");
        System.out.println("// CI regenerates this file and fails if the checked-in copy differs.");
        System.out.println("// Regenerate with:");
        System.out.println("//   java tools/vectors/JavaRandomVectors.java"
            + " > tests/unit/java_random_vectors.inc");
        System.out.println();
        System.out.println("#include <array>");
        System.out.println("#include <cstdint>");
        System.out.println();
        System.out.println("// clang-format off");
        System.out.println();

        emitNextInt();
        emitNextIntBounded();
        emitNextLong();
        emitNextBoolean();
        emitNextFloat();
        emitNextDouble();
        emitNextGaussian();
        emitInterleaved();

        System.out.println("// clang-format on");
    }

    private static void emitNextInt() {
        System.out.println("struct IntSequence {");
        System.out.println("    std::int64_t seed;");
        System.out.println("    std::array<std::int32_t, " + SEQUENCE_LENGTH + "> values;");
        System.out.println("};");
        System.out.println();
        System.out.println("constexpr auto kNextIntVectors = std::to_array<IntSequence>({");
        for (long seed : SEEDS) {
            Random random = new Random(seed);
            StringBuilder line = new StringBuilder("    {" + i64(seed) + ", {{");
            for (int i = 0; i < SEQUENCE_LENGTH; i++) {
                line.append(i == 0 ? "" : ", ").append(i32(random.nextInt()));
            }
            System.out.println(line + "}}},");
        }
        System.out.println("});");
        System.out.println();
    }

    private static void emitNextIntBounded() {
        System.out.println("struct BoundedIntSequence {");
        System.out.println("    std::int64_t seed;");
        System.out.println("    std::int32_t bound;");
        System.out.println("    std::array<std::int32_t, " + SEQUENCE_LENGTH + "> values;");
        System.out.println("};");
        System.out.println();
        System.out.println("constexpr auto kNextIntBoundedVectors ="
            + " std::to_array<BoundedIntSequence>({");
        for (long seed : SEEDS) {
            for (int bound : BOUNDS) {
                Random random = new Random(seed);
                StringBuilder line =
                    new StringBuilder("    {" + i64(seed) + ", " + i32(bound) + ", {{");
                for (int i = 0; i < SEQUENCE_LENGTH; i++) {
                    line.append(i == 0 ? "" : ", ").append(i32(random.nextInt(bound)));
                }
                System.out.println(line + "}}},");
            }
        }
        System.out.println("});");
        System.out.println();
    }

    private static void emitNextLong() {
        System.out.println("struct LongSequence {");
        System.out.println("    std::int64_t seed;");
        System.out.println("    std::array<std::int64_t, " + SEQUENCE_LENGTH + "> values;");
        System.out.println("};");
        System.out.println();
        System.out.println("constexpr auto kNextLongVectors = std::to_array<LongSequence>({");
        for (long seed : SEEDS) {
            Random random = new Random(seed);
            StringBuilder line = new StringBuilder("    {" + i64(seed) + ", {{");
            for (int i = 0; i < SEQUENCE_LENGTH; i++) {
                line.append(i == 0 ? "" : ", ").append(i64(random.nextLong()));
            }
            System.out.println(line + "}}},");
        }
        System.out.println("});");
        System.out.println();
    }

    private static void emitNextBoolean() {
        System.out.println("struct BoolSequence {");
        System.out.println("    std::int64_t seed;");
        System.out.println("    std::array<bool, 16> values;");
        System.out.println("};");
        System.out.println();
        System.out.println("constexpr auto kNextBooleanVectors = std::to_array<BoolSequence>({");
        for (long seed : SEEDS) {
            Random random = new Random(seed);
            StringBuilder line = new StringBuilder("    {" + i64(seed) + ", {{");
            for (int i = 0; i < 16; i++) {
                line.append(i == 0 ? "" : ", ").append(random.nextBoolean());
            }
            System.out.println(line + "}}},");
        }
        System.out.println("});");
        System.out.println();
    }

    private static void emitNextFloat() {
        System.out.println("struct FloatBitsSequence {");
        System.out.println("    std::int64_t seed;");
        System.out.println("    std::array<std::uint32_t, " + SEQUENCE_LENGTH + "> bits;");
        System.out.println("};");
        System.out.println();
        System.out.println("constexpr auto kNextFloatVectors = std::to_array<FloatBitsSequence>({");
        for (long seed : SEEDS) {
            Random random = new Random(seed);
            StringBuilder line = new StringBuilder("    {" + i64(seed) + ", {{");
            for (int i = 0; i < SEQUENCE_LENGTH; i++) {
                int bits = Float.floatToRawIntBits(random.nextFloat());
                line.append(i == 0 ? "" : ", ")
                    .append(String.format("UINT32_C(0x%08X)", bits));
            }
            System.out.println(line + "}}},");
        }
        System.out.println("});");
        System.out.println();
    }

    private static void emitNextDouble() {
        System.out.println("struct DoubleBitsSequence {");
        System.out.println("    std::int64_t seed;");
        System.out.println("    std::array<std::uint64_t, " + SEQUENCE_LENGTH + "> bits;");
        System.out.println("};");
        System.out.println();
        System.out.println("constexpr auto kNextDoubleVectors ="
            + " std::to_array<DoubleBitsSequence>({");
        for (long seed : SEEDS) {
            Random random = new Random(seed);
            StringBuilder line = new StringBuilder("    {" + i64(seed) + ", {{");
            for (int i = 0; i < SEQUENCE_LENGTH; i++) {
                long bits = Double.doubleToRawLongBits(random.nextDouble());
                line.append(i == 0 ? "" : ", ")
                    .append(String.format("UINT64_C(0x%016X)", bits));
            }
            System.out.println(line + "}}},");
        }
        System.out.println("});");
        System.out.println();
    }

    private static void emitNextGaussian() {
        System.out.println("struct GaussianBitsSequence {");
        System.out.println("    std::int64_t seed;");
        System.out.println("    std::array<std::uint64_t, " + SEQUENCE_LENGTH + "> bits;");
        System.out.println("};");
        System.out.println();
        System.out.println("constexpr auto kNextGaussianVectors ="
            + " std::to_array<GaussianBitsSequence>({");
        for (long seed : SEEDS) {
            Random random = new Random(seed);
            StringBuilder line = new StringBuilder("    {" + i64(seed) + ", {{");
            for (int i = 0; i < SEQUENCE_LENGTH; i++) {
                long bits = Double.doubleToRawLongBits(random.nextGaussian());
                line.append(i == 0 ? "" : ", ")
                    .append(String.format("UINT64_C(0x%016X)", bits));
            }
            System.out.println(line + "}}},");
        }
        System.out.println("});");
        System.out.println();
    }

    // Mixed call orders matter: every method consumes a different number of
    // LCG steps, and getting one wrong desynchronises everything downstream
    // without changing any single value in isolation.
    private static void emitInterleaved() {
        System.out.println("struct InterleavedSequence {");
        System.out.println("    std::int64_t seed;");
        System.out.println("    std::array<std::int64_t, 12> values;");
        System.out.println("};");
        System.out.println();
        System.out.println("constexpr auto kInterleavedVectors ="
            + " std::to_array<InterleavedSequence>({");
        for (long seed : SEEDS) {
            Random random = new Random(seed);
            long[] values = new long[12];
            values[0] = random.nextInt();
            values[1] = random.nextBoolean() ? 1L : 0L;
            values[2] = random.nextLong();
            values[3] = random.nextInt(17);
            values[4] = Double.doubleToRawLongBits(random.nextDouble());
            values[5] = Float.floatToRawIntBits(random.nextFloat()) & 0xFFFFFFFFL;
            values[6] = random.nextInt(1 << 30);
            values[7] = Double.doubleToRawLongBits(random.nextGaussian());
            values[8] = random.nextInt();
            values[9] = Double.doubleToRawLongBits(random.nextGaussian());
            values[10] = random.nextLong();
            values[11] = random.nextInt(100);
            StringBuilder line = new StringBuilder("    {" + i64(seed) + ", {{");
            for (int i = 0; i < values.length; i++) {
                line.append(i == 0 ? "" : ", ").append(i64(values[i]));
            }
            System.out.println(line + "}}},");
        }
        System.out.println("});");
        System.out.println();
    }

    private JavaRandomVectors() {}
}
