#pragma once
#include "core/camera.h"
#include "material/material.h"
#include "render/debug_view.h"
#include "rhi/vulkan/swapchain.h"
#include "voxel/runtime.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>
#include <vector>

class VulkanContext;

class Renderer {
public:
    bool init(VulkanContext& context);
    void drawFrame(const Camera& camera, DebugViewMode debugMode, uint32_t overlayFlags);
    void shutdown();
    bool uploadMaterialTable(const std::vector<oro::material::GpuMaterialEntry>& materialTable);
    bool ingestVoxelMeshSnapshot(oro::voxel::VoxelRuntime::PublishedMeshSnapshot snapshot);
    uint64_t ingestedMeshVersion() const { return m_ingestedMeshVersion; }
    uint64_t visibleMeshVersion() const { return m_visibleMeshVersion; }
    std::optional<uint64_t> popActivatedMeshVersion();

private:
    struct GpuBuffer {
        VkBuffer handle = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
    };

    struct SceneUniforms {
        glm::vec3 cameraPosition{0.0f};
        float _pad0 = 0.0f;
        glm::vec3 lightDirection{0.0f, 1.0f, 0.0f};
        float _pad1 = 0.0f;
        glm::vec3 lightColor{1.0f};
        float lightIntensity = 1.0f;
        glm::vec3 ambientColor{0.08f, 0.09f, 0.1f};
        float ambientIntensity = 1.0f;
        glm::mat4 lightSpaceMatrix{1.0f};
    };
    static_assert(sizeof(SceneUniforms) == 128, "SceneUniforms must match shader std140 layout");

    struct MeshGpuResources {
        uint64_t sourceVersion = 0;
        std::size_t vertexCount = 0;
        std::size_t indexCount = 0;
        std::size_t lineVertexCount = 0;
        std::size_t lineIndexCount = 0;
        GpuBuffer vertexBuffer{};
        GpuBuffer indexBuffer{};
        GpuBuffer lineVertexBuffer{};
        GpuBuffer lineIndexBuffer{};
    };

    struct ChunkCacheEntry {
        MeshGpuResources resources{};
        float voxelSize = oro::voxel::kDefaultVoxelSize;
        bool forceTwoSidedFallback = false;
    };

    struct PendingSnapshot {
        oro::voxel::MeshPatchBatch patchBatch;
        float voxelSize = oro::voxel::kDefaultVoxelSize;
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
    bool createShadowResources();
    void destroyShadowResources();
    bool createShadowRenderPass();
    void destroyShadowRenderPass();
    bool createFramebuffers();
    void destroyFramebuffers();
    bool createShadowFramebuffer();
    void destroyShadowFramebuffer();
    bool createGraphicsPipeline();
    bool createShadowPipeline();
    void destroyShadowPipeline();
    bool createPipeline(VkPolygonMode polygonMode,
                        VkPrimitiveTopology topology,
                        VkCullModeFlags cullMode,
                        VkPipelineLayout pipelineLayout,
                        VkPipeline* outPipeline);
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
    VkFormat findSupportedShadowDepthFormat() const;
    glm::mat4 computeDynamicLightSpaceMatrix(const glm::vec3& lightDirection) const;
    bool recordDrawCommands(VkCommandBuffer commandBuffer,
                            uint32_t imageIndex,
                            const Camera& camera,
                            DebugViewMode debugMode,
                            uint32_t overlayFlags);
    bool createMaterialDescriptorResources();
    void destroyMaterialDescriptorResources();
    bool updateMaterialDescriptorSets();
    bool recreateSwapchain();
    bool waitForFrameFence(VkFence fence) const;

    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required) const;
    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, GpuBuffer& outBuffer);
    void destroyBuffer(GpuBuffer& buffer);
    bool copyBuffer(const GpuBuffer& src, const GpuBuffer& dst, VkDeviceSize size);
    bool uploadMeshBuffers(const oro::voxel::MeshBuffers& mesh, MeshGpuResources& uploaded);
    bool uploadChunkLineOverlay(const oro::voxel::ChunkCoord& coord, float voxelSize, MeshGpuResources& uploaded);
    bool applyPendingPatchBatch();
    void destroyMeshResources(MeshGpuResources& resources);

    struct FrameResources {
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkSemaphore imageAvailableSemaphore = VK_NULL_HANDLE;
        VkFence inFlightFence = VK_NULL_HANDLE;
    };

    static constexpr uint32_t kMaxFramesInFlight = 2;
    static constexpr uint32_t kShadowMapResolution = 2048;

    VulkanContext* m_context = nullptr;
    VkFence m_uploadFence = VK_NULL_HANDLE;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    VulkanSwapchain m_swapchain;
    VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;
    VkImage m_depthImage = VK_NULL_HANDLE;
    VkDeviceMemory m_depthImageMemory = VK_NULL_HANDLE;
    VkImageView m_depthImageView = VK_NULL_HANDLE;
    VkFormat m_shadowDepthFormat = VK_FORMAT_UNDEFINED;
    VkImage m_shadowDepthImage = VK_NULL_HANDLE;
    VkDeviceMemory m_shadowDepthImageMemory = VK_NULL_HANDLE;
    VkImageView m_shadowDepthImageView = VK_NULL_HANDLE;
    VkSampler m_shadowSampler = VK_NULL_HANDLE;
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkRenderPass m_shadowRenderPass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> m_framebuffers;
    VkFramebuffer m_shadowFramebuffer = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_shadowPipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_materialDescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_materialDescriptorPool = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kMaxFramesInFlight> m_materialDescriptorSets{};
    GpuBuffer m_materialBuffer{};
    std::array<GpuBuffer, kMaxFramesInFlight> m_sceneUniformBuffers{};
    VkPipeline m_graphicsPipeline = VK_NULL_HANDLE;
    VkPipeline m_graphicsNoCullPipeline = VK_NULL_HANDLE;
    VkPipeline m_wireframePipeline = VK_NULL_HANDLE;
    VkPipeline m_chunkLinePipeline = VK_NULL_HANDLE;
    VkPipeline m_shadowPipeline = VK_NULL_HANDLE;
    std::array<FrameResources, kMaxFramesInFlight> m_frames{};
    uint32_t m_currentFrame = 0;
    std::vector<VkFence> m_imagesInFlight;
    std::vector<VkSemaphore> m_renderFinishedSemaphores;
    std::deque<PendingSnapshot> m_pendingMeshSnapshots;
    std::unordered_map<oro::voxel::ChunkCoord, ChunkCacheEntry, oro::voxel::ChunkCoordHash> m_chunkMeshCache;
    std::deque<uint64_t> m_activatedMeshVersions;
    uint64_t m_ingestedMeshVersion = 0;
    uint64_t m_visibleMeshVersion = 0;
    uint64_t m_lastFrameFallbackChunkDraws = 0;
    uint64_t m_lastFrameFallbackIndexDraws = 0;
};