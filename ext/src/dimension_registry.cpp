// Stratum — frozen pipelines on disk, and the compiled dimensions the whole
// process shares.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum/biome/parameter_list.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/freeze/pipeline.hpp>
#include <stratum/mapping/biome.hpp>
#include <stratum/mapping/block_state.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/surface/rule_graph.hpp>

#include <stratum_pmmp/chunk_encoder.hpp>
#include <stratum_pmmp/dimension_registry.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <fstream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace stratum::pmmp {

namespace {

/// Every biome @p list names that has no Bedrock id, each once, in the
/// order the list first names it.
[[nodiscard]] std::vector<std::string> unmappedBiomes(const biome::ParameterList& list) {
    std::vector<std::string> unmapped;
    for (const biome::Entry& entry : list.entries()) {
        if (mapping::bedrockBiomeId(entry.biome).has_value()) {
            continue;
        }
        std::string name = entry.biome.toString();
        if (std::ranges::find(unmapped, name) == unmapped.end()) {
            unmapped.push_back(std::move(name));
        }
    }
    return unmapped;
}

/// @p state as a pack spells it: `minecraft:stone`, or
/// `minecraft:water[level=0]` with its properties.
[[nodiscard]] std::string spelled(const settings::BlockState& state) {
    std::string text = state.name.toString();
    if (!state.properties.empty()) {
        std::string properties;
        for (const auto& [key, value] : state.properties) {
            properties += properties.empty() ? "" : ",";
            properties += key + "=" + value;
        }
        text += "[" + properties + "]";
    }
    return text;
}

/// Every block state @p noise names itself — its default block, its
/// default fluid, and the block of every `minecraft:block` rule in
/// @p surfaceRules — that this build's mapping table cannot place: each
/// once, with where it is named and the table's own reason, in the order
/// first named. Everything else a dimension places (bedrock, ore veins, the
/// aquifer's lava, the bandlands' terracotta, air) is one of the engine's
/// own vanilla constants, never the pack's, and the table carries them all.
[[nodiscard]] std::vector<std::string> unmappedBlockStates(const settings::NoiseSettings& noise,
                                                           const surface::RuleGraph& surfaceRules) {
    std::vector<std::pair<std::string_view, const settings::BlockState*>> named = {
        {"default_block", &noise.defaultBlock},
        {"default_fluid", &noise.defaultFluid},
    };
    for (std::size_t index = 0; index < surfaceRules.ruleCount(); ++index) {
        const surface::Rule& rule = surfaceRules.rule(static_cast<surface::RuleIndex>(index));
        if (rule.type == surface::RuleType::Block) {
            named.emplace_back("surface rule result_state", &rule.block);
        }
    }
    std::vector<std::string> seen;
    std::vector<std::string> unmapped;
    for (const auto& [where, state] : named) {
        std::string described = std::string(where) + " " + spelled(*state);
        if (std::ranges::find(seen, described) != seen.end()) {
            continue;
        }
        seen.push_back(described);
        try {
            static_cast<void>(mapping::javaBlockStateId(*state));
        } catch (const mapping::BlockMappingError& error) {
            unmapped.push_back(described + " (" + error.what() + ")");
        }
    }
    return unmapped;
}

/// What a write's partial file adds to the name of the file it replaces.
constexpr std::string_view kPartialInfix = ".partial-";

/// A name beside @p path that no other write in this process, and almost
/// surely no other process, is using: the clock separates processes, the
/// serial writes within one.
[[nodiscard]] std::filesystem::path partialPathFor(const std::filesystem::path& path) {
    static std::atomic<std::uint64_t> serial{0};
    const std::uint64_t number = serial.fetch_add(1, std::memory_order_relaxed) + 1U;
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path partial = path;
    partial += std::string(kPartialInfix) + std::to_string(ticks) + "-" + std::to_string(number);
    return partial;
}

/// The directory @p path is in, as a path that can be opened.
[[nodiscard]] std::filesystem::path directoryOf(const std::filesystem::path& path) {
    const std::filesystem::path parent = path.parent_path();
    return parent.empty() ? std::filesystem::path(".") : parent;
}

/// The partial files writes in this process are writing now.
class PartialsInFlight {
public:
    /// Registers @p partial, a partial of @p path about to be written, and
    /// first removes every other `<path>.partial-*` beside @p path that no
    /// write in this process is writing: what a process killed mid-write
    /// left (a write that throws removes its own). Under one lock, so a
    /// partial another thread is writing is either registered, and kept,
    /// or not yet created.
    static void begin(const std::filesystem::path& path, const std::filesystem::path& partial) {
        const std::lock_guard<std::mutex> lock(mutex());
        const std::string prefix = path.filename().string() + std::string(kPartialInfix);
        std::error_code error;
        std::vector<std::filesystem::path> stale;
        std::filesystem::directory_iterator entry(directoryOf(path), error);
        while (!error && entry != std::filesystem::directory_iterator()) {
            const std::filesystem::path& found = entry->path();
            if (found.filename().string().starts_with(prefix) && !written().contains(found)) {
                stale.push_back(found);
            }
            entry.increment(error);
        }
        // Best effort, and harmless when it fails: a stale partial is
        // never read, and the write below refuses loudly if the directory
        // itself is unusable.
        for (const std::filesystem::path& file : stale) {
            std::filesystem::remove(file, error);
        }
        written().insert(partial);
    }

