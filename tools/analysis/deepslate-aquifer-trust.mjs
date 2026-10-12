// Stratum — deepslate's aquifer and per-position random source, sampled for
// comparison against the server.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// deepslate (MIT, Misode) is a BLACK BOX here, exactly as in
// tools/vectors/deepslate_aquifer_vectors.mjs: it is run and what it outputs
// is recorded. Its source is not read and nothing is derived from it; only
// its .d.ts declarations were consulted, to know what to call. The two
// position mixes below are this project's own: the documented one (SPEC §11)
// and the same formula evaluated without wrapping, which is the hypothesis
// deepslate's output is scored against.
//
// Run by deepslate-aquifer-trust.sh, which compares the output with the
// server's probe corpora. Modes:
//
//   blocks <seed> <lava> <fill|aquifer> <chunks> <out> [router overrides]
//       Block categories over chunks [0,n)x[0,n) of the open-void probe world
//       (constant final_density -1, barrier -2, floodedness 0.5, spread 0,
//       surface 96, sea 63; a JSON object of router entries replaces any of
//       them) as bytes, [y + 64][z][x]: 0 air, 1 water, 2 lava, 3 stone, 4
//       anything else. `fill` is NoiseChunkGenerator.fill; `aquifer` drives
//       NoiseAquifer.compute directly at density -1, with the global fluid
//       picker built here from SPEC §11's Q1.2.
//   mix <cells>
//       How many cells' generators PositionalRandom.at matches under each
//       candidate mix.
//   jitter <seed> <chunks> <cy>
//       Per cell (cx, cy, cz) of the window: the bounded draws from deepslate's
//       at(), then from the documented mix fed to deepslate's own generator.
import { writeFileSync } from 'node:fs';

import {
    BlockState, ChunkPos, Chunk, FixedBiomeSource, FluidStatus, Identifier, NoiseAquifer,
    NoiseChunk, NoiseChunkGenerator, NoiseGeneratorSettings, NoiseSettings, RandomState,
    XoroshiroRandom,
} from 'deepslate';

const [, , mode, ...args] = process.argv;

const settingsFor = (lava, overrides = {}) => {
    const settings = NoiseGeneratorSettings.fromJson({
        sea_level: 63, disable_mob_generation: true, aquifers_enabled: true,
        ore_veins_enabled: false, legacy_random_source: false,
        default_block: { Name: 'minecraft:stone' },
        default_fluid: { Name: 'minecraft:water', Properties: { level: '0' } },
        noise: { min_y: -64, height: 384, size_horizontal: 1, size_vertical: 1 },
        spawn_target: [],
        surface_rule: { type: 'minecraft:block', result_state: { Name: 'minecraft:stone' } },
        noise_router: {
            barrier: -2.0, fluid_level_floodedness: 0.5, fluid_level_spread: 0.0, lava,
            temperature: 0, vegetation: 0, continents: 0, erosion: 0, depth: 0, ridges: 0,
            preliminary_surface_level: 96.0, final_density: -1.0,
            vein_toggle: 0, vein_ridged: 0, vein_gap: 0, ...overrides,
        },
    });
    // fromJson's defaults are not documented; a world that silently lost its
    // aquifer would measure nothing.
    if (!settings.aquifersEnabled || settings.oreVeinsEnabled || settings.legacyRandomSource ||
        settings.seaLevel !== 63) {
        throw new Error('deepslate did not take the probe settings as written');
    }
    return settings;
};

const settingsForRandom = () => NoiseGeneratorSettings.create({
    noise: NoiseSettings.create({ minY: -64, height: 384, xzSize: 1, ySize: 1 }),
    legacyRandomSource: false,
});

const int64 = (value) => BigInt.asIntN(64, value);
const int32 = (value) => BigInt.asIntN(32, value);
const mixes = {
    // SPEC §11: the x term a 32-bit product, every step wrapping to 64 bits.
    wrapped: (x, y, z) => {
        let l = int64(int32(BigInt(x) * 3129871n) ^ (BigInt(z) * 116129781n) ^ BigInt(y));
        l = int64(l * l * 42317861n + l * 11n);
        return l >> 16n;
    },
    // The same formula with nothing reduced before the shift.
    unwrapped: (x, y, z) => {
        const l = (BigInt(x) * 3129871n) ^ (BigInt(z) * 116129781n) ^ BigInt(y);
        return (l * l * 42317861n + l * 11n) >> 16n;
    },
};

