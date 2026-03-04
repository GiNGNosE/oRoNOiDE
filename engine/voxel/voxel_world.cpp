#include "voxel/voxel_world.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace oro::voxel {

std::size_t ChunkCoordHash::operator()(const ChunkCoord& coord) const noexcept {
    const auto hx = static_cast<std::size_t>(std::hash<int>{}(coord.x));
    const auto hy = static_cast<std::size_t>(std::hash<int>{}(coord.y));
    const auto hz = static_cast<std::size_t>(std::hash<int>{}(coord.z));
    return hx ^ (hy << 1U) ^ (hz << 2U);
}

VoxelWorld::VoxelWorld() = default;

VoxelChunk& VoxelWorld::ensureChunk(const ChunkCoord& coord) {
    const auto it = m_chunks.find(coord);
    if (it != m_chunks.end()) {
        return it->second;
    }
    VoxelChunk created{};
    created.coord = coord;
    created.activeVoxelCount = 0;
    auto [inserted, ok] = m_chunks.emplace(coord, created);
    (void)ok;
    return inserted->second;
}

const VoxelChunk* VoxelWorld::findChunk(const ChunkCoord& coord) const {
    const auto it = m_chunks.find(coord);
    return it == m_chunks.end() ? nullptr : &it->second;
}

VoxelChunk* VoxelWorld::findChunk(const ChunkCoord& coord) {
    const auto it = m_chunks.find(coord);
    return it == m_chunks.end() ? nullptr : &it->second;
}

std::size_t VoxelWorld::flatIndex(int localX, int localY, int localZ) {
    return static_cast<std::size_t>((localZ * kChunkEdge * kChunkEdge) + (localY * kChunkEdge) + localX);
}

void VoxelWorld::unpackIndex(std::size_t index, int& localX, int& localY, int& localZ) {
    const int i = static_cast<int>(index);
    localZ = i / (kChunkEdge * kChunkEdge);
    const int rem = i % (kChunkEdge * kChunkEdge);
    localY = rem / kChunkEdge;
    localX = rem % kChunkEdge;
}

std::array<float, 3> VoxelWorld::cellCenterWorld(const ChunkCoord& coord, int localX, int localY, int localZ,
                                                  float voxelSize) {
    const float worldX = static_cast<float>((coord.x * kChunkEdge) + localX) * voxelSize + (0.5F * voxelSize);
    const float worldY = static_cast<float>((coord.y * kChunkEdge) + localY) * voxelSize + (0.5F * voxelSize);
    const float worldZ = static_cast<float>((coord.z * kChunkEdge) + localZ) * voxelSize + (0.5F * voxelSize);
    return {worldX, worldY, worldZ};
}

float VoxelWorld::sphereSdf(const std::array<float, 3>& point, const std::array<float, 3>& center, float radius) {
    const float dx = point[0] - center[0];
    const float dy = point[1] - center[1];
    const float dz = point[2] - center[2];
    const float dist = std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
    return dist - radius;
}

float VoxelWorld::ellipsoidSdf(const std::array<float, 3>& point, const std::array<float, 3>& center,
                               const std::array<float, 3>& radii) {
    const float rx = std::max(radii[0], 1.0e-4F);
    const float ry = std::max(radii[1], 1.0e-4F);
    const float rz = std::max(radii[2], 1.0e-4F);
    const float qx = (point[0] - center[0]) / rx;
    const float qy = (point[1] - center[1]) / ry;
    const float qz = (point[2] - center[2]) / rz;
    const float qLen = std::sqrt((qx * qx) + (qy * qy) + (qz * qz));
    const float minRadius = std::min({rx, ry, rz});
    return (qLen - 1.0F) * minRadius;
}

