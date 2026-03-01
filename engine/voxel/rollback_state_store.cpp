#include "voxel/rollback_state_store.h"

#include <algorithm>

namespace oro::voxel {

bool RollbackStateStore::has(uint64_t targetVersion) const {
    return m_entries.contains(targetVersion);
}

std::optional<RollbackState> RollbackStateStore::get(uint64_t targetVersion) const {
    const auto it = m_entries.find(targetVersion);
    if (it == m_entries.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool RollbackStateStore::put(RollbackState state) {
    if (state.token.version == 0) {
        return false;
    }
    const auto existing = m_entries.find(state.token.version);
    if (existing != m_entries.end()) {
        m_usedBytes -= existing->second.estimatedBytes;
        existing->second = std::move(state);
        m_usedBytes += existing->second.estimatedBytes;
        return true;
    }

    const uint64_t version = state.token.version;
    m_usedBytes += state.estimatedBytes;
    m_entries.emplace(version, std::move(state));
    m_fifoOrder.push_back(version);
    return true;
}

void RollbackStateStore::erase(uint64_t targetVersion) {
    const auto it = m_entries.find(targetVersion);
    if (it == m_entries.end()) {
        return;
    }
    m_usedBytes -= it->second.estimatedBytes;
    m_entries.erase(it);
    auto fifoIt = std::find(m_fifoOrder.begin(), m_fifoOrder.end(), targetVersion);
    if (fifoIt != m_fifoOrder.end()) {
        m_fifoOrder.erase(fifoIt);
    }
}

std::vector<uint64_t> RollbackStateStore::versionsOldestFirst() const {
    std::vector<uint64_t> out;
    out.reserve(m_fifoOrder.size());
    for (uint64_t v : m_fifoOrder) {
        if (m_entries.contains(v)) {
            out.push_back(v);
        }
    }
    return out;
}

}  // namespace oro::voxel
