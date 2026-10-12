// Stratum — deepslate's named noise under legacy_random_source, recorded.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// deepslate (MIT, Misode) is used here strictly as a BLACK BOX, the way
// tools/fetch-vanilla uses the Minecraft server and the way the two other
// drivers in this directory use it: it is run, and what it outputs is
// recorded. Its source is not read, and nothing in this repository is derived
// from it. Only its published API — the .d.ts declarations, which carry
// signatures and no algorithm — was consulted, to know what to call:
// NoiseGeneratorSettings.create, WorldgenRegistries.NOISE.register,
// NoiseParameters.create, RandomState (random, createVisitor,
// getOrCreateNoise), PositionalRandom.seedKey and fromHashOf,
// DensityFunction.fromJson and compute, NormalNoise.first/second,
// PerlinNoise.noiseLevels, ImprovedNoise.xo/yo/zo/p, and LegacyRandom.
//
// WHAT THIS IS FOR. The legacy-seed tables carried one candidate under the
// label "deepslate's own derivation": rule 290 of
// tools/analysis/legacy-seed-analyze.cpp (rule 182 of the 900-rule
// enumeration the goldens analyzers share) — base JavaRandom(worldSeed)
// .nextLong(), XOR the first eight bytes of MD5("ns:path") big-endian, one
// further JavaRandom(seed).nextLong() fork, driving the LCG, at block offset
// 0. Nothing recorded how that was known. This records what deepslate
// actually builds for a named noise in a legacy_random_source dimension, so
// tests/unit/deepslate_legacy_noise_oracle_test.cpp can compare it with that
// rule's prediction computed by this repository's own code.
//
// WHAT IS RECORDED, per world seed:
//
//   * The positional generator's seedKey(), and the first nextLong() of
//     fromHashOf(id) for each noise — the seed half, with no noise built.
//   * Every octave slot of both stacks of getOrCreateNoise(id): whether
//     deepslate built one there, and if so its origin (xo, yo, zo) and its
//     256-entry permutation. The permutation is the fingerprint: it is drawn
//     with nextInt alone, so it says exactly which LCG state the octave was
//     built from.
//   * The noise's value through the noise ROUTER — a `minecraft:noise`
//     density function at xz_scale 1, y_scale 0, applied through
//     RandomState.createVisitor exactly as legacy-seed-probe.sh's dimensions
//     carry it — at fixed (x, 0, z). The driver refuses to write a value the
//     router and getOrCreateNoise(id).sample do not agree on to the bit.
//   * The same values with legacyRandomSource false, where this build's
//     derivation is settled against the server: the control that says the
//     parameters, the coordinates and the router are wired as the test reads
//     them.
//   * LegacyRandom's own primitives at fixed seeds — nextLong, nextInt(256)
//     and nextDouble from a fresh generator each — because its nextDouble is
//     not java.util.Random's (the test pins by how much), which is why an
//     origin, and so a value, can only be compared to a bound and a
//     permutation exactly.
//
// The four noises are legacy-seed-probe.sh's, with its parameters, so the
// identifiers are the ones the rule was scored against on the server.
// Nothing Mojang-derived goes in or comes out: the noises, seeds and points
// are written here, and the settings are built through deepslate's own API.

import {
    DensityFunction, Identifier, LegacyRandom, NoiseGeneratorSettings, NoiseParameters,
    NoiseSettings, RandomState, WorldgenRegistries,
} from 'deepslate';

// legacy-seed-probe.sh's noises, in its <spec>.noises.json order.
const NOISES = [
    { id: 'stratum:na', firstOctave: -3, amplitudes: [1.0] },
    { id: 'stratum:nb', firstOctave: -3, amplitudes: [1.0] },
    { id: 'stratum:nmulti', firstOctave: -5, amplitudes: [1.0, 1.0, 1.0] },
    { id: 'stratum:nskip', firstOctave: -5, amplitudes: [1.0, 0.0, 1.0] },
];

// The two legacy-seed probe seeds, and two more whose 48-bit LCG states sit
// at the ends of the range.
const SEEDS = [42n, 31337n, 0n, -1n];

// The probe's readback grid is every fourth column of r.0.0; these are some
// of its points, then a few outside it, negative and far.
const POINTS = [
    [0, 0], [4, 12], [128, 256], [508, 4], [260, 500],
    [-4, -8], [-1000, 733], [12345, -6789],
];

