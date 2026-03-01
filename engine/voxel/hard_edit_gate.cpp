#include "voxel/hard_edit_gate.h"

namespace oro::voxel {

HardEditGate::HardEditGate(GateThresholdsMs thresholds)
    : m_thresholds(thresholds) {}

void HardEditGate::start(const EditCommand& command, uint64_t targetTopologyVersion, std::vector<ChunkCoord> gateRegion) {
    m_criticality = command.criticality;
    m_targetVersion = targetTopologyVersion;
    m_region = std::move(gateRegion);
    m_elapsedMs = 0;
    m_state = (m_criticality == EditCriticality::Hard) ? GateState::Pending : GateState::Resolved;
}

void HardEditGate::tick(uint64_t elapsedMs, const VersionFence& versions) {
    if (m_state == GateState::Idle || m_state == GateState::Resolved) {
        return;
    }
    m_elapsedMs += elapsedMs;
    m_telemetry.gateDurationMs = m_elapsedMs;

    if (versions.collisionVersion >= m_targetVersion) {
        m_state = GateState::Resolved;
        return;
    }
    if (m_elapsedMs >= m_thresholds.failover) {
        m_state = GateState::Failover;
        ++m_telemetry.failoverCount;
    } else if (m_elapsedMs >= m_thresholds.degrade) {
        m_state = GateState::Degraded;
        ++m_telemetry.timeoutCount;
    } else if (m_elapsedMs >= m_thresholds.warn) {
        m_state = GateState::Warned;
    }
}

bool HardEditGate::isInteractionBlocked(const ChunkCoord& coord) const {
    if (m_criticality != EditCriticality::Hard || m_state == GateState::Idle || m_state == GateState::Resolved) {
        return false;
    }
    return inGateRegion(coord);
}

bool HardEditGate::inGateRegion(const ChunkCoord& coord) const {
    for (const ChunkCoord& c : m_region) {
        if (c == coord) {
            return true;
        }
    }
    return false;
}

}  // namespace oro::voxel
