#pragma once

#include "voxel/dual_contouring.h"
#include "voxel/side_effect_fences.h"
#include "voxel/state_version.h"

#include <cstdint>
#include <optional>
#include <unordered_map>

namespace oro::voxel {

struct AsyncArtifacts {
    uint64_t targetVersion = 0;
    uint64_t generation = 0;
    uint64_t rollbackEpoch = 0;
    std::optional<MeshPatchBatch> mesh;
    bool meshStale = false;
    bool meshHasFallbackChunks = false;
    bool collisionReady = false;
    bool collisionStale = false;
    bool gpuReady = false;
    bool cacheReady = false;
};

struct PublicationTelemetry {
    uint64_t meshPublished = 0;
    uint64_t meshFallbackPublished = 0;
    uint64_t collisionPublished = 0;
    uint64_t meshStaleRejected = 0;
    uint64_t collisionStaleRejected = 0;
};

class AsyncPublication {
public:
    explicit AsyncPublication(VersionFence& fences);

    void stageOutbox(const SideEffectOutbox& outbox, const VersionToken& token);
    void stageMeshResult(const VersionToken& token, std::optional<MeshPatchBatch> mesh, bool stale);
    void stageCollisionResult(const VersionToken& token, bool ready, bool stale);
    void markGpuReady(const VersionToken& token);
    void markCacheReady(const VersionToken& token);

    void setLatestRequiredMeshVersion(uint64_t version);
    void setLatestRequiredCollisionVersion(uint64_t version);

    bool publishMeshDomain(std::optional<MeshPatchBatch>& publishedMesh);
    bool publishCollisionDomain(uint64_t& publishedVersion);
    bool acknowledgeMeshVisible(const VersionToken& token);
    void cancelSuperseded(uint64_t newestTopologyVersion);
    void invalidateByEpochCutoff(uint64_t minEpoch);
    bool hasPendingForVersion(uint64_t version) const;

    uint64_t latestMeshProducedVersion() const { return m_fences.meshProducedVersion; }
    uint64_t latestMeshVisibleVersion() const { return m_fences.meshVisibleVersion; }
    uint64_t latestCollisionVersion() const { return m_fences.collisionVersion; }
    PublicationTelemetry telemetry() const { return m_telemetry; }

private:
    bool meshPublishAllowed(const AsyncArtifacts& entry) const;
    bool collisionPublishAllowed(const AsyncArtifacts& entry) const;
    AsyncArtifacts& entryForToken(const VersionToken& token);
    bool tokenMatches(const AsyncArtifacts& entry, const VersionToken& token) const;

    std::unordered_map<uint64_t, AsyncArtifacts> m_pending;
    VersionFence& m_fences;
    uint64_t m_latestRequiredMeshVersion = 0;
    uint64_t m_latestRequiredCollisionVersion = 0;
    PublicationTelemetry m_telemetry{};
};

}  // namespace oro::voxel
