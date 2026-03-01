#include "voxel/transaction.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace oro::voxel {

namespace {

constexpr float kMinEditRadius = 0.001F;
constexpr float kSeamEpsilon = 0.0001F;

ChunkCoord neighborCoord(const ChunkCoord& c, int dx, int dy, int dz) {
    return ChunkCoord{c.x + dx, c.y + dy, c.z + dz};
}

bool isInside(float phi) {
    return phi < kIsoValue;
}

}  // namespace

VoxelTransaction::VoxelTransaction(VoxelWorld& world)
    : m_world(world) {}

bool VoxelTransaction::precheck(const EditCommand& command, InvariantFailure& failure) const {
    if (!std::isfinite(command.radius) || command.radius < kMinEditRadius) {
        failure = {InvariantCode::InvalidPhi, 0, 0, 0, "Edit radius is invalid"};
        return false;
    }
    if (!std::isfinite(command.center[0]) || !std::isfinite(command.center[1]) || !std::isfinite(command.center[2])) {
        failure = {InvariantCode::InvalidPhi, 0, 0, 0, "Edit center contains non-finite values"};
        return false;
    }
    return true;
}

bool VoxelTransaction::begin(const EditCommand& command) {
    if (m_open) {
        return false;
    }
    InvariantFailure failure{};
    if (!precheck(command, failure)) {
        return false;
    }
    m_command = command;
    m_metrics = {};
    m_workingChunks.clear();
    m_targetVersion = m_world.versions().topologyVersion + 1;
    m_open = collectTouchedChunks();
    if (!m_open) {
        return false;
    }
    m_sideEffects.begin(m_targetVersion);
    return true;
}

bool VoxelTransaction::collectTouchedChunks() {
    const float chunkWorldEdge = static_cast<float>(kChunkEdge) * m_world.voxelSize();
    const float radius = m_command.radius;
    const auto center = m_command.center;
    const int minChunkX = static_cast<int>(std::floor((center[0] - radius) / chunkWorldEdge));
    const int minChunkY = static_cast<int>(std::floor((center[1] - radius) / chunkWorldEdge));
    const int minChunkZ = static_cast<int>(std::floor((center[2] - radius) / chunkWorldEdge));
    const int maxChunkX = static_cast<int>(std::floor((center[0] + radius) / chunkWorldEdge));
    const int maxChunkY = static_cast<int>(std::floor((center[1] + radius) / chunkWorldEdge));
    const int maxChunkZ = static_cast<int>(std::floor((center[2] + radius) / chunkWorldEdge));

    for (int z = minChunkZ; z <= maxChunkZ; ++z) {
        for (int y = minChunkY; y <= maxChunkY; ++y) {
            for (int x = minChunkX; x <= maxChunkX; ++x) {
                const ChunkCoord coord{x, y, z};
                const VoxelChunk* source = m_world.findChunk(coord);
                if (source == nullptr) {
                    VoxelChunk created{};
                    created.coord = coord;
                    auto [it, ok] = m_workingChunks.emplace(coord, created);
                    (void)ok;
                } else {
                    auto [it, ok] = m_workingChunks.emplace(coord, *source);
                    (void)ok;
                }
            }
        }
    }
    m_metrics.chunksDirty = static_cast<uint32_t>(m_workingChunks.size());
    return !m_workingChunks.empty();
}

bool VoxelTransaction::applySphereSdfToChunk(VoxelChunk& chunk, const EditCommand& command) {
    uint32_t active = 0;
    for (std::size_t i = 0; i < chunk.cells.size(); ++i) {
        int lx = 0;
        int ly = 0;
        int lz = 0;
        VoxelWorld::unpackIndex(i, lx, ly, lz);
        const auto pos = VoxelWorld::cellCenterWorld(chunk.coord, lx, ly, lz, m_world.voxelSize());
        const float spherePhi = VoxelWorld::sphereSdf(pos, command.center, command.radius);

        VoxelCell& cell = chunk.cells[i];
        const float oldPhi = cell.phi;
        if (command.mode == EditMode::Carve) {
            cell.phi = std::max(cell.phi, -spherePhi);
        } else {
            cell.phi = std::min(cell.phi, spherePhi);
        }
        cell.material.initialized = true;

        if (std::abs(oldPhi - cell.phi) > kSeamEpsilon) {
            ++m_metrics.cellsDirty;
        }
        if ((oldPhi < kIsoValue && cell.phi >= kIsoValue) || (oldPhi >= kIsoValue && cell.phi < kIsoValue)) {
            ++m_metrics.surfaceCellsDirty;
        }
        if (cell.phi < kIsoValue) {
            ++active;
        }
    }
    chunk.activeVoxelCount = active;
    return true;
}

bool VoxelTransaction::apply() {
    if (!m_open) {
        return false;
    }
    for (auto& [coord, chunk] : m_workingChunks) {
        (void)coord;
        if (!applySphereSdfToChunk(chunk, m_command)) {
            return false;
        }
    }
    if (SideEffectOutbox* box = m_sideEffects.writable(); box != nullptr) {
        box->completedJobs.push_back("edit_apply");
        box->cacheOps.push_back("invalidate_dirty_region");
    }
    return true;
}

