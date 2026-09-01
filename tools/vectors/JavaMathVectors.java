// Stratum — known-answer vector generator for stratum::javamath.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Emits C++ tables recording what a real JVM does for the integer
// operations our Java-semantics helpers reimplement. The vectors are
// observed output of java.lang.Math and the Java language's own shift and
// narrowing rules — public, specified behaviour, no Mojang code involved.
//
// Regenerate with:
//     java tools/vectors/JavaMathVectors.java > tests/unit/javamath_vectors.inc
//
// Any change to the emitted file must come from re-running this generator,
// never from editing the numbers by hand.

public final class JavaMathVectors {

    private static final int[] INT_DIVIDENDS = {
        0, 1, -1, 2, -2, 7, -7, 15, -15, 16, -16, 1000, -1000,
        Integer.MAX_VALUE, Integer.MIN_VALUE,
    };

    private static final int[] INT_DIVISORS = {
        1, -1, 2, -2, 3, -3, 16, -16, Integer.MAX_VALUE, Integer.MIN_VALUE,
    };

    private static final long[] LONG_DIVIDENDS = {
        0L, 1L, -1L, 3L, -3L, 17L, -17L, 4096L, -4096L,
        6364136223846793005L, -6364136223846793005L,
        Long.MAX_VALUE, Long.MIN_VALUE,
    };

    private static final long[] LONG_DIVISORS = {
        1L, -1L, 2L, -2L, 7L, -7L, 4096L, -4096L,
        Long.MAX_VALUE, Long.MIN_VALUE,
    };

    private static final int[] SHIFT_VALUES_32 = {
        0, 1, -1, 2, -2, 255, -255, 1 << 30, Integer.MAX_VALUE, Integer.MIN_VALUE,
    };

    private static final long[] SHIFT_VALUES_64 = {
        0L, 1L, -1L, 2L, -2L, 255L, -255L, 1L << 62, Long.MAX_VALUE, Long.MIN_VALUE,
    };

    // Java masks shift distances (& 31 for int, & 63 for long), so 32, 64
    // and negative distances are NOT no-ops or errors. C++ would be
    // undefined behaviour here; this is exactly the landmine the helpers
    // exist to defuse.
    private static final int[] SHIFT_DISTANCES = {0, 1, 7, 31, 32, 33, 63, 64, 65, -1, -32};

    private static final double[] NARROWING_DOUBLES = {
        0.0, -0.0, 1.0, -1.0, 2.5, -2.5, 0.5, -0.5,
        2147483647.0, 2147483648.0, -2147483648.0, -2147483649.0,
        9.2233720368547758E18, -9.2233720368547758E18,
        1.0E30, -1.0E30,
        Double.NaN, Double.POSITIVE_INFINITY, Double.NEGATIVE_INFINITY,
    };

    private static String i32(int v) {
        // -2147483648 does not exist as an int literal in C++: it parses as
        // unary minus applied to a value that does not fit in int.
        return v == Integer.MIN_VALUE ? "(-2147483647 - 1)" : Integer.toString(v);
    }

    private static String i64(long v) {
        return v == Long.MIN_VALUE
            ? "(-9223372036854775807LL - 1)"
            : v + "LL";
    }

    private static String f64(double v) {
        if (Double.isNaN(v)) {
            return "std::numeric_limits<double>::quiet_NaN()";
        }
        if (v == Double.POSITIVE_INFINITY) {
            return "std::numeric_limits<double>::infinity()";
        }
        if (v == Double.NEGATIVE_INFINITY) {
            return "-std::numeric_limits<double>::infinity()";
        }
        // %.17g round-trips every finite double exactly.
        String s = String.format("%.17g", v);
        return s.contains(".") || s.contains("e") || s.contains("E") ? s : s + ".0";
    }

    public static void main(String[] args) {
        System.out.println("// GENERATED FILE — DO NOT EDIT BY HAND.");
        System.out.println("//");
        System.out.println("// Known-answer vectors observed from a real JVM.");
        System.out.println("// The behaviour recorded here is specified by the Java language and");
        System.out.println("// java.lang.Math, so it is stable across JVM versions: CI regenerates");
        System.out.println("// this file and fails if the checked-in copy differs.");
        System.out.println("// Regenerate with:");
        System.out.println("//   java tools/vectors/JavaMathVectors.java"
            + " > tests/unit/javamath_vectors.inc");
        System.out.println();
        // Self-sufficient on purpose: an include-sorting pass may place this
        // file above the test's own standard-library includes.
        System.out.println("#include <array>");
        System.out.println("#include <cstdint>");
        System.out.println("#include <limits>");
        System.out.println();
        System.out.println("// clang-format off");
        System.out.println();

        emitDiv32();
        emitDiv64();
        emitShift32();
        emitShift64();
        emitNarrowing();

        System.out.println("// clang-format on");
    }

