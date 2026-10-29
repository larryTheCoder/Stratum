// Stratum — running a dimension's surface rules.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#include <stratum/rng/java_random.hpp>
#include <stratum/surface/executor.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace stratum::surface {

namespace {

/// The identity the next compiled Executor takes (Executor::id_). Touched once
/// per compile, never while rules run; 0 is never issued.
std::atomic<std::uint64_t> nextExecutorId{1};

/// The identifiers the surface depth reads, whatever the tree names.
constexpr std::string_view kSurfaceNoise = "minecraft:surface";
constexpr std::string_view kSurfaceSecondaryNoise = "minecraft:surface_secondary";

/// `value` as a message prints it: every digit a double carries.
[[nodiscard]] std::string spell(double value) {
    std::ostringstream text;
    text << std::setprecision(17) << value;
    return text.str();
}

/// One step of the rules' 32-bit arithmetic, computed exactly and refused by
/// name where it leaves int32 (SPEC §8, "Inputs that would reach undefined
/// behaviour"). In C++ that overflow is undefined; in Java an int wraps, but
/// what the server's own expression is, and so where it would wrap, is not
/// something this build knows — so it neither wraps nor widens, it refuses.
/// Every schema-valid int can reach here (an anchor, an offset or a sea level
/// near 2^31), and no vanilla pack comes within a factor of 10^6.
[[noreturn]] void refuseNarrowing(const std::int64_t value, const char* what) {
    throw ExecutionError(std::string(what) + " comes to " + std::to_string(value) +
                         ", outside the 32-bit range; this build refuses it rather than "
                         "guess how the server's own arithmetic wraps there (SPEC §8)");
}

/// The check inline, its refusal (refuseNarrowing) out of line: it sits on
/// every surface test's path.
[[nodiscard]] std::int32_t narrowed(const std::int64_t value, const char* what) {
    if (value < std::numeric_limits<std::int32_t>::min() ||
        value > std::numeric_limits<std::int32_t>::max()) [[unlikely]] {
        refuseNarrowing(value, what);
    }
    return static_cast<std::int32_t>(value);
}

[[noreturn]] void refuseTruncation(const double value, const char* what) {
    throw ExecutionError(std::string(what) + " is " + spell(value) +
                         ", which no 32-bit integer holds; this build refuses it rather "
                         "than guess what the server makes of it (SPEC §8)");
}

/// A double that becomes a 32-bit int by truncation, refused where the
/// conversion is undefined: outside (-2^31 - 1, 2^31), or NaN. The check
/// inline, its refusal (refuseTruncation) out of line, as narrowed's.
[[nodiscard]] std::int32_t truncated(const double value, const char* what) {
    if (std::isnan(value) || value <= -2147483649.0 || value >= 2147483648.0) [[unlikely]] {
        refuseTruncation(value, what);
    }
    return static_cast<std::int32_t>(value);
}

/// The surface tests' own refusals, out of line so that the tests they guard
/// stay small enough to inline into the compiled list (Executor::runProgram).
[[noreturn]] void refuseNoSource(const std::string& randomName) {
    throw ExecutionError("no positional source for random_name '" + randomName + "'");
}

[[noreturn]] void refuseUnissuedBiomeKey(const std::uint32_t index) {
    throw ExecutionError("a biome key whose index (" + std::to_string(index) +
                         ") this executor never issued");
}

[[noreturn]] void refuseMissingBiome() {
    throw ExecutionError("a biome condition reached the executor without a biome");
}

[[noreturn]] void refuseMissingNoise() {
    throw ExecutionError("a noise_threshold reached the executor without a noise");
}

/// `noise.sample(x, 0, z)`, refused by name where the noise is not defined
/// (noise::NormalNoise::accepts) — a datapack's `firstOctave` reaching 2^64,
/// or a column far enough out (SPEC §8).
[[nodiscard]] double sampleColumn(const noise::NormalNoise& noise, std::string_view name,
                                  const std::int32_t x, const std::int32_t z) {
    const auto atX = static_cast<double>(x);
    const auto atZ = static_cast<double>(z);
    if (!noise.accepts(atX, 0.0, atZ)) {
        throw ExecutionError("noise '" + std::string(name) + "' cannot be sampled at column (" +
                             std::to_string(x) + ", " + std::to_string(z) + "): past " +
                             spell(noise.inputLimit()) +
                             " its highest-frequency octave's lattice leaves the 32-bit range "
                             "(SPEC §8)");
    }
    return noise.sample(atX, 0.0, atZ);
}

} // namespace

