// Stratum — running a dimension's surface rules.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#include <stratum/rng/java_random.hpp>
#include <stratum/surface/executor.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <set>
#include <string>
#include <string_view>

namespace stratum::surface {

namespace {

/// The identifiers the surface depth reads, whatever the tree names.
constexpr std::string_view kSurfaceNoise = "minecraft:surface";
constexpr std::string_view kSurfaceSecondaryNoise = "minecraft:surface_secondary";

/// Does this tree read a surface depth anywhere? Only then are the two noises
/// above required, so a tree that never asks for one still compiles without
/// them.
[[nodiscard]] bool readsSurfaceDepth(const RuleGraph& graph) {
    for (ConditionIndex index = 0; index < graph.conditionCount(); ++index) {
        const Condition& condition = graph.condition(index);
        switch (condition.type) {
            case ConditionType::Hole:
                return true;
            case ConditionType::StoneDepth:
                if (condition.addSurfaceDepth || condition.secondaryDepthRange != 0) {
                    return true;
                }
                break;
            case ConditionType::Water:
            case ConditionType::YAbove:
                if (condition.surfaceDepthMultiplier != 0) {
                    return true;
                }
                break;
            default:
                break;
        }
    }
    return false;
}

/// Does this tree place `bandlands` anywhere? Only then is its table built
/// and its noise required.
[[nodiscard]] bool usesBandlands(const RuleGraph& graph) {
    for (RuleIndex index = 0; index < graph.ruleCount(); ++index) {
        if (graph.rule(index).type == RuleType::Bandlands) {
            return true;
        }
    }
    return false;
}

constexpr std::string_view kClayBandsOffsetNoise = "minecraft:clay_bands_offset";
/// The salt `clay_bands`' own construction RNG is derived with — the same
/// "fork the world seed, salt with this name's MD5" shape every other named
/// object in this project derives from (spec/bandlands-spec.md Q3.2).
constexpr std::string_view kClayBandsSalt = "minecraft:clay_bands";

[[nodiscard]] settings::BlockState clayColor(std::string_view name) {
    return settings::BlockState{.name = data::ResourceLocation{"minecraft", std::string(name)},
                                .properties = {}};
}

/// `bandlands`' table: 192 entries, one fixed random source threaded through
/// five passes in order, each overwriting some of what came before it
/// (spec/bandlands-spec.md Q2). Built once per dimension, never touched
/// again after compile.
[[nodiscard]] std::array<settings::BlockState, kClayBandsSize>
buildClayBands(rng::Xoroshiro128PlusPlus& random) {
    constexpr auto kSize = static_cast<std::int32_t>(kClayBandsSize);
    std::array<settings::BlockState, kClayBandsSize> bands;
    bands.fill(clayColor("terracotta"));

    // Pass (a): a sparse scatter of orange, stepping by draw+2 in total
    // (Q2.2) — but split across two additions, not one. `index0 = 0` reads
    // literally: each iteration writes at `index + draw + 1`, THEN advances
    // one further to the base the next iteration's loop check and draw both
    // see. Collapsing that into a single `index += draw + 2` before writing
    // looks equivalent and is not: it changes which value the loop condition
    // sees at the boundary, so it stops one iteration early or late for
    // seeds where that boundary matters. Caught against real server output,
    // not assumed from the spec's prose — one of three probed seeds agreed
    // with the collapsed form by coincidence, and the other two did not,
    // which is what exposed it (SPEC §11's clean-room provision expects
    // independent golden verification of every claim for exactly this
    // reason).
    {
        std::int32_t index = 0;
        while (index < kSize) {
            index += random.nextInt(5) + 1;
            if (index < kSize) {
                bands[static_cast<std::size_t>(index)] = clayColor("orange_terracotta");
            }
            ++index;
        }
    }

    // Passes (b): three "band" calls sharing one procedure, in this order
    // and with these (base width, colour) pairs (Q2.3).
    struct BandPass {
        std::int32_t baseWidth;
        std::string_view color;
    };

    constexpr std::array<BandPass, 3> kBandPasses{{
        {1, "yellow_terracotta"},
        {2, "brown_terracotta"},
        {1, "red_terracotta"},
    }};
    for (const BandPass& pass : kBandPasses) {
        const std::int32_t runCount = 6 + random.nextInt(10);
        for (std::int32_t run = 0; run < runCount; ++run) {
            const std::int32_t width = pass.baseWidth + random.nextInt(3);
            const std::int32_t start = random.nextInt(kSize);
            // Truncates silently at the array's end rather than wrapping;
            // later runs overwrite earlier ones and earlier passes both.
            for (std::int32_t offset = 0; offset < width; ++offset) {
                const std::int32_t position = start + offset;
                if (position >= kSize) {
                    break;
                }
                bands[static_cast<std::size_t>(position)] = clayColor(pass.color);
            }
        }
    }

    // Pass (c): a final sparse scatter of white, with an independent chance
    // of light grey on each still-in-bounds neighbour — the left guard is
    // `index - 1 > 0`, strictly, not `>= 0`; that asymmetry is vanilla's own,
    // not a mistake to "fix" here (Q2.4).
    //
    // The "fair coin" is bit 0 of the raw draw, NOT this class's own
    // nextBoolean() (bit 63, the sign of the top 32 bits) — measured
    // directly against the real server: bit 0 predicted all 27 reachable
    // coin flips across the two golden tables exactly, bit 63 disagreed on
    // more than a third of them. Whether nextBoolean() itself needs
    // revisiting for OTHER call sites is a separate question this
    // construct's own verification does not settle; nothing else in this
    // codebase has exercised nextBoolean() against a server-verified vector
    // before now.
    {
        const auto fairCoin = [&random]() noexcept { return (random.nextLong() & 1) != 0; };
        const std::int32_t target = 9 + random.nextInt(7);
        std::int32_t placed = 0;
        std::int32_t index = 0;
        while (placed < target && index < kSize) {
            bands[static_cast<std::size_t>(index)] = clayColor("white_terracotta");
            if (index - 1 > 0 && fairCoin()) {
                bands[static_cast<std::size_t>(index) - 1] = clayColor("light_gray_terracotta");
            }
            if (index + 1 < kSize && fairCoin()) {
                bands[static_cast<std::size_t>(index) + 1] = clayColor("light_gray_terracotta");
            }
            ++placed;
            index += random.nextInt(16) + 4;
        }
    }

    return bands;
}

} // namespace

Executor Executor::compile(const RuleGraph& graph, const std::int64_t worldSeed,
                           const settings::NoiseGeometry& geometry,
                           const density::NoiseRegistry* noises, const std::int32_t seaLevel) {
    const std::vector<std::string> blocked = graph.unrunnable();
    if (!blocked.empty()) {
        std::string message = "this build cannot run " + std::to_string(blocked.size()) +
                              " of the constructs in these surface rules:";
        for (const std::string& name : blocked) {
            message += "\n  " + name;
        }
        message += "\nRather than skip them, the whole tree is refused (SPEC §8).";
        throw ExecutionError(message);
    }

    const bool wantsDepth = readsSurfaceDepth(graph);
    const bool wantsBandlands = usesBandlands(graph);
    const bool wantsNoise = !graph.referencedNoises().empty();
    if (noises == nullptr && (wantsDepth || wantsBandlands || wantsNoise)) {
        throw ExecutionError("these surface rules read noise, so they cannot be compiled without "
                             "a noise registry");
    }

    // Seeded from the constant 1234 rather than the world seed, so the field
    // is identical in every world.
    rng::JavaRandom temperatureRandom{1234};
    Executor executor{graph,     geometry, noises,
                      worldSeed, seaLevel, noise::PerlinNoise::fromRandom(temperatureRandom)};
    if (wantsDepth) {
        executor.surface_ = &noises->get(data::ResourceLocation::parse(std::string(kSurfaceNoise)));
        executor.surfaceSecondary_ =
            &noises->get(data::ResourceLocation::parse(std::string(kSurfaceSecondaryNoise)));
    }
    if (wantsBandlands) {
        executor.clayBandsOffset_ =
            &noises->get(data::ResourceLocation::parse(std::string(kClayBandsOffsetNoise)));
        // One fork of the world seed, salted with this name's MD5 — built
        // once here and never touched again after the table is filled
        // (spec/bandlands-spec.md Q3.2).
        rng::Xoroshiro128PlusPlus tableRandom =
            rng::XoroshiroPositionalFactory{worldSeed}.fromHashOf(kClayBandsSalt);
        executor.clayBands_ = buildClayBands(tableRandom);
        executor.clayBandsBuilt_ = true;
    }
    // One source per name, so that a rule fired millions of times pays for no
    // MD5 and no forking.
    for (ConditionIndex index = 0; index < graph.conditionCount(); ++index) {
        const Condition& condition = graph.condition(index);
        if (condition.type != ConditionType::VerticalGradient) {
            continue;
        }
        if (!executor.gradients_.contains(condition.randomName)) {
            executor.gradients_.emplace(condition.randomName,
                                        rng::positionalSourceFor(worldSeed, condition.randomName));
        }
    }
    return executor;
}

std::int32_t Executor::surfaceDepth(const std::int32_t x, const std::int32_t z) const {
    if (surface_ == nullptr) {
        throw ExecutionError("this tree was compiled without a surface depth, so nothing in it "
                             "may ask for one");
    }
    const double field = surface_->sample(static_cast<double>(x), 0.0, static_cast<double>(z));
    // One draw from the UNSALTED positional source, at y = 0 rather than the
    // block's own y.
    rng::Xoroshiro128PlusPlus draw = jitter_.at(x, 0, z);
    const double raw = (2.75 * field) + 3.0 + (0.25 * draw.nextDouble());
    // Truncation toward zero, and NO clamp: `floor` and `max(0, floor)` were
    // both refuted against the server, and the difference is visible because
    // the value goes below zero on roughly one column in 22000.
    return static_cast<std::int32_t>(raw);
}

bool Executor::freezing(const std::int32_t x, const std::int32_t y, const std::int32_t z,
                        const float biomeTemperature) const noexcept {
    const std::int32_t origin = seaLevel_ + 17;
    float value = biomeTemperature;
    if (y > origin) {
        // Float32 and left to right from here down. `* 0.05F / 40.0F` is NOT
        // `* 0.00125F`: the grouping is what was measured, and the folded
        // constant disagrees with the server.
        const auto field =
            static_cast<float>(8.0 * temperature_.sampleSimplex2D(static_cast<double>(x) / 8.0,
                                                                  static_cast<double>(z) / 8.0));
        value -= (field + static_cast<float>(y) - static_cast<float>(origin)) * 0.05F / 40.0F;
    }
    return value < 0.15F;
}

const settings::BlockState* Executor::apply(const Context& at) const {
    return runRule(graph_->root(), at);
}

const settings::BlockState& Executor::clayBandAt(const std::size_t index) const {
    if (!clayBandsBuilt_) {
        throw ExecutionError("this tree was compiled without bandlands, so it has no clay-bands "
                             "table to read");
    }
    if (index >= kClayBandsSize) {
        throw ExecutionError("clay-bands index " + std::to_string(index) + " is outside the " +
                             std::to_string(kClayBandsSize) + "-entry table");
    }
    return clayBands_[index];
}

const settings::BlockState& Executor::bandlandsAt(const std::int32_t x, const std::int32_t y,
                                                  const std::int32_t z) const {
    const double raw =
        clayBandsOffset_->sample(static_cast<double>(x), 0.0, static_cast<double>(z));
    // Round-half-up (Java's Math.round: floor(v + 0.5)) — ties go toward
    // positive infinity, not away from zero (spec/bandlands-spec.md Q4.4).
    const auto offset = static_cast<std::int32_t>(std::floor((raw * 4.0) + 0.5));
    // Vanilla's OWN index arithmetic: one Java '%' after a single '+192', not
    // a safe floorMod (spec/bandlands-spec.md Q4.5). C++'s '%' on int32_t
    // already matches Java's here — both truncate toward zero — so this is
    // deliberately the raw operator, not javamath::floorMod: for a
    // sufficiently negative y this can itself go negative, reproducing
    // vanilla's own reproduced ArrayIndexOutOfBoundsException (Q5.1) rather
    // than silently wrapping it into something vanilla never places.
    const std::int32_t index = (y + offset + 192) % 192;
    if (index < 0 || index >= static_cast<std::int32_t>(kClayBandsSize)) {
        throw ExecutionError(
            "bandlands' index (" + std::to_string(index) + ") is outside its " +
            std::to_string(kClayBandsSize) + "-entry table at y = " + std::to_string(y) +
            " — this is vanilla's own unguarded arithmetic doing this, reproduced rather than "
            "clamped (spec/bandlands-spec.md Q5.1); not reachable inside vanilla's own overworld "
            "height range, but a tall or deep custom dimension can reach it");
    }
    return clayBands_[static_cast<std::size_t>(index)];
}

const settings::BlockState* Executor::runRule(const RuleIndex index, const Context& at) const {
    const Rule& rule = graph_->rule(index);
    switch (rule.type) {
        case RuleType::Block:
            return &rule.block;
        case RuleType::Sequence:
            // First rule that places something wins; the rest are not tried.
            for (const RuleIndex child : rule.sequence) {
                if (const settings::BlockState* placed = runRule(child, at); placed != nullptr) {
                    return placed;
                }
            }
            return nullptr;
        case RuleType::Condition:
            return test(rule.condition, at) ? runRule(rule.thenRun, at) : nullptr;
        case RuleType::Bandlands:
            return &bandlandsAt(at.x, at.y, at.z);
        default:
            // compile() refuses anything else this build cannot run, so
            // reaching one here is a bug in this file rather than a real
            // construct slipping through.
            throw ExecutionError("reached a rule type compile() should have refused");
    }
}

std::int32_t Executor::depthFor(const Condition& condition, const Context& at) const {
    // Only pay for the noise and the draw when a multiplier actually uses it.
    return condition.surfaceDepthMultiplier == 0 ? 0 : surfaceDepth(at.x, at.z);
}

bool Executor::test(const ConditionIndex index, const Context& at) const {
    const Condition& condition = graph_->condition(index);
    switch (condition.type) {
        case ConditionType::Not:
            return !test(condition.invert, at);

        case ConditionType::VerticalGradient: {
            const auto found = gradients_.find(condition.randomName);
            if (found == gradients_.end()) {
                throw ExecutionError("no positional source for random_name '" +
                                     condition.randomName + "'");
            }
            return verticalGradientFires(found->second, at.x, at.y, at.z,
                                         condition.trueAtAndBelow.resolve(*geometry_),
                                         condition.falseAtAndAbove.resolve(*geometry_));
        }

        case ConditionType::YAbove: {
            // `add_stone_depth`, not `add_surface_depth` — the two conditions
            // that read a stone-depth run and the one that reads a surface
            // depth are different fields on Condition for exactly this
            // reason, and this case was reading the wrong one: every
            // `add_stone_depth: true` was silently treated as false. Real
            // impact, not a hypothetical — vanilla's own overworld and Nether
            // trees together set it on ten y_above/water conditions.
            const std::int32_t left = at.y + (condition.addStoneDepth ? at.stoneDepthAbove : 0);
            const std::int32_t right = condition.anchor.resolve(*geometry_) +
                                       (condition.surfaceDepthMultiplier * depthFor(condition, at));
            return left >= right;
        }

        case ConditionType::Water: {
            // No fluid in the column at all makes this unconditionally true —
            // and `sea_level` alone does not create one, only real blocks do.
            if (!at.waterHeight.has_value()) {
                return true;
            }
            // `add_stone_depth`, not `add_surface_depth` — see YAbove above.
            const std::int32_t left = at.y + (condition.addStoneDepth ? at.stoneDepthAbove : 0);
            const std::int32_t right = *at.waterHeight + condition.offset +
                                       (condition.surfaceDepthMultiplier * depthFor(condition, at));
            return left >= right;
        }

        case ConditionType::StoneDepth: {
            // The stored counter is 1 at a run's top; the comparison is
            // against a 0-based depth, hence the -1.
            const bool ceiling = condition.surfaceType == "ceiling";
            const std::int32_t depth = (ceiling ? at.stoneDepthBelow : at.stoneDepthAbove) - 1;
            std::int32_t threshold = condition.offset;
            if (condition.addSurfaceDepth) {
                threshold += surfaceDepth(at.x, at.z);
            }
            if (condition.secondaryDepthRange != 0) {
                const double secondary = surfaceSecondary_->sample(static_cast<double>(at.x), 0.0,
                                                                   static_cast<double>(at.z));
                // From [-1, 1], truncated, no clamp — `round` and the other
                // three mappings were all refuted.
                threshold += static_cast<std::int32_t>(
                    (secondary + 1.0) * 0.5 * static_cast<double>(condition.secondaryDepthRange));
            }
            return depth <= threshold;
        }

        case ConditionType::Hole:
            // A pure function of the column: it never looks at the terrain.
            return surfaceDepth(at.x, at.z) <= 0;

        case ConditionType::Steep:
            // WEST minus EAST, and SOUTH minus NORTH. The asymmetry is real
            // and measured — `abs()` on either axis is refuted by 17375
            // columns that have to stay false — so do not "tidy" it.
            return (at.heightWest - at.heightEast >= 4) || (at.heightSouth - at.heightNorth >= 4);

        case ConditionType::NoiseThreshold: {
            // The resolver requires the name, so an absent one is a bug here
            // rather than bad input — but it is still checked, because the
            // alternative is a dereference that is only usually safe.
            if (!condition.noise.has_value()) {
                throw ExecutionError("a noise_threshold reached the executor without a noise");
            }
            const noise::NormalNoise* sampled = noises_->find(*condition.noise);
            if (sampled == nullptr) {
                throw ExecutionError("no noise '" + condition.noise->toString() +
                                     "' for a noise_threshold");
            }
            // y is the literal 0.0 for every block, so this is constant down a
            // column; x and z are absolute and unquantised. Closed interval.
            const double value =
                sampled->sample(static_cast<double>(at.x), 0.0, static_cast<double>(at.z));
            return condition.minThreshold <= value && value <= condition.maxThreshold;
        }

        case ConditionType::AbovePreliminarySurface:
            // The DOCUMENTED reading. Its strictness has never been separated
            // by measurement: the one probe that reached it found the
            // condition true everywhere, which cannot distinguish >= from >
            // (SPEC §11). Flagged rather than presented as settled.
            return at.y >= at.preliminarySurface;

        case ConditionType::Biome: {
            // The caller supplies the biome; compile() has already refused any
            // tree that names this condition when none can be, so an absent
            // one here is a programming error rather than a wrong world.
            if (!at.biome.has_value()) {
                throw ExecutionError("a biome condition reached the executor without a biome");
            }
            return std::ranges::find(condition.biomes, *at.biome) != condition.biomes.end();
        }

        case ConditionType::Temperature:
            return freezing(at.x, at.y, at.z, at.biomeTemperature);

        default:
            throw ExecutionError("reached a condition type compile() should have refused");
    }
}

} // namespace stratum::surface
