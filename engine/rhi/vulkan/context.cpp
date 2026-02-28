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
    appInfo.engineName         = "oRoNOiDE Engine";
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
