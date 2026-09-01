// Stratum — known-answer vectors for stratum::fdlibm.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// StrictMath is fdlibm and returns identical bits on every JVM, which makes
// it the oracle for our vendored port. Inputs and results are both emitted
// as raw bit patterns: a decimal round-trip would hide exactly the one-ulp
// differences these vectors exist to catch.
//
// Inputs are chosen deterministically (fixed seeds, fixed constants), so
// this generator's output is reproducible and CI can diff it.
//
// Regenerate with:
//     java tools/vectors/StrictMathVectors.java > tests/unit/fdlibm_vectors.inc

import java.util.ArrayList;
import java.util.List;
import java.util.Random;

public final class StrictMathVectors {

    private static List<Double> logInputs() {
        List<Double> inputs = new ArrayList<>();

        // Special cases named in fdlibm's own header comment.
        inputs.add(0.0);
        inputs.add(-0.0);
        inputs.add(-1.0);
        inputs.add(-Double.MAX_VALUE);
        inputs.add(Double.POSITIVE_INFINITY);
        inputs.add(Double.NEGATIVE_INFINITY);
        inputs.add(Double.NaN);

        // Exact and near-exact anchors.
        inputs.add(1.0);
        inputs.add(2.0);
        inputs.add(0.5);
        inputs.add(Math.E);
        inputs.add(10.0);
        inputs.add(Double.MIN_VALUE);          // smallest subnormal
        inputs.add(Double.MIN_NORMAL);         // 2^-1022
        inputs.add(Double.MAX_VALUE);

        // The |f| < 2**-20 short-circuit branch, from both sides of 1.0.
        for (int exponent = 1; exponent <= 60; exponent++) {
            double delta = Math.scalb(1.0, -exponent);
            inputs.add(1.0 + delta);
            inputs.add(1.0 - delta);
        }
        inputs.add(Math.nextUp(1.0));
        inputs.add(Math.nextDown(1.0));

        // The argument-reduction boundaries: sqrt(2)/2 and sqrt(2) decide
        // which normalisation branch is taken.
        double[] boundaries = {Math.sqrt(2.0) / 2.0, Math.sqrt(2.0)};
        for (double boundary : boundaries) {
            inputs.add(boundary);
            inputs.add(Math.nextUp(boundary));
            inputs.add(Math.nextDown(boundary));
        }

        // Powers of two across the whole exponent range, and their
        // neighbours.
        for (int exponent = -1074; exponent <= 1023; exponent += 37) {
            double value = Math.scalb(1.0, exponent);
            if (value > 0.0 && !Double.isInfinite(value)) {
                inputs.add(value);
                inputs.add(Math.nextUp(value));
            }
        }

        // The domain nextGaussian actually samples: s in (0, 1).
        Random random = new Random(20260902L);
        for (int i = 0; i < 400; i++) {
            double s = random.nextDouble();
            if (s > 0.0) {
                inputs.add(s);
            }
        }

        // A wide sweep over positive finite doubles, by exponent and
        // mantissa, so no branch of the polynomial goes unexercised.
        Random bitRandom = new Random(-4172144997902289642L);
        for (int i = 0; i < 400; i++) {
            long mantissa = bitRandom.nextLong() & 0x000FFFFFFFFFFFFFL;
            long exponent = (long) bitRandom.nextInt(2046) + 1L;
            inputs.add(Double.longBitsToDouble((exponent << 52) | mantissa));
        }

        return inputs;
    }

    public static void main(String[] args) {
        System.out.println("// GENERATED FILE — DO NOT EDIT BY HAND.");
        System.out.println("//");
        System.out.println("// Known-answer vectors for java.lang.StrictMath (fdlibm), observed");
        System.out.println("// from a real JVM. Inputs and results are raw bit patterns because");
        System.out.println("// the whole point is to catch one-ulp differences.");
        System.out.println("// CI regenerates this file and fails if the checked-in copy differs.");
        System.out.println("// Regenerate with:");
        System.out.println("//   java tools/vectors/StrictMathVectors.java"
            + " > tests/unit/fdlibm_vectors.inc");
        System.out.println();
        System.out.println("#include <array>");
        System.out.println("#include <cstdint>");
        System.out.println();
        System.out.println("// clang-format off");
        System.out.println();
        System.out.println("struct LogVector {");
        System.out.println("    std::uint64_t inputBits;");
        System.out.println("    std::uint64_t resultBits;");
        System.out.println("};");
        System.out.println();
        System.out.println("constexpr auto kLogVectors = std::to_array<LogVector>({");
        for (double input : logInputs()) {
            System.out.printf("    {UINT64_C(0x%016X), UINT64_C(0x%016X)},%n",
                Double.doubleToRawLongBits(input),
                Double.doubleToRawLongBits(StrictMath.log(input)));
        }
        System.out.println("});");
        System.out.println();
        System.out.println("// clang-format on");
    }

    private StrictMathVectors() {}
}
