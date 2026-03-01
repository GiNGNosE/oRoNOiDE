#pragma once

#include <cstdint>

namespace oro::voxel {

struct VersionFence {
    uint64_t topologyVersion = 0;
    uint64_t meshProducedVersion = 0;
    uint64_t meshVisibleVersion = 0;
    uint64_t collisionVersion = 0;
};

struct VersionToken {
    uint64_t version = 0;
    uint64_t generation = 0;
    uint64_t rollbackEpoch = 0;
};

inline bool operator==(const VersionToken& a, const VersionToken& b) {
    return a.version == b.version && a.generation == b.generation && a.rollbackEpoch == b.rollbackEpoch;
}

}  // namespace oro::voxel