    static void end(const std::filesystem::path& partial) {
        const std::lock_guard<std::mutex> lock(mutex());
        written().erase(partial);
    }

private:
    [[nodiscard]] static std::mutex& mutex() {
        static std::mutex instance;
        return instance;
    }

    [[nodiscard]] static std::set<std::filesystem::path>& written() {
        static std::set<std::filesystem::path> instance;
        return instance;
    }
};

/// One write's partial file: registered while it is written, and removed
/// with it unless renamed into place.
class PartialFile {
public:
    explicit PartialFile(const std::filesystem::path& path) : path_(partialPathFor(path)) {
        PartialsInFlight::begin(path, path_);
    }

    PartialFile(const PartialFile&) = delete;
    PartialFile& operator=(const PartialFile&) = delete;
    PartialFile(PartialFile&&) = delete;
    PartialFile& operator=(PartialFile&&) = delete;

    ~PartialFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        PartialsInFlight::end(path_);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

#if !defined(_WIN32)
/// fsync(2) of @p path — a file's data, or a directory's entries — through
/// a descriptor of its own: 0, or the errno it failed with.
[[nodiscard]] int syncToDevice(const std::filesystem::path& path, const bool directory) {
    const int descriptor =
        ::open(path.c_str(), O_RDONLY | O_CLOEXEC | (directory ? O_DIRECTORY : 0));
    if (descriptor < 0) {
        return errno;
    }
    int result = 0;
    do {
        result = ::fsync(descriptor);
    } while (result != 0 && errno == EINTR);
    const int failure = result == 0 ? 0 : errno;
    ::close(descriptor);
    return failure;
}
#endif

} // namespace

std::vector<std::byte> readPipelineFile(const std::filesystem::path& path) {
    const std::string refusal = "cannot read the frozen pipeline at " + path.string();
    std::error_code error;
    // A directory opens as a stream on some platforms and then reads
    // nothing; refused as unreadable, by name, rather than as an empty blob.
    if (!std::filesystem::is_regular_file(path, error)) {
        throw std::runtime_error(refusal);
    }
    // The size from the open file itself, never from the path: a write
    // (writePipelineFile) renames a new file over the path, and one landing
    // between a stat of the path and an open of it paired the size of one
    // file with the bytes of the next, refusing a blob that was whole in
    // both or returning one neither write wrote.
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error(refusal);
    }
    const auto size = static_cast<std::streamoff>(file.tellg());
    file.seekg(0, std::ios::beg);
    if (size < 0 || !file) {
        throw std::runtime_error(refusal);
    }
    std::vector<std::byte> blob(static_cast<std::size_t>(size));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    file.read(reinterpret_cast<char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
    if (file.gcount() != static_cast<std::streamsize>(blob.size())) {
        throw std::runtime_error(refusal);
    }
    return blob;
}

void writePipelineFile(const std::filesystem::path& path, const std::span<const std::byte> blob) {
    const std::string refusal = "cannot write the frozen pipeline to " + path.string();
    const PartialFile partial(path);
    {
        std::ofstream out(partial.path(), std::ios::binary | std::ios::trunc);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        out.write(reinterpret_cast<const char*>(blob.data()),
                  static_cast<std::streamsize>(blob.size()));
        out.close();
        if (!out) {
            throw std::runtime_error(refusal);
        }
    }
#if !defined(_WIN32)
    // On the device before it is renamed into place: a rename can reach the
    // disk before the data it names, and a power cut between the two left a
    // blob of nothing under a world whose level.dat existed.
    if (const int failure = syncToDevice(partial.path(), false); failure != 0) {
        throw std::runtime_error(refusal + ": " + std::generic_category().message(failure));
    }
#endif
    // The one step a reader can observe: before it, the old file; after it,
    // the new one, whole.
    std::error_code error;
    std::filesystem::rename(partial.path(), path, error);
    if (error) {
        throw std::runtime_error(refusal + ": " + error.message());
    }
#if !defined(_WIN32)
    // And the rename itself, which is the directory's. EINVAL is a file
    // system that cannot sync a directory, where there is nothing more to
    // do; anything else is refused, the new blob already in place.
    if (const int failure = syncToDevice(directoryOf(path), true);
        failure != 0 && failure != EINVAL) {
        throw std::runtime_error(
            "wrote the frozen pipeline to " + path.string() +
            ", but cannot sync its directory to disk: " + std::generic_category().message(failure));
    }
#endif
}

std::unique_ptr<const world::CompiledDimension>
compileForPocketMine(const std::span<const std::byte> blob, const std::string& noiseSettings,
                     const std::string& biomeSource, const std::int64_t worldSeed) {
    // What is wrong with the names before what is wrong with the file.
    const world::BiomeSource source = world::BiomeSource::parse(biomeSource);
    const data::ResourceLocation settings = data::ResourceLocation::parse(noiseSettings);
    freeze::Pipeline pipeline = freeze::read(blob);

    // Read before compile takes the pipeline, refused after it, so that
    // compile's own refusals (no such settings, no such list, a surface rule
    // it cannot resolve) come first.
    std::string list;
    std::vector<std::string> unmapped;
    if (const auto& named = source.parameterList(); named.has_value()) {
        if (const auto found = pipeline.biomeParameters.find(*named);
            found != pipeline.biomeParameters.end()) {
            list = named->toString();
            unmapped = unmappedBiomes(found->second);
        }
    }
    std::vector<std::string> unmappedStates;
    // compile resolves the same rules from the same JSON under the same id,
    // so a tree that does not resolve here is refused by compile first. Kept
    // and thrown after it all the same, rather than let through should
    // compile ever accept what this refused.
    std::exception_ptr unresolvedRules;
    if (const auto found = pipeline.settings.find(settings); found != pipeline.settings.end()) {
        try {
            unmappedStates = unmappedBlockStates(
                found->second, surface::RuleGraph::resolve(found->second.surfaceRule, settings));
        } catch (const surface::RuleError&) {
            unresolvedRules = std::current_exception();
        }
    }

    std::unique_ptr<const world::CompiledDimension> dimension =
        world::CompiledDimension::compile(std::move(pipeline), settings, source, worldSeed);
    if (unresolvedRules) {
        std::rethrow_exception(unresolvedRules);
    }
    requireFitsChunk(dimension->geometry());
    if (!unmapped.empty()) {
        std::string names;
        for (const std::string& name : unmapped) {
            names += names.empty() ? "'" : ", '";
            names += name;
            names += "'";
        }
        throw EncodeError("biome parameter list '" + list + "' names " + names +
                          (unmapped.size() == 1 ? ", which has" : ", which have") +
                          " no Bedrock id in this build's table (a custom or datapack biome; "
                          "the nearest-biome fallback is not built yet)");
    }
    if (!unmappedStates.empty()) {
        std::string states;
        for (const std::string& state : unmappedStates) {
            states += states.empty() ? "" : "; ";
            states += state;
        }
        throw EncodeError("noise settings '" + settings.toString() + "' name " +
                          (unmappedStates.size() == 1 ? "a block state" : "block states") +
                          " this build's mapping table cannot place: " + states);
    }
    return dimension;
}

std::shared_ptr<const world::CompiledDimension>
DimensionRegistry::open(const std::span<const std::byte> blob, const std::string& noiseSettings,
                        const std::string& biomeSource, const std::int64_t worldSeed) {
    // Every byte, header included (the file comment says why), hashed
    // before the lock is taken.
    Key key{hash::md5(blob), noiseSettings, biomeSource, worldSeed};

    const std::lock_guard<std::mutex> lock(mutex_);
    if (const auto found = dimensions_.find(key); found != dimensions_.end()) {
        if (auto existing = found->second.lock()) {
            return existing;
        }
    }
    std::shared_ptr<const world::CompiledDimension> compiled =
        compileForPocketMine(blob, noiseSettings, biomeSource, worldSeed);
    std::erase_if(dimensions_, [](const auto& entry) { return entry.second.expired(); });
    dimensions_.insert_or_assign(std::move(key), compiled);
    return compiled;
}

std::size_t DimensionRegistry::entries() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return dimensions_.size();
}

} // namespace stratum::pmmp