bool VoxelTransaction::checkChunkInvariants(const VoxelChunk& chunk, std::vector<InvariantFailure>& failures) const {
    uint32_t active = 0;
    for (std::size_t i = 0; i < chunk.cells.size(); ++i) {
        const VoxelCell& cell = chunk.cells[i];
        if (!std::isfinite(cell.phi)) {
            failures.push_back({InvariantCode::InvalidPhi, chunk.coord.x, chunk.coord.y, chunk.coord.z, "Found non-finite phi"});
            return false;
        }
        if (!cell.material.initialized) {
            failures.push_back(
                {InvariantCode::UninitializedMaterial, chunk.coord.x, chunk.coord.y, chunk.coord.z, "Material field is uninitialized"});
            return false;
        }
        if (cell.phi < kIsoValue) {
            ++active;
        }
    }
    if (active != chunk.activeVoxelCount) {
        failures.push_back(
            {InvariantCode::ActiveCountMismatch, chunk.coord.x, chunk.coord.y, chunk.coord.z, "Chunk active voxel count mismatch"});
        return false;
    }
    return true;
}

std::optional<VoxelCell> VoxelTransaction::cellFromWorld(const ChunkCoord& coord, int localX, int localY, int localZ) const {
    const VoxelChunk* chunk = m_world.findChunk(coord);
    if (chunk == nullptr) {
        return std::nullopt;
    }
    const std::size_t idx = VoxelWorld::flatIndex(localX, localY, localZ);
    return chunk->cells[idx];
}

std::optional<VoxelCell> VoxelTransaction::cellFromWorkingOrWorld(const ChunkCoord& coord, int localX, int localY, int localZ) const {
    const auto it = m_workingChunks.find(coord);
    if (it != m_workingChunks.end()) {
        const std::size_t idx = VoxelWorld::flatIndex(localX, localY, localZ);
        return it->second.cells[idx];
    }
    return cellFromWorld(coord, localX, localY, localZ);
}

bool VoxelTransaction::checkSeamInvariants(std::vector<InvariantFailure>& failures) const {
    for (const auto& [coord, chunk] : m_workingChunks) {
        const ChunkCoord nx = neighborCoord(coord, 1, 0, 0);
        const ChunkCoord ny = neighborCoord(coord, 0, 1, 0);
        const ChunkCoord nz = neighborCoord(coord, 0, 0, 1);
        const std::array<ChunkCoord, 3> neighbors = {nx, ny, nz};
        for (const ChunkCoord& neighbor : neighbors) {
            for (int i = 0; i < kChunkEdge; ++i) {
                for (int j = 0; j < kChunkEdge; ++j) {
                    std::optional<VoxelCell> a;
                    std::optional<VoxelCell> b;
                    if (neighbor.x == coord.x + 1) {
                        a = cellFromWorkingOrWorld(coord, kChunkEdge - 1, i, j);
                        b = cellFromWorkingOrWorld(neighbor, 0, i, j);
                    } else if (neighbor.y == coord.y + 1) {
                        a = cellFromWorkingOrWorld(coord, i, kChunkEdge - 1, j);
                        b = cellFromWorkingOrWorld(neighbor, i, 0, j);
                    } else {
                        a = cellFromWorkingOrWorld(coord, i, j, kChunkEdge - 1);
                        b = cellFromWorkingOrWorld(neighbor, i, j, 0);
                    }
                    if (!a.has_value() || !b.has_value()) {
                        continue;
                    }
                    if (isInside(a->phi) != isInside(b->phi)) {
                        failures.push_back(
                            {InvariantCode::SeamOccupancyMismatch, coord.x, coord.y, coord.z, "Seam occupancy mismatch at chunk border"});
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

bool VoxelTransaction::commitChecks(std::vector<InvariantFailure>& failures) const {
    if (!m_open) {
        failures.push_back({InvariantCode::VersionParityMismatch, 0, 0, 0, "Commit checks called without open transaction"});
        return false;
    }
    for (const auto& [coord, chunk] : m_workingChunks) {
        (void)coord;
        if (!checkChunkInvariants(chunk, failures)) {
            return false;
        }
    }
    if (!checkSeamInvariants(failures)) {
        return false;
    }
    const VersionFence v = m_world.versions();
    if (v.meshProducedVersion > v.topologyVersion || v.collisionVersion > v.topologyVersion ||
        v.meshVisibleVersion > v.meshProducedVersion) {
        failures.push_back({InvariantCode::VersionParityMismatch, 0, 0, 0, "Version fence parity violation"});
        return false;
    }
    return true;
}

TransactionResult VoxelTransaction::commit() {
    TransactionResult result{};
    result.metrics = m_metrics;
    if (!m_open) {
        return result;
    }
    std::vector<InvariantFailure> failures;
    if (!commitChecks(failures)) {
        result.failures = failures;
        rollback();
        return result;
    }

    for (const auto& [coord, working] : m_workingChunks) {
        VoxelChunk& dst = m_world.ensureChunk(coord);
        dst = working;
        m_world.markChunkDirty(coord);
        result.dirtyChunks.push_back(coord);
    }
    m_world.incrementTopologyVersion();
    result.committedTopologyVersion = m_world.versions().topologyVersion;

    if (SideEffectOutbox* box = m_sideEffects.writable(); box != nullptr) {
        box->completedJobs.push_back("commit_publish");
        box->targetTopologyVersion = result.committedTopologyVersion;
    }
    m_sideEffects.markComplete();
    result.success = true;

    m_workingChunks.clear();
    m_open = false;
    m_targetVersion = 0;
    return result;
}

void VoxelTransaction::rollback() {
    m_workingChunks.clear();
    m_metrics = {};
    m_open = false;
    m_targetVersion = 0;
    m_sideEffects.discard();
}

std::vector<ChunkCoord> VoxelTransaction::touchedChunks() const {
    std::vector<ChunkCoord> chunks;
    chunks.reserve(m_workingChunks.size());
    for (const auto& [coord, chunk] : m_workingChunks) {
        (void)chunk;
        chunks.push_back(coord);
    }
    return chunks;
}

}  // namespace oro::voxel
