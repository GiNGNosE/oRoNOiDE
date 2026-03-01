#pragma once
#include <vulkan/vulkan.h>

struct SDL_Window;

class VulkanContext {
public:
    bool init(SDL_Window* window);
    void shutdown();

    VkInstance       instance()       const { return m_instance; }
    VkPhysicalDevice physicalDevice() const { return m_physicalDevice; }
    VkDevice         device()         const { return m_device; }
    VkQueue          graphicsQueue()  const { return m_graphicsQueue; }
    VkSurfaceKHR     surface()        const { return m_surface; }
    uint32_t         graphicsFamily() const { return m_graphicsFamily; }

private:
    bool createInstance();
    bool setupDebugMessenger();
    bool createSurface(SDL_Window* window);
    bool pickPhysicalDevice();
    bool createLogicalDevice();
    bool checkValidationLayerSupport() const;
    void destroyDebugMessenger();

    VkInstance               m_instance       = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR             m_surface        = VK_NULL_HANDLE;
    VkPhysicalDevice         m_physicalDevice = VK_NULL_HANDLE;
    VkDevice                 m_device         = VK_NULL_HANDLE;
    VkQueue                  m_graphicsQueue  = VK_NULL_HANDLE;
    uint32_t                 m_graphicsFamily = 0;
    bool                     m_validationEnabled = false;
};
