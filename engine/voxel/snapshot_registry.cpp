#include "voxel/snapshot_registry.h"

namespace oro::voxel {

JobSnapshotHandle SnapshotRegistry::createAndRegister(JobSnapshot snapshot) {
    std::shared_ptr<const JobSnapshot> handle = std::make_shared<const JobSnapshot>(std::move(snapshot));
    std::scoped_lock lock(m_mutex);
    m_snapshots[handle->topologyVersion] = handle;
    return handle;
}

JobSnapshotHandle SnapshotRegistry::acquire(uint64_t topologyVersion) const {
    std::scoped_lock lock(m_mutex);
    const auto it = m_snapshots.find(topologyVersion);
    if (it == m_snapshots.end()) {
        return {};
    }
    return it->second.lock();
}

void SnapshotRegistry::pruneReleased() {
    std::scoped_lock lock(m_mutex);
    for (auto it = m_snapshots.begin(); it != m_snapshots.end();) {
        if (it->second.expired()) {
            it = m_snapshots.erase(it);
        } else {
            ++it;
        }
    }
}

bool SnapshotRegistry::contains(uint64_t topologyVersion) const {
    std::scoped_lock lock(m_mutex);
    const auto it = m_snapshots.find(topologyVersion);
    if (it == m_snapshots.end()) {
        return false;
    }
    return !it->second.expired();
}

}  // namespace oro::voxel
