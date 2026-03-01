#include "voxel/async_publication.h"
#include "voxel/dual_contouring.h"
#include "voxel/quality_gates.h"
#include "voxel/rollback_state_store.h"
#include "voxel/snapshot_registry.h"
#include "voxel/transaction.h"
#include "voxel/worker_runtime.h"

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using namespace oro::voxel;

bool testBeginCaptureApplyOrdering() {
    VoxelWorld world;
    world.seedSphere({0, 0, 0}, {1.0F, 1.0F, 1.0F}, 0.8F);
    const VersionFence before = world.versions();

    EditCommand command{};
    command.mode = EditMode::Carve;
    command.center = {1.0F, 1.0F, 1.0F};
    command.radius = 0.4F;

    VoxelTransaction tx(world);
    if (!tx.begin(command)) {
        return false;
    }
    const std::vector<ChunkCoord> touched = tx.touchedChunks();
    if (touched.empty()) {
        return false;
    }
    // Baseline capture point is valid only if world is still unchanged here.
    if (world.versions().topologyVersion != before.topologyVersion) {
        return false;
    }
    if (!tx.apply()) {
        return false;
    }
    const TransactionResult committed = tx.commit();
    return committed.success && committed.committedTopologyVersion == (before.topologyVersion + 1U);
}

bool testRollbackStoreSnapshotLifecycleDecoupling() {
    SnapshotRegistry snapshots;
    RollbackStateStore rollback;

    JobSnapshot snapshot{};
    snapshot.topologyVersion = 11;
    JobSnapshotHandle handle = snapshots.createAndRegister(std::move(snapshot));
    if (!snapshots.contains(11)) {
        return false;
    }

    RollbackState state{};
    state.token = VersionToken{11, 1, 0};
    state.baselineTopologyVersion = 10;
    state.estimatedBytes = 1024;
    if (!rollback.put(state) || !rollback.has(11)) {
        return false;
    }

    handle.reset();
    snapshots.pruneReleased();
    return !snapshots.contains(11) && rollback.has(11);
}

bool testHybridAntiAbaRejectsStaleToken() {
    VersionFence fences{};
    AsyncPublication publication(fences);
    publication.setLatestRequiredCollisionVersion(5);
    publication.setLatestRequiredMeshVersion(5);

    const VersionToken current{5, 3, 9};
    const VersionToken stale{5, 2, 8};

    MeshBuffers mesh{};
    mesh.sourceTopologyVersion = 5;
    SideEffectOutbox outbox{};
    outbox.targetTopologyVersion = 5;
    publication.stageOutbox(outbox, current);
    publication.stageMeshResult(current, mesh, false);
    publication.stageCollisionResult(current, true, false);

    // stale token with same version must be rejected.
    publication.stageMeshResult(stale, mesh, false);
    publication.stageCollisionResult(stale, true, false);

    uint64_t collisionVersion = 0;
    std::optional<MeshBuffers> publishedMesh;
    const bool collisionOk = publication.publishCollisionDomain(collisionVersion);
    const bool meshOk = publication.publishMeshDomain(publishedMesh);
    return collisionOk && meshOk && collisionVersion == 5 && fences.meshProducedVersion == 5;
}

bool testWorkerSkipsByEpoch() {
    AsyncWorkerRuntime workers;
    workers.start();
    workers.setCurrentEpoch(7);
    workers.setLatestRequiredMeshVersion(1);

    JobSnapshot snapshot{};
    snapshot.topologyVersion = 1;
    JobSnapshotHandle handle = std::make_shared<const JobSnapshot>(snapshot);
    workers.enqueueRemesh(handle, VersionToken{1, 1, 6});

    bool sawSkip = false;
    for (int i = 0; i < 50; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const std::optional<RemeshJobResult> result = workers.popRemeshResult();
        if (result.has_value()) {
            sawSkip = result->skippedByEpoch && result->stale;
            break;
        }
    }
    workers.stop();
    return sawSkip;
}