const generatorFor = (random, mix, x, y, z) => {
    const [lo, hi] = random.seedKey();
    return new XoroshiroRandom([
        BigInt.asUintN(64, BigInt.asUintN(64, lo) ^ BigInt.asUintN(64, mix(x, y, z))),
        BigInt.asUintN(64, hi),
    ]);
};

if (mode === 'blocks') {
    const [seed, lava, entry, chunks, out, overrides = '{}'] = args;
    const settings = settingsFor(Number(lava), JSON.parse(overrides));
    const state = new RandomState(settings, BigInt(seed));
    const n = Number(chunks);
    const width = n * 16;
    const bytes = new Uint8Array(384 * width * width);
    const code = (block) => {
        if (block === undefined) return 3; // NoiseAquifer's "solid"
        const name = block.getName().toString();
        return { 'minecraft:air': 0, 'minecraft:water': 1, 'minecraft:lava': 2, 'minecraft:stone': 3 }[name] ?? 4;
    };
    const lavaSea = new FluidStatus(-54, BlockState.LAVA);
    const sea = new FluidStatus(settings.seaLevel, settings.defaultFluid);
    const picker = (x, y) => (y < Math.min(-54, settings.seaLevel) ? lavaSea : sea);
    const noise = settings.noise;
    for (let cz = 0; cz < n; ++cz) {
        for (let cx = 0; cx < n; ++cx) {
            let at;
            if (entry === 'aquifer') {
                const noiseChunk = new NoiseChunk(16 / NoiseSettings.cellWidth(noise),
                    NoiseSettings.cellCountY(noise), NoiseSettings.minCellY(noise), state,
                    cx * 16, cz * 16, noise, true, picker);
                const aquifer = new NoiseAquifer(noiseChunk, ChunkPos.create(cx, cz), state.router,
                    state.aquiferRandom, noise.minY, noise.height, picker);
                at = (x, y, z) => aquifer.compute({ x: cx * 16 + x, y, z: cz * 16 + z }, -1);
            } else if (entry === 'fill') {
                const generator = new NoiseChunkGenerator(
                    new FixedBiomeSource(Identifier.parse('minecraft:plains')), settings);
                const chunk = new Chunk(-64, 384, ChunkPos.create(cx, cz));
                generator.fill(state, chunk);
                at = (x, y, z) => chunk.getBlockState([x, y, z]);
            } else {
                throw new Error(`unknown entry point ${entry}`);
            }
            for (let y = -64; y < 320; ++y) {
                for (let z = 0; z < 16; ++z) {
                    for (let x = 0; x < 16; ++x) {
                        bytes[(y + 64) * width * width + (cz * 16 + z) * width + cx * 16 + x] = code(at(x, y, z));
                    }
                }
            }
        }
    }
    writeFileSync(out, bytes);
} else if (mode === 'mix') {
    const count = Number(args[0]);
    const random = new RandomState(settingsForRandom(), 42n).aquiferRandom;
    // A fixed xorshift, so the cells are the same on every run: a quarter
    // small, a quarter mid-range, half out to the world border's cells.
    let s = 0x9E3779B9;
    const next = () => { s ^= s << 13; s ^= s >>> 17; s ^= s << 5; s >>>= 0; return s; };
    const within = (k) => (next() % (2 * k + 1)) - k;
    const score = { wrapped: 0, unwrapped: 0 };
    for (let i = 0; i < count; ++i) {
        const k = i < count / 4 ? 8 : i < count / 2 ? 2000 : 1875000;
        const [x, y, z] = [within(k), within(17) + 10, within(k)];
        const want = random.at(x, y, z).nextLong();
        for (const [name, mix] of Object.entries(mixes)) {
            score[name] += Number(generatorFor(random, mix, x, y, z).nextLong() === want);
        }
    }
    console.log(`cells ${count} wrapped ${score.wrapped} unwrapped ${score.unwrapped}`);
} else if (mode === 'jitter') {
    const [seed, chunks, cy] = args.map(Number);
    const random = new RandomState(settingsForRandom(), BigInt(seed)).aquiferRandom;
    const draws = (g) => [g.nextInt(10), g.nextInt(9), g.nextInt(10)];
    for (let cz = 0; cz < chunks; ++cz) {
        for (let cx = 0; cx < chunks; ++cx) {
            console.log(cx, cz, ...draws(random.at(cx, cy, cz)),
                ...draws(generatorFor(random, mixes.wrapped, cx, cy, cz)));
        }
    }
} else {
    console.error(`unknown mode ${mode}`);
    process.exit(2);
}
