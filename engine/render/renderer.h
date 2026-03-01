#pragma once
#include "rhi/vulkan/swapchain.h"
#include "voxel/dual_contouring.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

class VulkanContext;

class Renderer {
public:
    bool init(VulkanContext& context);
    void drawFrame();
    void shutdown();
    bool ingestVoxelMeshSnapshot(oro::voxel::MeshBuffers snapshot);
    uint64_t ingestedMeshVersion() const { return m_ingestedMeshVersion; }
    uint64_t visibleMeshVersion() const { return m_visibleMeshVersion; }
    std::optional<uint64_t> popActivatedMeshVersion();

private:
    struct GpuBuffer {
        VkBuffer handle = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
    };

    struct MeshGpuResources {
        uint64_t sourceVersion = 0;
        std::size_t vertexCount = 0;
        std::size_t indexCount = 0;
        GpuBuffer vertexBuffer{};
        GpuBuffer indexBuffer{};
    };

    bool createCommandPool();
    void destroyCommandPool();
    bool createFrameResources();
    void destroyFrameResources();
    bool createSwapchainSemaphores();
    void destroySwapchainSemaphores();
    bool createRenderPass();
    void destroyRenderPass();
    bool createDepthResources();
    void destroyDepthResources();
    bool createFramebuffers();
    void destroyFramebuffers();
    bool createGraphicsPipeline();
    void destroyGraphicsPipeline();
    bool createImage(uint32_t width,
                     uint32_t height,
                     VkFormat format,
                     VkImageTiling tiling,
                     VkImageUsageFlags usage,
                     VkMemoryPropertyFlags properties,
                     VkImage& outImage,
                     VkDeviceMemory& outMemory) const;
    VkFormat findDepthFormat() const;
    bool recordDrawCommands(VkCommandBuffer commandBuffer, uint32_t imageIndex);
    bool recreateSwapchain();
    bool waitForFrameFence(VkFence fence) const;

    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required) const;
    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, GpuBuffer& outBuffer);
    void destroyBuffer(GpuBuffer& buffer);
    bool copyBuffer(const GpuBuffer& src, const GpuBuffer& dst, VkDeviceSize size);
    bool uploadPendingMesh();
    void swapInUploadedMesh();

    struct FrameResources {
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkSemaphore imageAvailableSemaphore = VK_NULL_HANDLE;
        VkFence inFlightFence = VK_NULL_HANDLE;
    };

    static constexpr uint32_t kMaxFramesInFlight = 2;

    VulkanContext* m_context = nullptr;
    VkFence m_uploadFence = VK_NULL_HANDLE;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    VulkanSwapchain m_swapchain;
    VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;
    VkImage m_depthImage = VK_NULL_HANDLE;
    VkDeviceMemory m_depthImageMemory = VK_NULL_HANDLE;
    VkImageView m_depthImageView = VK_NULL_HANDLE;
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> m_framebuffers;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_graphicsPipeline = VK_NULL_HANDLE;
    std::array<FrameResources, kMaxFramesInFlight> m_frames{};
    uint32_t m_currentFrame = 0;
    std::vector<VkFence> m_imagesInFlight;
    std::vector<VkSemaphore> m_renderFinishedSemaphores;
    std::deque<oro::voxel::MeshBuffers> m_pendingMeshSnapshots;
    std::optional<MeshGpuResources> m_uploadedMesh;
    std::optional<MeshGpuResources> m_activeMesh;
    std::deque<uint64_t> m_activatedMeshVersions;
    uint64_t m_ingestedMeshVersion = 0;
    uint64_t m_visibleMeshVersion = 0;
};