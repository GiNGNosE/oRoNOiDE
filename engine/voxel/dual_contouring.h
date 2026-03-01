#pragma once

#include "voxel/snapshot_registry.h"
#include "voxel/voxel_world.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace oro::voxel {

struct HermiteSample {
    std::array<float, 3> intersection{};
    std::array<float, 3> normal{};
    uint16_t materialId = 1;
};

struct MeshVertex {
    std::array<float, 3> position{};
    std::array<float, 3> normal{};
    uint16_t materialId = 0;
};

struct MeshBuffers {
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    uint64_t sourceTopologyVersion = 0;
};

struct MeshStructuralStats {
    uint32_t nanCount = 0;
    uint32_t invalidIndexCount = 0;
    uint32_t degenerateCount = 0;
};

class DualContouringMesher {
public:
    MeshBuffers remeshDirtyRegions(const VoxelWorld& world, const std::vector<ChunkCoord>& dirtyChunks) const;
    MeshBuffers remeshSnapshot(const JobSnapshot& snapshot) const;
    MeshStructuralStats validateStructural(const MeshBuffers& buffers, float voxelSize) const;

private:
    static std::array<float, 3> normalize(const std::array<float, 3>& v);
    static float triangleArea(const MeshVertex& a, const MeshVertex& b, const MeshVertex& c);
};

}  // namespace oro::voxel