bool readsSurfaceDepth(const RuleGraph& graph) {
    for (ConditionIndex index = 0; index < graph.conditionCount(); ++index) {
        const Condition& condition = graph.condition(index);
        switch (condition.type) {
            case ConditionType::Hole:
            // Measured, not documented anywhere: this condition's boundary
            // carries the column's surface depth (see its own case in
            // `test`), so a tree naming it needs the two noises even when
            // nothing in it mentions a depth.
            case ConditionType::AbovePreliminarySurface:
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

namespace {

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

std::vector<std::string> verticalGradientNames(const RuleGraph& graph) {
    // The `random_name` of every `vertical_gradient` in the tree, sorted and
    // deduplicated. Each one is a SALT into the dimension's declared random
    // source, so each one is a place where a legacy dimension needs a random
    // this build cannot derive — which is why the list is public rather than
    // a detail of Executor::compile: a caller has to know BEFORE compiling,
    // the same way it has to know which noises the tree needs.
    std::set<std::string> unique;
    for (ConditionIndex index = 0; index < graph.conditionCount(); ++index) {
        const Condition& condition = graph.condition(index);
        if (condition.type == ConditionType::VerticalGradient) {
            unique.insert(condition.randomName);
        }
    }
    return {unique.begin(), unique.end()};
}

std::vector<data::ResourceLocation> requiredNoises(const RuleGraph& graph) {
    std::set<data::ResourceLocation> unique;
    for (const data::ResourceLocation& id : graph.referencedNoises()) {
        unique.insert(id);
    }
    if (readsSurfaceDepth(graph)) {
        unique.insert(data::ResourceLocation::parse(std::string(kSurfaceNoise)));
        unique.insert(data::ResourceLocation::parse(std::string(kSurfaceSecondaryNoise)));
    }
    if (usesBandlands(graph)) {
        unique.insert(data::ResourceLocation::parse(std::string(kClayBandsOffsetNoise)));
    }
    return {unique.begin(), unique.end()};
}

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

    // What the conditions compute from constants alone is refused here, by
    // name, before any block is run (SPEC §8): an anchor whose y is not a
    // 32-bit integer, and a sea level the freezing test cannot add 17 to.
    for (ConditionIndex index = 0; index < graph.conditionCount(); ++index) {
        const Condition& condition = graph.condition(index);
        const auto refuseAnchor = [&](const VerticalAnchor& anchor, const char* field) {
            if (!anchor.resolvable(geometry)) {
                throw ExecutionError(
                    "a '" + std::string(conditionTypeName(condition.type)) + "' condition's " +
                    field + " (" + std::to_string(anchor.value) +
                    ") names a y outside the 32-bit range in a dimension from " +
                    std::to_string(geometry.minY) + " of height " +
                    std::to_string(geometry.height) + "; this build refuses it (SPEC §8)");
            }
        };
        if (condition.type == ConditionType::YAbove) {
            refuseAnchor(condition.anchor, "anchor");
        } else if (condition.type == ConditionType::VerticalGradient) {
            refuseAnchor(condition.trueAtAndBelow, "true_at_and_below");
            refuseAnchor(condition.falseAtAndAbove, "false_at_and_above");
        } else if (condition.type == ConditionType::Temperature &&
                   seaLevel > std::numeric_limits<std::int32_t>::max() - kFreezingOriginAbove) {
            throw ExecutionError(
                "a 'minecraft:temperature' condition measures height from sea_level + " +
                std::to_string(kFreezingOriginAbove) + ", and sea_level " +
                std::to_string(seaLevel) +
                " leaves that outside the 32-bit range; this build refuses it (SPEC §8)");
        }
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
    // hashing and no forking — and of the kind the dimension declares. Under
    // `legacy_random_source` the coin is java.util.Random, two forks around
    // the name's String.hashCode: the aquifer lattice's primitive, measured
    // on the server's legacy aquifer worlds, and exact out of sample on the
    // golden Nether's bedrock floor and roof, where the Xoroshiro coin agrees
    // at chance (SPEC §11; vanilla_legacy_gradient_gap_test.cpp). This build
    // refused a legacy gradient by name until the executor ran that coin.
    const bool legacy = noises != nullptr && noises->source() == density::RandomSource::Legacy;
    std::map<std::string, std::size_t> gradientOf;
    // Each noise_threshold's slot in a ColumnMemo: one per distinct name, in
    // the order the conditions first name it.
    std::map<data::ResourceLocation, std::size_t> noiseSlotOf;
    executor.compiled_.assign(graph.conditionCount(), CompiledCondition{});
    for (ConditionIndex index = 0; index < graph.conditionCount(); ++index) {
        const Condition& condition = graph.condition(index);
        CompiledCondition& compiled = executor.compiled_[index];
        switch (condition.type) {
            case ConditionType::VerticalGradient: {
                // Both anchors resolve: the loop at the top refused any that
                // do not.
                compiled.trueAtAndBelow = condition.trueAtAndBelow.resolve(*executor.geometry_);
                compiled.falseAtAndAbove = condition.falseAtAndAbove.resolve(*executor.geometry_);
                const auto [found, added] =
                    gradientOf.emplace(condition.randomName, executor.gradientSources_.size());
                if (added) {
                    if (legacy) {
                        executor.gradientSources_.emplace_back(
                            rng::legacyPositionalSourceFor(worldSeed, condition.randomName));
                    } else {
                        executor.gradientSources_.emplace_back(
                            rng::positionalSourceFor(worldSeed, condition.randomName));
                    }
                }
                compiled.gradient = found->second;
                break;
            }
            case ConditionType::YAbove:
                // Resolves, likewise.
                compiled.anchor = condition.anchor.resolve(*executor.geometry_);
                break;
            case ConditionType::NoiseThreshold:
                if (condition.noise.has_value()) {
                    compiled.noiseSlot =
                        noiseSlotOf.emplace(*condition.noise, noiseSlotOf.size()).first->second;
                }
                break;
            case ConditionType::StoneDepth:
                compiled.ceiling = condition.surfaceType == "ceiling";
                break;
            case ConditionType::Biome:
                compiled.biomeSlot = executor.biomeSlots_++;
                for (const data::ResourceLocation& biome : condition.biomes) {
                    executor.biomeIndex_.emplace(biome, 0U);
                }
                break;
            default:
                break;
        }
    }
    executor.noiseSlots_ = noiseSlotOf.size();
    // biomeKey()'s table (BiomeKey): an index per identifier the conditions
    // list, in the map's order, and one past them for every other
    // identifier. Each answer is the comparison a keyless block makes, made
    // here once per (condition, identifier); for an identifier no condition
    // lists, every condition's comparison is false, which the zero fill is.
    std::uint32_t named = 0;
    for (auto& [biome, index] : executor.biomeIndex_) {
        index = named++;
    }
    executor.biomeKeys_ = named + 1U;
    executor.biomeAnswers_.assign(
        static_cast<std::size_t>(executor.biomeKeys_) * executor.biomeSlots_, char{0});
    for (ConditionIndex index = 0; index < graph.conditionCount(); ++index) {
        const Condition& condition = graph.condition(index);
        if (condition.type != ConditionType::Biome) {
            continue;
        }
        for (const auto& [biome, key] : executor.biomeIndex_) {
            executor.biomeAnswers_[(static_cast<std::size_t>(key) * executor.biomeSlots_) +
                                   executor.compiled_[index].biomeSlot] =
                std::ranges::find(condition.biomes, biome) != condition.biomes.end() ? char{1}
                                                                                     : char{0};
        }
    }
    executor.id_ = nextExecutorId.fetch_add(1, std::memory_order_relaxed);
    executor.rules_.reserve(graph.ruleCount());
    for (RuleIndex index = 0; index < graph.ruleCount(); ++index) {
        executor.rules_.push_back(&graph.rule(index));
    }
    executor.conditions_.reserve(graph.conditionCount());
    for (ConditionIndex index = 0; index < graph.conditionCount(); ++index) {
        executor.conditions_.push_back(&graph.condition(index));
    }
    // The tree as a list (ProgramOp), or walked as before where it cannot be.
    if (executor.lowerRule(graph, graph.root(), 0) && executor.program_.size() < kMaxProgram) {
        executor.program_.push_back(ProgramOp{.code = ProgramOp::Code::Nothing,
                                              .negate = false,
                                              .condition = 0,
                                              .jump = 0,
                                              .block = nullptr,
                                              .tested = nullptr,
                                              .next = 0});
        executor.bandPrograms();
    } else {
        executor.program_.clear();
    }
    return executor;
}

void Executor::bandPrograms() {
    using Code = ProgramOp::Code;
    const auto terminal = [](const ProgramOp& op) {
        return op.code == Code::Place || op.code == Code::Bandlands || op.code == Code::Nothing;
    };
    const std::int64_t minY = geometry_->minY;
    const std::int64_t topY = minY + std::int64_t{geometry_->height};
    constexpr std::uint32_t kNoKey = std::numeric_limits<std::uint32_t>::max();
    // A test's answer at @p y for a block whose biome key is @p key, where
    // those alone decide it (THE Y BANDS, in the header), or nothing where
    // they do not.
    const auto decided = [this](const ProgramOp& op, const std::int64_t y,
                                const std::uint32_t key) -> std::optional<bool> {
        if (op.code == Code::Gradient) {
            const CompiledCondition& compiled = compiled_[op.condition];
            if (compiled.gradient == kNoSource) {
                return std::nullopt;
            }
            // testGradient's own two comparisons, in its order.
            if (y <= compiled.trueAtAndBelow) {
                return true;
            }
            if (y >= compiled.falseAtAndAbove) {
                return false;
            }
            return std::nullopt;
        }
        if (op.code == Code::YAbove && op.tested->surfaceDepthMultiplier == 0 &&
            !op.tested->addStoneDepth) {
            return y >= compiled_[op.condition].anchor;
        }
        if (op.code == Code::Biome && key != kNoKey) {
            // biomeMatches' answer for one of this executor's own keys.
            return biomeAnswers_[(static_cast<std::size_t>(key) * biomeSlots_) +
                                 compiled_[op.condition].biomeSlot] != 0;
        }
        return std::nullopt;
    };

    // The cuts: every y at which one of those answers can change, the
    // dimension's own two ends included.
    std::vector<std::int64_t> cuts{minY, topY};
    bool anyDecided = false;
    bool anyBiome = false;
    for (const ProgramOp& op : program_) {
        if (op.code == Code::Gradient && compiled_[op.condition].gradient != kNoSource) {
            cuts.push_back(std::int64_t{compiled_[op.condition].trueAtAndBelow} + 1);
            cuts.push_back(compiled_[op.condition].falseAtAndAbove);
            anyDecided = true;
        } else if (op.code == Code::YAbove && op.tested->surfaceDepthMultiplier == 0 &&
                   !op.tested->addStoneDepth) {
            cuts.push_back(compiled_[op.condition].anchor);
            anyDecided = true;
        } else if (op.code == Code::Biome) {
            anyBiome = true;
        }
    }
    if ((!anyDecided && !anyBiome) || topY <= minY) {
        // Nothing a y or a key decides: the list as lowered serves every block.
        return;
    }
    std::erase_if(cuts, [&](const std::int64_t cut) { return cut < minY || cut > topY; });
    std::ranges::sort(cuts);
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    const std::size_t bands = cuts.size() - 1;
    if (bands > kMaxBands) {
        return;
    }

    const std::size_t lowered = program_.size();
    std::vector<ProgramOp> banded = program_;
    std::vector<std::size_t> settled(lowered);
    std::vector<char> reached(lowered);
    std::vector<std::uint32_t> renumbered(lowered);
    // Each copy made, as (base, size) in `banded`, by a hash of its ops, so
    // that alike copies are one.
    std::unordered_map<std::uint64_t, std::vector<std::pair<std::uint32_t, std::uint32_t>>> copies;
    // The list's copy for one (band, key) — the band's lowest y and the key
    // standing for all of it — appended to `banded` unless an alike one is
    // there: its entry, or nothing where `banded` would outgrow its bound.
    const auto copyFor = [&](const std::int64_t low,
                             const std::uint32_t key) -> std::optional<std::uint32_t> {
        // Where each op leads once the decided tests are followed: itself,
        // unless it is decided, and then where its answer goes — forward
        // jumps only, so from the end back.
        for (std::size_t i = lowered; i-- > 0;) {
            const ProgramOp& op = program_[i];
            const std::optional<bool> answer = decided(op, low, key);
            settled[i] = answer.has_value() ? settled[*answer != op.negate ? op.next : op.jump] : i;
        }
        // What the copy can reach from its start, in the list's order.
        std::ranges::fill(reached, char{0});
        reached[settled[0]] = 1;
        for (std::size_t i = 0; i < lowered; ++i) {
            const ProgramOp& op = program_[i];
            if (reached[i] == 0 || settled[i] != i || terminal(op)) {
                continue;
            }
            reached[settled[op.next]] = 1;
            reached[settled[op.jump]] = 1;
        }
        std::vector<ProgramOp> copy;
        for (std::size_t i = 0; i < lowered; ++i) {
            if (reached[i] != 0) {
                renumbered[i] = static_cast<std::uint32_t>(copy.size());
                copy.push_back(program_[i]);
            }
        }
        std::uint64_t hash = UINT64_C(14695981039346656037);
        const auto mix = [&hash](const std::uint64_t value) {
            hash = (hash ^ value) * UINT64_C(1099511628211);
        };
        for (std::size_t i = 0, kept = 0; i < lowered; ++i) {
            if (reached[i] == 0) {
                continue;
            }
            ProgramOp& op = copy[kept++];
            if (!terminal(op)) {
                op.next = renumbered[settled[program_[i].next]];
                op.jump = renumbered[settled[program_[i].jump]];
            }
            mix(static_cast<std::uint64_t>(op.code));
            mix(op.negate ? 1U : 0U);
            mix(op.condition);
            mix(op.next);
            mix(op.jump);
            mix(reinterpret_cast<std::uintptr_t>(op.block));
        }
        const std::uint32_t start = renumbered[settled[0]];
        // The same copy as an earlier one, offsets and all, is that one.
        std::vector<std::pair<std::uint32_t, std::uint32_t>>& alike = copies[hash];
        for (const auto& [earlier, size] : alike) {
            if (size == copy.size() &&
                std::equal(copy.begin(), copy.end(), banded.begin() + earlier,
                           [&](const ProgramOp& a, const ProgramOp& b) {
                               ProgramOp moved = a;
                               if (!terminal(moved)) {
                                   moved.next += earlier;
                                   moved.jump += earlier;
                               }
                               return moved == b;
                           })) {
                return earlier + start;
            }
        }
        if (banded.size() + copy.size() > kMaxBandedProgram) {
            return std::nullopt;
        }
        const auto base = static_cast<std::uint32_t>(banded.size());
        for (ProgramOp op : copy) {
            if (!terminal(op)) {
                op.next += base;
                op.jump += base;
            }
            banded.push_back(op);
        }
        alike.emplace_back(base, static_cast<std::uint32_t>(copy.size()));
        return base + start;
    };

    // First a copy per band for a block without one of this executor's
    // keys; then, where the tree tests a biome and the count stays bounded,
    // one per band and key. Keyed copies that would not fit are given up
    // whole, and the bands kept.
    std::vector<std::uint8_t> bandOf(static_cast<std::size_t>(topY - minY));
    std::vector<std::uint32_t> unkeyed(bands);
    for (std::size_t band = 0; band < bands; ++band) {
        for (std::int64_t y = cuts[band]; y < cuts[band + 1]; ++y) {
            bandOf[static_cast<std::size_t>(y - minY)] = static_cast<std::uint8_t>(band);
        }
        const std::optional<std::uint32_t> entry = copyFor(cuts[band], kNoKey);
        if (!entry.has_value()) {
            return;
        }
        unkeyed[band] = *entry;
    }
    std::size_t stride = 1;
    std::vector<std::uint32_t> entries = unkeyed;
    if (anyBiome && bands * (std::size_t{biomeKeys_} + 1U) <= kMaxBandCopies) {
        const std::size_t unkeyedSize = banded.size();
        const auto keyedStride = std::size_t{biomeKeys_} + 1U;
        std::vector<std::uint32_t> keyed(bands * keyedStride);
        bool fits = true;
        for (std::size_t band = 0; band < bands && fits; ++band) {
            keyed[band * keyedStride] = unkeyed[band];
            for (std::uint32_t key = 0; key < biomeKeys_ && fits; ++key) {
                const std::optional<std::uint32_t> entry = copyFor(cuts[band], key);
                fits = entry.has_value();
                keyed[(band * keyedStride) + 1U + key] = entry.value_or(0U);
            }
        }
        if (fits) {
            stride = keyedStride;
            entries = std::move(keyed);
        } else {
            banded.resize(unkeyedSize);
        }
    }
    program_ = std::move(banded);
    bandOf_ = std::move(bandOf);
    bandEntry_ = std::move(entries);
    bandStride_ = stride;
    bandMinY_ = geometry_->minY;
}

bool Executor::lowerRule(const RuleGraph& graph, const RuleIndex index, const std::size_t depth) {
    // Bounded as the walk is: no tree the resolver accepts nests deeper than
    // kMaxRuleDepth, and the list is bounded by kMaxProgram.
    if (depth > kMaxRuleDepth || index >= graph.ruleCount() || program_.size() >= kMaxProgram) {
        return false;
    }
    const Rule& rule = graph.rule(index);
    switch (rule.type) {
        case RuleType::Block:
            program_.push_back(ProgramOp{.code = ProgramOp::Code::Place,
                                         .negate = false,
                                         .condition = 0,
                                         .jump = 0,
                                         .block = &rule.block,
                                         .tested = nullptr,
                                         .next = 0});
            return true;
        case RuleType::Bandlands:
            program_.push_back(ProgramOp{.code = ProgramOp::Code::Bandlands,
                                         .negate = false,
                                         .condition = 0,
                                         .jump = 0,
                                         .block = nullptr,
                                         .tested = nullptr,
                                         .next = 0});
            return true;
        case RuleType::Sequence:
            // Each child in order; one that places leaves the list.
            return std::ranges::all_of(rule.sequence, [&](const RuleIndex child) {
                return lowerRule(graph, child, depth + 1);
            });
        case RuleType::Condition: {
            // The `not`s around the condition, as a parity on the one at
            // their end, which is all the walk evaluates.
            ConditionIndex tested = rule.condition;
            bool negate = false;
            for (std::size_t nots = 0;; ++nots) {
                if (tested >= graph.conditionCount() || nots > kMaxRuleDepth) {
                    return false;
                }
                const Condition& condition = graph.condition(tested);
                if (condition.type != ConditionType::Not) {
                    break;
                }
                negate = !negate;
                tested = condition.invert;
            }
            // The op names the type it tests (ProgramOp), and any type it
            // has no code for goes through test(), as every one once did.
            const auto codeOf = [](const ConditionType type) {
                switch (type) {
                    case ConditionType::VerticalGradient:
                        return ProgramOp::Code::Gradient;
                    case ConditionType::YAbove:
                        return ProgramOp::Code::YAbove;
                    case ConditionType::Water:
                        return ProgramOp::Code::Water;
                    case ConditionType::StoneDepth:
                        return ProgramOp::Code::StoneDepth;
                    case ConditionType::Hole:
                        return ProgramOp::Code::Hole;
                    case ConditionType::Steep:
                        return ProgramOp::Code::Steep;
                    case ConditionType::NoiseThreshold:
                        return ProgramOp::Code::Noise;
                    case ConditionType::AbovePreliminarySurface:
                        return ProgramOp::Code::AbovePreliminarySurface;
                    case ConditionType::Biome:
                        return ProgramOp::Code::Biome;
                    case ConditionType::Temperature:
                        return ProgramOp::Code::Temperature;
                    default:
                        return ProgramOp::Code::Test;
                }
            };
            const std::size_t test = program_.size();
            const Condition& condition = graph.condition(tested);
            program_.push_back(ProgramOp{.code = codeOf(condition.type),
                                         .negate = negate,
                                         .condition = tested,
                                         .jump = 0,
                                         .block = nullptr,
                                         .tested = &condition,
                                         .next = static_cast<std::uint32_t>(test + 1)});
            if (!lowerRule(graph, rule.thenRun, depth + 1) || program_.size() >= kMaxProgram) {
                return false;
            }
            // A failed test goes past the rule's code, to what follows the
            // condition, as the walk's nothing does.
            program_[test].jump = static_cast<std::uint32_t>(program_.size());
            return true;
        }
        default:
            // The walk refuses it where it reaches it; so the walk it is.
            return false;
    }
}

const settings::BlockState* Executor::runProgram(const Context& at, ColumnMemo& memo) const {
    const ProgramOp* const ops = program_.data();
    // Every typed op's condition is in range (lowerRule took it from the
    // graph), and compiled_ holds one entry per condition.
    const CompiledCondition* const compiled = compiled_.data();
    // A block of the dimension starts in its band's copy (bandPrograms);
    // any other y in the list as lowered.
    std::size_t pc = 0;
    if (!bandOf_.empty()) {
        const std::int64_t offset = std::int64_t{at.y} - bandMinY_;
        if (offset >= 0 && offset < static_cast<std::int64_t>(bandOf_.size())) {
            // A key this executor issued picks its keyed copy, where there
            // are keyed copies; any other block, the band's own.
            std::size_t slot = 0;
            if (bandStride_ > 1 && at.biomeKey.executor == id_ && at.biomeKey.index < biomeKeys_) {
                slot = std::size_t{at.biomeKey.index} + 1U;
            }
            pc = bandEntry_[(std::size_t{bandOf_[static_cast<std::size_t>(offset)]} * bandStride_) +
                            slot];
        }
    }
    while (true) {
        const ProgramOp& op = ops[pc];
        bool passed = false;
        switch (op.code) {
            case ProgramOp::Code::Place:
                return op.block;
            case ProgramOp::Code::Bandlands:
                return &bandlandsAt(at, memo);
            case ProgramOp::Code::Nothing:
                return nullptr;
            case ProgramOp::Code::Test:
                passed = test(op.condition, at, memo);
                break;
            case ProgramOp::Code::Gradient:
                passed = testGradient(*op.tested, compiled[op.condition], at);
                break;
            case ProgramOp::Code::YAbove:
                passed = testYAbove(*op.tested, compiled[op.condition], at, memo);
                break;
            case ProgramOp::Code::Water:
                passed = testWater(*op.tested, at, memo);
                break;
            case ProgramOp::Code::StoneDepth:
                passed = testStoneDepth(*op.tested, compiled[op.condition], at, memo);
                break;
            case ProgramOp::Code::Hole:
                passed = testHole(at, memo);
                break;
            case ProgramOp::Code::Steep:
                passed = testSteep(at);
                break;
            case ProgramOp::Code::Noise:
                passed = testNoise(*op.tested, compiled[op.condition], at, memo);
                break;
            case ProgramOp::Code::AbovePreliminarySurface:
                passed = testAbovePreliminarySurface(at, memo);
                break;
            case ProgramOp::Code::Biome:
                passed = biomeMatches(*op.tested, compiled[op.condition].biomeSlot, at);
                break;
            case ProgramOp::Code::Temperature:
                passed = freezing(at, memo);
                break;
        }
        pc = passed != op.negate ? op.next : op.jump;
    }
}

double Executor::surfaceDepthRaw(const std::int32_t x, const std::int32_t z) const {
    if (surface_ == nullptr) {
        throw ExecutionError("this tree was compiled without a surface depth, so nothing in it "
                             "may ask for one");
    }
    const double field = sampleColumn(*surface_, kSurfaceNoise, x, z);
    // One draw from the UNSALTED positional source, at y = 0 rather than the
    // block's own y.
    rng::Xoroshiro128PlusPlus draw = jitter_.at(x, 0, z);
    return (2.75 * field) + 3.0 + (0.25 * draw.nextDouble());
}

std::int32_t Executor::surfaceDepth(const std::int32_t x, const std::int32_t z) const {
    // Truncation toward zero, and NO BOTTOM CLAMP AT 0: `floor`,
    // `max(0, floor)` and `max(0, surfaceDepth)` were all refuted against the
    // server. The last of those was open for two milestones because the
    // golden regions contain no column where it would show — 0 of 2097152 —
    // and a negative depth is rare rather than impossible: 98 columns in
    // 8589934592 over the eight golden seeds, 1 in 87652393.
    // `aps-clamp-probe.sh` reads three of those clusters at three seeds with
    // nothing between the condition and the readout, and the band's lower
    // edge is `psl + surfaceDepth - 8` on all 49 separating columns. Those 49
    // raws all lie in (-1.14, -1.00), so that one reading separates THREE
    // candidates at once — `floor` would put the edge at `psl - 10`, this
    // cast at `psl - 9`, a clamp at 0 at `psl - 8` — and it is the only
    // evidence anywhere that tells this cast from `floor`, since above -1
    // they are the same function.
    //
    // AT 0, and no further. All 49 separating columns are at depth exactly
    // -1, and the lowest raw in the whole 8589934592-column sweep is
    // -1.134416806, so a depth of -2 or below has never been seen. A clamp at
    // -1 or lower would agree with this line on every column ever read and is
    // NOT refuted by it — it is simply not implemented, because nothing
    // documents one. Said in the header, measured in the conformance harness.
    //
    // Refused, by name, where no int holds it — a datapack's `minecraft:surface`
    // amplitudes in the trillions — rather than converted undefined (SPEC §8).
    return truncated(surfaceDepthRaw(x, z), "the surface depth (minecraft:surface)");
}

bool Executor::freezing(const std::int32_t x, const std::int32_t y, const std::int32_t z,
                        const float biomeTemperature) const noexcept {
    // Formed in 64 bits, so that this public method is defined for every sea
    // level an executor can hold (SPEC §5.7): in 32 it overflowed for one
    // within 17 of INT32_MAX. compile() refuses such a sea level for any tree
    // that reaches this test — Java's int sum would wrap there, and nothing
    // has measured what the server does — so the exact reading here, in which
    // no y lies above an origin past INT32_MAX, is never what a pack
    // generates. Wherever the sum fits an int, the comparison and the float
    // conversion below are exactly the 32-bit ones.
    const std::int64_t origin = std::int64_t{seaLevel_} + kFreezingOriginAbove;
    float value = biomeTemperature;
    if (y > origin) {
        // Float32 and left to right from here down. `* 0.05F / 40.0F` is NOT
        // `* 0.00125F`: the grouping is what was measured, and the folded
        // constant disagrees with the server.
        const float field = freezingField(x, z);
        value -= (field + static_cast<float>(y) - static_cast<float>(origin)) * 0.05F / 40.0F;
    }
    return value < 0.15F;
}

float Executor::freezingField(const std::int32_t x, const std::int32_t z) const noexcept {
    return static_cast<float>(8.0 * temperature_.sampleSimplex2D(static_cast<double>(x) / 8.0,
                                                                 static_cast<double>(z) / 8.0));
}

bool Executor::freezing(const Context& at, ColumnMemo& memo) const noexcept {
    // freezing(x, y, z, temperature) step for step, with the field, a
    // function of the column alone, kept in the memo.
    const std::int64_t origin = std::int64_t{seaLevel_} + kFreezingOriginAbove;
    float value = at.biomeTemperature;
    if (at.y > origin) {
        if (!memo.hasFreezingField_) {
            memo.freezingField_ = freezingField(at.x, at.z);
            memo.hasFreezingField_ = true;
        }
        const float field = memo.freezingField_;
        value -= (field + static_cast<float>(at.y) - static_cast<float>(origin)) * 0.05F / 40.0F;
    }
    return value < 0.15F;
}

const settings::BlockState* Executor::apply(const Context& at) const {
    ColumnMemo memo;
    return apply(at, memo);
}

const settings::BlockState* Executor::apply(const Context& at, ColumnMemo& memo) const {
    memo.bind(id_, at.x, at.z);
    return program_.empty() ? runRule(graph_->root(), at, memo) : runProgram(at, memo);
}

BiomeKey Executor::biomeKey(const data::ResourceLocation& biome) const {
    const auto found = biomeIndex_.find(biome);
    return BiomeKey{.executor = id_,
                    .index = found != biomeIndex_.end() ? found->second : biomeKeys_ - 1U};
}

inline bool Executor::biomeMatches(const Condition& condition, const std::size_t biomeSlot,
                                   const Context& at) const {
    if (at.biomeKey.executor == id_) {
        // One of this executor's own keys: the answer compile() worked out
        // for its biome. Only biomeKey() issues them, and only in range; a
        // key altered by hand past the table is refused rather than read.
        if (at.biomeKey.index >= biomeKeys_) [[unlikely]] {
            refuseUnissuedBiomeKey(at.biomeKey.index);
        }
        return biomeAnswers_[(static_cast<std::size_t>(at.biomeKey.index) * biomeSlots_) +
                             biomeSlot] != 0;
    }
    // The caller supplies the biome; compile() has already refused any tree
    // that names this condition when none can be, so an absent one here is a
    // programming error rather than a wrong world.
    if (!at.biome.has_value()) {
        refuseMissingBiome();
    }
    return std::ranges::find(condition.biomes, *at.biome) != condition.biomes.end();
}

std::int32_t Executor::columnSurfaceDepth(const Context& at, ColumnMemo& memo) const {
    if (!memo.hasDepth_) {
        memo.depth_ = surfaceDepth(at.x, at.z);
        memo.hasDepth_ = true;
    }
    return memo.depth_;
}

double Executor::columnSecondary(const Context& at, ColumnMemo& memo) const {
    if (!memo.hasSecondary_) {
        memo.secondary_ = sampleColumn(*surfaceSecondary_, kSurfaceSecondaryNoise, at.x, at.z);
        memo.hasSecondary_ = true;
    }
    return memo.secondary_;
}

double Executor::columnNoise(const data::ResourceLocation& name, const std::size_t slot,
                             const Context& at, ColumnMemo& memo) const {
    if (memo.hasNoise_.size() < noiseSlots_) {
        memo.noise_.resize(noiseSlots_, 0.0);
        memo.hasNoise_.resize(noiseSlots_, char{0});
    }
    if (memo.hasNoise_[slot] == 0) {
        // The same lookup and the same refusals, in the same order, as the
        // uncached read always made: the condition's noise found in the
        // registry by name, then sampled at the column.
        const noise::NormalNoise* sampled = noises_->find(name);
        if (sampled == nullptr) {
            throw ExecutionError("no noise '" + name.toString() + "' for a noise_threshold");
        }
        memo.noise_[slot] = sampleColumn(*sampled, name.toString(), at.x, at.z);
        memo.hasNoise_[slot] = 1;
    }
    return memo.noise_[slot];
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

namespace {

/// `bandlands`' per-column shift, from its noise's raw sample at the column.
[[nodiscard]] std::int32_t bandOffset(const double raw) {
    // Round-half-up (Java's Math.round: floor(v + 0.5)) — ties go toward
    // positive infinity, not away from zero (spec/bandlands-spec.md Q4.4).
    // Refused where no int holds it (SPEC §8): a datapack's amplitudes can
    // put it there, and what the server makes of such an offset is
    // unmeasured.
    return truncated(std::floor((raw * 4.0) + 0.5),
                     "bandlands' offset (minecraft:clay_bands_offset)");
}

} // namespace

const settings::BlockState& Executor::bandlandsAt(const Context& at, ColumnMemo& memo) const {
    if (!memo.hasBandOffset_) {
        memo.bandOffset_ =
            bandOffset(sampleColumn(*clayBandsOffset_, kClayBandsOffsetNoise, at.x, at.z));
        memo.hasBandOffset_ = true;
    }
    return bandAt(at.y, memo.bandOffset_);
}

const settings::BlockState& Executor::bandlandsAt(const std::int32_t x, const std::int32_t y,
                                                  const std::int32_t z) const {
    return bandAt(y, bandOffset(sampleColumn(*clayBandsOffset_, kClayBandsOffsetNoise, x, z)));
}

const settings::BlockState& Executor::bandAt(const std::int32_t y,
                                             const std::int32_t offset) const {
    // Vanilla's OWN index arithmetic: one Java '%' after a single '+192', not
    // a safe floorMod (spec/bandlands-spec.md Q4.5). C++'s '%' on int32_t
    // already matches Java's here — both truncate toward zero — so this is
    // deliberately the raw operator, not javamath::floorMod: for a
    // sufficiently negative y this can itself go negative, reproducing
    // vanilla's own reproduced ArrayIndexOutOfBoundsException (Q5.1) rather
    // than silently wrapping it into something vanilla never places.
    const std::int32_t index =
        narrowed(std::int64_t{narrowed(std::int64_t{y} + offset, "bandlands' y plus its offset")} +
                     192,
                 "bandlands' y plus its offset plus 192") %
        192;
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

const settings::BlockState* Executor::runRule(const RuleIndex index, const Context& at,
                                              ColumnMemo& memo) const {
    const Rule& rule = ruleAt(index);
    switch (rule.type) {
        case RuleType::Block:
            return &rule.block;
        case RuleType::Sequence:
            // First rule that places something wins; the rest are not tried.
            for (const RuleIndex child : rule.sequence) {
                if (const settings::BlockState* placed = runRule(child, at, memo);
                    placed != nullptr) {
                    return placed;
                }
            }
            return nullptr;
        case RuleType::Condition:
            return test(rule.condition, at, memo) ? runRule(rule.thenRun, at, memo) : nullptr;
        case RuleType::Bandlands:
            return &bandlandsAt(at, memo);
        default:
            // compile() refuses anything else this build cannot run, so
            // reaching one here is a bug in this file rather than a real
            // construct slipping through.
            throw ExecutionError("reached a rule type compile() should have refused");
    }
}

std::int32_t Executor::depthFor(const Condition& condition, const Context& at,
                                ColumnMemo& memo) const {
    // Only pay for the noise and the draw when a multiplier actually uses it.
    return condition.surfaceDepthMultiplier == 0 ? 0 : columnSurfaceDepth(at, memo);
}

inline bool Executor::testGradient(const Condition& condition, const CompiledCondition& compiled,
                                   const Context& at) const {
    // The source and both anchors, found and resolved at compile.
    if (compiled.gradient == kNoSource) [[unlikely]] {
        refuseNoSource(condition.randomName);
    }
    // verticalGradientFires' own two certain bands, answered before its call
    // by the same two comparisons in the same order: only a block strictly
    // between the anchors pays for the call, the source's variant and the
    // draw, and that one is answered by verticalGradientFires as before.
    if (at.y <= compiled.trueAtAndBelow) {
        return true;
    }
    if (at.y >= compiled.falseAtAndAbove) {
        return false;
    }
    return std::visit(
        [&](const auto& source) {
            return verticalGradientFires(source, at.x, at.y, at.z, compiled.trueAtAndBelow,
                                         compiled.falseAtAndAbove);
        },
        gradientSources_[compiled.gradient]);
}

inline bool Executor::testYAbove(const Condition& condition, const CompiledCondition& compiled,
                                 const Context& at, ColumnMemo& memo) const {
    // `add_stone_depth`, not `add_surface_depth` — the two conditions that
    // read a stone-depth run and the one that reads a surface depth are
    // different fields on Condition for exactly this reason, and this case
    // was reading the wrong one: every `add_stone_depth: true` was silently
    // treated as false. Real impact, not a hypothetical — vanilla's own
    // overworld and Nether trees together set it on ten y_above/water
    // conditions.
    //
    // Every step checked (`narrowed`): an anchor, a multiplier or a depth
    // near the int range would otherwise overflow (SPEC §8).
    const std::int32_t left =
        narrowed(std::int64_t{at.y} + (condition.addStoneDepth ? at.stoneDepthAbove : 0),
                 "y_above's y plus the stone depth");
    const std::int32_t scaled =
        narrowed(std::int64_t{condition.surfaceDepthMultiplier} * depthFor(condition, at, memo),
                 "y_above's surface_depth_multiplier times the surface depth");
    const std::int32_t right = narrowed(std::int64_t{compiled.anchor} + scaled,
                                        "y_above's anchor plus its scaled surface depth");
    return left >= right;
}

inline bool Executor::testWater(const Condition& condition, const Context& at,
                                ColumnMemo& memo) const {
    // No fluid in the column at all makes this unconditionally true — and
    // `sea_level` alone does not create one, only real blocks do.
    if (!at.waterHeight.has_value()) {
        return true;
    }
    // `add_stone_depth`, not `add_surface_depth` — see testYAbove.
    const std::int32_t left =
        narrowed(std::int64_t{at.y} + (condition.addStoneDepth ? at.stoneDepthAbove : 0),
                 "water's y plus the stone depth");
    const std::int32_t raised = narrowed(std::int64_t{*at.waterHeight} + condition.offset,
                                         "water's water height plus its offset");
    const std::int32_t scaled =
        narrowed(std::int64_t{condition.surfaceDepthMultiplier} * depthFor(condition, at, memo),
                 "water's surface_depth_multiplier times the surface depth");
    const std::int32_t right =
        narrowed(std::int64_t{raised} + scaled, "water's level plus its scaled surface depth");
    return left >= right;
}

inline bool Executor::testStoneDepth(const Condition& condition, const CompiledCondition& compiled,
                                     const Context& at, ColumnMemo& memo) const {
    const std::int32_t run = compiled.ceiling ? at.stoneDepthBelow : at.stoneDepthAbove;
    // 0 means the position is not in a solid run AT ALL — reset by air, per
    // Context's own doc — and "N blocks deep in stone" cannot be true where
    // there is no stone to be deep in. Without this guard the
    // stored-counter-minus-one below reads that as depth -1, which trivially
    // satisfies almost any non-negative offset: measured against a real
    // probe world, this painted grass and dirt across the entire sky above
    // the terrain and deepslate over open water, 292005 of 393216 blocks in
    // four chunks (golden_fill_test.cpp).
    if (run <= 0) {
        return false;
    }
    // The stored counter is 1 at a run's top; the comparison is against a
    // 0-based depth, hence the -1.
    const std::int32_t depth = run - 1;
    std::int32_t threshold = condition.offset;
    if (condition.addSurfaceDepth) {
        threshold = narrowed(std::int64_t{threshold} + columnSurfaceDepth(at, memo),
                             "stone_depth's offset plus the surface depth");
    }
    if (condition.secondaryDepthRange != 0) {
        const double secondary = columnSecondary(at, memo);
        // From [-1, 1], truncated, no clamp — `round` and the other three
        // mappings were all refuted. Refused where no int holds it (a
        // datapack's amplitudes), never converted undefined.
        const std::int32_t secondaryDepth =
            truncated((secondary + 1.0) * 0.5 * static_cast<double>(condition.secondaryDepthRange),
                      "stone_depth's secondary depth (minecraft:surface_secondary)");
        threshold = narrowed(std::int64_t{threshold} + secondaryDepth,
                             "stone_depth's threshold plus its secondary depth");
    }
    return depth <= threshold;
}

inline bool Executor::testHole(const Context& at, ColumnMemo& memo) const {
    // A pure function of the column: it never looks at the terrain.
    return columnSurfaceDepth(at, memo) <= 0;
}

inline bool Executor::testSteep(const Context& at) noexcept {
    // WEST minus EAST, and SOUTH minus NORTH. The asymmetry is real and
    // measured — `abs()` on either axis is refuted by 17375 columns that have
    // to stay false — so do not "tidy" it.
    return (at.heightWest - at.heightEast >= 4) || (at.heightSouth - at.heightNorth >= 4);
}

inline bool Executor::testNoise(const Condition& condition, const CompiledCondition& compiled,
                                const Context& at, ColumnMemo& memo) const {
    // The resolver requires the name, so an absent one is a bug here rather
    // than bad input — but it is still checked, because the alternative is a
    // dereference that is only usually safe.
    if (!condition.noise.has_value()) [[unlikely]] {
        refuseMissingNoise();
    }
    // y is the literal 0.0 for every block, so this is constant down a
    // column, and kept in the memo; x and z are absolute and unquantised.
    // Closed interval.
    const double value = columnNoise(*condition.noise, compiled.noiseSlot, at, memo);
    return condition.minThreshold <= value && value <= condition.maxThreshold;
}

inline bool Executor::testAbovePreliminarySurface(const Context& at, ColumnMemo& memo) const {
    // MEASURED, and not what the name says. The boundary is not the
    // preliminary surface: the condition opens `8 - surfaceDepth` blocks
    // BELOW it — two to eight blocks on ordinary terrain — and the 8 is a
    // literal, invariant under cell height, cell width, min_y, height and sea
    // level (SPEC §11).
    //
    // The strictness this project called unmeasured for two milestones was
    // never the open question. The boundary is true at exactly this y and
    // false one block under it on every column measured, so `>= C` and
    // `> C - 1` are the same predicate; what was unknown was C, and C is not
    // `preliminarySurface`.
    return at.y >= narrowed(std::int64_t{narrowed(std::int64_t{at.preliminarySurface} +
                                                      columnSurfaceDepth(at, memo),
                                                  "the preliminary surface plus the surface "
                                                  "depth")} -
                                8,
                            "the preliminary surface plus the surface depth, less 8");
}

bool Executor::test(const ConditionIndex index, const Context& at, ColumnMemo& memo) const {
    const Condition& condition = conditionAt(index);
    switch (condition.type) {
        case ConditionType::Not:
            return !test(condition.invert, at, memo);
        case ConditionType::VerticalGradient:
            return testGradient(condition, compiled_[index], at);
        case ConditionType::YAbove:
            return testYAbove(condition, compiled_[index], at, memo);
        case ConditionType::Water:
            return testWater(condition, at, memo);
        case ConditionType::StoneDepth:
            return testStoneDepth(condition, compiled_[index], at, memo);
        case ConditionType::Hole:
            return testHole(at, memo);
        case ConditionType::Steep:
            return testSteep(at);
        case ConditionType::NoiseThreshold:
            return testNoise(condition, compiled_[index], at, memo);
        case ConditionType::AbovePreliminarySurface:
            return testAbovePreliminarySurface(at, memo);
        case ConditionType::Biome:
            return biomeMatches(condition, compiled_[index].biomeSlot, at);
        case ConditionType::Temperature:
            return freezing(at, memo);
        default:
            throw ExecutionError("reached a condition type compile() should have refused");
    }
}

} // namespace stratum::surface
