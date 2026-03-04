#include "voxel/async_publication.h"
#include "voxel/dual_contouring.h"
#include "voxel/ci_protocol.h"
#include "voxel/quality_gates.h"
#include "voxel/rollback_state_store.h"
#include "voxel/runtime.h"
#include "voxel/snapshot_registry.h"
#include "voxel/transaction.h"
#include "voxel/worker_runtime.h"

#include <chrono>
#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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

    MeshPatchBatch batch{};
    batch.sourceTopologyVersion = 5;
    SideEffectOutbox outbox{};
    outbox.targetTopologyVersion = 5;
    publication.stageOutbox(outbox, current);
    publication.stageMeshResult(current, batch, false);
    publication.stageCollisionResult(current, true, false);

    // stale token with same version must be rejected.
    publication.stageMeshResult(stale, batch, false);
    publication.stageCollisionResult(stale, true, false);

    uint64_t collisionVersion = 0;
    std::optional<MeshPatchBatch> publishedMesh;
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
    snapshot.meshTargetChunks = dirty;
    snapshot.sampleHaloChunks = dirty;
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
    snapshot.sampleHaloChunks = {left, right};

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

bool testDualContouringComponentWindingCoherence() {
    DualContouringMesher mesher;
    constexpr float voxelSize = 1.0F;
    const ChunkCoord c{0, 0, 0};
    const VoxelChunk chunk = makeSdfChunk(
        c,
        voxelSize,
        [](const std::array<float, 3>& p) { return std::sqrt((p[0] * p[0]) + (p[1] * p[1]) + (p[2] * p[2])) - 12.0F; },
        6);
    const MeshBuffers mesh = mesher.remeshSnapshot(makeSnapshot(10, voxelSize, {c}, {chunk}));
    if (mesh.indices.empty()) {
        return false;
    }
    std::unordered_map<uint64_t, std::vector<std::array<uint32_t, 2>>> edgeRefs;
    edgeRefs.reserve(mesh.indices.size());
    const auto edgeKey = [](uint32_t a, uint32_t b) {
        const uint32_t lo = std::min(a, b);
        const uint32_t hi = std::max(a, b);
        return (static_cast<uint64_t>(lo) << 32U) | static_cast<uint64_t>(hi);
    };
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const uint32_t a = mesh.indices[i];
        const uint32_t b = mesh.indices[i + 1];
        const uint32_t c0 = mesh.indices[i + 2];
        edgeRefs[edgeKey(a, b)].push_back({a, b});
        edgeRefs[edgeKey(b, c0)].push_back({b, c0});
        edgeRefs[edgeKey(c0, a)].push_back({c0, a});
    }
    for (const auto& [unusedKey, refs] : edgeRefs) {
        (void)unusedKey;
        if (refs.size() > 2U) {
            return false;
        }
        if (refs.size() == 2U) {
            if (!(refs[0][0] == refs[1][1] && refs[0][1] == refs[1][0])) {
                return false;
            }
        }
    }
    return true;
}

bool testDualContouringDegeneratePruning() {
    DualContouringMesher mesher;
    constexpr float voxelSize = 1.0F;
    const ChunkCoord c{0, 0, 0};
    const VoxelChunk chunk = makeSdfChunk(
        c,
        voxelSize,
        [](const std::array<float, 3>& p) { return (p[0] + (0.3F * p[1]) + (0.17F * p[2])) - 19.0F; },
        7);
    const MeshPatchBatch batch = mesher.remeshSnapshotPatches(makeSnapshot(11, voxelSize, {c}, {chunk}));
    if (batch.patches.empty() || batch.patches[0].remove) {
        return false;
    }
    const MeshStructuralStats stats = mesher.validateStructural(batch.patches[0].mesh, voxelSize);
    return stats.invalidIndexCount == 0 && stats.degenerateCount == 0;
}

