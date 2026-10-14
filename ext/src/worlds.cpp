// Stratum — every world the process generates.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum_pmmp/worlds.hpp>

#include <utility>

namespace stratum::pmmp {

std::string WorldOptions::key() const {
    return fluidUpdateWorldKey(blobPath, noiseSettings, biomeSource, seed);
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
    const std::string key = options.key();
    {
        const std::lock_guard<std::mutex> lock(pinsMutex_);
        if (const auto found = pins_.find(key); found != pins_.end()) {
            return found->second.world;
        }
    }
    return openFile(options, key);
}

void Worlds::pin(const WorldOptions& options) {
    const std::string key = options.key();
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
    OpenWorld opened = openFile(options, key);
    const std::lock_guard<std::mutex> lock(pinsMutex_);
    // Another pin of the same world may have landed meanwhile; the first
    // one's state is the world's.
    Pin& pin = pins_.try_emplace(key, Pin{.world = std::move(opened), .count = 0}).first->second;
    ++pin.count;
}

bool Worlds::unpin(const WorldOptions& options) {
    const std::lock_guard<std::mutex> lock(pinsMutex_);
    const auto found = pins_.find(options.key());
    if (found == pins_.end()) {
        return false;
    }
    if (--found->second.count == 0) {
        pins_.erase(found);
    }
    return true;
}

bool Worlds::pinned(const WorldOptions& options) const {
    const std::lock_guard<std::mutex> lock(pinsMutex_);
    return pins_.contains(options.key());
}

std::vector<FluidUpdate> Worlds::takeFluidUpdates(const WorldOptions& options,
                                                  const std::int32_t chunkX,
                                                  const std::int32_t chunkZ) {
    // No outbox held for the world means no generator has generated for
    // it: nothing to take, and nothing is created by asking.
    const auto outbox = outboxes_.find(options.key());
    return outbox == nullptr ? std::vector<FluidUpdate>{} : outbox->take(chunkX, chunkZ);
}

} // namespace stratum::pmmp
