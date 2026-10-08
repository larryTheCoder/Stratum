// Stratum — known-answer vectors for the aquifer's random source, from deepslate.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// deepslate (MIT, Misode) is used here strictly as a BLACK BOX, the way
// tools/fetch-vanilla uses the Minecraft server and the way
// tools/vectors/deepslate_vectors.mjs uses it for old_blended_noise: it is
// run, and what it outputs is recorded. Its source is not read, and nothing
// in this repository is derived from it. Only its published API — the .d.ts
// declarations, which carry signatures and no algorithm — was consulted, to
// know what to call: RandomState.aquiferRandom, PositionalRandom.seedKey and
// at, Random.nextLong and nextInt.
//
// WHAT THESE VECTORS ARE. The aquifer places its cell centres from a
// per-cell generator: the world seed forked into a 128-bit base, salted with
// the MD5 of `minecraft:aquifer`, forked again, and XORed with a mix of the
// cell's index (SPEC §11, "The jitter draw, recovered"). This records, for
// nine world seeds, the base deepslate derives (seedKey), and for eight cells
// each the first three longs that cell's generator draws, plus the three
// bounded draws (10, 9, 10, in that order) from a fresh one. Those bounds and
// that order are this project's own reading, recovered against the server;
// deepslate supplies only what its generator yields when asked for them.
//
// WHICH CELLS, AND WHY ONLY THOSE. deepslate's own aquifer, and most of its
// per-position generator, were checked against the server first, and neither
// reproduces it (tools/analysis/deepslate-aquifer-trust.sh measures both):
//
//   * Its aquifer. On the five open-void probe worlds this project's corpora
//     were generated from (pslvar/c96, comb_{42,7,12345,999}/jv, chunks
//     [0,4)x[0,4)), NoiseChunkGenerator.fill does not run the aquifer at all
//     — its output is the global fluid picker and does not move when the
//     floodedness, spread, lava or surface level do — and NoiseAquifer
//     driven directly disagrees with the frozen 1.21.11 server on 298 934 to
//     375 270 of 1 572 864 blocks a world, placing no barrier and no aquifer
//     lava anywhere, where the server's own flowing blocks number 1 to 3.
//
//   * PositionalRandom.at. It evaluates the position mix WITHOUT wrapping:
//     on 400 of 400 fixed cells, out to the world border's, its generator is
//     the documented mix taken in arbitrary precision (neither the x term nor
//     the squared product reduced to 32 or 64 bits before the shift), and on
//     0 of 400 the wrapped one. The server takes the wrapped one: on the
//     jitter readout (vanilla_aquifer_jitter_test.cpp) the wrapped mix places
//     256 of 256 cells over four seeds, deepslate's at() 193 of 255.
//
// The two mixes agree exactly where nothing wraps, and on a cell index that
// means the y axis: with x = z = 0 the mixed value is y itself, and
// 42317861 y^2 + 11 y stays far inside 64 bits. So the cells below sit on
// that axis, the driver refuses any other, and what the vectors pin is the
// part of the derivation deepslate gets right: the salt, both forks, the MD5
// halves' order, and the mix's y term, its shift and the half it is XORed
// into. Not the x and z terms, not the wraps, not arithmetic against logical
// shift (no value on this axis is negative before the shift), and not the
// bounds or the order of the draws — none of those has a CI oracle; the
// server-backed conformance cases pin them.
//
// Nothing Mojang-derived goes in or comes out: the seeds and cells are
// written here, and the settings are built through deepslate's own API.

import { NoiseGeneratorSettings, NoiseSettings, RandomState } from 'deepslate';

// Small, negative and both extremes: the seed is upgraded to 128 bits and
// forked, and the sign bit has to survive every step of that.
const SEEDS = [
    0n, 1n, -1n, 42n, 7n, 12345n, 999n,
    9223372036854775807n, -9223372036854775808n,
];

// Cell indices on the y axis, from the lowest cell a world from -64 reaches
// (-7, the window's dy = -1 under cell -6) to the highest (27), and the
// origin, where every shift agrees. No two share a magnitude: y and -y mix to
// one value unless 42317861 y^2 + 11 y and 42317861 y^2 - 11 y straddle a
// multiple of 65536, so a pair would be one vector written twice.
const CELL_YS = [-7, -6, -4, 0, 1, 5, 13, 27];
const CELLS = CELL_YS.map((y) => [0, y, 0]);

