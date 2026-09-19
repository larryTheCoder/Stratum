// Stratum — which legacy dimensions name which noises, per router entry.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// THE UNIVERSAL THE REFUSAL RESTS ON. `NoiseRegistry::create` no longer
// refuses every `legacy_random_source` dimension; it refuses one that asks
// for a NAMED noise, because that — and only that — is what the unsolved
// Java-LCG name-to-seed derivation blocks (SPEC §11). Whether that narrowing
// is worth anything is a claim about vanilla's own data: it is worth a whole
// dimension if some legacy dimension names nothing, and worth nothing if they
// all do. So the claim is asserted here rather than assumed, from the pinned
// pack, by walking the JSON — and it is a regression guard as much as a
// measurement: if a future pack gives the End's router a named noise, this
// fails by name and by entry rather than the End quietly generating with a
// noise nobody seeded.
//
// TWO WALKS, BECAUSE THE FIRST OBVIOUS ONE IS WRONG. The round that opened
// this work was handed a table built by scanning each noise_settings document
// for `"noise": "<id>"` fields. That table says the Nether, caves and
// floating_islands name exactly `temperature` and `vegetation` in their
// routers. It is an UNDERCOUNT, in two independent ways, and this file exists
// partly to pin the correction:
//
//   * `shift`, `shift_a` and `shift_b` spell their noise field "argument",
//     not "noise" — mcdoc's own name for it — so a walk keyed on "noise"
//     cannot see them;
//   * a router entry is mostly REFERENCES. The Nether's `temperature` is a
//     `shifted_noise` whose shift_x is the string "minecraft:shift_x", and
//     that entry — in worldgen/density_function/, a different file — is what
//     carries `shift_a` over `minecraft:offset`.
//
// So the real answer for those three dimensions is THREE names, not two:
// `minecraft:offset` as well. It changes nothing about the narrowing (three
// is as non-empty as two, and the End still names zero) and everything about
// what the refusal's message should say.
//
// walkDocument() below is the corrected document walk, kept deliberately
// independent of the resolved graph: its own hand-written table of which node
// type carries its noise under which field name, its own reference
// resolution. noisesReachableFrom() is the engine's answer, computed by
// reachability over an already-resolved graph. The two agreeing is worth
// having; the second agreeing with itself would not be.
//
// The numbers below are measured from 1.21.11's own worldgen tree.
#include <stratum/data/pack.hpp>
#include <stratum/data/registry.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/graph.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/executor.hpp>
#include <stratum/surface/rule_graph.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

[[nodiscard]] std::filesystem::path worldgenTree() {
    return std::filesystem::path{STRATUM_FIXTURES_DIR} / "1.21.11" / "worldgen";
}

/// Which density function types carry a `worldgen/noise` name, and under
/// which field. Written out here rather than read from
/// `stratum::density::fieldsOf`, on purpose: this walk is the independent
/// check on the engine's own reading of the schema, and sharing the schema
/// lookup would make the two agree by construction.
[[nodiscard]] std::string_view noiseFieldOf(std::string_view type) {
    if (type == "minecraft:noise" || type == "minecraft:shifted_noise" ||
        type == "minecraft:weird_scaled_sampler" ||
        // A SURFACE RULE condition rather than a density function, and the
        // only one of those that names a noise. It shares the walk because
        // it shares the spelling, and because the surface rule is the other
        // half of what a dimension needs seeded.
        type == "minecraft:noise_threshold") {
        return "noise";
    }
    if (type == "minecraft:shift" || type == "minecraft:shift_a" || type == "minecraft:shift_b") {
        return "argument";
    }
    return {};
}