const PRIMITIVE_SEEDS = [0n, 1n, -1n, 42n, 31337n, 12345n, 9223372036854775807n, -9223372036854775808n];

for (const noise of NOISES) {
    WorldgenRegistries.NOISE.register(Identifier.parse(noise.id),
        NoiseParameters.create(noise.firstOctave, noise.amplitudes));
}

const settingsFor = (legacy) => {
    const settings = NoiseGeneratorSettings.create({
        noise: NoiseSettings.create({ minY: -64, height: 384, xzSize: 1, ySize: 2 }),
        legacyRandomSource: legacy,
    });
    if (settings.legacyRandomSource !== legacy) {
        throw new Error('deepslate did not take legacyRandomSource as written');
    }
    return settings;
};

const MIN_INT64 = -(1n << 63n);
const int64 = (value) => (value === MIN_INT64
    ? '(INT64_C(-9223372036854775807) - 1)'
    : `INT64_C(${value})`);
const u64 = (value) => `UINT64_C(0x${BigInt.asUintN(64, value).toString(16).toUpperCase().padStart(16, '0')})`;
const bits = (value) => {
    if (typeof value !== 'number' || !Number.isFinite(value)) {
        throw new Error(`not a finite double: ${value}`);
    }
    const buffer = new ArrayBuffer(8);
    new DataView(buffer).setFloat64(0, value);
    return u64(new DataView(buffer).getBigUint64(0));
};
const permutation = (table) => {
    if (table.length !== 256) {
        throw new Error(`a permutation of ${table.length} entries`);
    }
    const seen = new Set(table.map((entry) => entry & 0xff));
    if (seen.size !== 256) {
        throw new Error('a permutation that is not one');
    }
    return '"' + table.map((entry) => (entry & 0xff).toString(16).padStart(2, '0')).join('') + '"';
};

const lines = [];
const out = (line) => lines.push(line);

out('// GENERATED FILE — DO NOT EDIT BY HAND.');
out('//');
out("// What deepslate (MIT, Misode), run as a black box, builds for a named noise");
out('// in a legacy_random_source dimension, and the same noises with the flag off.');
out('// Its source is not read and nothing here is derived from it; see');
out('// tools/vectors/deepslate_legacy_noise_vectors.mjs for what that means and');
out('// for what each table is for.');
out('//');
out('// Regenerate with: tools/vectors/generate-deepslate-vectors.sh');
out('//');
out('// Longs and doubles are raw bit patterns; a permutation is 256 bytes in hex.');
out('');
out('#include <array>');
out('#include <cstdint>');
out('#include <string_view>');
out('');
out('// clang-format off');
out('');
out('struct DeepslateLegacyNoiseParameters {');
out('    std::string_view id;');
out('    std::int32_t firstOctave;');
out('    std::array<double, 3> amplitudes;');
out('    std::int32_t amplitudeCount;');
out('};');
out('');
out('constexpr auto kDeepslateLegacyNoises = std::to_array<DeepslateLegacyNoiseParameters>({');
for (const noise of NOISES) {
    const padded = [...noise.amplitudes, 0, 0, 0].slice(0, 3);
    out(`    {"${noise.id}", ${noise.firstOctave}, {${padded.map((a) => a.toFixed(1)).join(', ')}}, ` +
        `${noise.amplitudes.length}},`);
}
out('});');
out('');
out('/// A fresh LegacyRandom(seed) per primitive: its first nextLong(), nextInt(256)');
out('/// and nextDouble().');
out('struct DeepslateLegacyRandomPrimitive {');
out('    std::int64_t seed;');
out('    std::uint64_t nextLong;');
out('    std::int32_t nextInt256;');
out('    std::uint64_t nextDouble;');
out('};');
out('');
out('constexpr auto kDeepslateLegacyRandomPrimitives = std::to_array<DeepslateLegacyRandomPrimitive>({');
for (const seed of PRIMITIVE_SEEDS) {
    const nextLong = new LegacyRandom(seed).nextLong();
    const nextInt = new LegacyRandom(seed).nextInt(256);
    const nextDouble = new LegacyRandom(seed).nextDouble();
    if (!Number.isInteger(nextInt) || nextInt < 0 || nextInt >= 256) {
        throw new Error(`nextInt(256) gave ${nextInt}`);
    }
    out(`    {${int64(seed)}, ${u64(nextLong)}, ${nextInt}, ${bits(nextDouble)}},`);
}
out('});');
out('');
out('/// RandomState.random under the legacy source: its seedKey(), and the first');
out('/// nextLong() of fromHashOf(id).');
out('struct DeepslateLegacyNamedGenerator {');
out('    std::int64_t seed;');
out('    std::string_view id;');
out('    std::uint64_t seedKeyLo;');
out('    std::uint64_t seedKeyHi;');
out('    std::uint64_t firstLong;');
out('};');
out('');