// This project's reading of the aquifer's draws: x, y, z from one generator,
// ten values horizontally and nine vertically.
const BOUNDS = [10, 9, 10];

const MIN_INT64 = -(1n << 63n);
const int64 = (value) => (value === MIN_INT64
    ? '(INT64_C(-9223372036854775807) - 1)'
    : `INT64_C(${value})`);
const bits = (value) => `UINT64_C(0x${BigInt.asUintN(64, value).toString(16).toUpperCase().padStart(16, '0')})`;

for (const [x, y, z] of CELLS) {
    // Off the y axis deepslate's at() is not the server's (header), so a
    // vector there would be a fact about deepslate and not about vanilla.
    if (x !== 0 || z !== 0 || Math.abs(y) > 1024) {
        throw new Error(`cell ${x} ${y} ${z} is outside the range where deepslate's mix is vanilla's`);
    }
}

const settings = NoiseGeneratorSettings.create({
    noise: NoiseSettings.create({ minY: -64, height: 384, xzSize: 1, ySize: 1 }),
    legacyRandomSource: false,
});

const lines = [];
const out = (line) => lines.push(line);

out('// GENERATED FILE — DO NOT EDIT BY HAND.');
out('//');
out("// Known-answer vectors for the aquifer's per-cell random source, produced");
out('// by deepslate (MIT, Misode) run as a black box. Its source is not read and');
out('// nothing here is derived from it; see');
out('// tools/vectors/deepslate_aquifer_vectors.mjs for what that means, for why');
out("// every cell sits on the y axis, and for why deepslate's aquifer itself is");
out('// NOT used as an oracle.');
out('//');
out('// Regenerate with: tools/vectors/generate-deepslate-vectors.sh');
out('//');
out('// Longs are raw bit patterns: parity here is bit-exact or it is nothing.');
out('');
out('#include <array>');
out('#include <cstdint>');
out('');
out('// clang-format off');
out('');
out('struct AquiferRandomVector {');
out('    std::int64_t seed;');
out('    /// The positional base: PositionalRandom.seedKey() of RandomState.aquiferRandom.');
out('    std::uint64_t baseLo;');
out('    std::uint64_t baseHi;');
out('    /// The cell index handed to PositionalRandom.at.');
out('    std::int32_t cx;');
out('    std::int32_t cy;');
out('    std::int32_t cz;');
out("    /// The first three nextLong() of that cell's generator.");
out('    std::array<std::uint64_t, 3> longs;');
out('    /// nextInt(10), nextInt(9), nextInt(10), in that order, from a fresh one.');
out('    std::array<std::int32_t, 3> draws;');
out('};');
out('');
out('constexpr auto kDeepslateAquiferRandomVectors = std::to_array<AquiferRandomVector>({');

for (const seed of SEEDS) {
    const random = new RandomState(settings, seed).aquiferRandom;
    const [baseLo, baseHi] = random.seedKey();
    for (const [cx, cy, cz] of CELLS) {
        const longs = random.at(cx, cy, cz);
        const raw = [longs.nextLong(), longs.nextLong(), longs.nextLong()];
        const bounded = random.at(cx, cy, cz);
        const draws = BOUNDS.map((bound) => bounded.nextInt(bound));
        for (const [i, draw] of draws.entries()) {
            if (!Number.isInteger(draw) || draw < 0 || draw >= BOUNDS[i]) {
                throw new Error(`draw ${draw} out of [0, ${BOUNDS[i]}) at seed ${seed}, cell ${cx} ${cy} ${cz}`);
            }
        }
        out(`    {${int64(seed)}, ${bits(baseLo)}, ${bits(baseHi)}, ${cx}, ${cy}, ${cz}, ` +
            `{${raw.map(bits).join(', ')}}, {${draws.join(', ')}}},`);
    }
}

out('});');
out('');
out('// clang-format on');
process.stdout.write(lines.join('\n') + '\n');
