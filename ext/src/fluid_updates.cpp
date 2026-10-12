// Stratum — fluid updates, carried from PocketMine-MP's generation workers to
// the thread that can schedule them.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum_pmmp/fluid_updates.hpp>

namespace stratum::pmmp {

void FluidUpdateOutbox::put(const std::int32_t chunkX, const std::int32_t chunkZ,
                            std::vector<FluidUpdate> updates) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (updates.empty()) {
        held_.erase({chunkX, chunkZ});
        return;
    }
    held_.insert_or_assign({chunkX, chunkZ}, std::move(updates));
}

std::vector<FluidUpdate> FluidUpdateOutbox::take(const std::int32_t chunkX,
                                                 const std::int32_t chunkZ) {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto found = held_.find({chunkX, chunkZ});
    if (found == held_.end()) {
        return {};
    }
    std::vector<FluidUpdate> updates = std::move(found->second);
    held_.erase(found);
    return updates;
}

std::size_t FluidUpdateOutbox::chunksHeld() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return held_.size();
}

std::shared_ptr<FluidUpdateOutbox> FluidUpdateOutboxes::attach(const std::string& world) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (const auto found = outboxes_.find(world); found != outboxes_.end()) {
        if (auto existing = found->second.lock()) {
            return existing;
        }
    }
    // Entries whose outbox has gone are dropped as new ones arrive, so the
    // map never holds more than the worlds open now plus this one.
    std::erase_if(outboxes_, [](const auto& entry) { return entry.second.expired(); });
    auto created = std::make_shared<FluidUpdateOutbox>();
    outboxes_.insert_or_assign(world, created);
    return created;
}

std::shared_ptr<FluidUpdateOutbox> FluidUpdateOutboxes::find(const std::string& world) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto found = outboxes_.find(world);
    return found == outboxes_.end() ? nullptr : found->second.lock();
}

std::string fluidUpdateWorldKey(const std::string& blobPath, const std::string& noiseSettings,
                                const std::string& biomeParameterList, const std::int64_t seed) {
    // Each part prefixed by its length, so no choice of strings can make two
    // different tuples spell the same key.
    std::string key;
    for (const std::string* part : {&blobPath, &noiseSettings, &biomeParameterList}) {
        key += std::to_string(part->size());
        key += ':';
        key += *part;
    }
    key += std::to_string(seed);
    return key;
}

} // namespace stratum::pmmp