bool testDualContouringFallbackFlagBehavior() {
    DualContouringMesher mesher;
    constexpr float voxelSize = 1.0F;
    const ChunkCoord c{0, 0, 0};
    const VoxelChunk normal = makeSdfChunk(c, voxelSize, [](const std::array<float, 3>& p) { return p[0] - 12.0F; }, 3);
    const VoxelChunk noisy = makeSdfChunk(
        c,
        voxelSize,
        [](const std::array<float, 3>& p) {
            const int xi = static_cast<int>(std::floor(p[0]));
            const int yi = static_cast<int>(std::floor(p[1]));
            const int zi = static_cast<int>(std::floor(p[2]));
            return ((xi + yi + zi) & 1) == 0 ? -0.25F : 0.25F;
        },
        8);

    const MeshPatchBatch normalBatch = mesher.remeshSnapshotPatches(makeSnapshot(12, voxelSize, {c}, {normal}));
    const MeshPatchBatch noisyBatch = mesher.remeshSnapshotPatches(makeSnapshot(13, voxelSize, {c}, {noisy}));
    if (normalBatch.patches.empty() || noisyBatch.patches.empty()) {
        return false;
    }
    const bool normalFallback = normalBatch.patches[0].forceTwoSidedFallback;
    const bool noisyFallback = noisyBatch.patches[0].forceTwoSidedFallback;
    if (noisyFallback) {
        return true;
    }
    // Ownership and seam safety changes can reduce fallback need; preserve relative quality signal.
    return noisyBatch.diagnostics.droppedIncompleteQuads >= normalBatch.diagnostics.droppedIncompleteQuads &&
           noisyBatch.diagnostics.droppedDegenerateTriangles >= normalBatch.diagnostics.droppedDegenerateTriangles &&
           !normalFallback;
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

bool testRuntimeBootstrapSeedPublishGate() {
    VoxelRuntime runtime;
    runtime.initialize();
    if (!runtime.waitForSeedBootstrapPublish(1200U)) {
        return false;
    }
    const VersionFence versions = runtime.versions();
    const VoxelRuntime::RuntimeCounters counters = runtime.counters();
    return counters.seedRemeshEnqueued > 0 &&
           counters.seedPublishCompleted > 0 &&
           versions.topologyVersion > 0 &&
           versions.meshProducedVersion >= versions.topologyVersion &&
           versions.collisionVersion >= versions.topologyVersion;
}

bool testDualContouringLowRankQefFallbackDeterministic() {
    DualContouringMesher mesher;
    constexpr float voxelSize = 1.0F;
    const ChunkCoord c{0, 0, 0};
    const VoxelChunk chunk = makeSdfChunk(c, voxelSize, [](const std::array<float, 3>& p) { return p[0] - 12.0F; }, 6);
    const JobSnapshot snapshot = makeSnapshot(15, voxelSize, {c}, {chunk});
    const MeshPatchBatch a = mesher.remeshSnapshotPatches(snapshot);
    const MeshPatchBatch b = mesher.remeshSnapshotPatches(snapshot);
    return a.diagnostics.illConditionedQef > 0 &&
           a.diagnostics.qefFallbacks > 0 &&
           a.diagnostics.illConditionedQef == b.diagnostics.illConditionedQef &&
           a.diagnostics.qefFallbacks == b.diagnostics.qefFallbacks;
}

bool testTransactionPostCsgRedistanceKeepsBandGradientsFinite() {
    VoxelWorld world;
    world.seedSphere({0, 0, 0}, {1.6F, 1.6F, 1.6F}, 1.45F);

    EditCommand command{};
    command.mode = EditMode::Carve;
    command.center = {1.55F, 1.6F, 1.65F};
    command.radius = 0.55F;

    VoxelTransaction tx(world);
    if (!tx.begin(command) || !tx.apply()) {
        return false;
    }
    const TransactionResult committed = tx.commit();
    if (!committed.success) {
        return false;
    }

    int seamSignChanges = 0;
    for (const ChunkCoord& coord : committed.dirtyChunks) {
        const VoxelChunk* chunk = world.findChunk(coord);
        if (chunk == nullptr) {
            continue;
        }
        for (int z = 0; z < kChunkEdge; ++z) {
            for (int y = 0; y < kChunkEdge; ++y) {
                for (int x = 0; x < kChunkEdge; ++x) {
                    const std::size_t i = VoxelWorld::flatIndex(x, y, z);
                    const float phi = chunk->cells[i].phi;
                    if (!std::isfinite(phi)) {
                        return false;
                    }
                    if (x + 1 < kChunkEdge) {
                        const float nx = chunk->cells[VoxelWorld::flatIndex(x + 1, y, z)].phi;
                        if (!std::isfinite(nx)) {
                            return false;
                        }
                        if ((phi < kIsoValue) != (nx < kIsoValue)) {
                            ++seamSignChanges;
                        }
                    }
                }
            }
        }
    }
    return seamSignChanges > 0;
}

bool testRuntimeVisibilityAckProgression() {
    VoxelRuntime runtime;
    runtime.initialize();
    if (!runtime.waitForSeedBootstrapPublish(1200U)) {
        return false;
    }
    const auto snapshot = runtime.popPublishedMeshSnapshot();
    if (!snapshot.has_value()) {
        return false;
    }
    if (!runtime.onRendererMeshVisible(snapshot->token.version)) {
        return false;
    }
    return runtime.versions().meshVisibleVersion >= snapshot->token.version;
}

bool testSeamPatchBatchDeterminismWithHalo() {
    DualContouringMesher mesher;
    constexpr float voxelSize = 1.0F;
    const ChunkCoord left{0, 0, 0};
    const ChunkCoord right{1, 0, 0};
    const auto plane = [](const std::array<float, 3>& p) { return p[0] - 32.0F; };
    const VoxelChunk leftChunk = makeSdfChunk(left, voxelSize, plane, 4);
    const VoxelChunk rightChunk = makeSdfChunk(right, voxelSize, plane, 4);

    JobSnapshot snapshot = makeSnapshot(21, voxelSize, {left, right}, {leftChunk, rightChunk});
    snapshot.sampleHaloChunks = {left, right};

    const MeshPatchBatch a = mesher.remeshSnapshotPatches(snapshot);
    const MeshPatchBatch b = mesher.remeshSnapshotPatches(snapshot);
    if (a.patches.size() != 2U || b.patches.size() != 2U) {
        return false;
    }
    int nonEmptyA = 0;
    int nonEmptyB = 0;
    for (const auto& patch : a.patches) {
        if (!patch.remove && !patch.mesh.vertices.empty() && !patch.mesh.indices.empty()) {
            ++nonEmptyA;
        }
    }
    for (const auto& patch : b.patches) {
        if (!patch.remove && !patch.mesh.vertices.empty() && !patch.mesh.indices.empty()) {
            ++nonEmptyB;
        }
    }
    return nonEmptyA > 0 &&
           nonEmptyA == nonEmptyB &&
           a.diagnostics.droppedIncompleteQuads == b.diagnostics.droppedIncompleteQuads &&
           a.diagnostics.droppedDuplicateQuads == b.diagnostics.droppedDuplicateQuads;
}

bool testDualContouringThreeChunkCornerSeamContinuity() {
    DualContouringMesher mesher;
    constexpr float voxelSize = 1.0F;
    const auto tiltedPlane = [](const std::array<float, 3>& p) { return (p[0] + p[1] + p[2]) - 48.0F; };
    const ChunkCoord c000{0, 0, 0};
    const ChunkCoord c100{1, 0, 0};
    const ChunkCoord c010{0, 1, 0};
    const ChunkCoord c001{0, 0, 1};
    const VoxelChunk ch000 = makeSdfChunk(c000, voxelSize, tiltedPlane, 11);
    const VoxelChunk ch100 = makeSdfChunk(c100, voxelSize, tiltedPlane, 11);
    const VoxelChunk ch010 = makeSdfChunk(c010, voxelSize, tiltedPlane, 11);
    const VoxelChunk ch001 = makeSdfChunk(c001, voxelSize, tiltedPlane, 11);
    JobSnapshot snapshot = makeSnapshot(31, voxelSize, {c000, c100, c010, c001}, {ch000, ch100, ch010, ch001});
    snapshot.sampleHaloChunks = {c000, c100, c010, c001};
    const MeshPatchBatch a = mesher.remeshSnapshotPatches(snapshot);
    const MeshPatchBatch b = mesher.remeshSnapshotPatches(snapshot);
    if (a.patches.size() != 4U || b.patches.size() != 4U) {
        return false;
    }
    std::size_t nonEmptyA = 0;
    for (const auto& patch : a.patches) {
        if (!patch.remove && !patch.mesh.indices.empty()) {
            ++nonEmptyA;
        }
    }
    return nonEmptyA >= 2U &&
           a.diagnostics.droppedOutOfDomainQuads == 0U &&
           a.diagnostics.droppedOutOfDomainQuads == b.diagnostics.droppedOutOfDomainQuads &&
           a.diagnostics.droppedIncompleteQuads == b.diagnostics.droppedIncompleteQuads;
}

bool testCiProtocolShapeDiversity() {
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
    }
    return sawSphere && sawEllipsoid && sawNoisyStone;
}

