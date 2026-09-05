// Stratum — running a dimension's surface rules.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#include <stratum/surface/executor.hpp>

#include <cmath>
#include <set>
#include <string>

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

} // namespace

Executor Executor::compile(const RuleGraph& graph, const std::int64_t worldSeed,
                           const settings::NoiseGeometry& geometry,
                           const density::NoiseRegistry* noises) {
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
    const bool wantsNoise = !graph.referencedNoises().empty();
    if (noises == nullptr && (wantsDepth || wantsNoise)) {
        throw ExecutionError("these surface rules read noise, so they cannot be compiled without "
                             "a noise registry");
    }

    Executor executor{graph, geometry, noises, worldSeed};
    if (wantsDepth) {
        executor.surface_ = &noises->get(data::ResourceLocation::parse(std::string(kSurfaceNoise)));
        executor.surfaceSecondary_ =
            &noises->get(data::ResourceLocation::parse(std::string(kSurfaceSecondaryNoise)));
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

const settings::BlockState* Executor::apply(const Context& at) const {
    return runRule(graph_->root(), at);
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
        default:
            // compile() refuses these, so reaching one is a bug in this file
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
            const std::int32_t left = at.y + (condition.addSurfaceDepth ? at.stoneDepthAbove : 0);
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
            const std::int32_t left = at.y + (condition.addSurfaceDepth ? at.stoneDepthAbove : 0);
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

        case ConditionType::Biome:
        case ConditionType::Temperature:
        default:
            throw ExecutionError("reached a condition type compile() should have refused");
    }
}

} // namespace stratum::surface
