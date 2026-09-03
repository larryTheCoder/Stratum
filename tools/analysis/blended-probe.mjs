// Stratum — bulk samples of old_blended_noise, from deepslate.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// deepslate (MIT, Misode) is a BLACK BOX here, exactly as in
// tools/vectors/deepslate_vectors.mjs: it is run and what it prints is
// recorded. Its source is not read and nothing is derived from it. See that
// file's header for the provenance rule and for how deepslate was checked
// against the golden regions before being trusted at all.
//
// The vectors file answers "is this candidate right at these twelve points".
// This answers a different question: what are the *statistics* of the field?
// Those are what a candidate can be judged on while its seeding is still
// unknown, because the spectrum, the distribution shape and the ratio of
// spreads do not depend on the seed. That is the whole reason this exists.
import { NoiseGeneratorSettings, NoiseSettings, RandomState, DensityFunction } from 'deepslate';

const CONFIGS = {
    overworld: { xz_scale: 0.25, y_scale: 0.125, xz_factor: 80.0, y_factor: 160.0, smear_scale_multiplier: 8.0 },
    smear1:    { xz_scale: 0.25, y_scale: 0.125, xz_factor: 80.0, y_factor: 160.0, smear_scale_multiplier: 1.0 },
};

const settings = NoiseGeneratorSettings.create({
    noise: NoiseSettings.create({ minY: -64, height: 384, xzSize: 1, ySize: 2 }),
    legacyRandomSource: false,
});

const [, , mode, config = 'overworld', seed = '0'] = process.argv;
if (!CONFIGS[config]) {
    console.error(`unknown config '${config}'; try ${Object.keys(CONFIGS).join(', ')}`);
    process.exit(2);
}
const state = new RandomState(settings, BigInt(seed));
const visitor = state.createVisitor(settings.noise, settings.legacyRandomSource);
const fn = visitor.apply(DensityFunction.fromJson({
    type: 'minecraft:old_blended_noise', ...CONFIGS[config],
}));

// `lines` is sampled at y = 0 on purpose. That is the one height where this
// build's smear_scale_multiplier provably does nothing, so a comparison there
// measures normalisation alone and is not contaminated by the smear question.
// The 32 lines are 977 apart, which is past the field's correlation length,
// so their spectra can be averaged as independent estimates.
if (mode === 'lines') {
    for (let k = 0; k < 32; ++k) {
        const z = k * 977;
        for (let x = 0; x < 4096; ++x) { console.log(fn.compute({ x, y: 0, z })); }
    }
} else if (mode === 'planes') {
    // 4096 (x, z) samples at each of several heights, for the smear question:
    // how the spread varies with height is where a wrong smear shows up.
    for (const y of [-64, -32, 0, 1, 2, 4, 8, 16, 32, 64, 128, 192, 256, 320]) {
        for (let i = 0; i < 4096; ++i) {
            console.log(`${y} ${fn.compute({ x: (i % 64) * 71, y, z: ((i / 64) | 0) * 89 })}`);
        }
    }
} else {
    console.error("mode must be 'lines' or 'planes'");
    process.exit(2);
}