/// Every `worldgen/noise` entry reachable from @p value, following
/// `worldgen/density_function` references through @p pack.
///
/// @p seen guards against a reference cycle. Vanilla's tree has none — the
/// resolver refuses one — but this walk does not go through the resolver, so
/// it carries its own guard rather than trusting that.
void walkDocument(const stratum::data::Pack& pack, const nlohmann::json& value,
                  std::set<std::string>& into, std::set<std::string>& seen) {
    if (value.is_string()) {
        // A density function referenced by name. Noise names never reach here
        // as bare strings: they are only ever read out of a noise field by
        // the object case below, which does not recurse into them.
        const std::string reference = value.get<std::string>();
        if (!seen.insert(reference).second) {
            return;
        }
        const stratum::data::PackEntry* entry =
            pack.find(stratum::data::Registry::DensityFunction,
                      stratum::data::ResourceLocation::parse(reference));
        if (entry != nullptr) {
            walkDocument(pack, entry->json, into, seen);
        }
        return;
    }
    if (value.is_array()) {
        for (const nlohmann::json& child : value) {
            walkDocument(pack, child, into, seen);
        }
        return;
    }
    if (!value.is_object()) {
        return;
    }

    std::string_view noiseField;
    if (value.contains("type") && value.at("type").is_string()) {
        noiseField = noiseFieldOf(value.at("type").get<std::string>());
    }
    for (const auto& [key, child] : value.items()) {
        if (!noiseField.empty() && key == noiseField) {
            // A noise field spelled as an OBJECT is parameters written
            // inline, which names nothing; only the string spelling is a
            // name, and neither spelling is a density function to recurse
            // into.
            if (child.is_string()) {
                into.insert(child.get<std::string>());
            }
            continue;
        }
        walkDocument(pack, child, into, seen);
    }
}

[[nodiscard]] std::set<std::string> namesUnder(const stratum::data::Pack& pack,
                                               const nlohmann::json& value) {
    std::set<std::string> found;
    std::set<std::string> seen;
    walkDocument(pack, value, found, seen);
    return found;
}

[[nodiscard]] std::set<std::string>
toStrings(const std::vector<stratum::data::ResourceLocation>& ids) {
    std::set<std::string> out;
    for (const stratum::data::ResourceLocation& id : ids) {
        out.insert(id.toString());
    }
    return out;
}

/// The four dimensions vanilla 1.21.11 ships with `legacy_random_source`.
constexpr std::array<std::string_view, 4> kLegacySettings = {
    "minecraft:end", "minecraft:nether", "minecraft:caves", "minecraft:floating_islands"};

} // namespace

