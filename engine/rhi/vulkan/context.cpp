#include "rhi/vulkan/context.h"
#include "core/log.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vector>

bool VulkanContext::createInstance() {
    VkApplicationInfo appInfo = {};
    appInfo.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName   = "oRoNOiDE";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.pEngineName         = "oRoNOiDE Engine";
    appInfo.engineVersion      = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion         = VK_API_VERSION_1_3;

    uint32_t sdlExtCount = 0;
    const char* const* sdlExts = SDL_Vulkan_GetInstanceExtensions(&sdlExtCount);
    if (!sdlExts) {
        ORO_LOG_ERROR("Failed to get Vulkan instance extensions from SDL");
        return false;
    }

    std::vector<const char*> extensions(sdlExts, sdlExts + sdlExtCount);
    extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);

    VkInstanceCreateInfo createInfo{};
    createInfo.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo        = &appInfo;
    createInfo.enabledExtensionCount   = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();
    createInfo.flags                   = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;

    VkResult result = vkCreateInstance(&createInfo, nullptr, &m_instance);
    if (result != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed to create Vulkan instance: %d", result);
        return false;
    }

    ORO_LOG_INFO("Vulkan instance created successfully");
    return true;
}

bool VulkanContext::createSurface(SDL_Window* window) {
    if (!window) {
        ORO_LOG_ERROR("Failed to create Vulkan surface: Window is not valid");
        return false;
    }

    if (m_instance == VK_NULL_HANDLE) {
        ORO_LOG_ERROR("Failed to create Vulkan surface: Instance is not valid");
        return false;
    }

    return SDL_Vulkan_CreateSurface(window, m_instance, nullptr, &m_surface);
}

bool VulkanContext::pickPhysicalDevice() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    if (count == 0) return false;

    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(m_instance, &count, devices.data());

    for (auto dev : devices) {
        uint32_t qCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qCount, nullptr);
        std::vector<VkQueueFamilyProperties> qProps(qCount);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qCount, qProps.data());

        for (uint32_t i = 0; i < qCount; i++) {
            if (qProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) continue;
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, m_surface, &present);
            if (!present) continue;

            m_physicalDevice = dev;
            m_graphicsFamily = i;
            return true;
        }
    }
    return true;
}

bool VulkanContext::createLogicalDevice() {
    if (m_physicalDevice == VK_NULL_HANDLE) return false;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = m_graphicsFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    const char* exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.ppEnabledExtensionNames = exts;

    VkResult r = vkCreateDevice(m_physicalDevice, &dci, nullptr, &m_device);
    if (r != VK_SUCCESS) return false;

    vkGetDeviceQueue(m_device, m_graphicsFamily, 0, &m_graphicsQueue);
    return true;
}

bool VulkanContext::init(SDL_Window* window) {
    if (!window) {
        ORO_LOG_ERROR("Failed to initiate Vulkan context: Window is not valid");
        return false;
    }

    if (!createInstance()) {
        ORO_LOG_ERROR("Failed to create Vulkan instance");
        return false;
    }

    if (!createSurface(window)) {
        ORO_LOG_ERROR("Failed to create Vulkan surface");
        return false;
    }

    if (!pickPhysicalDevice()) {
        ORO_LOG_ERROR("Failed to pick Vulkan physical device");
        return false;
    }

    if (!createLogicalDevice()) {
        ORO_LOG_ERROR("Failed to create Vulkan logical device");
        return false;
    }
    
    ORO_LOG_INFO("Vulkan context created successfully");
    return true;
}

void VulkanContext::shutdown() {
    if (m_device != VK_NULL_HANDLE) {
        vkDestroyDevice(m_device, nullptr);
        m_device = VK_NULL_HANDLE;
    }

    if (m_surface != VK_NULL_HANDLE) {
        SDL_Vulkan_DestroySurface(m_instance, m_surface, nullptr);
        m_surface = VK_NULL_HANDLE;
    }

    if (m_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
    }
}