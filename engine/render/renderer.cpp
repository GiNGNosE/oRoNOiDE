#include "render/renderer.h"
#include "rhi/vulkan/context.h"
#include "core/log.h"

bool Renderer::init(VulkanContext& context) {
    m_context = &context;
    if (!m_swapchain.init(context)) return false;

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkResult r = vkCreateFence(context.device(), &fci, nullptr, &m_acquireFence);
    if (r != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed to create Vulkan acquire fence: %d", r);
        m_swapchain.shutdown();
        m_context = nullptr;
        return false;
    }
    return true;
}

VkResult Renderer::acquireNextImage(uint32_t& imageIndex) {
    if (!m_context) return VK_ERROR_INITIALIZATION_FAILED;
    if (m_swapchain.m_swapchain == VK_NULL_HANDLE) return VK_ERROR_INITIALIZATION_FAILED;

    // Fence must be unsignaled before reuse
    vkResetFences(m_context->device(), 1, &m_acquireFence);

    return vkAcquireNextImageKHR(
        m_context->device(),
        m_swapchain.m_swapchain,
        UINT64_MAX,                 // block until image available
        VK_NULL_HANDLE,             // no semaphore yet
        m_acquireFence,             // use fence for minimal sync
        &imageIndex                
    );
}

VkResult Renderer::presentImage(uint32_t imageIndex) {
    if (!m_context) return VK_ERROR_INITIALIZATION_FAILED;

    VkSwapchainKHR sc = m_swapchain.m_swapchain;
    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.swapchainCount = 1;
    pi.pSwapchains = &sc;
    pi.pImageIndices = &imageIndex;
    pi.waitSemaphoreCount = 0;       // minimal path (no render submit yet)

    return vkQueuePresentKHR(m_context->graphicsQueue(), &pi);
}

void Renderer::drawFrame() {
    if (!m_context) return;

    uint32_t imageIndex = 0;
    VkResult acquireResult = acquireNextImage(imageIndex);
    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) return;
    if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) return;

    VkResult presentResult = presentImage(imageIndex);
    if (presentResult != VK_SUCCESS) return;
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR) return;
    if (presentResult == VK_SUBOPTIMAL_KHR) return;
}

void Renderer::shutdown() {
    if (m_context && m_acquireFence != VK_NULL_HANDLE) {
        vkDestroyFence(m_context->device(), m_acquireFence, nullptr);
        m_acquireFence = VK_NULL_HANDLE;
    }
    m_swapchain.shutdown();
    m_context = nullptr;
}
