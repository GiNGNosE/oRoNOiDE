#pragma once
#include "rhi/vulkan/swapchain.h"

class VulkanContext;

class Renderer {
public:
    bool init(VulkanContext& context);
    VkResult acquireNextImage(uint32_t& imageIndex);
    VkResult presentImage(uint32_t imageIndex);
    void drawFrame();
    void shutdown();

private:
    VulkanContext* m_context = nullptr;
    VkFence m_acquireFence = VK_NULL_HANDLE;
    VulkanSwapchain m_swapchain;
};