// Stratum — running a dimension's surface rules.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#include <stratum/surface/executor.hpp>

#include <set>
#include <string>

namespace stratum::surface {

Executor Executor::compile(const RuleGraph& graph, const std::int64_t worldSeed,
                           const settings::NoiseGeometry& geometry) {
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

    Executor executor{graph, geometry};
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

        case ConditionType::AbovePreliminarySurface:
            // The DOCUMENTED reading. Its strictness has never been separated
            // by measurement: the one probe that reached it found the
            // condition true everywhere, which cannot distinguish >= from >
            // (SPEC §11). Flagged rather than presented as settled.
            return at.y >= at.preliminarySurface;

        case ConditionType::Biome:
        case ConditionType::Temperature:
        case ConditionType::Hole:
        case ConditionType::NoiseThreshold:
        case ConditionType::Steep:
        case ConditionType::StoneDepth:
        case ConditionType::Water:
        case ConditionType::YAbove:
        default:
            throw ExecutionError("reached a condition type compile() should have refused");
    }
}

} // namespace stratum::surface