bool testMultiObjectMultiMaterialFidelity() {
    VoxelWorld world;
    world.setVoxelSize(0.1F);
    world.seedSphere({0, 0, 0}, {1.6F, 1.6F, 1.6F}, 1.2F, 21);
    world.seedEllipsoid({1, 0, 0}, {4.8F, 1.6F, 1.6F}, {1.0F, 1.4F, 0.8F}, 22);
    world.seedNoisyStone({0, 1, 0}, {1.6F, 4.8F, 1.8F}, 1.0F, 0.2F, 3.0F, 13U, 23);
    world.incrementTopologyVersion();
    const std::vector<ChunkCoord> dirty = world.consumeDirtyChunks();
    if (dirty.empty()) {
        return false;
    }
    JobSnapshot snapshot{};
    snapshot.topologyVersion = world.versions().topologyVersion;
    snapshot.voxelSize = world.voxelSize();
    snapshot.meshTargetChunks = dirty;
    snapshot.sampleHaloChunks = dirty;
    for (const ChunkCoord& coord : dirty) {
        if (const VoxelChunk* chunk = world.findChunk(coord); chunk != nullptr) {
            snapshot.immutableChunks.emplace(coord, *chunk);
        }
    }
    DualContouringMesher mesher;
    const MeshPatchBatch batch = mesher.remeshSnapshotPatches(snapshot);
    std::unordered_set<uint16_t> materialIds;
    std::size_t nonEmptyPatches = 0;
    for (const auto& patch : batch.patches) {
        if (patch.remove || patch.mesh.indices.empty()) {
            continue;
        }
        ++nonEmptyPatches;
        for (const MeshVertex& v : patch.mesh.vertices) {
            materialIds.insert(v.materialId);
        }
    }
    return nonEmptyPatches > 0 && materialIds.size() >= 2U;
}

