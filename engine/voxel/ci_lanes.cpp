#include "voxel/ci_lanes.h"
#include "voxel/ci_protocol.h"

namespace oro::voxel {

LaneReport CiRegressionLanes::runTailSpikeLane(EditSizeClass klass, const std::vector<LatencySample>& samples,
                                               const QualityGateEvaluator& evaluator) const {
    const std::vector<GateViolation> violations = evaluator.evaluateLatency(klass, samples);
    return {violations.empty(), "tail_spike", violations.size()};
}

LaneReport CiRegressionLanes::runSnapshotLifetimeLane() const {
    SnapshotRegistry registry;
    JobSnapshotHandle handle;
    {
        JobSnapshot snapshot{};
        snapshot.topologyVersion = 7;
        handle = registry.createAndRegister(std::move(snapshot));
    }
    const bool acquiredWhileHeld = static_cast<bool>(registry.acquire(7));
    handle.reset();
    registry.pruneReleased();
    const bool releasedAfterDrop = !registry.contains(7);
    const bool pass = acquiredWhileHeld && releasedAfterDrop;
    return {pass, "snapshot_lifetime", pass ? 0U : 1U};
}

LaneReport CiRegressionLanes::runStalePublishRejectLane() const {
    VersionFence fences{};
    AsyncPublication publication(fences);
    publication.setLatestRequiredMeshVersion(5);
    MeshPatchBatch staleBatch{};
    staleBatch.sourceTopologyVersion = 4;
    publication.stageMeshResult(VersionToken{4, 1, 0}, staleBatch, false);
    std::optional<MeshPatchBatch> published;
    const bool any = publication.publishMeshDomain(published);
    const bool pass = !any && fences.meshProducedVersion == 0;
    return {pass, "stale_publish_reject", pass ? 0U : 1U};
}

LaneReport CiRegressionLanes::runShapeDiversityLane() const {
    CiProtocol protocol{};
    const std::vector<CiScenario> scenarios = protocol.buildDeterministicScenarios();
    bool sawSphere = false;
    bool sawEllipsoid = false;
    bool sawNoisyStone = false;
    for (const CiScenario& scenario : scenarios) {
        if (scenario.command.shape == EditShape::Sphere) {
            sawSphere = true;
        } else if (scenario.command.shape == EditShape::Ellipsoid) {
            sawEllipsoid = true;
        } else if (scenario.command.shape == EditShape::NoisyStone) {
            sawNoisyStone = true;
        }
        if (sawSphere && sawEllipsoid && sawNoisyStone) {
            break;
        }
    }
    const bool pass = sawSphere && sawEllipsoid && sawNoisyStone;
    return {pass, "shape_diversity", pass ? 0U : 1U};
}

LaneReport CiRegressionLanes::runMeshPayloadPublishLane() const {
    VersionFence fences{};
    AsyncPublication publication(fences);
    publication.setLatestRequiredMeshVersion(6);

    MeshPatchBatch batch{};
    batch.sourceTopologyVersion = 6;
    ChunkMeshPatch patch{};
    patch.coord = {0, 0, 0};
    patch.mesh.sourceTopologyVersion = 6;
    patch.mesh.vertices.push_back({});
    patch.mesh.vertices.push_back({});
    patch.mesh.indices = {0U, 1U, 0U};
    batch.patches.push_back(std::move(patch));
    const VersionToken token{6, 1, 0};
    publication.stageMeshResult(token, batch, false);
    publication.stageCollisionResult(token, true, false);
    uint64_t collisionVersion = 0;
    const bool collisionPublished = publication.publishCollisionDomain(collisionVersion);

    std::optional<MeshPatchBatch> published;
    const bool firstPublished = publication.publishMeshDomain(published);
    const bool payloadIntact = published.has_value() &&
                               published->sourceTopologyVersion == 6 &&
                               published->patches.size() == 1U &&
                               published->patches.front().mesh.vertices.size() == 2U &&
                               published->patches.front().mesh.indices.size() == 3U;

    std::optional<MeshPatchBatch> duplicate;
    const bool duplicatePublished = publication.publishMeshDomain(duplicate);
    const bool duplicateRejected = !duplicatePublished;
    const bool pass = collisionPublished && firstPublished && payloadIntact && duplicateRejected &&
                      fences.meshProducedVersion == 6 && fences.collisionVersion == 6;
    return {pass, "mesh_payload_publish", pass ? 0U : 1U};
}

LaneReport CiRegressionLanes::runMeshAheadCollisionLane() const {
    VersionFence fences{};
    AsyncPublication publication(fences);
    publication.setLatestRequiredMeshVersion(3);
    publication.setLatestRequiredCollisionVersion(3);
    MeshPatchBatch batch{};
    batch.sourceTopologyVersion = 3;
    const VersionToken token{3, 1, 0};
    publication.stageMeshResult(token, batch, false);
    std::optional<MeshPatchBatch> publishedBeforeCollision;
    const bool meshPublishedBeforeCollision = publication.publishMeshDomain(publishedBeforeCollision);

    publication.stageCollisionResult(token, true, false);
    uint64_t collisionVersion = 0;
    const bool collisionPublished = publication.publishCollisionDomain(collisionVersion);
    std::optional<MeshPatchBatch> publishedAfterCollision;
    const bool meshPublishedAfterCollision = publication.publishMeshDomain(publishedAfterCollision);
    const bool pass = !meshPublishedBeforeCollision && collisionPublished && meshPublishedAfterCollision &&
                      fences.meshProducedVersion == 3 && fences.collisionVersion == 3;
    return {pass, "mesh_ahead_collision", pass ? 0U : 1U};
}

LaneReport CiRegressionLanes::runDeadlockLane() {
    HardEditGate gate{};
    EditCommand hard{};
    hard.criticality = EditCriticality::Hard;
    gate.start(hard, 3, {ChunkCoord{0, 0, 0}});

    VersionFence versions{};
    versions.topologyVersion = 3;
    versions.collisionVersion = 0;
    gate.tick(600, versions);
    const bool progressed = gate.state() == GateState::Failover;
    return {progressed, "deadlock", progressed ? 0U : 1U};
}

LaneReport CiRegressionLanes::runFaultInjectionLane() {
    SideEffectFence fence;
    fence.begin(5);
    SideEffectOutbox* outbox = fence.writable();
    if (outbox != nullptr) {
        outbox->meshArtifacts.push_back("mesh_chunk_0");
        outbox->collisionArtifacts.push_back("collision_chunk_0");
    }
    fence.discard();
    const bool pass = !fence.isOpen();
    return {pass, "fault_injection", pass ? 0U : 1U};
}

}  // namespace oro::voxel
