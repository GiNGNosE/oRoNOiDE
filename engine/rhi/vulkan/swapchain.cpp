#include "rhi/vulkan/context.h"
#include "rhi/vulkan/swapchain.h"
#include "core/log.h"
#include <vector>


bool VulkanSwapchain::init(VulkanContext& context) {
    shutdown();
    m_device = context.device();
    if (m_device == VK_NULL_HANDLE ||
        context.physicalDevice() == VK_NULL_HANDLE ||
        context.surface() == VK_NULL_HANDLE) {
        ORO_LOG_ERROR("Failed to initiate Vulkan swapchain: Invalid context");
        return false;
    }

    // 1) Surface capabilities
    VkSurfaceCapabilitiesKHR caps;
    VkResult r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        context.physicalDevice(), context.surface(), &caps);
    if (r != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed to get Vulkan surface capabilities: %d", r);
        return false;
    }

    // 2) Surface formats
    uint32_t formatCount = 0;
    r = vkGetPhysicalDeviceSurfaceFormatsKHR(
        context.physicalDevice(), context.surface(), &formatCount, nullptr);
    if (r != VK_SUCCESS || formatCount == 0) {
        ORO_LOG_ERROR("Failed to get Vulkan surface formats: %d", r);
        return false;
    }

    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    r = vkGetPhysicalDeviceSurfaceFormatsKHR(
        context.physicalDevice(), context.surface(), &formatCount, formats.data());
    if (r != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed to get Vulkan surface formats: %d", r);
        return false;
    }

    VkSurfaceFormatKHR chosenFormat = formats[0];
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && 
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosenFormat = f;
            break;
        }
    }

    // 3) Present modes
    uint32_t presentModeCount = 0;
    r = vkGetPhysicalDeviceSurfacePresentModesKHR(
        context.physicalDevice(), context.surface(), &presentModeCount, nullptr);
    if (r != VK_SUCCESS || presentModeCount == 0) {
        ORO_LOG_ERROR("Failed to get Vulkan present modes: %d", r);
        return false;
    }

    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    r = vkGetPhysicalDeviceSurfacePresentModesKHR(
        context.physicalDevice(), context.surface(), &presentModeCount, presentModes.data());
    if (r != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed to get Vulkan present modes: %d", r);
        return false;
    }

    VkPresentModeKHR chosenPresentMode = VK_PRESENT_MODE_FIFO_KHR;
    for (auto pm : presentModes) {
        if (pm == VK_PRESENT_MODE_MAILBOX_KHR) {
            chosenPresentMode = pm;
            break;
        }
    }

    // 4) Image count (min + 1, clamped)
    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
        imageCount = caps.maxImageCount;
    }

    // 5) Extent (for now: currentExtent)
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX || extent.height == UINT32_MAX) {
        // Some platforms don't fix currentExtent; use min as safe fallback for now
        extent = caps.minImageExtent;
    }

    // 6) Create swapchain
    VkSwapchainCreateInfoKHR ci{};
    ci.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface          = context.surface();
    ci.minImageCount = imageCount;
    ci.imageFormat   = chosenFormat.format;
    ci.imageColorSpace = chosenFormat.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = chosenPresentMode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = VK_NULL_HANDLE;

    r = vkCreateSwapchainKHR(m_device, &ci, nullptr, &m_swapchain);
    if (r != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed to create Vulkan swapchain: %d", r);
        return false;
    }

    uint32_t createdImageCount = 0;
    r = vkGetSwapchainImagesKHR(m_device, m_swapchain, &createdImageCount, nullptr);
    if (r != VK_SUCCESS || createdImageCount == 0) {
        ORO_LOG_ERROR("Failed to query Vulkan swapchain images: %d", r);
        shutdown();
        return false;
    }

    m_images.resize(createdImageCount);
    r = vkGetSwapchainImagesKHR(m_device, m_swapchain, &createdImageCount, m_images.data());
    if (r != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed to fetch Vulkan swapchain images: %d", r);
        shutdown();
        return false;
    }

    m_imageViews.clear();
    m_imageViews.reserve(createdImageCount);
    for (VkImage image : m_images) {
        VkImageViewCreateInfo viewCreateInfo{};
        viewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewCreateInfo.image = image;
        viewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewCreateInfo.format = chosenFormat.format;
        viewCreateInfo.components = {
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY};
        viewCreateInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewCreateInfo.subresourceRange.baseMipLevel = 0;
        viewCreateInfo.subresourceRange.levelCount = 1;
        viewCreateInfo.subresourceRange.baseArrayLayer = 0;
        viewCreateInfo.subresourceRange.layerCount = 1;

        VkImageView imageView = VK_NULL_HANDLE;
        r = vkCreateImageView(m_device, &viewCreateInfo, nullptr, &imageView);
        if (r != VK_SUCCESS) {
            ORO_LOG_ERROR("Failed to create Vulkan swapchain image view: %d", r);
            shutdown();
            return false;
        }
        m_imageViews.push_back(imageView);
    }

    m_format = chosenFormat.format;
    m_extent = extent;

    ORO_LOG_INFO("Swapchain created: images=%u format=%d extent=%ux%u presentMode=%d",
        createdImageCount, chosenFormat.format, extent.width, extent.height, chosenPresentMode);
    return true;
}

void VulkanSwapchain::shutdown() {
    for (VkImageView imageView : m_imageViews) {
        if (imageView != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE) {
            vkDestroyImageView(m_device, imageView, nullptr);
        }
    }
    m_imageViews.clear();
    m_images.clear();

    if (m_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
        m_swapchain = VK_NULL_HANDLE;
    }
    m_format = VK_FORMAT_UNDEFINED;
    m_extent = {};
    m_device = VK_NULL_HANDLE;
}