const generators = [];
const octaves = [];
const samples = [];
for (const legacy of [true, false]) {
    const settings = settingsFor(legacy);
    for (const seed of SEEDS) {
        const state = new RandomState(settings, seed);
        const visitor = state.createVisitor(settings.noise, settings.legacyRandomSource);
        const [seedKeyLo, seedKeyHi] = state.random.seedKey();
        for (const noise of NOISES) {
            const normal = state.getOrCreateNoise(Identifier.parse(noise.id));
            if (legacy) {
                const firstLong = state.random.fromHashOf(noise.id).nextLong();
                generators.push(`    {${int64(seed)}, "${noise.id}", ${u64(seedKeyLo)}, ` +
                    `${u64(seedKeyHi)}, ${u64(firstLong)}},`);
                for (const [stack, perlin] of [[0, normal.first], [1, normal.second]]) {
                    for (let slot = 0; slot < noise.amplitudes.length; ++slot) {
                        const level = perlin.noiseLevels[slot];
                        if (level === undefined) {
                            octaves.push(`    {${int64(seed)}, "${noise.id}", ${stack}, ${slot}, false, ` +
                                `UINT64_C(0), UINT64_C(0), UINT64_C(0), ""},`);
                        } else {
                            octaves.push(`    {${int64(seed)}, "${noise.id}", ${stack}, ${slot}, true, ` +
                                `${bits(level.xo)}, ${bits(level.yo)}, ${bits(level.zo)}, ` +
                                `${permutation(level.p)}},`);
                        }
                    }
                    if (perlin.noiseLevels.length > noise.amplitudes.length) {
                        throw new Error(`${noise.id}: more octave slots than amplitudes`);
                    }
                }
            }
            const routed = visitor.apply(DensityFunction.fromJson({
                type: 'minecraft:noise', noise: noise.id, xz_scale: 1.0, y_scale: 0.0,
            }));
            for (const [x, z] of POINTS) {
                const value = routed.compute({ x, y: 0, z });
                const direct = normal.sample(x, 0, z);
                if (!Object.is(value, direct)) {
                    throw new Error(`${noise.id} at ${x} 0 ${z}: router ${value}, noise ${direct}`);
                }
                samples.push(`    {${legacy}, ${int64(seed)}, "${noise.id}", ${x}, ${z}, ${bits(value)}},`);
            }
        }
    }
}

out('constexpr auto kDeepslateLegacyNamedGenerators = std::to_array<DeepslateLegacyNamedGenerator>({');
generators.forEach(out);
out('});');
out('');
out('/// One octave slot of getOrCreateNoise(id) under the legacy source: stack 0 is');
out('/// NormalNoise.first, 1 is .second; slot is the declared amplitude index.');
out('struct DeepslateLegacyOctave {');
out('    std::int64_t seed;');
out('    std::string_view id;');
out('    std::int32_t stack;');
out('    std::int32_t slot;');
out('    bool present;');
out('    std::uint64_t xo;');
out('    std::uint64_t yo;');
out('    std::uint64_t zo;');
out('    std::string_view permutation;');
out('};');
out('');
out('constexpr auto kDeepslateLegacyOctaves = std::to_array<DeepslateLegacyOctave>({');
octaves.forEach(out);
out('});');
out('');
out('/// The noise through the router, a `minecraft:noise` at xz_scale 1 and');
out('/// y_scale 0, at (x, 0, z); legacy says which source the dimension declared.');
out('struct DeepslateLegacyNoiseSample {');
out('    bool legacy;');
out('    std::int64_t seed;');
out('    std::string_view id;');
out('    std::int32_t x;');
out('    std::int32_t z;');
out('    std::uint64_t value;');
out('};');
out('');
out('constexpr auto kDeepslateLegacyNoiseSamples = std::to_array<DeepslateLegacyNoiseSample>({');
samples.forEach(out);
out('});');
out('');
out('// clang-format on');
process.stdout.write(lines.join('\n') + '\n');
