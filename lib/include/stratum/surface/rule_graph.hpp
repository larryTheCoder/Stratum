// Stratum — a dimension's surface rules, resolved.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The chunk filler decides stone, fluid or air. Surface rules decide what the
// stone actually is — grass over dirt at the top, gravel under water, bedrock
// at the floor, deepslate below zero — by walking each column and asking a
// tree of conditions. Measured against the aquifer-free reference, they are
// 17.987% of the blocks in a chunk, so a world without them is bare stone.
//
// WHAT THIS FILE IS, AND IS NOT. It resolves the tree and says, by name, what
// this build cannot yet run. It does not run it. That split is deliberate:
// the STRUCTURE of every one of the fifteen types vanilla uses is documented
// and can be loaded today, while the SEMANTICS of several are not, and SPEC §8
// would rather refuse a rule by name than approximate it into a world that
// generates and is quietly wrong.
//
// The schema is written out here rather than generated from mcdoc, and that
// is a debt rather than a preference (SPEC §11). Five of the fifteen —
// `bandlands`, `above_preliminary_surface`, `hole`, `steep` and `temperature`
// — are not in mcdoc at all, so they would have to be written by hand
// whatever happens, exactly as tools/mcdoc/schema.py already does for
// `blend_alpha` and `end_islands`. The other ten could be generated, and the
// generator cannot reach them yet: it cannot parse either surface-rule mcdoc
// file, and the types they need live in a third file it cannot parse either.
#pragma once

#include <stratum/data/resource_location.hpp>
#include <stratum/rng/xoroshiro128.hpp>
#include <stratum/settings/noise_settings.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace stratum::surface {

/// Raised when a surface rule cannot be resolved. Always names the rule or
/// condition it stopped on.
class RuleError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Where a vertical anchor sits, once the dimension's own height is known.
/// Vanilla writes all three spellings and this build sees all three: 72
/// `absolute`, 10 `above_bottom`, 5 `below_top` across the seven settings.
struct VerticalAnchor {
    enum class Kind : std::uint8_t { Absolute, AboveBottom, BelowTop };

    Kind kind = Kind::Absolute;
    std::int32_t value = 0;

    /// The y this names in a dimension of @p geometry.
    [[nodiscard]] std::int32_t resolve(const settings::NoiseGeometry& geometry) const noexcept;

    [[nodiscard]] bool operator==(const VerticalAnchor& other) const = default;
};

/// Whether a `vertical_gradient` fires at one block.
///
/// The rule is a coin weighted by height: certain at and below one anchor,
/// impossible at and above another, and linear between them, drawn once per
/// block. The probability was measured early and was never the hard part —
///
///     p(y) = (falseAtAndAbove - y) / (falseAtAndAbove - trueAtAndBelow)
///
/// — the random source was, and it took nineteen refuted derivations before
/// the aquifer's own draw showed what shape to look for. It is
/// `rng::positionalSourceFor(seed, randomName).at(x, y, z)`, one `nextFloat`,
/// fired when the draw is BELOW p. Checked against the server on 27 million
/// blocks: 786432 on one band, 21626880 across a 55-band bracket sweep where
/// every band has to follow from the same single draw, and 4718592 more over
/// two world seeds and three names including one outside the `minecraft`
/// namespace. Exact on every one.
///
/// @p source must be the one built for the rule's `random_name`.
[[nodiscard]] bool verticalGradientFires(const rng::PositionalSource& source, std::int32_t x,
                                         std::int32_t y, std::int32_t z,
                                         std::int32_t trueAtAndBelow,
                                         std::int32_t falseAtAndAbove) noexcept;

/// The four things a rule can be.
enum class RuleType : std::uint8_t {
    Sequence,  ///< try each in turn, first result wins
    Condition, ///< run the inner rule where the condition holds
    Block,     ///< place this block
    Bandlands, ///< the mesa banding, which places terracotta of several colours
};

/// The eleven things a condition can be.
enum class ConditionType : std::uint8_t {
    Biome,
    NoiseThreshold,
    Not,
    StoneDepth,
    VerticalGradient,
    Water,
    YAbove,
    AbovePreliminarySurface,
    Hole,
    Steep,
    Temperature,
};

[[nodiscard]] std::string_view ruleTypeName(RuleType type) noexcept;
[[nodiscard]] std::string_view conditionTypeName(ConditionType type) noexcept;

using RuleIndex = std::uint32_t;
using ConditionIndex = std::uint32_t;

struct Rule {
    RuleType type = RuleType::Block;
    /// Sequence.
    std::vector<RuleIndex> sequence;
    /// Condition.
    ConditionIndex condition = 0;
    RuleIndex thenRun = 0;
    /// Block.
    settings::BlockState block;
};

struct Condition {
    ConditionType type = ConditionType::Hole;

    /// Biome. Vanilla writes a list even for one.
    std::vector<data::ResourceLocation> biomes;

    /// NoiseThreshold.
    std::optional<data::ResourceLocation> noise;
    double minThreshold = 0.0;
    double maxThreshold = 0.0;

    /// Not.
    ConditionIndex invert = 0;

    /// StoneDepth.
    std::int32_t offset = 0;
    bool addSurfaceDepth = false;
    std::int32_t secondaryDepthRange = 0;
    /// "floor" or "ceiling", kept as written; the schema has no other values.
    std::string surfaceType;

    /// VerticalGradient.
    std::string randomName;
    VerticalAnchor trueAtAndBelow;
    VerticalAnchor falseAtAndAbove;

    /// Water and YAbove.
    VerticalAnchor anchor;
    std::int32_t surfaceDepthMultiplier = 0;
    bool addStoneDepth = false;
};

/// One dimension's surface rules, as a tree.
class RuleGraph {
public:
    /// Resolves @p json. Throws RuleError, naming the type, for anything the
    /// schema does not define — never a silent skip (SPEC §8).
    [[nodiscard]] static RuleGraph resolve(const nlohmann::json& json,
                                           const data::ResourceLocation& id);

    [[nodiscard]] const Rule& rule(RuleIndex index) const;
    [[nodiscard]] const Condition& condition(ConditionIndex index) const;

    [[nodiscard]] RuleIndex root() const noexcept { return root_; }

    [[nodiscard]] std::size_t ruleCount() const noexcept { return rules_.size(); }

    [[nodiscard]] std::size_t conditionCount() const noexcept { return conditions_.size(); }

    /// The `worldgen/noise` entries this tree names, so a caller can build
    /// them before running anything — the same contract the density graph's
    /// referencedNoises() has.
    [[nodiscard]] std::vector<data::ResourceLocation> referencedNoises() const;

    /// Why this build cannot run @p type, or nothing if it can. Depends only
    /// on the type: what is missing is the semantics, not the context.
    [[nodiscard]] static std::optional<std::string_view>
    unrunnableReason(ConditionType type) noexcept;
    [[nodiscard]] static std::optional<std::string_view> unrunnableReason(RuleType type) noexcept;

    /// Every distinct thing in this tree that cannot be run, by name, sorted.
    /// Empty means the whole tree could be executed.
    [[nodiscard]] std::vector<std::string> unrunnable() const;

private:
    std::vector<Rule> rules_;
    std::vector<Condition> conditions_;
    RuleIndex root_ = 0;
};

} // namespace stratum::surface
