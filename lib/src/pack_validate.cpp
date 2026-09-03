// Stratum — checking a pack without generating from it.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum/data/pack.hpp>
#include <stratum/data/registry.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/graph.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/validate/pack_report.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace stratum::validate {

namespace {

/// The registries this build reads and acts on, as opposed to the ones it
/// loads and files away. Keeping the list here rather than in registry.hpp
/// is deliberate: `isSupported` is the public contract and does not move,
/// while this one shrinks every milestone, and conflating them would let a
/// clean report quietly overclaim.
[[nodiscard]] bool isInterpretedYet(data::Registry registry) noexcept {
    return registry == data::Registry::DensityFunction || registry == data::Registry::Noise ||
           registry == data::Registry::NoiseSettings;
}

void add(Report& report, Severity severity, std::string subject, std::string message) {
    report.findings.push_back(Finding{
        .severity = severity, .subject = std::move(subject), .message = std::move(message)});
}

/// Counts what is in the pack, and reports what will not be executed.
void reportRegistries(const data::Pack& pack, Report& report) {
    std::map<data::Registry, std::size_t> loaded;
    for (const data::PackEntry& entry : pack.entries()) {
        ++loaded[entry.registry];
    }
    std::map<data::Registry, std::size_t> rejected;
    for (const data::RejectedEntry& entry : pack.rejected()) {
        ++rejected[entry.registry];
    }

    // Loaded and rejected are disjoint — a registry is either executed by
    // this build or it is not — so the first hit is the answer.
    const auto entriesIn = [&loaded, &rejected](data::Registry registry) -> std::size_t {
        if (const auto found = loaded.find(registry); found != loaded.end()) {
            return found->second;
        }
        if (const auto found = rejected.find(registry); found != rejected.end()) {
            return found->second;
        }
        return 0;
    };

    for (const data::Registry registry : data::allRegistries()) {
        const std::size_t count = entriesIn(registry);
        if (count == 0) {
            continue;
        }
        report.registries.push_back(RegistryCount{.registry = registry,
                                                  .entries = count,
                                                  .supported = data::isSupported(registry),
                                                  .interpreted = isInterpretedYet(registry)});
    }

    for (const auto& [registry, count] : rejected) {
        // Not an error: vanilla's own data ships hundreds of these, and
        // features are out of scope for v1 by design (SPEC §8). Reported by
        // registry and count so that whoever holds the pack knows exactly
        // what this engine will leave undone.
        add(report, Severity::Warning, std::string(data::registryDirectory(registry)),
            std::to_string(count) + " entr" + (count == 1 ? "y" : "ies") +
                " in a registry this engine does not execute (SPEC §8, v2)");
    }

    for (const RegistryCount& entry : report.registries) {
        if (entry.supported && !entry.interpreted) {
            // The honest half of a clean report. These load, and nothing
            // reads them yet, so silence about them is not approval.
            add(report, Severity::Note, std::string(data::registryDirectory(entry.registry)),
                std::to_string(entry.entries) +
                    " entries load and are addressable, but nothing in this build interprets "
                    "them yet (SPEC §10)");
        }
    }
}

/// Every noise the graph names, checked one at a time so that a pack with
/// four broken noises hears about four of them.
[[nodiscard]] bool reportNoises(const data::Pack& pack,
                                const std::vector<data::ResourceLocation>& referenced,
                                Report& report) {
    bool allUsable = true;
    for (const data::ResourceLocation& id : referenced) {
        const data::PackEntry* entry = pack.find(data::Registry::Noise, id);
        if (entry == nullptr) {
            add(report, Severity::Error, id.toString(),
                "a density function references this noise and the pack does not define it");
            allUsable = false;
            continue;
        }
        try {
            static_cast<void>(density::NoiseParameters::fromJson(entry->json, id));
        } catch (const density::NoiseError& error) {
            add(report, Severity::Error, id.toString(), error.what());
            allUsable = false;
        }
    }
    return allUsable;
}

} // namespace

std::string_view severityName(Severity severity) noexcept {
    switch (severity) {
        case Severity::Note:
            return "note";
        case Severity::Warning:
            return "warning";
        case Severity::Error:
            return "error";
    }
    return "unknown";
}

std::size_t Report::count(Severity severity) const noexcept {
    return static_cast<std::size_t>(std::ranges::count_if(
        findings, [severity](const Finding& finding) { return finding.severity == severity; }));
}

