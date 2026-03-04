#pragma once

#include "voxel/async_publication.h"
#include "voxel/hard_edit_gate.h"
#include "voxel/quality_gates.h"
#include "voxel/snapshot_registry.h"
#include "voxel/side_effect_fences.h"

#include <vector>

namespace oro::voxel {

struct LaneReport {
    bool pass = true;
    const char* lane = "";
    std::size_t failureCount = 0;
};

class CiRegressionLanes {
public:
    LaneReport runTailSpikeLane(EditSizeClass klass, const std::vector<LatencySample>& samples,
                                const QualityGateEvaluator& evaluator) const;
    LaneReport runSnapshotLifetimeLane() const;
    LaneReport runStalePublishRejectLane() const;
    LaneReport runShapeDiversityLane() const;
    LaneReport runMeshPayloadPublishLane() const;
    LaneReport runMeshAheadCollisionLane() const;
    LaneReport runDeadlockLane();
    LaneReport runFaultInjectionLane();
};

}  // namespace oro::voxel