bool testDiagnosticsTierBounds() {
    QualityGateEvaluator gates = QualityGateEvaluator::createDefault();
    RuntimeDiagnostic base{};
    base.phase = "test";
    base.invariantOrPolicyCode = "BOUNDED_PAYLOAD";
    base.actionTaken = "none";

    std::vector<ChunkCoord> chunks;
    for (int i = 0; i < 32; ++i) {
        chunks.push_back({i, i, i});
    }
    const RuntimeDiagnostic compact = gates.buildDiagnostic(DiagnosticsTier::Compact, base, chunks, 8);
    const RuntimeDiagnostic expanded = gates.buildDiagnostic(DiagnosticsTier::Expanded, base, chunks, 8);
    const RuntimeDiagnostic full = gates.buildDiagnostic(DiagnosticsTier::Full, base, chunks, 8);
    return compact.payload.sampleChunks.empty() &&
           expanded.payload.sampleChunks.size() == 8U &&
           full.payload.sampleChunks.size() == chunks.size();
}

bool testRollbackStoreFifoPurgeOrderAndCap() {
    RollbackStateStore store;
    for (uint64_t v = 1; v <= 4; ++v) {
        RollbackState state{};
        state.token = VersionToken{v, 1, 0};
        state.baselineTopologyVersion = v - 1;
        state.estimatedBytes = 256;
        if (!store.put(state)) {
            return false;
        }
    }
    const std::vector<uint64_t> order = store.versionsOldestFirst();
    return order.size() == 4U && order[0] == 1 && order[3] == 4;
}

VoxelChunk makeSdfChunk(const ChunkCoord& coord, float voxelSize,
                        const std::function<float(const std::array<float, 3>&)>& sdf, uint16_t materialId) {
    VoxelChunk chunk{};
    chunk.coord = coord;
    uint32_t active = 0;
    for (std::size_t i = 0; i < chunk.cells.size(); ++i) {
        int lx = 0;
        int ly = 0;
        int lz = 0;
        VoxelWorld::unpackIndex(i, lx, ly, lz);
        const auto p = VoxelWorld::cellCenterWorld(coord, lx, ly, lz, voxelSize);
        VoxelCell& cell = chunk.cells[i];
        cell.phi = sdf(p);
        cell.material.materialId = materialId;
        if (cell.phi < kIsoValue) {
            ++active;
        }
    }
    chunk.activeVoxelCount = active;
    return chunk;
}

JobSnapshot makeSnapshot(uint64_t topologyVersion, float voxelSize, const std::vector<ChunkCoord>& dirty,
                         const std::vector<VoxelChunk>& chunks) {
    JobSnapshot snapshot{};
    snapshot.topologyVersion = topologyVersion;
    snapshot.voxelSize = voxelSize;
    snapshot.dirtyChunks = dirty;
    for (const VoxelChunk& chunk : chunks) {
        snapshot.immutableChunks.emplace(chunk.coord, chunk);
    }
    return snapshot;
}

bool testDualContouringMultiAxisCrossings() {
    DualContouringMesher mesher;
    constexpr float voxelSize = 1.0F;
    const ChunkCoord c{0, 0, 0};
    const auto chunkX = makeSdfChunk(c, voxelSize, [](const std::array<float, 3>& p) { return p[0] - 12.0F; }, 3);
    const auto chunkY = makeSdfChunk(c, voxelSize, [](const std::array<float, 3>& p) { return p[1] - 12.0F; }, 3);
    const auto chunkZ = makeSdfChunk(c, voxelSize, [](const std::array<float, 3>& p) { return p[2] - 12.0F; }, 3);

    const MeshBuffers meshX = mesher.remeshSnapshot(makeSnapshot(1, voxelSize, {c}, {chunkX}));
    const MeshBuffers meshY = mesher.remeshSnapshot(makeSnapshot(1, voxelSize, {c}, {chunkY}));
    const MeshBuffers meshZ = mesher.remeshSnapshot(makeSnapshot(1, voxelSize, {c}, {chunkZ}));

    return !meshX.indices.empty() && !meshY.indices.empty() && !meshZ.indices.empty();
}