Report validatePack(const data::Pack& pack, const ValidateOptions& options) {
    Report report;
    reportRegistries(pack, report);
    report.densityFunctions = pack.entriesOf(data::Registry::DensityFunction).size();

    // The settings' routers carry density functions written inline, so they
    // are resolved into the same graph as the pack's named ones — which
    // means a broken router is found here rather than left for whatever
    // first asks a dimension to generate.
    std::optional<settings::LoadedSettings> loaded;
    try {
        loaded = settings::loadAll(pack);
    } catch (const density::ResolveError& error) {
        // Resolution is all-or-nothing — one cycle stops the whole graph —
        // so this is the first problem, not necessarily the only one. Said
        // out loud, because a caller who fixes it and finds another would
        // otherwise think the report had lied.
        add(report, Severity::Error, {},
            std::string(error.what()) +
                " (resolution stops at the first problem; there may be more behind it)");
        return report;
    } catch (const settings::SettingsError& error) {
        add(report, Severity::Error, {},
            std::string(error.what()) +
                " (loading stops at the first problem; there may be more behind it)");
        return report;
    }

    const density::Graph* graph = &loaded->graph;
    report.resolved = true;
    report.nodes = graph->nodeCount();
    report.splines = graph->splineCount();
    report.noiseSettings = loaded->settings.size();

    if (!loaded->settings.empty()) {
        // Read and kept, never interpreted. Saying so is the difference
        // between a clean report and a clean report that means something.
        add(report, Severity::Note,
            std::string(data::registryDirectory(data::Registry::NoiseSettings)),
            "surface rules and spawn targets inside these entries are kept verbatim and are "
            "not interpreted by this build (SPEC §10, M4)");
    }

    const std::vector<data::ResourceLocation> referenced = graph->referencedNoises();
    report.noisesReferenced = referenced.size();
    if (!reportNoises(pack, referenced, report)) {
        // Without every noise there is no interpreter, so the per-function
        // walk below cannot run. What was found above still stands.
        return report;
    }

    // Xoroshiro for the pack-level pass, deliberately: a density function
    // file belongs to no dimension, so it has no random source of its own,
    // and what is being asked here is which node *types* can be evaluated —
    // a structural question that does not depend on how a noise was seeded.
    // The per-dimension passes below are the ones that care.
    const density::NoiseRegistry noises = density::NoiseRegistry::create(
        pack, referenced, options.seed, density::RandomSource::Xoroshiro);
    const density::Interpreter interpreter(*graph, noises);

    for (const auto& [id, dimension] : loaded->settings) {
        // Each dimension gets its own noises and its own interpreter. Both
        // are properties of the dimension rather than of the function: the
        // same density function is sampled on 4x8 cells by the overworld and
        // 8x4 by the End, and the same `minecraft:temperature` is seeded by
        // Xoroshiro in the overworld and by the Java LCG in the Nether.
        const auto source = dimension.legacyRandomSource ? density::RandomSource::Legacy
                                                         : density::RandomSource::Xoroshiro;
        std::optional<density::NoiseRegistry> dimensionNoises;
        try {
            dimensionNoises.emplace(
                density::NoiseRegistry::create(pack, referenced, options.seed, source));
        } catch (const density::NoiseError& error) {
            // A warning, not an error: the pack is not wrong, this build
            // cannot seed that dimension. Its router entries are left
            // *unchecked* rather than counted as failures — "we did not
            // look" and "we looked and it does not work" are different
            // things and the counts should not conflate them.
            add(report, Severity::Warning, id.toString(), error.what());
            continue;
        }

        const density::Interpreter sampler(
            *graph, *dimensionNoises,
            density::CellGeometry{.width = dimension.geometry.cellWidth(),
                                  .height = dimension.geometry.cellHeight()});
        ++report.dimensionsChecked;

        for (std::size_t i = 0; i < settings::kRouterEntryCount; ++i) {
            const auto entry = static_cast<settings::RouterEntry>(i);
            ++report.routerEntries;
            try {
                sampler.requireEvaluable(dimension.router.at(entry));
                ++report.routerEntriesEvaluable;
            } catch (const density::UnbuildableError& error) {
                // An error, not a warning, and the difference was measured
                // rather than assumed: the vanilla server refuses to build a
                // world whose router reaches this, so the pack is broken
                // wherever it is taken. Caught before EvalError because it
                // derives from it.
                add(report, Severity::Error,
                    id.toString() + " " + std::string(settings::routerEntryName(entry)),
                    error.what());
            } catch (const density::EvalError& error) {
                add(report, Severity::Warning,
                    id.toString() + " " + std::string(settings::routerEntryName(entry)),
                    error.what());
            }
        }
    }

    for (const auto& [id, root] : graph->roots()) {
        try {
            interpreter.requireEvaluable(root);
            ++report.evaluable;
        } catch (const density::EvalError& error) {
            // A warning rather than an error even for an UnbuildableError,
            // which the router loop above reports as one. The difference is
            // reach: a `worldgen/density_function` file that no dimension's
            // router names is never visited, and a vanilla server carrying
            // one starts and generates perfectly well — measured, alongside
            // the refusals (tools/analysis/inline-noise-probe.sh). It
            // becomes an error at the dimension that reaches it, which is
            // exactly where vanilla raises it.
            add(report, Severity::Warning, id.toString(), error.what());
        }
    }

    // Worst first, and stable, so that a long report opens with the thing
    // that stops the pack working rather than burying it under notes. The
    // order within a severity stays the order they were found in, which is
    // registry order and then identifier order — both already deterministic.
    std::ranges::stable_sort(report.findings, [](const Finding& left, const Finding& right) {
        return left.severity > right.severity;
    });

    return report;
}

} // namespace stratum::validate
