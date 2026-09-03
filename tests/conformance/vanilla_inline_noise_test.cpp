// Stratum — a noise written inline, and what the vanilla server does with it.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// mcdoc declares every density-function `noise` field as
// `#[id="worldgen/noise"] string | NoiseParameters`, so the parameters may be
// written in place of the identifier. A named noise is seeded from the MD5 of
// its identifier; an inline one has no identifier, and SPEC §11 carried "how
// vanilla seeds a noise that has no name" as an open question.
//
// It has no answer, because vanilla never seeds one.
// tools/analysis/inline-noise-probe.sh puts eleven datapacks in front of the
// server, each differing from the others in exactly one thing — how one noise
// is spelled — and records what happened. The server loads the JSON without a
// word and then dies building the world:
//
//     java.util.NoSuchElementException: No value present
//             at java.base/java.util.Optional.orElseThrow
//             at eve$a.a(SourceFile:71)
//
// for every field that takes the union, for both sets of parameters, and even
// for a router entry the dimension never samples — while a named noise, and an
// inline one in a `worldgen/density_function` file that no router reaches, are
// both fine.
//
// So this build's refusal is parity rather than a gap, and this is what keeps
// it honest: each recorded pack is replayed through this build, which must
// reach the same verdict — taken from the recording, not from anything written
// here. If a later version of the game starts accepting the spelling,
// re-running the probe turns this red, which is the point of reading the
// verdict out of a fixture instead of hard-coding it.
//
// The recording is Mojang-derived and never committed (SPEC §12). Without it
// this SKIPs, naming the command that produces it.

#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/density/graph.hpp>
#include <stratum/density/interpreter.hpp>
#include <stratum/density/noise_registry.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace {

using stratum::data::Pack;
using stratum::data::ResourceLocation;
using stratum::density::Graph;
using stratum::density::Interpreter;
using stratum::density::NoiseRegistry;
using stratum::density::UnbuildableError;

[[nodiscard]] std::filesystem::path verdictsFile() {
    return std::filesystem::path(STRATUM_FIXTURES_DIR) / "1.21.11" / "probes" / "inline-noise" /
           "verdicts.json";
}

/// A data pack on disk, which is the only way in: Pack opens a directory.
///
/// A data pack rather than a bare worldgen tree because the probe's packs
/// span two namespaces — its own copies under `stratum:` and the vanilla
/// entry under `minecraft:` — and a worldgen tree is opened as exactly one.
class TempPack {
public:
    TempPack() : path_(std::filesystem::temp_directory_path() / uniqueName()) {
        std::filesystem::create_directories(path_);
        std::ofstream(path_ / "pack.mcmeta")
            << R"({"pack":{"description":"stratum inline noise replay",)"
               R"("min_format":[94,1],"max_format":94}})";
    }

    TempPack(const TempPack&) = delete;
    TempPack& operator=(const TempPack&) = delete;
    TempPack(TempPack&&) = delete;
    TempPack& operator=(TempPack&&) = delete;

    ~TempPack() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    void write(const std::string& registry, const ResourceLocation& id,
               const std::string& json) const {
        const std::filesystem::path file =
            path_ / "data" / id.namespaceName() / "worldgen" / registry / (id.path() + ".json");
        std::filesystem::create_directories(file.parent_path());
        std::ofstream(file) << json;
    }

    [[nodiscard]] Pack pack() const { return Pack::openDataPack(path_); }

private:
    [[nodiscard]] static std::string uniqueName() {
        static int counter = 0;
        return "stratum-inline-noise-replay-" + std::to_string(++counter);
    }

    std::filesystem::path path_;
};

} // namespace

TEST_CASE("this build refuses exactly the inline noises the vanilla server refuses",
          "[conformance][density][inline_noise]") {
    const std::filesystem::path file = verdictsFile();
    if (!std::filesystem::is_regular_file(file)) {
        SKIP("no probe recording at "
             << file << "; produce it with tools/analysis/inline-noise-probe.sh --accept-eula");
    }

    std::ifstream in(file);
    const nlohmann::json recorded = nlohmann::json::parse(in);

    REQUIRE(recorded.at("version").get<std::string>() == "1.21.11");
    const auto seed = recorded.at("seed").get<std::int64_t>();
    const auto& variants = recorded.at("variants");
    // Two controls, one unreferenced case and eight inline spellings. Pinned
    // so that a recording an interrupted probe left half-written cannot pass
    // by having nothing left to disagree about.
    REQUIRE(variants.size() == 11U);

    std::size_t accepted = 0;
    std::size_t refused = 0;

    for (const auto& variant : variants) {
        const auto name = variant.at("name").get<std::string>();
        const auto verdict = variant.at("verdict").get<std::string>();
        INFO("variant " << name << ", which the server " << verdict);
        REQUIRE((verdict == "accepted" || verdict == "refused"));

        const TempPack tree;
        for (const auto& [id, parameters] : variant.at("noises").items()) {
            tree.write("noise", ResourceLocation::parse(id), parameters.dump());
        }

        // Every variant differs from the others in one density function, and
        // which one it is depends on the variant: `inline_unused` is the one
        // that establishes that the server minds an inline noise in a router
        // entry it never samples, so what it is really about is the function
        // it put in `barrier` rather than the named one under its flat_cache.
        const nlohmann::json& subject = variant.at("barrier").is_object()
                                            ? variant.at("barrier")
                                            : variant.at("density_function");
        const ResourceLocation root = ResourceLocation::parse("stratum:probe");
        tree.write("density_function", root, subject.dump());
        if (!variant.at("unreferenced_function").is_null()) {
            tree.write("density_function", ResourceLocation::parse("stratum:unreferenced"),
                       variant.at("unreferenced_function").dump());
        }

        // Loading is where the two must agree first, and for every variant
        // they do: the spelling is legal input, and the server said so by
        // getting past its own codecs before it failed.
        const Pack pack = tree.pack();
        const Graph graph = Graph::resolveAll(pack);
        const NoiseRegistry noises = NoiseRegistry::create(
            pack, graph.referencedNoises(), seed, stratum::density::RandomSource::Xoroshiro);
        const Interpreter interpreter(graph, noises);

        if (verdict == "accepted") {
            ++accepted;
            CHECK_NOTHROW(interpreter.requireEvaluable(graph.rootOf(root)));
        } else {
            ++refused;
            // UnbuildableError specifically, not EvalError: the whole reason
            // that type exists is that "this pack does not work anywhere" and
            // "this build is not finished" are different answers, and
            // `stratum validate` reports them at different severities.
            CHECK_THROWS_AS(interpreter.requireEvaluable(graph.rootOf(root)), UnbuildableError);
        }

        // The unreferenced case is the one that fixes *where* the refusal
        // belongs. The server starts and generates with such a file in the
        // registry, so this build must load it and walk every other root
        // without complaint; it is only a dimension reaching it that is fatal.
        if (!variant.at("unreferenced_function").is_null()) {
            CHECK(graph.roots().size() == 2U);
            CHECK_THROWS_AS(interpreter.requireEvaluable(
                                graph.rootOf(ResourceLocation::parse("stratum:unreferenced"))),
                            UnbuildableError);
        }
    }

    // The recording is trusted for *which* verdict each pack got, but not for
    // the run having been a real experiment: a probe whose server jar had gone
    // missing would record eleven refusals and this loop would happily agree
    // with all of them. Three controls that started and eight packs that did
    // not is the shape a run that measured something has.
    CHECK(accepted == 3U);
    CHECK(refused == 8U);
}