TEST_CASE("every legacy dimension's named noises, per router entry and in its surface rule",
          "[conformance][settings][legacy]") {
    if (!std::filesystem::is_directory(worldgenTree())) {
        SKIP("no worldgen tree at " << worldgenTree() << "; run tools/fetch-vanilla");
    }
    const auto pack = stratum::data::Pack::open(worldgenTree());
    const auto loaded = stratum::settings::loadAll(pack);

    // EXACTLY THESE FOUR, AND NO OTHERS. The tables below say what each
    // legacy dimension names; they say nothing at all about a legacy
    // dimension that is not in the list. Without this assertion a fifth
    // could appear in a later pack — or an existing settings entry could
    // gain the flag — and every claim in this file would still pass while
    // covering less of the pack than it says it does. So the SET is pinned
    // from the pack, not just the members.
    std::set<std::string> declared;
    for (const auto& [id, settings] : loaded.settings) {
        if (settings.legacyRandomSource) {
            declared.insert(id.toString());
        }
    }
    CHECK(declared == std::set<std::string>{"minecraft:caves", "minecraft:end",
                                            "minecraft:floating_islands", "minecraft:nether"});
    // And the other three are not legacy, which is the half that makes
    // "4 of 7" a statement about the whole pack rather than about four
    // entries of it.
    CHECK(loaded.settings.size() == 7U);

    // Per ROUTER ENTRY, because that is the granularity the narrowing is
    // argued at: an entry absent from a dimension's map names nothing.
    //
    // Read the shape, not just the names. EVERY terrain-shaping entry —
    // final_density, depth, erosion, ridges, continents, barrier,
    // preliminary_surface_level, the two fluid_level entries, lava and the
    // three vein entries — is absent from all four maps. The climate pair is
    // the whole of it, and `minecraft:offset` rides in on the `shift_x` /
    // `shift_z` those two entries are built from (see the file comment).
    const std::map<std::string, std::map<std::string, std::set<std::string>>> expectedRouter = {
        {"minecraft:end", {}},
        {"minecraft:nether",
         {{"temperature", {"minecraft:offset", "minecraft:temperature"}},
          {"vegetation", {"minecraft:offset", "minecraft:vegetation"}}}},
        {"minecraft:caves",
         {{"temperature", {"minecraft:offset", "minecraft:temperature"}},
          {"vegetation", {"minecraft:offset", "minecraft:vegetation"}}}},
        {"minecraft:floating_islands",
         {{"temperature", {"minecraft:offset", "minecraft:temperature"}},
          {"vegetation", {"minecraft:offset", "minecraft:vegetation"}}}},
    };

    const std::map<std::string, std::set<std::string>> expectedSurface = {
        {"minecraft:end", {}},
        {"minecraft:nether",
         {"minecraft:gravel_layer", "minecraft:nether_state_selector", "minecraft:nether_wart",
          "minecraft:netherrack", "minecraft:patch", "minecraft:soul_sand_layer"}},
        {"minecraft:caves",
         {"minecraft:calcite", "minecraft:gravel", "minecraft:ice", "minecraft:packed_ice",
          "minecraft:powder_snow", "minecraft:surface", "minecraft:surface_swamp"}},
        {"minecraft:floating_islands",
         {"minecraft:calcite", "minecraft:gravel", "minecraft:ice", "minecraft:packed_ice",
          "minecraft:powder_snow", "minecraft:surface", "minecraft:surface_swamp"}},
    };

    std::size_t needNothing = 0;
    for (const std::string_view name : kLegacySettings) {
        const auto id = stratum::data::ResourceLocation::parse(std::string(name));
        const stratum::data::PackEntry* entry =
            pack.find(stratum::data::Registry::NoiseSettings, id);
        REQUIRE(entry != nullptr);
        INFO("noise settings " << name);

        // The premise. A dimension that stopped declaring the flag would make
        // every claim below vacuous, so it is checked before them.
        REQUIRE(entry->json.at("legacy_random_source").get<bool>());
        const auto& settings = loaded.settings.at(id);
        REQUIRE(settings.legacyRandomSource);

        // --- the router, entry by entry, from the document ---------------
        std::map<std::string, std::set<std::string>> byEntry;
        for (const auto& [routerEntry, value] : entry->json.at("noise_router").items()) {
            std::set<std::string> names = namesUnder(pack, value);
            if (!names.empty()) {
                byEntry.emplace(routerEntry, std::move(names));
            }
        }
        CHECK(byEntry == expectedRouter.at(std::string(name)));

        // --- the surface rule, from the document -------------------------
        //
        // A surface rule's `noise_threshold` spells its field "noise" like a
        // density function's does, so the same walk reads it; nothing in a
        // rule tree is a density function reference, so nothing is followed.
        const std::set<std::string> surfaceNames = namesUnder(pack, entry->json.at("surface_rule"));
        CHECK(surfaceNames == expectedSurface.at(std::string(name)));

        // --- and the same, through the engine's own two graphs -----------
        const std::set<std::string> fromRouter =
            toStrings(loaded.graph.noisesReachableFrom(std::vector<stratum::density::NodeIndex>{
                settings.router.entries.begin(), settings.router.entries.end()}));
        std::set<std::string> fromDocument;
        for (const auto& [routerEntry, names] : byEntry) {
            fromDocument.insert(names.begin(), names.end());
        }
        CHECK(fromRouter == fromDocument);

        // `surface::requiredNoises` is a SUPERSET of what the document
        // spells: it adds `minecraft:surface`/`surface_secondary` where a
        // depth is read and `clay_bands_offset` where bandlands is placed,
        // and none of those three appears in any field of the tree. So
        // containment, not equality — and the End, where both are empty,
        // pinned exactly.
        const auto rules = stratum::surface::RuleGraph::resolve(settings.surfaceRule, id);
        const std::set<std::string> fromRules = toStrings(stratum::surface::requiredNoises(rules));
        for (const std::string& carried : surfaceNames) {
            INFO("surface rule names " << carried);
            CHECK(fromRules.contains(carried));
        }
        // AND THE SIZE, pinned, because the difference between what the tree
        // SPELLS and what it NEEDS is the whole reason this function exists
        // and the reason the refusal names 8/9/9 rather than 6/7/7. A
        // containment check alone would pass if `requiredNoises` quietly
        // stopped adding the three.
        const std::map<std::string, std::size_t> expectedRequired = {
            {"minecraft:end", 0U},
            {"minecraft:nether", 8U},
            {"minecraft:caves", 9U},
            {"minecraft:floating_islands", 9U},
        };
        CHECK(fromRules.size() == expectedRequired.at(std::string(name)));

        if (fromRouter.empty() && fromRules.empty()) {
            ++needNothing;
            CHECK(name == "minecraft:end");
        }
    }

    // The payoff, stated as its own assertion rather than left to be read off
    // the tables above: of the four dimensions the unsolved derivation used
    // to block outright, exactly one needs no named noise anywhere, and it is
    // the End. That one is what golden_end_test.cpp then generates, exactly,
    // on eight seeds.
    CHECK(needNothing == 1U);
}

