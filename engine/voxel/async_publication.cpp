#include "voxel/async_publication.h"

namespace oro::voxel {

AsyncPublication::AsyncPublication(VersionFence& fences)
    : m_fences(fences) {}

AsyncArtifacts& AsyncPublication::entryForToken(const VersionToken& token) {
    AsyncArtifacts& entry = m_pending[token.version];
    if (entry.targetVersion != 0 && !tokenMatches(entry, token)) {
        return entry;
    }
    entry.targetVersion = token.version;
    entry.generation = token.generation;
    entry.rollbackEpoch = token.rollbackEpoch;
    return entry;
}

bool AsyncPublication::tokenMatches(const AsyncArtifacts& entry, const VersionToken& token) const {
    return entry.targetVersion == token.version &&
           entry.generation == token.generation &&
           entry.rollbackEpoch == token.rollbackEpoch;
}

void AsyncPublication::stageOutbox(const SideEffectOutbox& outbox, const VersionToken& token) {
    AsyncArtifacts& entry = entryForToken(token);
    if (!tokenMatches(entry, token)) {
        return;
    }
    if (!outbox.collisionArtifacts.empty()) {
        entry.collisionReady = true;
    }
    if (!outbox.gpuUploads.empty()) {
        entry.gpuReady = true;
    }
    if (!outbox.cacheOps.empty()) {
        entry.cacheReady = true;
    }
}

void AsyncPublication::stageMeshResult(const VersionToken& token, std::optional<MeshPatchBatch> mesh, bool stale) {
    AsyncArtifacts& entry = entryForToken(token);
    if (!tokenMatches(entry, token)) {
        ++m_telemetry.meshStaleRejected;
        return;
    }
    entry.meshHasFallbackChunks = false;
    if (mesh.has_value()) {
        for (const ChunkMeshPatch& patch : mesh->patches) {
            if (patch.forceTwoSidedFallback) {
                entry.meshHasFallbackChunks = true;
                break;
            }
        }
    }
    entry.mesh = std::move(mesh);
    entry.meshStale = stale;
}

void AsyncPublication::stageCollisionResult(const VersionToken& token, bool ready, bool stale) {
    AsyncArtifacts& entry = entryForToken(token);
    if (!tokenMatches(entry, token)) {
        ++m_telemetry.collisionStaleRejected;
        return;
    }
    entry.collisionReady = ready;
    entry.collisionStale = stale;
}

void AsyncPublication::markGpuReady(const VersionToken& token) {
    AsyncArtifacts& entry = entryForToken(token);
    if (!tokenMatches(entry, token)) {
        return;
    }
    entry.gpuReady = true;
}

void AsyncPublication::markCacheReady(const VersionToken& token) {
    AsyncArtifacts& entry = entryForToken(token);
    if (!tokenMatches(entry, token)) {
        return;
    }
    entry.cacheReady = true;
}

void AsyncPublication::setLatestRequiredMeshVersion(uint64_t version) {
    m_latestRequiredMeshVersion = version;
}

void AsyncPublication::setLatestRequiredCollisionVersion(uint64_t version) {
    m_latestRequiredCollisionVersion = version;
}

bool AsyncPublication::meshPublishAllowed(const AsyncArtifacts& entry) const {
    if (!entry.mesh.has_value()) {
        return false;
    }
    if (entry.meshStale) {
        return false;
    }
    if (!entry.collisionReady || entry.collisionStale) {
        return false;
    }
    if (entry.targetVersion < m_latestRequiredMeshVersion) {
        return false;
    }
    if (entry.targetVersion < m_latestRequiredCollisionVersion) {
        return false;
    }
    // Strict parity: never expose mesh to renderer before matching collision
    // version is already publishable/published.
    if (entry.targetVersion > m_fences.collisionVersion) {
        return false;
    }
    return entry.targetVersion > m_fences.meshProducedVersion;
}

bool AsyncPublication::collisionPublishAllowed(const AsyncArtifacts& entry) const {
    if (!entry.collisionReady || entry.collisionStale) {
        return false;
    }
    if (entry.targetVersion < m_latestRequiredCollisionVersion) {
        return false;
    }
    return entry.targetVersion > m_fences.collisionVersion;
}

bool AsyncPublication::publishMeshDomain(std::optional<MeshPatchBatch>& publishedMesh) {
    uint64_t bestVersion = 0;
    for (const auto& [version, entry] : m_pending) {
        if (meshPublishAllowed(entry) && version > bestVersion) {
            bestVersion = version;
        }
    }
    if (bestVersion == 0) {
        for (const auto& [version, entry] : m_pending) {
            if (entry.meshStale || (entry.mesh.has_value() && version < m_latestRequiredMeshVersion)) {
                ++m_telemetry.meshStaleRejected;
            }
        }
        return false;
    }
    AsyncArtifacts& chosen = m_pending[bestVersion];
    if (!chosen.mesh.has_value()) {
        return false;
    }
    publishedMesh = std::move(chosen.mesh);
    m_fences.meshProducedVersion = bestVersion;
    ++m_telemetry.meshPublished;
    if (chosen.meshHasFallbackChunks) {
        ++m_telemetry.meshFallbackPublished;
    }
    return true;
}

bool AsyncPublication::publishCollisionDomain(uint64_t& publishedVersion) {
    uint64_t bestVersion = 0;
    for (const auto& [version, entry] : m_pending) {
        if (collisionPublishAllowed(entry) && version > bestVersion) {
            bestVersion = version;
        }
    }
    if (bestVersion == 0) {
        for (const auto& [version, entry] : m_pending) {
            if (entry.collisionStale || (entry.collisionReady && version < m_latestRequiredCollisionVersion)) {
                ++m_telemetry.collisionStaleRejected;
            }
        }
        return false;
    }
    m_fences.collisionVersion = bestVersion;
    publishedVersion = bestVersion;
    ++m_telemetry.collisionPublished;
    return true;
}

bool AsyncPublication::acknowledgeMeshVisible(const VersionToken& token) {
    if (token.version <= m_fences.meshVisibleVersion) {
        return false;
    }
    if (token.version > m_fences.meshProducedVersion) {
        return false;
    }
    auto it = m_pending.find(token.version);
    if (it != m_pending.end()) {
        if (!tokenMatches(it->second, token)) {
            return false;
        }
        it->second.gpuReady = true;
    }
    m_fences.meshVisibleVersion = token.version;
    return true;
}

void AsyncPublication::cancelSuperseded(uint64_t newestTopologyVersion) {
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        const bool olderThanHead = it->first < newestTopologyVersion;
        const bool producedDone = it->first <= m_fences.meshProducedVersion;
        const bool collisionDone = it->first <= m_fences.collisionVersion;
        const bool gpuAcked = it->second.gpuReady || it->first <= m_fences.meshVisibleVersion;
        if (olderThanHead && producedDone && collisionDone && gpuAcked) {
            it = m_pending.erase(it);
        } else {
            ++it;
        }
    }
}

void AsyncPublication::invalidateByEpochCutoff(uint64_t minEpoch) {
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        if (it->second.rollbackEpoch < minEpoch) {
            it = m_pending.erase(it);
        } else {
            ++it;
        }
    }
}

bool AsyncPublication::hasPendingForVersion(uint64_t version) const {
    return m_pending.find(version) != m_pending.end();
}

}  // namespace oro::voxel
