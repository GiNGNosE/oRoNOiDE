#include "voxel/side_effect_fences.h"

namespace oro::voxel {

void SideEffectFence::begin(uint64_t targetTopologyVersion) {
    SideEffectOutbox outbox{};
    outbox.targetTopologyVersion = targetTopologyVersion;
    m_staged = outbox;
}

void SideEffectFence::discard() {
    m_staged.reset();
}

void SideEffectFence::markComplete() {
    if (!m_staged.has_value()) {
        return;
    }
    m_staged->complete = true;
}

bool SideEffectFence::isOpen() const {
    return m_staged.has_value();
}

bool SideEffectFence::isComplete() const {
    return m_staged.has_value() && m_staged->complete;
}

uint64_t SideEffectFence::targetVersion() const {
    return m_staged.has_value() ? m_staged->targetTopologyVersion : 0;
}

SideEffectOutbox* SideEffectFence::writable() {
    return m_staged.has_value() ? &m_staged.value() : nullptr;
}

const SideEffectOutbox* SideEffectFence::staged() const {
    return m_staged.has_value() ? &m_staged.value() : nullptr;
}

std::optional<SideEffectOutbox> SideEffectFence::publish() {
    if (!m_staged.has_value() || !m_staged->complete) {
        return std::nullopt;
    }
    std::optional<SideEffectOutbox> output = m_staged;
    m_staged.reset();
    return output;
}

}  // namespace oro::voxel