TEST_CASE("the reach walk follows splines, which the overworld proves",
          "[conformance][settings][legacy]") {
    if (!std::filesystem::is_directory(worldgenTree())) {
        SKIP("no worldgen tree at " << worldgenTree() << "; run tools/fetch-vanilla");
    }
    const auto pack = stratum::data::Pack::open(worldgenTree());
    const auto loaded = stratum::settings::loadAll(pack);
    const auto& overworld =
        loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:overworld"));

    // THE CASE THAT MAKES THE EMPTY REACHES ABOVE MEAN SOMETHING. `depth` is
    // offset plus a spline over continents/erosion/ridges, and every one of
    // those three is reached ONLY through the spline's `coordinate` — they
    // appear in no argument list above it. A walk that stopped at node
    // arguments would report `offset` alone, and would then have reported
    // "names nothing" for a legacy entry that named something.
    const std::set<std::string> depth = toStrings(loaded.graph.noisesReachableFrom(
        overworld.router.at(stratum::settings::RouterEntry::Depth)));
    CHECK(depth == std::set<std::string>{"minecraft:continentalness", "minecraft:erosion",
                                         "minecraft:offset", "minecraft:ridge"});

    // And the whole-graph union is a strict superset of any one entry's
    // reach, which is exactly why the per-entry walk was needed at all.
    const std::set<std::string> whole = toStrings(loaded.graph.referencedNoises());
    CHECK(whole.size() > depth.size());
    for (const std::string& one : depth) {
        INFO(one);
        CHECK(whole.contains(one));
    }
}

TEST_CASE("the End's final_density is reachable and evaluable, and the Nether's is too",
          "[conformance][settings][legacy]") {
    if (!std::filesystem::is_directory(worldgenTree())) {
        SKIP("no worldgen tree at " << worldgenTree() << "; run tools/fetch-vanilla");
    }
    const auto pack = stratum::data::Pack::open(worldgenTree());
    const auto loaded = stratum::settings::loadAll(pack);
    const auto& end = loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:end"));
    const auto& nether =
        loaded.settings.at(stratum::data::ResourceLocation::parse("minecraft:nether"));

    // The narrowed refusal lets this through: nothing named, nothing to
    // derive. Before the narrowing this threw.
    const std::vector<stratum::data::ResourceLocation> nothing;
    const auto registry = stratum::density::NoiseRegistry::create(
        pack, nothing, 42, stratum::density::RandomSource::Legacy);
    CHECK(registry.size() == 0);

    // The End's final_density DOES reach `minecraft:end_islands` — it is
    // `end/sloped_cheese`, which is end_islands + end/base_3d_noise, not
    // pure old_blended_noise — and that node is now evaluable. Both halves
    // are asserted: the reach, so that a future pack rewiring the End cannot
    // make this vacuous, and the evaluability, so that re-refusing the type
    // fails here rather than in a golden diff.
    const stratum::density::Interpreter endInterpreter(
        loaded.graph, registry,
        stratum::density::CellGeometry{.width = end.geometry.cellWidth(),
                                       .height = end.geometry.cellHeight()});
    bool reachesEndIslands = false;
    for (const stratum::density::NodeIndex index :
         loaded.graph.reachableFrom(end.router.at(stratum::settings::RouterEntry::FinalDensity))) {
        const auto type = loaded.graph.node(index).type;
        if (type == stratum::density::NodeType::EndIslands) {
            reachesEndIslands = true;
        }
        INFO(stratum::density::nodeTypeName(type));
        CHECK_FALSE(endInterpreter.unevaluableReason(type).has_value());
    }
    CHECK(reachesEndIslands);

    // The Nether, by contrast, reaches nothing unevaluable and no
    // end_islands — which is the claim the terrain measurement rests on.
    const stratum::density::Interpreter netherInterpreter(
        loaded.graph, registry,
        stratum::density::CellGeometry{.width = nether.geometry.cellWidth(),
                                       .height = nether.geometry.cellHeight()});
    for (const stratum::density::NodeIndex index : loaded.graph.reachableFrom(
             nether.router.at(stratum::settings::RouterEntry::FinalDensity))) {
        const auto type = loaded.graph.node(index).type;
        INFO(stratum::density::nodeTypeName(type));
        CHECK_FALSE(netherInterpreter.unevaluableReason(type).has_value());
        CHECK(type != stratum::density::NodeType::EndIslands);
    }
}
