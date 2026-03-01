#pragma once

#include "voxel/edit_types.h"
#include "voxel/state_version.h"
#include "voxel/voxel_world.h"

#include <cstdint>
#include <vector>

namespace oro::voxel {

enum class GateState {
    Idle = 0,
    Pending,
    Warned,
    Degraded,
    Failover,
    Resolved,
};

struct GateThresholdsMs {
    uint32_t warn = 50;
    uint32_t degrade = 150;
    uint32_t failover = 500;
};

struct GateTelemetry {
    uint64_t gateDurationMs = 0;
    uint64_t timeoutCount = 0;
    uint64_t failoverCount = 0;
    uint64_t blockedInteractionCount = 0;
};

class HardEditGate {
public:
    explicit HardEditGate(GateThresholdsMs thresholds = {});

    void start(const EditCommand& command, uint64_t targetTopologyVersion, std::vector<ChunkCoord> gateRegion);
    void tick(uint64_t elapsedMs, const VersionFence& versions);
    bool isInteractionBlocked(const ChunkCoord& coord) const;

    GateState state() const { return m_state; }
    const GateTelemetry& telemetry() const { return m_telemetry; }
    uint64_t targetVersion() const { return m_targetVersion; }

private:
    bool inGateRegion(const ChunkCoord& coord) const;

    GateThresholdsMs m_thresholds;
    GateState m_state = GateState::Idle;
    uint64_t m_elapsedMs = 0;
    uint64_t m_targetVersion = 0;
    EditCriticality m_criticality = EditCriticality::Soft;
    std::vector<ChunkCoord> m_region;
    GateTelemetry m_telemetry{};
};

}  // namespace oro::voxel