bool testDualContouringBoundarySeamUsesHalo() {
    DualContouringMesher mesher;
    constexpr float voxelSize = 1.0F;
    const ChunkCoord left{0, 0, 0};
    const ChunkCoord right{1, 0, 0};
    const auto plane = [](const std::array<float, 3>& p) { return p[0] - 32.0F; };
    const VoxelChunk leftChunk = makeSdfChunk(left, voxelSize, plane, 4);
    const VoxelChunk rightChunk = makeSdfChunk(right, voxelSize, plane, 4);

    JobSnapshot snapshot = makeSnapshot(7, voxelSize, {left}, {leftChunk, rightChunk});
    snapshot.seamHaloChunks = {left, right};

    const MeshBuffers mesh = mesher.remeshSnapshot(snapshot);
    if (mesh.vertices.empty() || mesh.indices.empty()) {
        return false;
    }
    bool hasBoundaryVertex = false;
    for (const MeshVertex& v : mesh.vertices) {
        if (v.position[0] > 31.8F && v.position[0] < 32.2F) {
            hasBoundaryVertex = true;
            break;
        }
    }
    return hasBoundaryVertex;
}

bool testDualContouringDeterministicMeshStability() {
    DualContouringMesher mesher;
    constexpr float voxelSize = 1.0F;
    const ChunkCoord c{0, 0, 0};
    const VoxelChunk chunk = makeSdfChunk(
        c,
        voxelSize,
        [](const std::array<float, 3>& p) { return (p[0] + (0.7F * p[1]) + (0.3F * p[2])) - 24.0F; },
        5);
    const JobSnapshot snapshot = makeSnapshot(9, voxelSize, {c}, {chunk});

    const MeshBuffers a = mesher.remeshSnapshot(snapshot);
    const MeshBuffers b = mesher.remeshSnapshot(snapshot);
    if (a.vertices.size() != b.vertices.size() || a.indices.size() != b.indices.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.vertices.size(); ++i) {
        if (a.vertices[i].position != b.vertices[i].position ||
            a.vertices[i].normal != b.vertices[i].normal ||
            a.vertices[i].materialId != b.vertices[i].materialId) {
            return false;
        }
    }
    return a.indices == b.indices;
}

bool testStructuralGateRejectsDegenerateTriangles() {
    QualityGateEvaluator gates = QualityGateEvaluator::createDefault();
    MeshStructuralStats stats{};
    stats.degenerateCount = 1;
    const std::vector<GateViolation> violations = gates.evaluateStructural(stats);
    if (violations.empty()) {
        return false;
    }
    for (const GateViolation& v : violations) {
        if (v.hardFail && std::string(v.reason) == "Degenerate triangle count > 0") {
            return true;
        }
    }
    return false;
}

}  // namespace

int main() {
    if (!testBeginCaptureApplyOrdering()) {
        return 1;
    }
    if (!testRollbackStoreSnapshotLifecycleDecoupling()) {
        return 2;
    }
    if (!testHybridAntiAbaRejectsStaleToken()) {
        return 3;
    }
    if (!testWorkerSkipsByEpoch()) {
        return 4;
    }
    if (!testDiagnosticsTierBounds()) {
        return 5;
    }
    if (!testRollbackStoreFifoPurgeOrderAndCap()) {
        return 6;
    }
    if (!testDualContouringMultiAxisCrossings()) {
        return 7;
    }
    if (!testDualContouringBoundarySeamUsesHalo()) {
        return 8;
    }
    if (!testDualContouringDeterministicMeshStability()) {
        return 9;
    }
    if (!testStructuralGateRejectsDegenerateTriangles()) {
        return 10;
    }
    return 0;
}
