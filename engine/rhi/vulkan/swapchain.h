#pragma once
#include <vulkan/vulkan.h>

#include <vector>

class VulkanContext;

class VulkanSwapchain {
public:
    VkDevice m_device = VK_NULL_HANDLE;
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkFormat m_format = VK_FORMAT_UNDEFINED;
    VkExtent2D m_extent{};
    std::vector<VkImage> m_images;
    std::vector<VkImageView> m_imageViews;

    bool init(VulkanContext& context);
    void shutdown();

    VkFormat format() const { return m_format; }
    VkExtent2D extent() const { return m_extent; }
    const std::vector<VkImageView>& imageViews() const { return m_imageViews; }
    uint32_t imageCount() const { return static_cast<uint32_t>(m_images.size()); }
};