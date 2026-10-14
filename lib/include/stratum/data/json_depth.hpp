// Stratum — how deep a JSON value nests, measured without recursion.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// nlohmann's parser builds a value of any depth without recursing, and its
// destructor takes one apart the same way; but copying a value, printing it
// with dump() and comparing two with == all recurse once per level, and so
// does every walk this build makes over what it reads. Twenty thousand levels
// of `not` in a surface rule ran the stack out in the copy that keeps the rule
// in its noise settings, before any of this build's own code saw it. A value
// that deep is refused by name where it is read instead (SPEC §5.7, §8,
// "Inputs that would reach undefined behaviour"): every pack file
// (data::Pack), and the surface rules and spawn targets a frozen pipeline
// stores as text (freeze::read).

#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace stratum::data {

/// The deepest JSON this build reads, in arrays and objects (jsonDepth).
///
/// Sized by the stack, which is what it protects. A copy, a dump() and an ==
/// of 128 levels of objects, one after another, ran in 448 KiB of stack in
/// the sanitize preset's build (Clang, unoptimised, UBSan, where every local
/// has a slot of its own), 160 KiB in GCC's unoptimised build and 48 KiB in
/// its optimised one: inside the 1 MiB a Windows main thread or a JVM thread
/// gets by default. The deepest file in vanilla's 1.21.11 worldgen is 23 —
/// the overworld noise settings, whose surface rules nest furthest — so this
/// is five and a half times that. A density function nested deeper than one
/// file allows is written across files, as references (density::kMaxDepth
/// bounds the graph they make). What the server does with JSON past it is
/// unmeasured.
inline constexpr std::size_t kMaxJsonDepth = 128;

/// How many arrays and objects deep @p value nests: 0 for a number, string,
/// boolean or null, 1 for `[]`, `{}` or `{"a": 1}`, 2 for `{"a": []}`. Walked
/// with a stack of its own rather than the C++ one, so a value of any depth is
/// measured rather than overflowing the thing measuring it.
[[nodiscard]] inline std::size_t jsonDepth(const nlohmann::json& value) {
    std::size_t deepest = 0;
    std::vector<std::pair<const nlohmann::json*, std::size_t>> pending{{&value, 0}};
    while (!pending.empty()) {
        const auto [at, above] = pending.back();
        pending.pop_back();
        if (!at->is_structured()) {
            continue;
        }
        const std::size_t depth = above + 1;
        deepest = std::max(deepest, depth);
        // An object iterates its values, an array its elements.
        for (const nlohmann::json& child : *at) {
            pending.emplace_back(&child, depth);
        }
    }
    return deepest;
}

} // namespace stratum::data
