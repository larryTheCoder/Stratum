// Stratum — every world the process generates.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum_pmmp/worlds.hpp>

#include <optional>
#include <stdexcept>
#include <utility>

namespace stratum::pmmp {

std::string WorldOptions::key() const {
    return fluidUpdateWorldKey(blobPath, noiseSettings, biomeSource, seed);
}

void Worlds::setDataPath(const std::filesystem::path& dataPath) {
    if (!dataPath.is_absolute()) {
        throw std::runtime_error("the server's data path, which relative frozen pipeline paths "
                                 "are read against, must be absolute, not '" +
                                 dataPath.string() + "'");
    }
    const std::filesystem::path normal = dataPath.lexically_normal();
    // Taken in this order, and nowhere the other way round: no pin is made
    // under the old path while this decides.
    const std::lock_guard<std::mutex> pins(pinsMutex_);
    const std::lock_guard<std::mutex> lock(dataPathMutex_);
    if (normal == dataPath_) {
        return;
    }
    if (!pins_.empty()) {
        throw std::runtime_error(
            "the server's data path cannot change from '" + dataPath_.string() + "' to '" +
            normal.string() +
            "' while a world is pinned: its relative frozen pipeline path would then name another "
            "file than the one it was pinned with");
    }
    dataPath_ = normal;
}

std::optional<std::filesystem::path> Worlds::tryResolve(const std::string& blobPath) const {
    const std::filesystem::path path(blobPath);
    if (path.is_absolute()) {
        return path.lexically_normal();
    }
    const std::lock_guard<std::mutex> lock(dataPathMutex_);
    if (dataPath_.empty()) {
        return std::nullopt;
    }
    return (dataPath_ / path).lexically_normal();
}

std::filesystem::path Worlds::resolvePipelinePath(const std::string& blobPath) const {
    std::optional<std::filesystem::path> path = tryResolve(blobPath);
    if (!path.has_value()) {
        throw std::runtime_error("the frozen pipeline path '" + blobPath +
                                 "' is relative to the server's data path, and none has been "
                                 "set (Stratum\\setDataPath)");
    }
    return std::move(*path);
}

WorldOptions Worlds::resolved(const WorldOptions& options) const {
    WorldOptions world = options;
    world.blobPath = resolvePipelinePath(options.blobPath).string();
    return world;
}

std::optional<WorldOptions> Worlds::resolvedIfAny(const WorldOptions& options) const {
    std::optional<std::filesystem::path> path = tryResolve(options.blobPath);
    if (!path.has_value()) {
        return std::nullopt;
    }
    WorldOptions world = options;
    world.blobPath = path->string();
    return world;
}

OpenWorld Worlds::openFile(const WorldOptions& options, const std::string& key) {
    const std::vector<std::byte> blob = readPipelineFile(options.blobPath);
    OpenWorld world;
    world.dimension =
        dimensions_.open(blob, options.noiseSettings, options.biomeSource, options.seed);
    // Attached only once the dimension opened: a refused open leaves no
    // outbox behind.
    world.fluidUpdates = outboxes_.attach(key);
    return world;
}

OpenWorld Worlds::open(const WorldOptions& options) {
    const WorldOptions world = resolved(options);
    const std::string key = world.key();
    {
        const std::lock_guard<std::mutex> lock(pinsMutex_);
        if (const auto found = pins_.find(key); found != pins_.end()) {
            return found->second.world;
        }
    }
    return openFile(world, key);
}

void Worlds::pin(const WorldOptions& options) {
    const WorldOptions world = resolved(options);
    const std::string key = world.key();
    {
        const std::lock_guard<std::mutex> lock(pinsMutex_);
        if (const auto found = pins_.find(key); found != pins_.end()) {
            ++found->second.count;
            return;
        }
    }
    // Opened outside pinsMutex_, so a worker opening a world already pinned
    // never waits on this compile. The registry still takes its one lock
    // for every lookup and compile (dimension_registry.hpp), so an open of
    // an unpinned world waits on it, and it on theirs.
    OpenWorld opened = openFile(world, key);
    const std::lock_guard<std::mutex> lock(pinsMutex_);
    // Another pin of the same world may have landed meanwhile; the first
    // one's state is the world's.
    Pin& pin = pins_.try_emplace(key, Pin{.world = std::move(opened), .count = 0}).first->second;
    ++pin.count;
}

bool Worlds::unpin(const WorldOptions& options) {
    // A relative path with no data path set names no world anything could
    // have pinned.
    const std::optional<WorldOptions> world = resolvedIfAny(options);
    if (!world.has_value()) {
        return false;
    }
    const std::lock_guard<std::mutex> lock(pinsMutex_);
    const auto found = pins_.find(world->key());
    if (found == pins_.end()) {
        return false;
    }
    if (--found->second.count == 0) {
        pins_.erase(found);
    }
    return true;
}

bool Worlds::pinned(const WorldOptions& options) const {
    const std::optional<WorldOptions> world = resolvedIfAny(options);
    if (!world.has_value()) {
        return false;
    }
    const std::lock_guard<std::mutex> lock(pinsMutex_);
    return pins_.contains(world->key());
}

std::vector<FluidUpdate> Worlds::takeFluidUpdates(const WorldOptions& options,
                                                  const std::int32_t chunkX,
                                                  const std::int32_t chunkZ) {
    // No outbox held for the world means no generator has generated for
    // it: nothing to take, and nothing is created by asking. Nor can one
    // have, under a relative path with no data path to read it against.
    const std::optional<WorldOptions> world = resolvedIfAny(options);
    if (!world.has_value()) {
        return {};
    }
    const auto outbox = outboxes_.find(world->key());
    return outbox == nullptr ? std::vector<FluidUpdate>{} : outbox->take(chunkX, chunkZ);
}

} // namespace stratum::pmmp