    private static void emitDiv32() {
        System.out.println("struct DivVector32 {");
        System.out.println("    std::int32_t x;");
        System.out.println("    std::int32_t y;");
        System.out.println("    std::int32_t floorDiv;");
        System.out.println("    std::int32_t floorMod;");
        System.out.println("    std::int32_t truncDiv;");
        System.out.println("    std::int32_t remainder;");
        System.out.println("};");
        System.out.println();
        System.out.println("constexpr auto kDivVectors32 = std::to_array<DivVector32>({");
        for (int x : INT_DIVIDENDS) {
            for (int y : INT_DIVISORS) {
                // Java's / and % overflow silently for MIN_VALUE / -1; the
                // same expression is undefined behaviour in C++.
                int truncDiv = (x == Integer.MIN_VALUE && y == -1) ? Integer.MIN_VALUE : x / y;
                int rem = (x == Integer.MIN_VALUE && y == -1) ? 0 : x % y;
                System.out.printf("    {%s, %s, %s, %s, %s, %s},%n",
                    i32(x), i32(y), i32(Math.floorDiv(x, y)), i32(Math.floorMod(x, y)),
                    i32(truncDiv), i32(rem));
            }
        }
        System.out.println("});");
        System.out.println();
    }

    private static void emitDiv64() {
        System.out.println("struct DivVector64 {");
        System.out.println("    std::int64_t x;");
        System.out.println("    std::int64_t y;");
        System.out.println("    std::int64_t floorDiv;");
        System.out.println("    std::int64_t floorMod;");
        System.out.println("};");
        System.out.println();
        System.out.println("constexpr auto kDivVectors64 = std::to_array<DivVector64>({");
        for (long x : LONG_DIVIDENDS) {
            for (long y : LONG_DIVISORS) {
                System.out.printf("    {%s, %s, %s, %s},%n",
                    i64(x), i64(y), i64(Math.floorDiv(x, y)), i64(Math.floorMod(x, y)));
            }
        }
        System.out.println("});");
        System.out.println();
    }

    private static void emitShift32() {
        System.out.println("struct ShiftVector32 {");
        System.out.println("    std::int32_t value;");
        System.out.println("    int distance;");
        System.out.println("    std::int32_t shl;");
        System.out.println("    std::int32_t shr;");
        System.out.println("    std::int32_t ushr;");
        System.out.println("};");
        System.out.println();
        System.out.println("constexpr auto kShiftVectors32 = std::to_array<ShiftVector32>({");
        for (int value : SHIFT_VALUES_32) {
            for (int distance : SHIFT_DISTANCES) {
                System.out.printf("    {%s, %d, %s, %s, %s},%n",
                    i32(value), distance,
                    i32(value << distance), i32(value >> distance), i32(value >>> distance));
            }
        }
        System.out.println("});");
        System.out.println();
    }

    private static void emitShift64() {
        System.out.println("struct ShiftVector64 {");
        System.out.println("    std::int64_t value;");
        System.out.println("    int distance;");
        System.out.println("    std::int64_t shl;");
        System.out.println("    std::int64_t shr;");
        System.out.println("    std::int64_t ushr;");
        System.out.println("};");
        System.out.println();
        System.out.println("constexpr auto kShiftVectors64 = std::to_array<ShiftVector64>({");
        for (long value : SHIFT_VALUES_64) {
            for (int distance : SHIFT_DISTANCES) {
                System.out.printf("    {%s, %d, %s, %s, %s},%n",
                    i64(value), distance,
                    i64(value << distance), i64(value >> distance), i64(value >>> distance));
            }
        }
        System.out.println("});");
        System.out.println();
    }

    private static void emitNarrowing() {
        System.out.println("struct NarrowingVector {");
        System.out.println("    double value;");
        System.out.println("    std::int32_t toInt;");
        System.out.println("    std::int64_t toLong;");
        System.out.println("    std::int32_t floorToInt;");
        System.out.println("};");
        System.out.println();
        System.out.println("constexpr auto kNarrowingVectors = std::to_array<NarrowingVector>({");
        for (double value : NARROWING_DOUBLES) {
            // Java's (int) cast on a double saturates and maps NaN to 0;
            // the equivalent C++ conversion is undefined behaviour.
            System.out.printf("    {%s, %s, %s, %s},%n",
                f64(value), i32((int) value), i64((long) value),
                i32((int) Math.floor(value)));
        }
        System.out.println("});");
        System.out.println();
    }

    private JavaMathVectors() {}
}
