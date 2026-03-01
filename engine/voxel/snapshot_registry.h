#pragma once

#include "voxel/edit_classifier.h"
#include "voxel/voxel_world.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace oro::voxel {

struct JobSnapshot {
    uint64_t topologyVersion = 0;
    float voxelSize = kDefaultVoxelSize;
    DirtyMetrics metrics{};
    std::vector<ChunkCoord> dirtyChunks;
    std::vector<ChunkCoord> seamHaloChunks;
    std::unordered_map<ChunkCoord, VoxelChunk, ChunkCoordHash> immutableChunks;
};

using JobSnapshotHandle = std::shared_ptr<const JobSnapshot>;

class SnapshotRegistry {
public:
    JobSnapshotHandle createAndRegister(JobSnapshot snapshot);
    JobSnapshotHandle acquire(uint64_t topologyVersion) const;
    void pruneReleased();
    bool contains(uint64_t topologyVersion) const;

private:
    mutable std::mutex m_mutex;
    std::unordered_map<uint64_t, std::weak_ptr<const JobSnapshot>> m_snapshots;
};

}  // namespace oro::voxel
