#include "render/renderer.h"

bool Renderer::init(VulkanContext& context) {
    m_context = &context;
    return true;
}

void Renderer::drawFrame() {
    if (!m_context) return;
}

void Renderer::shutdown() {
    m_context = nullptr;
}