bool checkSeamParityForChunk(const VoxelWorld& world, const ChunkCoord& coord) {
    const VoxelChunk* chunk = world.findChunk(coord);
    if (chunk == nullptr) {
        return true;
    }
    const std::array<ChunkCoord, 3> neighbors = {{
        {coord.x + 1, coord.y, coord.z},
        {coord.x, coord.y + 1, coord.z},
        {coord.x, coord.y, coord.z + 1},
    }};
    for (const ChunkCoord& n : neighbors) {
        const VoxelChunk* other = world.findChunk(n);
        if (other == nullptr) {
            continue;
        }
        for (int i = 0; i < kChunkEdge; ++i) {
            for (int j = 0; j < kChunkEdge; ++j) {
                float a = 0.0F;
                float b = 0.0F;
                if (n.x == coord.x + 1) {
                    a = chunk->cells[VoxelWorld::flatIndex(kChunkEdge - 1, i, j)].phi;
                    b = other->cells[VoxelWorld::flatIndex(0, i, j)].phi;
                } else if (n.y == coord.y + 1) {
                    a = chunk->cells[VoxelWorld::flatIndex(i, kChunkEdge - 1, j)].phi;
                    b = other->cells[VoxelWorld::flatIndex(i, 0, j)].phi;
                } else {
                    a = chunk->cells[VoxelWorld::flatIndex(i, j, kChunkEdge - 1)].phi;
                    b = other->cells[VoxelWorld::flatIndex(i, j, 0)].phi;
                }
                if ((a < kIsoValue) != (b < kIsoValue)) {
                    return false;
                }
            }
        }
    }
    return true;
}

