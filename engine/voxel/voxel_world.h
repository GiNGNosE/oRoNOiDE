#pragma once

#include "voxel/edit_types.h"
#include "voxel/state_version.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace oro::voxel {

inline constexpr int kChunkEdge = 32;
inline constexpr int kChunkCellCount = kChunkEdge * kChunkEdge * kChunkEdge;
inline constexpr float kIsoValue = 0.0F;
inline constexpr float kDefaultVoxelSize = 0.1F;
inline constexpr bool kDefaultHighFidelitySphereSmoothing = true;
inline constexpr int kDefaultRedistanceNarrowBandVoxels = 24;

struct ChunkCoord {
    int x = 0;
    int y = 0;
    int z = 0;

    bool operator==(const ChunkCoord& other) const {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct ChunkCoordHash {
    std::size_t operator()(const ChunkCoord& coord) const noexcept;
};

struct VoxelMaterialFields {
    float mineral = 0.5F;
    float porosity = 0.2F;
    float moisture = 0.1F;
    uint16_t materialId = 1;
    bool initialized = true;
};

struct VoxelStructuralFields {
    float cohesion = 1.0F;
    float fractureToughness = 1.0F;
};

struct VoxelCell {
    float phi = 1.0F;
    VoxelMaterialFields material;
    VoxelStructuralFields structure;
};

struct VoxelChunk {
    ChunkCoord coord;
    std::array<VoxelCell, static_cast<std::size_t>(kChunkCellCount)> cells{};
    uint32_t activeVoxelCount = 0;
};

struct DirtyMetrics {
    uint32_t chunksDirty = 0;
    uint32_t cellsDirty = 0;
    uint32_t surfaceCellsDirty = 0;
};

class VoxelWorld {
public:
    VoxelWorld();

    VoxelChunk& ensureChunk(const ChunkCoord& coord);
    const VoxelChunk* findChunk(const ChunkCoord& coord) const;
    VoxelChunk* findChunk(const ChunkCoord& coord);

    static std::size_t flatIndex(int localX, int localY, int localZ);
    static void unpackIndex(std::size_t index, int& localX, int& localY, int& localZ);

    static std::array<float, 3> cellCenterWorld(const ChunkCoord& coord, int localX, int localY, int localZ,
                                                float voxelSize);
    static float sphereSdf(const std::array<float, 3>& point, const std::array<float, 3>& center, float radius);
    static float ellipsoidSdf(const std::array<float, 3>& point, const std::array<float, 3>& center,
                              const std::array<float, 3>& radii);
    static float noisyStoneSdf(const std::array<float, 3>& point, const std::array<float, 3>& center, float radius,
                               float noiseAmplitude, float noiseFrequency, uint32_t noiseSeed);
    static std::array<float, 3> sphereGradient(const std::array<float, 3>& point, const std::array<float, 3>& center);

    void seedSdf(const ChunkCoord& coord, const std::function<float(const std::array<float, 3>&)>& sdf,
                 uint16_t materialId = 1);
    void seedSphere(const ChunkCoord& coord, const std::array<float, 3>& center, float radius, uint16_t materialId = 1);
    void seedEllipsoid(const ChunkCoord& coord, const std::array<float, 3>& center, const std::array<float, 3>& radii,
                       uint16_t materialId = 1);
    void seedNoisyStone(const ChunkCoord& coord, const std::array<float, 3>& center, float radius, float noiseAmplitude,
                        float noiseFrequency, uint32_t noiseSeed, uint16_t materialId = 1);

    VersionFence versions() const { return m_versions; }
    void incrementTopologyVersion();
    void setTopologyVersion(uint64_t version);
    void forceVersionFence(const VersionFence& versions);
    void publishMeshProducedVersion(uint64_t version);
    void publishCollisionVersion(uint64_t version);

    void markChunkDirty(const ChunkCoord& coord);
    bool isChunkDirty(const ChunkCoord& coord) const;
    std::vector<ChunkCoord> consumeDirtyChunks();

    float voxelSize() const { return m_voxelSize; }
    void setVoxelSize(float value) { m_voxelSize = value; }

    const std::unordered_map<ChunkCoord, VoxelChunk, ChunkCoordHash>& chunks() const { return m_chunks; }

private:
    std::unordered_map<ChunkCoord, VoxelChunk, ChunkCoordHash> m_chunks;
    std::unordered_set<ChunkCoord, ChunkCoordHash> m_dirtyChunkSet;
    float m_voxelSize = kDefaultVoxelSize;
    VersionFence m_versions{};
};

}  // namespace oro::voxel
