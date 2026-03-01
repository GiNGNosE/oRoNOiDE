#pragma once
#include <vulkan/vulkan.h>

class VulkanContext;

class VulkanSwapchain {
public:
    VkDevice m_device = VK_NULL_HANDLE;
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    bool init(VulkanContext& context);
    void shutdown();
};