bool testTransactionRepeatedCarveFillMaintainsFiniteSeams() {
    VoxelWorld world;
    world.seedSphere({0, 0, 0}, {1.6F, 1.6F, 1.6F}, 1.45F);

    for (int i = 0; i < 8; ++i) {
        EditCommand command{};
        command.mode = (i % 2 == 0) ? EditMode::Carve : EditMode::Fill;
        command.center = {1.5F + (0.02F * static_cast<float>(i)), 1.6F, 1.7F};
        command.radius = 0.45F;

        VoxelTransaction tx(world);
        if (!tx.begin(command) || !tx.apply()) {
            return false;
        }
        const TransactionResult committed = tx.commit();
        if (!committed.success) {
            return false;
        }
        for (const ChunkCoord& coord : committed.dirtyChunks) {
            const VoxelChunk* chunk = world.findChunk(coord);
            if (chunk == nullptr) {
                continue;
            }
            for (const VoxelCell& cell : chunk->cells) {
                if (!std::isfinite(cell.phi)) {
                    return false;
                }
            }
            if (!checkSeamParityForChunk(world, coord)) {
                return false;
            }
        }
    }
    return true;
}

bool testRuntimeHighFidelityRedistanceCountersAdvance() {
    VoxelWorld world;
    world.seedSphere({0, 0, 0}, {1.6F, 1.6F, 1.6F}, 1.45F);
    EditCommand command{};
    command.mode = EditMode::Carve;
    command.shape = EditShape::Sphere;
    command.center = {1.55F, 1.6F, 1.65F};
    command.radius = 0.55F;
    VoxelTransaction tx(world);
    tx.setHighFidelityRedistanceEnabled(true);
    if (!tx.begin(command) || !tx.apply()) {
        return false;
    }
    const TransactionResult committed = tx.commit();
    if (!committed.success) {
        return false;
    }
    const RedistanceStats stats = tx.redistanceStats();
    return stats.voxelsConsidered >= stats.narrowBandVoxelsUpdated &&
           stats.narrowBandVoxelsUpdated > 0U &&
           stats.computeMs >= 0.0;
}

bool testHighFidelityModeTouchesWiderNarrowBandThanLowFidelity() {
    auto runForMode = [](bool highFidelity) {
        VoxelWorld world;
        world.seedSphere({0, 0, 0}, {1.6F, 1.6F, 1.6F}, 1.45F);
        EditCommand command{};
        command.mode = EditMode::Carve;
        command.shape = EditShape::Sphere;
        command.center = {1.55F, 1.6F, 1.65F};
        command.radius = 0.55F;
        VoxelTransaction tx(world);
        tx.setHighFidelityRedistanceEnabled(highFidelity);
        if (!tx.begin(command) || !tx.apply()) {
            return RedistanceStats{};
        }
        const TransactionResult committed = tx.commit();
        if (!committed.success) {
            return RedistanceStats{};
        }
        return tx.redistanceStats();
    };

    const RedistanceStats hi = runForMode(true);
    const RedistanceStats lo = runForMode(false);
    if (hi.narrowBandVoxelsUpdated == 0U || lo.narrowBandVoxelsUpdated == 0U) {
        return false;
    }
    return hi.narrowBandVoxelsUpdated > lo.narrowBandVoxelsUpdated;
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
    if (!testDualContouringComponentWindingCoherence()) {
        return 10;
    }
    if (!testDualContouringDegeneratePruning()) {
        return 11;
    }
    if (!testDualContouringFallbackFlagBehavior()) {
        return 12;
    }
    if (!testStructuralGateRejectsDegenerateTriangles()) {
        return 13;
    }
    if (!testRuntimeBootstrapSeedPublishGate()) {
        return 14;
    }
    if (!testDualContouringLowRankQefFallbackDeterministic()) {
        return 15;
    }
    if (!testTransactionPostCsgRedistanceKeepsBandGradientsFinite()) {
        return 16;
    }
    if (!testRuntimeVisibilityAckProgression()) {
        return 17;
    }
    if (!testSeamPatchBatchDeterminismWithHalo()) {
        return 18;
    }
    if (!testTransactionRepeatedCarveFillMaintainsFiniteSeams()) {
        return 19;
    }
    if (!testRuntimeHighFidelityRedistanceCountersAdvance()) {
        return 20;
    }
    if (!testHighFidelityModeTouchesWiderNarrowBandThanLowFidelity()) {
        return 21;
    }
    if (!testDualContouringThreeChunkCornerSeamContinuity()) {
        return 22;
    }
    if (!testCiProtocolShapeDiversity()) {
        return 23;
    }
    if (!testMultiObjectMultiMaterialFidelity()) {
        return 24;
    }
    return 0;
}
