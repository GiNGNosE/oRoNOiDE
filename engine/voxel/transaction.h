#pragma once

#include "voxel/edit_types.h"
#include "voxel/side_effect_fences.h"
#include "voxel/voxel_world.h"

#include <optional>
#include <unordered_map>
#include <vector>

namespace oro::voxel {

struct TransactionResult {
    bool success = false;
    DirtyMetrics metrics{};
    std::vector<InvariantFailure> failures;
    std::vector<ChunkCoord> dirtyChunks;
    uint64_t committedTopologyVersion = 0;
};

class VoxelTransaction {
public:
    explicit VoxelTransaction(VoxelWorld& world);

    bool precheck(const EditCommand& command, InvariantFailure& failure) const;
    bool begin(const EditCommand& command);
    bool apply();
    bool commitChecks(std::vector<InvariantFailure>& failures) const;
    TransactionResult commit();
    void rollback();
    std::vector<ChunkCoord> touchedChunks() const;

    const DirtyMetrics& dirtyMetrics() const { return m_metrics; }
    bool isOpen() const { return m_open; }
    uint64_t targetVersion() const { return m_targetVersion; }
    SideEffectFence& sideEffects() { return m_sideEffects; }
    const SideEffectFence& sideEffects() const { return m_sideEffects; }

private:
    bool collectTouchedChunks();
    bool applySphereSdfToChunk(VoxelChunk& chunk, const EditCommand& command);
    bool checkChunkInvariants(const VoxelChunk& chunk, std::vector<InvariantFailure>& failures) const;
    bool checkSeamInvariants(std::vector<InvariantFailure>& failures) const;

    std::optional<VoxelCell> cellFromWorld(const ChunkCoord& coord, int localX, int localY, int localZ) const;
    std::optional<VoxelCell> cellFromWorkingOrWorld(const ChunkCoord& coord, int localX, int localY, int localZ) const;

    VoxelWorld& m_world;
    EditCommand m_command{};
    bool m_open = false;
    uint64_t m_targetVersion = 0;
    DirtyMetrics m_metrics{};
    std::unordered_map<ChunkCoord, VoxelChunk, ChunkCoordHash> m_workingChunks;
    SideEffectFence m_sideEffects;
};

}  // namespace oro::voxel
