#include "voxel/runtime.h"

#include "core/log.h"

#include <algorithm>
#include <chrono>
#include <unordered_set>
#include <utility>

namespace oro::voxel {

namespace {

using Clock = std::chrono::steady_clock;

double toMs(Clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

}  // namespace

VoxelRuntime::VoxelRuntime()
    : m_publication(m_versions),
      m_workers(),
      m_classifier(EditClassThresholds{}),
      m_gates(QualityGateEvaluator::createDefault()) {}

VoxelRuntime::~VoxelRuntime() {
    m_workers.stop();
}

void VoxelRuntime::initialize() {
    m_world.seedSphere({0, 0, 0}, {1.6F, 1.6F, 1.6F}, 1.45F);
    m_versions = m_world.versions();
    m_workers.start();
    m_workers.setCurrentEpoch(m_rollbackEpoch);
}

bool VoxelRuntime::prePolicyAllowEdit(const EditCommand& command) const {
    (void)command;
    return !m_publicationFrozen;
}

VersionToken VoxelRuntime::makeToken(uint64_t version) {
    VersionToken token{};
    token.version = version;
    token.generation = ++m_generationCounter;
    token.rollbackEpoch = m_rollbackEpoch;
    m_tokensByVersion[version] = token;
    return token;
}

std::optional<VersionToken> VoxelRuntime::tokenForVersion(uint64_t version) const {
    const auto it = m_tokensByVersion.find(version);
    if (it == m_tokensByVersion.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<ChunkCoord> VoxelRuntime::gateRegionForDirty(const std::vector<ChunkCoord>& dirty) const {
    std::vector<ChunkCoord> region = dirty;
    region.reserve(dirty.size() * 2U);
    for (const ChunkCoord& coord : dirty) {
        region.push_back({coord.x + 1, coord.y, coord.z});
        region.push_back({coord.x - 1, coord.y, coord.z});
        region.push_back({coord.x, coord.y + 1, coord.z});
        region.push_back({coord.x, coord.y - 1, coord.z});
        region.push_back({coord.x, coord.y, coord.z + 1});
        region.push_back({coord.x, coord.y, coord.z - 1});
    }
    return region;
}

std::vector<ChunkCoord> VoxelRuntime::seamHaloForDirty(const std::vector<ChunkCoord>& dirty) const {
    std::vector<ChunkCoord> halo;
    halo.reserve(dirty.size() * 7U);
    for (const ChunkCoord& coord : dirty) {
        halo.push_back(coord);
        halo.push_back({coord.x + 1, coord.y, coord.z});
        halo.push_back({coord.x - 1, coord.y, coord.z});
        halo.push_back({coord.x, coord.y + 1, coord.z});
        halo.push_back({coord.x, coord.y - 1, coord.z});
        halo.push_back({coord.x, coord.y, coord.z + 1});
        halo.push_back({coord.x, coord.y, coord.z - 1});
    }
    std::vector<ChunkCoord> unique;
    unique.reserve(halo.size());
    std::unordered_set<ChunkCoord, ChunkCoordHash> seen;
    for (const ChunkCoord& coord : halo) {
        if (seen.insert(coord).second) {
            unique.push_back(coord);
        }
    }
    return unique;
}

JobSnapshotHandle VoxelRuntime::buildSnapshot(uint64_t topologyVersion, const DirtyMetrics& metrics,
                                              const std::vector<ChunkCoord>& dirtyChunks) {
    JobSnapshot snapshot{};
    snapshot.topologyVersion = topologyVersion;
    snapshot.voxelSize = m_world.voxelSize();
    snapshot.metrics = metrics;
    snapshot.dirtyChunks = dirtyChunks;
    snapshot.seamHaloChunks = seamHaloForDirty(dirtyChunks);

    for (const ChunkCoord& coord : snapshot.seamHaloChunks) {
        if (const VoxelChunk* chunk = m_world.findChunk(coord); chunk != nullptr) {
            snapshot.immutableChunks.emplace(coord, *chunk);
        }
    }
    return m_snapshotRegistry.createAndRegister(std::move(snapshot));
}

std::optional<RollbackState> VoxelRuntime::captureRollbackBaseline(const VoxelTransaction& tx, const VersionToken& token) const {
    std::vector<ChunkCoord> touched = tx.touchedChunks();
    if (touched.empty()) {
        return std::nullopt;
    }
    RollbackState state{};
    state.token = token;
    state.baselineTopologyVersion = token.version > 0 ? token.version - 1 : 0;
    state.region = seamHaloForDirty(touched);
    state.chunks.reserve(state.region.size());
    for (const ChunkCoord& coord : state.region) {
        if (const VoxelChunk* chunk = m_world.findChunk(coord); chunk != nullptr) {
            state.chunks.emplace(coord, *chunk);
        } else {
            VoxelChunk empty{};
            empty.coord = coord;
            state.chunks.emplace(coord, empty);
        }
    }
    state.estimatedBytes = (state.chunks.size() * sizeof(VoxelChunk)) + (state.region.size() * sizeof(ChunkCoord));
    return state;
}

bool VoxelRuntime::canPurgeRollbackVersion(uint64_t version, bool pressureMode) const {
    if (m_publicationFrozen) {
        return false;
    }
    if (m_versions.meshVisibleVersion < version || m_versions.collisionVersion < version) {
        return false;
    }
    if (m_publication.hasPendingForVersion(version) || m_snapshotRegistry.contains(version)) {
        return false;
    }
    if (pressureMode) {
        return true;
    }
    const auto criticalityIt = m_criticalityByVersion.find(version);
    const bool isHard = criticalityIt != m_criticalityByVersion.end() && criticalityIt->second == EditCriticality::Hard;
    const uint32_t minTicks = isHard ? m_cleanTicksThresholdHard : m_cleanTicksThresholdSoft;
    const uint64_t minMs = isHard ? m_cleanFloorMsHard : m_cleanFloorMsSoft;
    const uint32_t cleanTicks = m_cleanTicksByVersion.contains(version) ? m_cleanTicksByVersion.at(version) : 0;
    const uint64_t visibleMs = m_visibleSinceMsByVersion.contains(version) ? m_visibleSinceMsByVersion.at(version) : 0;
    return cleanTicks >= minTicks && visibleMs >= minMs;
}

void VoxelRuntime::purgeRollbackCandidates(bool pressureMode) {
    const std::size_t softWatermark = (m_rollbackMemoryBudgetBytes * 80U) / 100U;
    if (m_rollbackStore.usedBytes() <= softWatermark && m_rollbackStore.usedSlots() <= m_rollbackMaxSlots) {
        return;
    }
    const std::vector<uint64_t> versions = m_rollbackStore.versionsOldestFirst();
    for (uint64_t v : versions) {
        if (canPurgeRollbackVersion(v, pressureMode)) {
            m_rollbackStore.erase(v);
            RuntimeDiagnostic diag{};
            diag.phase = "rollback_purge";
            diag.invariantOrPolicyCode = "ROLLBACK_PURGED";
            diag.severity = DiagnosticSeverity::Info;
            diag.topologyVersion = m_versions.topologyVersion;
            diag.meshProducedVersion = m_versions.meshProducedVersion;
            diag.meshVisibleVersion = m_versions.meshVisibleVersion;
            diag.collisionVersion = m_versions.collisionVersion;
            diag.actionTaken = "purge_baseline";
            diag.purgeMode = pressureMode ? "pressure_relaxed" : "normal_strict";
            (void)emitDiagnostic(diag, {});
        }
        if (m_rollbackStore.usedBytes() <= softWatermark && m_rollbackStore.usedSlots() <= m_rollbackMaxSlots) {
            break;
        }
    }
}

bool VoxelRuntime::admitRollbackState(RollbackState state, EditCriticality criticality, std::vector<ChunkCoord> dirty) {
    const std::size_t softWatermark = (m_rollbackMemoryBudgetBytes * 80U) / 100U;
    const std::size_t highWatermark = (m_rollbackMemoryBudgetBytes * 95U) / 100U;

    purgeRollbackCandidates(false);
    if (m_rollbackStore.usedBytes() > highWatermark) {
        purgeRollbackCandidates(true);
    }

    const bool bytesFit = (m_rollbackStore.usedBytes() + state.estimatedBytes) <= m_rollbackMemoryBudgetBytes;
    const bool slotsFit = (m_rollbackStore.usedSlots() + 1U) <= m_rollbackMaxSlots;
    if (!bytesFit || !slotsFit) {
        RuntimeDiagnostic diag{};
        diag.phase = "capture";
        diag.invariantOrPolicyCode = "ROLLBACK_STORE_PRESSURE";
        diag.severity = DiagnosticSeverity::Error;
        diag.topologyVersion = m_versions.topologyVersion;
        diag.meshProducedVersion = m_versions.meshProducedVersion;
        diag.meshVisibleVersion = m_versions.meshVisibleVersion;
        diag.collisionVersion = m_versions.collisionVersion;
        diag.actionTaken = bytesFit ? "reject_by_slots" : "reject_by_bytes";
        (void)emitDiagnostic(diag, dirty);
        return false;
    }
    const uint64_t version = state.token.version;
    if (!m_rollbackStore.put(std::move(state))) {
        return false;
    }
    m_cleanTicksByVersion[version] = 0;
    m_visibleSinceMsByVersion[version] = 0;
    m_criticalityByVersion[version] = criticality;
    (void)softWatermark;
    return true;
}

bool VoxelRuntime::restoreRollbackVersion(uint64_t failedVersion) {
    const std::optional<RollbackState> rollback = m_rollbackStore.get(failedVersion);
    if (!rollback.has_value()) {
        return false;
    }
    for (const auto& [coord, chunk] : rollback->chunks) {
        VoxelChunk& dst = m_world.ensureChunk(coord);
        dst = chunk;
        m_world.markChunkDirty(coord);
    }
    VersionFence recovered = m_versions;
    recovered.topologyVersion = rollback->baselineTopologyVersion;
    recovered.meshProducedVersion = std::min(recovered.meshProducedVersion, recovered.topologyVersion);
    recovered.collisionVersion = std::min(recovered.collisionVersion, recovered.topologyVersion);
    recovered.meshVisibleVersion = std::min(recovered.meshVisibleVersion, recovered.meshProducedVersion);
    m_world.forceVersionFence(recovered);
    m_versions = m_world.versions();
    return true;
}

RuntimeDiagnostic VoxelRuntime::emitDiagnostic(RuntimeDiagnostic base, const std::vector<ChunkCoord>& chunks, bool hardLog) {
    RuntimeDiagnostic diag = m_gates.buildDiagnostic(m_diagnosticsTier, base, chunks);
    m_recentDiagnostics.push_back(diag);
    if (m_recentDiagnostics.size() > 64U) {
        m_recentDiagnostics.erase(m_recentDiagnostics.begin());
    }
    if (hardLog) {
        ORO_LOG_WARN("VoxelDiag phase=%s code=%s action=%s topo=%llu produced=%llu visible=%llu collision=%llu payloadCount=%u",
                     diag.phase,
                     diag.invariantOrPolicyCode,
                     diag.actionTaken,
                     static_cast<unsigned long long>(diag.topologyVersion),
                     static_cast<unsigned long long>(diag.meshProducedVersion),
                     static_cast<unsigned long long>(diag.meshVisibleVersion),
                     static_cast<unsigned long long>(diag.collisionVersion),
                     diag.payload.chunkCount);
    }
    return diag;
}

void VoxelRuntime::freezeRuntime(const char* reasonCode, const char* actionTaken, const std::vector<ChunkCoord>& chunks) {
    m_publicationFrozen = true;
    m_cleanRevalidationTicks = 0;
    m_frozenElapsedMs = 0;
    ++m_rollbackEpoch;
    m_workers.setCurrentEpoch(m_rollbackEpoch);
    m_publication.invalidateByEpochCutoff(m_rollbackEpoch);

    RuntimeDiagnostic diag{};
    diag.phase = "freeze";
    diag.invariantOrPolicyCode = reasonCode;
    diag.severity = DiagnosticSeverity::Error;
    diag.topologyVersion = m_versions.topologyVersion;
    diag.meshProducedVersion = m_versions.meshProducedVersion;
    diag.meshVisibleVersion = m_versions.meshVisibleVersion;
    diag.collisionVersion = m_versions.collisionVersion;
    diag.actionTaken = actionTaken;
    (void)emitDiagnostic(diag, chunks);
}

bool VoxelRuntime::evaluateFrozenRecovery(uint64_t deltaMs) {
    if (!m_publicationFrozen) {
        return true;
    }
    m_frozenElapsedMs += deltaMs;
    const bool coherent = m_versions.meshProducedVersion <= m_versions.topologyVersion &&
                          m_versions.collisionVersion <= m_versions.topologyVersion &&
                          m_versions.meshVisibleVersion <= m_versions.meshProducedVersion;
    if (coherent) {
        ++m_cleanRevalidationTicks;
    } else {
        m_cleanRevalidationTicks = 0;
    }
    const bool cleanWindow = m_cleanRevalidationTicks >= m_cleanTicksThresholdSoft && m_frozenElapsedMs >= m_cleanFloorMsSoft;
    if (cleanWindow) {
        m_publicationFrozen = false;
        m_frozenElapsedMs = 0;
        m_cleanRevalidationTicks = 0;
        RuntimeDiagnostic diag{};
        diag.phase = "recovery";
        diag.invariantOrPolicyCode = "RECOVERY_WINDOW_REACHED";
        diag.severity = DiagnosticSeverity::Info;
        diag.topologyVersion = m_versions.topologyVersion;
        diag.meshProducedVersion = m_versions.meshProducedVersion;
        diag.meshVisibleVersion = m_versions.meshVisibleVersion;
        diag.collisionVersion = m_versions.collisionVersion;
        diag.actionTaken = "unfreeze";
        (void)emitDiagnostic(diag, {});
    }
    return !m_publicationFrozen;
}

bool VoxelRuntime::processEdit(const EditCommand& command, uint64_t deltaMs) {
    VoxelTransaction tx(m_world);
    if (!tx.begin(command)) {
        ORO_LOG_WARN("Voxel transaction begin failed");
        return false;
    }
    if (!prePolicyAllowEdit(command)) {
        RuntimeDiagnostic diag{};
        diag.phase = "pre_policy";
        diag.invariantOrPolicyCode = "FROZEN_REJECT_EDIT";
        diag.severity = DiagnosticSeverity::Warn;
        diag.topologyVersion = m_versions.topologyVersion;
        diag.meshProducedVersion = m_versions.meshProducedVersion;
        diag.meshVisibleVersion = m_versions.meshVisibleVersion;
        diag.collisionVersion = m_versions.collisionVersion;
        diag.actionTaken = "reject_edit";
        (void)emitDiagnostic(diag, tx.touchedChunks(), false);
        tx.rollback();
        return false;
    }
    const VersionToken token = makeToken(tx.targetVersion());
    const std::optional<RollbackState> baseline = captureRollbackBaseline(tx, token);
    if (!baseline.has_value() || !admitRollbackState(*baseline, command.criticality, tx.touchedChunks())) {
        tx.rollback();
        return false;
    }

    const auto t0 = Clock::now();
    if (!tx.apply()) {
        tx.rollback();
        ORO_LOG_WARN("Voxel transaction apply failed");
        return false;
    }
    const TransactionResult commit = tx.commit();
    const auto t2 = Clock::now();
    if (!commit.success) {
        ORO_LOG_WARN("Voxel transaction commit failed with %zu invariant failures", commit.failures.size());
        freezeRuntime("COMMIT_INVARIANT_FAILURE", "freeze_on_commit_fail", tx.touchedChunks());
        return false;
    }
    m_versions.topologyVersion = commit.committedTopologyVersion;
    m_tokensByVersion[commit.committedTopologyVersion] = token;
    m_criticalityByVersion[commit.committedTopologyVersion] = command.criticality;

    SideEffectFence& sideEffects = tx.sideEffects();
    auto outbox = sideEffects.publish();
    if (outbox.has_value()) {
        m_publication.stageOutbox(*outbox, token);
        m_publication.setLatestRequiredMeshVersion(commit.committedTopologyVersion);
        m_publication.setLatestRequiredCollisionVersion(commit.committedTopologyVersion);
        m_workers.setLatestRequiredMeshVersion(commit.committedTopologyVersion);
        m_workers.setLatestRequiredCollisionVersion(commit.committedTopologyVersion);

        JobSnapshotHandle snapshot = buildSnapshot(commit.committedTopologyVersion, commit.metrics, commit.dirtyChunks);
        m_workers.enqueueRemesh(snapshot, token);
        m_workers.enqueueCollision(snapshot, token);

        const double txMs = toMs(t2 - t0);
        m_latencyByVersion[commit.committedTopologyVersion].transactionMs = txMs;

        const std::vector<ChunkCoord> region = gateRegionForDirty(commit.dirtyChunks);
        m_hardGate.start(command, commit.committedTopologyVersion, region);
        m_hardGate.tick(deltaMs, m_versions);

        const EditSizeClass klass = m_classifier.classify(commit.metrics);
        const std::vector<GateViolation> latencyViolations = m_gates.evaluateLatency(klass, m_latencySamples);
        if (!latencyViolations.empty()) {
            ORO_LOG_WARN("Latency gate warnings/failures: %zu", latencyViolations.size());
        }
    }
    return true;
}

void VoxelRuntime::processWorkerResults() {
    while (true) {
        const std::optional<RemeshJobResult> remeshResult = m_workers.popRemeshResult();
        if (!remeshResult.has_value()) {
            break;
        }
        if (remeshResult->skippedByEpoch) {
            RuntimeDiagnostic diag{};
            diag.phase = "worker";
            diag.invariantOrPolicyCode = "EPOCH_MISMATCH";
            diag.severity = DiagnosticSeverity::Info;
            diag.topologyVersion = m_versions.topologyVersion;
            diag.meshProducedVersion = m_versions.meshProducedVersion;
            diag.meshVisibleVersion = m_versions.meshVisibleVersion;
            diag.collisionVersion = m_versions.collisionVersion;
            diag.actionTaken = "skip_by_epoch";
            (void)emitDiagnostic(diag, {}, false);
        }
        if (remeshResult->stale) {
            m_publication.stageMeshResult(remeshResult->token, std::nullopt, true);
            continue;
        }
        m_latencyByVersion[remeshResult->token.version].remeshMs = remeshResult->computeMs;
        const MeshStructuralStats stats = m_mesher.validateStructural(remeshResult->mesh, m_world.voxelSize());
        const std::vector<GateViolation> structuralViolations = m_gates.evaluateStructural(stats);
        if (!structuralViolations.empty()) {
            ORO_LOG_ERROR("Structural gate failure count: %zu for version=%llu",
                          structuralViolations.size(),
                          static_cast<unsigned long long>(remeshResult->token.version));
            m_publication.stageMeshResult(remeshResult->token, std::nullopt, true);
            (void)restoreRollbackVersion(remeshResult->token.version);
            freezeRuntime("MESH_STRUCTURAL_FAIL", "restore_and_freeze", {});
            continue;
        }
        m_publication.stageMeshResult(remeshResult->token, remeshResult->mesh, false);
    }

    while (true) {
        const std::optional<CollisionJobResult> collisionResult = m_workers.popCollisionResult();
        if (!collisionResult.has_value()) {
            break;
        }
        if (collisionResult->skippedByEpoch) {
            RuntimeDiagnostic diag{};
            diag.phase = "worker";
            diag.invariantOrPolicyCode = "EPOCH_MISMATCH";
            diag.severity = DiagnosticSeverity::Info;
            diag.topologyVersion = m_versions.topologyVersion;
            diag.meshProducedVersion = m_versions.meshProducedVersion;
            diag.meshVisibleVersion = m_versions.meshVisibleVersion;
            diag.collisionVersion = m_versions.collisionVersion;
            diag.actionTaken = "skip_by_epoch";
            (void)emitDiagnostic(diag, {}, false);
        }
        m_latencyByVersion[collisionResult->token.version].collisionMs = collisionResult->computeMs;
        m_publication.stageCollisionResult(collisionResult->token, collisionResult->ready, collisionResult->stale);
    }

    uint64_t publishedCollisionVersion = 0;
    if (m_publication.publishCollisionDomain(publishedCollisionVersion)) {
        m_world.publishCollisionVersion(publishedCollisionVersion);
        m_versions.collisionVersion = m_world.versions().collisionVersion;
    }

    std::optional<MeshBuffers> publishedMesh;
    if (m_publication.publishMeshDomain(publishedMesh) && publishedMesh.has_value()) {
        const auto publishStart = Clock::now();
        const uint64_t publishedVersion = publishedMesh->sourceTopologyVersion;
        m_world.publishMeshProducedVersion(publishedVersion);
        m_versions.meshProducedVersion = m_world.versions().meshProducedVersion;
        if (const std::optional<VersionToken> token = tokenForVersion(publishedVersion); token.has_value()) {
            m_publishedMeshSnapshots.push_back({*token, std::move(*publishedMesh)});
        }
        const auto publishEnd = Clock::now();
        LatencySample sample = m_latencyByVersion[publishedVersion];
        sample.publishMs = toMs(publishEnd - publishStart);
        sample.endToEndMs = sample.transactionMs + sample.remeshMs + sample.collisionMs + sample.publishMs;
        m_latencySamples.push_back(sample);
        m_latencyByVersion.erase(publishedVersion);
    }

    m_publication.cancelSuperseded(m_world.versions().topologyVersion);
    m_snapshotRegistry.pruneReleased();
}

void VoxelRuntime::tick(uint64_t deltaMs) {
    processWorkerResults();
    (void)evaluateFrozenRecovery(deltaMs);
    for (auto& [version, visibleMs] : m_visibleSinceMsByVersion) {
        if (version <= m_versions.meshVisibleVersion) {
            visibleMs += deltaMs;
        }
    }
    for (auto& [version, cleanTicks] : m_cleanTicksByVersion) {
        if (version <= m_versions.meshVisibleVersion && version <= m_versions.collisionVersion) {
            ++cleanTicks;
        }
    }
    purgeRollbackCandidates(false);

    ++m_frameCounter;
    if (!m_publicationFrozen && m_frameCounter % 180U == 0U) {
        EditCommand command{};
        command.mode = (m_frameCounter % 360U == 0U) ? EditMode::Fill : EditMode::Carve;
        command.criticality = EditCriticality::Hard;
        command.center = {1.6F, 1.6F, 1.6F};
        command.radius = 0.6F;
        (void)processEdit(command, deltaMs);
    }
    m_hardGate.tick(deltaMs, m_versions);
}

bool VoxelRuntime::runDeterministicGateSmoke() {
    const std::vector<CiScenario> scenarios = m_ci.buildDeterministicScenarios();
    bool allOk = true;
    for (std::size_t i = 0; i < scenarios.size() && i < 8U; ++i) {
        if (!processEdit(scenarios[i].command, 16U)) {
            allOk = false;
            break;
        }
    }

    const LaneReport tail = m_lanes.runTailSpikeLane(EditSizeClass::Small, m_latencySamples, m_gates);
    const LaneReport snapshotLife = m_lanes.runSnapshotLifetimeLane();
    const LaneReport staleReject = m_lanes.runStalePublishRejectLane();
    const LaneReport payloadPublish = m_lanes.runMeshPayloadPublishLane();
    const LaneReport meshAhead = m_lanes.runMeshAheadCollisionLane();
    const LaneReport deadlock = m_lanes.runDeadlockLane();
    const LaneReport fault = m_lanes.runFaultInjectionLane();
    allOk = allOk && tail.pass && snapshotLife.pass && staleReject.pass && payloadPublish.pass &&
            meshAhead.pass && deadlock.pass && fault.pass;
    return allOk;
}

std::optional<VoxelRuntime::PublishedMeshSnapshot> VoxelRuntime::popPublishedMeshSnapshot() {
    if (m_publishedMeshSnapshots.empty()) {
        return std::nullopt;
    }
    PublishedMeshSnapshot snapshot = std::move(m_publishedMeshSnapshots.front());
    m_publishedMeshSnapshots.pop_front();
    return snapshot;
}

bool VoxelRuntime::onRendererMeshVisible(uint64_t sourceVersion) {
    const std::optional<VersionToken> token = tokenForVersion(sourceVersion);
    if (!token.has_value()) {
        return false;
    }
    if (!m_publication.acknowledgeMeshVisible(*token)) {
        return false;
    }
    m_versions.meshVisibleVersion = sourceVersion;
    if (!m_visibleSinceMsByVersion.contains(sourceVersion)) {
        m_visibleSinceMsByVersion[sourceVersion] = 0;
    }
    ORO_LOG_DEBUG("Runtime acknowledged renderer-visible mesh version=%llu",
                  static_cast<unsigned long long>(sourceVersion));
    return true;
}

}  // namespace oro::voxel
