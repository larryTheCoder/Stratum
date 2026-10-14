// Stratum — frozen pipelines on disk, and the compiled dimensions the whole
// process shares.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum/biome/parameter_list.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/freeze/pipeline.hpp>
#include <stratum/mapping/biome.hpp>

#include <stratum_pmmp/chunk_encoder.hpp>
#include <stratum_pmmp/dimension_registry.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <utility>

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

/// A name beside @p path that no other write in this process, and almost
/// surely no other process, is using: the clock separates processes, the
/// serial writes within one.
[[nodiscard]] std::filesystem::path partialPathFor(const std::filesystem::path& path) {
    static std::atomic<std::uint64_t> serial{0};
    const std::uint64_t number = serial.fetch_add(1, std::memory_order_relaxed) + 1U;
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path partial = path;
    partial += ".partial-" + std::to_string(ticks) + "-" + std::to_string(number);
    return partial;
}

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
    const std::filesystem::path partial = partialPathFor(path);
    std::error_code ignored;
    {
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        out.write(reinterpret_cast<const char*>(blob.data()),
                  static_cast<std::streamsize>(blob.size()));
        out.close();
        if (!out) {
            std::filesystem::remove(partial, ignored);
            throw std::runtime_error("cannot write the frozen pipeline to " + path.string());
        }
    }
    // The one step a reader can observe: before it, the old file; after it,
    // the new one, whole.
    std::error_code error;
    std::filesystem::rename(partial, path, error);
    if (error) {
        std::filesystem::remove(partial, ignored);
        throw std::runtime_error("cannot write the frozen pipeline to " + path.string() + ": " +
                                 error.message());
    }
}

std::unique_ptr<const world::CompiledDimension>
compileForPocketMine(const std::span<const std::byte> blob, const std::string& noiseSettings,
                     const std::string& biomeSource, const std::int64_t worldSeed) {
    // What is wrong with the names before what is wrong with the file.
    const world::BiomeSource source = world::BiomeSource::parse(biomeSource);
    const data::ResourceLocation settings = data::ResourceLocation::parse(noiseSettings);
    freeze::Pipeline pipeline = freeze::read(blob);

    // Read before compile takes the pipeline, refused after it, so that
    // compile's own refusals (no such settings, no such list) come first.
    std::string list;
    std::vector<std::string> unmapped;
    if (const auto& named = source.parameterList(); named.has_value()) {
        if (const auto found = pipeline.biomeParameters.find(*named);
            found != pipeline.biomeParameters.end()) {
            list = named->toString();
            unmapped = unmappedBiomes(found->second);
        }
    }

    std::unique_ptr<const world::CompiledDimension> dimension =
        world::CompiledDimension::compile(std::move(pipeline), settings, source, worldSeed);
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
