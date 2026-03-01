#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace oro::voxel {

struct SideEffectOutbox {
    uint64_t targetTopologyVersion = 0;
    std::vector<std::string> meshArtifacts;
    std::vector<std::string> collisionArtifacts;
    std::vector<std::string> gpuUploads;
    std::vector<std::string> cacheOps;
    std::vector<uint64_t> allocatedIds;
    std::vector<std::string> completedJobs;

    bool complete = false;
};

class SideEffectFence {
public:
    void begin(uint64_t targetTopologyVersion);
    void discard();
    void markComplete();

    bool isOpen() const;
    bool isComplete() const;
    uint64_t targetVersion() const;

    SideEffectOutbox* writable();
    const SideEffectOutbox* staged() const;
    std::optional<SideEffectOutbox> publish();

private:
    std::optional<SideEffectOutbox> m_staged;
};

}  // namespace oro::voxel
