// Stratum — known-answer vectors for old_blended_noise, from deepslate.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// deepslate (MIT, Misode) is used here strictly as a BLACK BOX, the way
// tools/fetch-vanilla uses the Minecraft server: it is run, and what it
// outputs is recorded. Its source is not read, and nothing in this
// repository is derived from it. Only its published API — the .d.ts
// declarations, which carry signatures and no algorithm — was consulted, to
// know what to call. CLAUDE.md's provenance rule permits observed
// input/output; this is that.
//
// WHY IT IS TRUSTED AT ALL. Because it was checked, not assumed. Running
// deepslate's own final_density down a column and comparing the highest
// positive y against the OCEAN_FLOOR heightmap vanilla itself wrote into the
// golden regions: 6143 of 6144 columns matched exactly, across the
// overworld, the Nether and the End and four seeds. It reproduces vanilla's
// terrain. That is a stronger claim than this repository can make about any
// of its other oracles, and it was made against the goldens, which are the
// authority (SPEC §7).
//
// WHAT THESE VECTORS ARE FOR. `old_blended_noise` has three unsettled
// questions (SPEC §11) and no way to check an answer. These give it one: any
// candidate implementation either reproduces these numbers or does not. They
// do not supply the algorithm, and finding it is still work.
//
// Nothing Mojang-derived goes in or comes out: the parameters below are
// written here rather than read from vanilla's files, and the settings are
// built through deepslate's own API.

import { NoiseGeneratorSettings, NoiseSettings, RandomState, DensityFunction } from 'deepslate';

const SEEDS = [0n, 1n, 42n, -1n, 123456789n];

// The three shapes vanilla's dimensions use, plus one that is nobody's, so
// that an implementation which happened to special-case a known set would
// still have to be right.
const CONFIGS = [
    { name: 'overworld', xz_scale: 0.25, y_scale: 0.125, xz_factor: 80.0, y_factor: 160.0, smear_scale_multiplier: 8.0 },
    { name: 'nether',    xz_scale: 0.25, y_scale: 0.375, xz_factor: 80.0, y_factor: 60.0,  smear_scale_multiplier: 8.0 },
    { name: 'end',       xz_scale: 0.25, y_scale: 0.25,  xz_factor: 80.0, y_factor: 160.0, smear_scale_multiplier: 4.0 },
    { name: 'arbitrary', xz_scale: 0.5,  y_scale: 0.75,  xz_factor: 40.0, y_factor: 200.0, smear_scale_multiplier: 1.0 },
];

// Negative y is deliberate: the smear folds nothing at y = 0 and something
// everywhere else, so a vector set that stayed above ground would leave the
// multiplier untested.
const COORDS = [
    [0, 0, 0], [1, 0, 0], [0, 1, 0], [0, 0, 1], [-1, -1, -1],
    [17, 33, -49], [100, 0, -100], [4, 8, 12], [-7, 319, 5],
    [0, -64, 0], [1000, -60, -1000], [-256, 128, 256],
];

const bits = (value) => {
    const buffer = new ArrayBuffer(8);
    new DataView(buffer).setFloat64(0, value);
    return '0x' + [...new Uint8Array(buffer)]
        .map((byte) => byte.toString(16).padStart(2, '0')).join('').toUpperCase();
};

const settings = NoiseGeneratorSettings.create({
    noise: NoiseSettings.create({ minY: -64, height: 384, xzSize: 1, ySize: 2 }),
    legacyRandomSource: false,
});

const lines = [];
const out = (line) => lines.push(line);

out('// GENERATED FILE — DO NOT EDIT BY HAND.');
out('//');
out('// Known-answer vectors for minecraft:old_blended_noise, produced by');
out('// deepslate (MIT, Misode) run as a black box. Its source is not read and');
out('// nothing here is derived from it; see tools/vectors/deepslate_vectors.mjs');
out('// for what that means and for how it was checked against the goldens.');
out('//');
out('// Regenerate with: tools/vectors/generate-deepslate-vectors.sh');
out('//');
out('// Doubles are raw bit patterns: parity here is bit-exact or it is nothing.');
out('');
out('#include <array>');
out('#include <cstdint>');
out('#include <string_view>');
out('');
out('// clang-format off');
out('');
out('struct BlendedNoiseVector {');
out('    std::int64_t seed;');
out('    std::string_view shape;');
out('    std::uint64_t xzScale;');
out('    std::uint64_t yScale;');
out('    std::uint64_t xzFactor;');
out('    std::uint64_t yFactor;');
out('    std::uint64_t smearScaleMultiplier;');
out('    std::int32_t x;');
out('    std::int32_t y;');
out('    std::int32_t z;');
out('    std::uint64_t value;');
out('};');
out('');
out('constexpr auto kDeepslateBlendedVectors = std::to_array<BlendedNoiseVector>({');

for (const seed of SEEDS) {
    const state = new RandomState(settings, seed);
    const visitor = state.createVisitor(settings.noise, settings.legacyRandomSource);
    for (const config of CONFIGS) {
        const { name, ...parameters } = config;
        const bound = visitor.apply(DensityFunction.fromJson({
            type: 'minecraft:old_blended_noise', ...parameters,
        }));
        for (const [x, y, z] of COORDS) {
            const value = bound.compute({ x, y, z });
            out(`    {INT64_C(${seed}), "${name}", ` +
                `UINT64_C(${bits(parameters.xz_scale)}), UINT64_C(${bits(parameters.y_scale)}), ` +
                `UINT64_C(${bits(parameters.xz_factor)}), UINT64_C(${bits(parameters.y_factor)}), ` +
                `UINT64_C(${bits(parameters.smear_scale_multiplier)}), ` +
                `${x}, ${y}, ${z}, UINT64_C(${bits(value)})},`);
        }
    }
}

out('});');
out('');
out('// clang-format on');
process.stdout.write(lines.join('\n') + '\n');
