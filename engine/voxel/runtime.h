#pragma once

#include "voxel/async_publication.h"
#include "voxel/ci_lanes.h"
#include "voxel/ci_protocol.h"
#include "voxel/dual_contouring.h"
#include "voxel/edit_classifier.h"
#include "voxel/hard_edit_gate.h"
#include "voxel/quality_gates.h"
#include "voxel/rollback_state_store.h"
#include "voxel/snapshot_registry.h"
#include "voxel/transaction.h"
#include "voxel/voxel_world.h"
#include "voxel/worker_runtime.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>
#include <vector>

namespace oro::voxel {

class VoxelRuntime {
public:
    struct RuntimeCounters {
        uint64_t seedRemeshEnqueued = 0;
        uint64_t seedPublishCompleted = 0;
        uint64_t droppedSeamQuads = 0;
        uint64_t qefIllConditioned = 0;
        uint64_t qefFallbacks = 0;
        uint64_t seamRejectedBatches = 0;
        uint64_t fallbackFlaggedChunks = 0;
        uint64_t fallbackPublishedBatches = 0;
        uint64_t redistancePasses = 0;
        uint64_t redistanceVoxelsConsidered = 0;
        uint64_t redistanceNarrowBandVoxelsUpdated = 0;
        double redistanceComputeMsTotal = 0.0;
        double redistanceComputeMsMax = 0.0;
    };

    VoxelRuntime();
    ~VoxelRuntime();

    void initialize();
    void tick(uint64_t deltaMs);
    bool waitForSeedBootstrapPublish(uint64_t timeoutMs);

    bool runDeterministicGateSmoke();
    const VersionFence& versions() const { return m_versions; }
    const RuntimeCounters& counters() const { return m_counters; }
    void setHighFidelitySphereSmoothingEnabled(bool enabled) { m_highFidelitySphereSmoothingEnabled = enabled; }
    bool highFidelitySphereSmoothingEnabled() const { return m_highFidelitySphereSmoothingEnabled; }
    bool onRendererMeshVisible(uint64_t sourceVersion);
    struct PublishedMeshSnapshot {
        VersionToken token{};
        MeshPatchBatch patchBatch;
        float voxelSize = kDefaultVoxelSize;
    };
    std::optional<PublishedMeshSnapshot> popPublishedMeshSnapshot();

private:
    bool processEdit(const EditCommand& command, uint64_t deltaMs);
    bool prePolicyAllowEdit(const EditCommand& command) const;
    std::vector<ChunkCoord> gateRegionForDirty(const std::vector<ChunkCoord>& dirty) const;
    std::vector<ChunkCoord> seamHaloForDirty(const std::vector<ChunkCoord>& dirty) const;
    JobSnapshotHandle buildSnapshot(uint64_t topologyVersion, const DirtyMetrics& metrics,
                                    const std::vector<ChunkCoord>& dirtyChunks);
    void processWorkerResults();
    std::optional<RollbackState> captureRollbackBaseline(const VoxelTransaction& tx, const VersionToken& token) const;
    bool admitRollbackState(RollbackState state, EditCriticality criticality, std::vector<ChunkCoord> dirty);
    void purgeRollbackCandidates(bool pressureMode);
    bool canPurgeRollbackVersion(uint64_t version, bool pressureMode) const;
    bool restoreRollbackVersion(uint64_t failedVersion);
    void freezeRuntime(const char* reasonCode, const char* actionTaken, const std::vector<ChunkCoord>& chunks);
    bool evaluateFrozenRecovery(uint64_t deltaMs);
    RuntimeDiagnostic emitDiagnostic(RuntimeDiagnostic base, const std::vector<ChunkCoord>& chunks, bool hardLog = true);
    VersionToken makeToken(uint64_t version);
    std::optional<VersionToken> tokenForVersion(uint64_t version) const;

    VoxelWorld m_world;
    VersionFence m_versions{};
    AsyncPublication m_publication;
    SnapshotRegistry m_snapshotRegistry;
    RollbackStateStore m_rollbackStore;
    AsyncWorkerRuntime m_workers;
    DualContouringMesher m_mesher;
    EditClassifier m_classifier;
    QualityGateEvaluator m_gates;
    CiProtocol m_ci;
    CiRegressionLanes m_lanes;
    HardEditGate m_hardGate;
    std::vector<LatencySample> m_latencySamples;
    std::unordered_map<uint64_t, LatencySample> m_latencyByVersion;
    std::deque<PublishedMeshSnapshot> m_publishedMeshSnapshots;
    std::vector<RuntimeDiagnostic> m_recentDiagnostics;
    std::unordered_map<uint64_t, VersionToken> m_tokensByVersion;
    std::unordered_map<uint64_t, EditCriticality> m_criticalityByVersion;
    std::unordered_map<uint64_t, uint64_t> m_visibleSinceMsByVersion;
    std::unordered_map<uint64_t, uint32_t> m_cleanTicksByVersion;
    DiagnosticsTier m_diagnosticsTier = DiagnosticsTier::Compact;
    uint64_t m_rollbackEpoch = 0;
    uint64_t m_generationCounter = 0;
    uint64_t m_seedTopologyVersion = 0;
    bool m_publicationFrozen = false;
    uint64_t m_frozenElapsedMs = 0;
    uint32_t m_cleanRevalidationTicks = 0;
    uint32_t m_cleanTicksThresholdSoft = 8;
    uint32_t m_cleanTicksThresholdHard = 16;
    uint64_t m_cleanFloorMsSoft = 200;
    uint64_t m_cleanFloorMsHard = 300;
    std::size_t m_rollbackMemoryBudgetBytes = 256U * 1024U * 1024U;
    std::size_t m_rollbackMaxSlots = 16U;
    uint64_t m_frameCounter = 0;
    RuntimeCounters m_counters{};
    bool m_highFidelitySphereSmoothingEnabled = kDefaultHighFidelitySphereSmoothing;
};

}  // namespace oro::voxel
