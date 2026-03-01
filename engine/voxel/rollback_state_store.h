#pragma once

#include "voxel/state_version.h"
#include "voxel/voxel_world.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>
#include <vector>

namespace oro::voxel {

struct RollbackState {
    VersionToken token{};
    uint64_t baselineTopologyVersion = 0;
    std::vector<ChunkCoord> region;
    std::unordered_map<ChunkCoord, VoxelChunk, ChunkCoordHash> chunks;
    std::size_t estimatedBytes = 0;
};

enum class PurgeMode : uint8_t {
    NormalStrict = 0,
    PressureRelaxed = 1,
};

class RollbackStateStore {
public:
    bool has(uint64_t targetVersion) const;
    std::optional<RollbackState> get(uint64_t targetVersion) const;
    bool put(RollbackState state);
    void erase(uint64_t targetVersion);
    std::vector<uint64_t> versionsOldestFirst() const;

    std::size_t usedBytes() const { return m_usedBytes; }
    std::size_t usedSlots() const { return m_entries.size(); }

private:
    std::unordered_map<uint64_t, RollbackState> m_entries;
    std::deque<uint64_t> m_fifoOrder;
    std::size_t m_usedBytes = 0;
};

}  // namespace oro::voxel