float VoxelWorld::noisyStoneSdf(const std::array<float, 3>& point, const std::array<float, 3>& center, float radius,
                                float noiseAmplitude, float noiseFrequency, uint32_t noiseSeed) {
    const float base = sphereSdf(point, center, radius);
    const float seed = static_cast<float>(noiseSeed) * 0.013F;
    const float fx = (point[0] + seed) * noiseFrequency;
    const float fy = (point[1] - (2.0F * seed)) * (noiseFrequency * 1.13F);
    const float fz = (point[2] + (3.0F * seed)) * (noiseFrequency * 0.87F);
    const float noise = std::sin(fx) * std::sin(fy) * std::sin(fz);
    return base - (noiseAmplitude * noise);
}

std::array<float, 3> VoxelWorld::sphereGradient(const std::array<float, 3>& point, const std::array<float, 3>& center) {
    const float dx = point[0] - center[0];
    const float dy = point[1] - center[1];
    const float dz = point[2] - center[2];
    const float len = std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
    if (len < std::numeric_limits<float>::epsilon()) {
        return {0.0F, 1.0F, 0.0F};
    }
    return {dx / len, dy / len, dz / len};
}

void VoxelWorld::seedSdf(const ChunkCoord& coord, const std::function<float(const std::array<float, 3>&)>& sdf,
                         uint16_t materialId) {
    VoxelChunk& chunk = ensureChunk(coord);
    uint32_t active = 0;
    for (std::size_t i = 0; i < chunk.cells.size(); ++i) {
        int lx = 0;
        int ly = 0;
        int lz = 0;
        unpackIndex(i, lx, ly, lz);
        const auto world = cellCenterWorld(coord, lx, ly, lz, m_voxelSize);
        VoxelCell& cell = chunk.cells[i];
        cell.phi = sdf(world);
        cell.material.materialId = materialId;
        cell.material.initialized = true;
        if (cell.phi < kIsoValue) {
            ++active;
        }
    }
    chunk.activeVoxelCount = active;
    markChunkDirty(coord);
}

void VoxelWorld::seedSphere(const ChunkCoord& coord, const std::array<float, 3>& center, float radius, uint16_t materialId) {
    seedSdf(coord, [&](const std::array<float, 3>& p) { return sphereSdf(p, center, radius); }, materialId);
}

void VoxelWorld::seedEllipsoid(const ChunkCoord& coord, const std::array<float, 3>& center,
                               const std::array<float, 3>& radii, uint16_t materialId) {
    seedSdf(coord, [&](const std::array<float, 3>& p) { return ellipsoidSdf(p, center, radii); }, materialId);
}

void VoxelWorld::seedNoisyStone(const ChunkCoord& coord, const std::array<float, 3>& center, float radius,
                                float noiseAmplitude, float noiseFrequency, uint32_t noiseSeed, uint16_t materialId) {
    seedSdf(coord,
            [&](const std::array<float, 3>& p) {
                return noisyStoneSdf(p, center, radius, noiseAmplitude, noiseFrequency, noiseSeed);
            },
            materialId);
}

void VoxelWorld::incrementTopologyVersion() {
    ++m_versions.topologyVersion;
}

void VoxelWorld::setTopologyVersion(uint64_t version) {
    m_versions.topologyVersion = version;
}

void VoxelWorld::forceVersionFence(const VersionFence& versions) {
    m_versions = versions;
}

void VoxelWorld::publishMeshProducedVersion(uint64_t version) {
    m_versions.meshProducedVersion = std::max(m_versions.meshProducedVersion, version);
}

void VoxelWorld::publishCollisionVersion(uint64_t version) {
    m_versions.collisionVersion = std::max(m_versions.collisionVersion, version);
}

void VoxelWorld::markChunkDirty(const ChunkCoord& coord) {
    (void)m_dirtyChunkSet.insert(coord);
}

bool VoxelWorld::isChunkDirty(const ChunkCoord& coord) const {
    return m_dirtyChunkSet.contains(coord);
}

std::vector<ChunkCoord> VoxelWorld::consumeDirtyChunks() {
    std::vector<ChunkCoord> dirty;
    dirty.reserve(m_dirtyChunkSet.size());
    for (const ChunkCoord& coord : m_dirtyChunkSet) {
        dirty.push_back(coord);
    }
    m_dirtyChunkSet.clear();
    return dirty;
}

}  // namespace oro::voxel
