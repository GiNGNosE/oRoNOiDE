#include "render/renderer.h"

#include "core/log.h"
#include "rhi/vulkan/context.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <sstream>
#include <type_traits>
#include <vector>

namespace {

struct PushConstants {
    glm::mat4 mvp{1.0f};
    uint32_t debugMode = 0;
    uint32_t _padding0 = 0;
    uint32_t _padding1 = 0;
    uint32_t _padding2 = 0;
};

struct ShadowPushConstants {
    glm::mat4 lightViewProj{1.0f};
};

const glm::vec3 kDefaultLightDirection = glm::normalize(glm::vec3(0.45f, 0.8f, 0.35f));
const glm::vec3 kDefaultLightColor = glm::vec3(1.0f, 0.98f, 0.95f);
constexpr float kDefaultLightIntensity = 3.2f;
const glm::vec3 kDefaultAmbientColor = glm::vec3(0.08f, 0.09f, 0.1f);
constexpr float kDefaultAmbientIntensity = 1.0f;
constexpr float kShadowBoundsPadding = 2.0f;
constexpr float kShadowFallbackHalfExtent = 12.0f;
void appendRendererDebugLog(const char* runId,
                            const char* hypothesisId,
                            const char* location,
                            const char* message,
                            const std::string& dataJson) {
    (void)runId;
    (void)hypothesisId;
    (void)location;
    (void)message;
    (void)dataJson;
}

std::vector<char> readBinaryFile(const char* path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        return {};
    }

    const std::streamsize fileSize = file.tellg();
    if (fileSize <= 0) {
        return {};
    }

    std::vector<char> buffer(static_cast<std::size_t>(fileSize));
    file.seekg(0);
    file.read(buffer.data(), fileSize);
    return buffer;
}

std::vector<char> loadShaderBinary(const std::vector<const char*>& candidatePaths) {
    for (const char* path : candidatePaths) {
        std::vector<char> code = readBinaryFile(path);
        if (!code.empty()) {
            ORO_LOG_INFO("Loaded shader binary: %s (%zu bytes)", path, code.size());
            return code;
        }
    }
    return {};
}

bool createShaderModule(VkDevice device, const std::vector<char>& code, VkShaderModule& outModule) {
    if (code.empty()) {
        return false;
    }
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    const VkResult r = vkCreateShaderModule(device, &ci, nullptr, &outModule);
    return r == VK_SUCCESS;
}

}  // namespace

bool Renderer::init(VulkanContext& context) {
    m_context = &context;
    m_currentFrame = 0;
    m_imagesInFlight.clear();
    m_materialDescriptorSets.fill(VK_NULL_HANDLE);

    if (!m_swapchain.init(context)) {
        m_context = nullptr;
        return false;
    }

    VkFenceCreateInfo uploadFenceInfo{};
    uploadFenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    const VkResult uploadFenceResult = vkCreateFence(context.device(), &uploadFenceInfo, nullptr, &m_uploadFence);
    if (uploadFenceResult != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed to create Vulkan upload fence: %d", uploadFenceResult);
        shutdown();
        return false;
    }

    if (!createCommandPool() ||
        !createFrameResources() ||
        !createSwapchainSemaphores() ||
        !createRenderPass() ||
        !createShadowRenderPass() ||
        !createShadowResources() ||
        !createShadowFramebuffer() ||
        !createMaterialDescriptorResources() ||
        !createGraphicsPipeline() ||
        !createShadowPipeline() ||
        !createDepthResources() ||
        !createFramebuffers()) {
        ORO_LOG_ERROR("Renderer initialization failed while creating frame resources");
        shutdown();
        return false;
    }

    m_imagesInFlight.assign(m_swapchain.imageCount(), VK_NULL_HANDLE);
    if (!uploadMaterialTable({})) {
        ORO_LOG_ERROR("Failed to initialize default renderer material table");
        shutdown();
        return false;
    }
    return true;
}

bool Renderer::createCommandPool() {
    if (!m_context) {
        return false;
    }
    VkCommandPoolCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    ci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    ci.queueFamilyIndex = m_context->graphicsFamily();
    const VkResult r = vkCreateCommandPool(m_context->device(), &ci, nullptr, &m_commandPool);
    if (r != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed to create renderer command pool: %d", r);
        m_commandPool = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

void Renderer::destroyCommandPool() {
    if (m_context && m_commandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_context->device(), m_commandPool, nullptr);
        m_commandPool = VK_NULL_HANDLE;
    }
}

bool Renderer::createFrameResources() {
    if (!m_context || m_commandPool == VK_NULL_HANDLE) {
        return false;
    }

    std::array<VkCommandBuffer, kMaxFramesInFlight> commandBuffers{};
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = m_commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = kMaxFramesInFlight;
    VkResult r = vkAllocateCommandBuffers(m_context->device(), &allocInfo, commandBuffers.data());
    if (r != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed to allocate frame command buffers: %d", r);
        return false;
    }

    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        m_frames[i].commandBuffer = commandBuffers[i];

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        r = vkCreateSemaphore(m_context->device(), &semaphoreInfo, nullptr, &m_frames[i].imageAvailableSemaphore);
        if (r != VK_SUCCESS) {
            ORO_LOG_ERROR("Failed to create image-available semaphore: %d", r);
            return false;
        }

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        r = vkCreateFence(m_context->device(), &fenceInfo, nullptr, &m_frames[i].inFlightFence);
        if (r != VK_SUCCESS) {
            ORO_LOG_ERROR("Failed to create frame in-flight fence: %d", r);
            return false;
        }
    }

    return true;
}

bool Renderer::createSwapchainSemaphores() {
    if (!m_context) {
        return false;
    }
    destroySwapchainSemaphores();
    m_renderFinishedSemaphores.reserve(m_swapchain.imageCount());

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (uint32_t i = 0; i < m_swapchain.imageCount(); ++i) {
        VkSemaphore semaphore = VK_NULL_HANDLE;
        const VkResult r = vkCreateSemaphore(m_context->device(), &semaphoreInfo, nullptr, &semaphore);
        if (r != VK_SUCCESS) {
            ORO_LOG_ERROR("Failed to create swapchain render-finished semaphore: %d", r);
            destroySwapchainSemaphores();
            return false;
        }
        m_renderFinishedSemaphores.push_back(semaphore);
    }
    return true;
}

void Renderer::destroySwapchainSemaphores() {
    if (!m_context) {
        return;
    }
    for (VkSemaphore semaphore : m_renderFinishedSemaphores) {
        if (semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(m_context->device(), semaphore, nullptr);
        }
    }
    m_renderFinishedSemaphores.clear();
}

void Renderer::destroyFrameResources() {
    if (!m_context) {
        return;
    }

    for (FrameResources& frame : m_frames) {
        if (frame.inFlightFence != VK_NULL_HANDLE) {
            vkDestroyFence(m_context->device(), frame.inFlightFence, nullptr);
            frame.inFlightFence = VK_NULL_HANDLE;
        }
        if (frame.imageAvailableSemaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(m_context->device(), frame.imageAvailableSemaphore, nullptr);
            frame.imageAvailableSemaphore = VK_NULL_HANDLE;
        }
    }

    if (m_commandPool != VK_NULL_HANDLE) {
        std::array<VkCommandBuffer, kMaxFramesInFlight> buffers{};
        for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
            buffers[i] = m_frames[i].commandBuffer;
            m_frames[i].commandBuffer = VK_NULL_HANDLE;
        }
        vkFreeCommandBuffers(m_context->device(), m_commandPool, kMaxFramesInFlight, buffers.data());
    } else {
        for (FrameResources& frame : m_frames) {
            frame.commandBuffer = VK_NULL_HANDLE;
        }
    }
}

VkFormat Renderer::findDepthFormat() const {
    if (!m_context) {
        return VK_FORMAT_UNDEFINED;
    }
    const std::array<VkFormat, 3> candidates = {
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D24_UNORM_S8_UINT};

    for (VkFormat format : candidates) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(m_context->physicalDevice(), format, &properties);
        if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
            return format;
        }
    }
    return VK_FORMAT_UNDEFINED;
}

VkFormat Renderer::findSupportedShadowDepthFormat() const {
    if (!m_context) {
        return VK_FORMAT_UNDEFINED;
    }

    const std::array<VkFormat, 3> candidates = {
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D24_UNORM_S8_UINT};

    for (VkFormat format : candidates) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(m_context->physicalDevice(), format, &properties);
        VkFormatFeatureFlags requiredFeatures =
            VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
#ifdef VK_FORMAT_FEATURE_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT
        requiredFeatures |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT;
#endif
        const bool supportsRequiredFeatures =
            (properties.optimalTilingFeatures & requiredFeatures) == requiredFeatures;
        if (supportsRequiredFeatures) {
            return format;
        }
    }

    return VK_FORMAT_UNDEFINED;
}

glm::mat4 Renderer::computeDynamicLightSpaceMatrix(const glm::vec3& lightDirection) const {
    glm::vec3 worldMin(std::numeric_limits<float>::max());
    glm::vec3 worldMax(std::numeric_limits<float>::lowest());
    bool hasBounds = false;

    for (const auto& [coord, entry] : m_chunkMeshCache) {
        const float chunkWorldEdge = static_cast<float>(oro::voxel::kChunkEdge) * entry.voxelSize;
        if (chunkWorldEdge <= 0.0f) {
            continue;
        }
        const glm::vec3 chunkMin(
            static_cast<float>(coord.x) * chunkWorldEdge,
            static_cast<float>(coord.y) * chunkWorldEdge,
            static_cast<float>(coord.z) * chunkWorldEdge);
        const glm::vec3 chunkMax = chunkMin + glm::vec3(chunkWorldEdge);
        worldMin = glm::min(worldMin, chunkMin);
        worldMax = glm::max(worldMax, chunkMax);
        hasBounds = true;
    }

    if (!hasBounds) {
        worldMin = glm::vec3(-kShadowFallbackHalfExtent);
        worldMax = glm::vec3(+kShadowFallbackHalfExtent);
    }

    glm::vec3 safeLightDirection = lightDirection;
    const float lightLenSq = glm::dot(safeLightDirection, safeLightDirection);
    if (lightLenSq <= 1e-6f) {
        safeLightDirection = kDefaultLightDirection;
    } else {
        safeLightDirection = glm::normalize(safeLightDirection);
    }

    const glm::vec3 center = 0.5f * (worldMin + worldMax);
    const glm::vec3 halfExtents = 0.5f * (worldMax - worldMin);
    const float sceneRadius = glm::length(halfExtents) + kShadowBoundsPadding;
    const glm::vec3 lightPosition = center - safeLightDirection * std::max(sceneRadius, 1.0f);

    glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
    if (std::abs(glm::dot(up, safeLightDirection)) > 0.98f) {
        up = glm::vec3(1.0f, 0.0f, 0.0f);
    }
    const glm::mat4 lightView = glm::lookAt(lightPosition, center, up);

    const std::array<glm::vec3, 8> corners = {
        glm::vec3(worldMin.x, worldMin.y, worldMin.z),
        glm::vec3(worldMax.x, worldMin.y, worldMin.z),
        glm::vec3(worldMin.x, worldMax.y, worldMin.z),
        glm::vec3(worldMax.x, worldMax.y, worldMin.z),
        glm::vec3(worldMin.x, worldMin.y, worldMax.z),
        glm::vec3(worldMax.x, worldMin.y, worldMax.z),
        glm::vec3(worldMin.x, worldMax.y, worldMax.z),
        glm::vec3(worldMax.x, worldMax.y, worldMax.z),
    };

    glm::vec3 lightMin(std::numeric_limits<float>::max());
    glm::vec3 lightMax(std::numeric_limits<float>::lowest());
    for (const glm::vec3& corner : corners) {
        const glm::vec3 lightSpaceCorner = glm::vec3(lightView * glm::vec4(corner, 1.0f));
        lightMin = glm::min(lightMin, lightSpaceCorner);
        lightMax = glm::max(lightMax, lightSpaceCorner);
    }

    const float left = lightMin.x - kShadowBoundsPadding;
    const float right = lightMax.x + kShadowBoundsPadding;
    const float bottom = lightMin.y - kShadowBoundsPadding;
    const float top = lightMax.y + kShadowBoundsPadding;
    float nearPlane = -lightMax.z - kShadowBoundsPadding;
    float farPlane = -lightMin.z + kShadowBoundsPadding;
    nearPlane = std::max(nearPlane, 0.1f);
    if (farPlane <= nearPlane) {
        farPlane = nearPlane + 1.0f;
    }

    static std::atomic<uint32_t> lightMatrixLogCount{0};
    const uint32_t loggedLightMatrices = lightMatrixLogCount.fetch_add(1U, std::memory_order_relaxed);
    if (loggedLightMatrices < 6U) {
        std::ostringstream oss;
        oss << "{\"hasBounds\":" << (hasBounds ? 1 : 0)
            << ",\"worldSpan\":[" << (worldMax.x - worldMin.x) << "," << (worldMax.y - worldMin.y) << ","
            << (worldMax.z - worldMin.z) << "]"
            << ",\"lightSpan\":[" << (lightMax.x - lightMin.x) << "," << (lightMax.y - lightMin.y) << ","
            << (lightMax.z - lightMin.z) << "]"
            << ",\"nearPlane\":" << nearPlane
            << ",\"farPlane\":" << farPlane
            << ",\"depthSpan\":" << (farPlane - nearPlane)
            << ",\"sceneRadius\":" << sceneRadius << "}";
        // #region agent log
        appendRendererDebugLog("shadow-scales-pass2",
                               "H17_H20",
                               "engine/render/renderer.cpp:computeDynamicLightSpaceMatrix",
                               "Shadow light-space fit metrics",
                               oss.str());
        // #endregion
    }

    glm::mat4 lightProjection = glm::orthoRH_ZO(left, right, bottom, top, nearPlane, farPlane);
    lightProjection[1][1] *= -1.0f;  // Match Vulkan's inverted clip-space Y convention.
    const glm::mat4 lightSpace = lightProjection * lightView;
    float minNdcX = std::numeric_limits<float>::max();
    float maxNdcX = std::numeric_limits<float>::lowest();
    float minNdcY = std::numeric_limits<float>::max();
    float maxNdcY = std::numeric_limits<float>::lowest();
    float minNdcZ = std::numeric_limits<float>::max();
    float maxNdcZ = std::numeric_limits<float>::lowest();
    uint32_t cornersOutsideShadowClip = 0;
    for (const glm::vec3& corner : corners) {
        const glm::vec4 clip = lightSpace * glm::vec4(corner, 1.0f);
        const float invW = std::abs(clip.w) > 1.0e-6f ? (1.0f / clip.w) : 0.0f;
        const float ndcX = clip.x * invW;
        const float ndcY = clip.y * invW;
        const float ndcZ = clip.z * invW;
        minNdcX = std::min(minNdcX, ndcX);
        maxNdcX = std::max(maxNdcX, ndcX);
        minNdcY = std::min(minNdcY, ndcY);
        maxNdcY = std::max(maxNdcY, ndcY);
        minNdcZ = std::min(minNdcZ, ndcZ);
        maxNdcZ = std::max(maxNdcZ, ndcZ);
        if (ndcX < -1.0f || ndcX > 1.0f || ndcY < -1.0f || ndcY > 1.0f || ndcZ < 0.0f || ndcZ > 1.0f) {
            ++cornersOutsideShadowClip;
        }
    }
    if (loggedLightMatrices < 6U) {
        std::ostringstream oss;
        oss << "{\"ndcRangeX\":[" << minNdcX << "," << maxNdcX
            << "],\"ndcRangeY\":[" << minNdcY << "," << maxNdcY
            << "],\"ndcRangeZ\":[" << minNdcZ << "," << maxNdcZ
            << "],\"cornersOutsideShadowClip\":" << cornersOutsideShadowClip << "}";
        // #region agent log
        appendRendererDebugLog("shadow-scales-pass3",
                               "H21",
                               "engine/render/renderer.cpp:computeDynamicLightSpaceMatrix",
                               "Shadow clip-space convention diagnostics",
                               oss.str());
        // #endregion
    }
    return lightSpace;
}

bool Renderer::createRenderPass() {
    if (!m_context) {
        return false;
    }
    const VkFormat depthFormat = findDepthFormat();
    if (depthFormat == VK_FORMAT_UNDEFINED) {
        ORO_LOG_ERROR("Failed to find supported Vulkan depth format");
        return false;
    }
    m_depthFormat = depthFormat;

    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = m_swapchain.format();
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = depthFormat;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorReference{};
    colorReference.attachment = 0;
    colorReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthReference{};
    depthReference.attachment = 1;
    depthReference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorReference;
    subpass.pDepthStencilAttachment = &depthReference;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    const std::array<VkAttachmentDescription, 2> attachments = {colorAttachment, depthAttachment};
    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    renderPassInfo.pAttachments = attachments.data();
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    const VkResult r = vkCreateRenderPass(m_context->device(), &renderPassInfo, nullptr, &m_renderPass);
    if (r != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed to create Vulkan render pass: %d", r);
        m_renderPass = VK_NULL_HANDLE;
        return false;
    }

    return true;
}

void Renderer::destroyRenderPass() {
    if (m_context && m_renderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(m_context->device(), m_renderPass, nullptr);
        m_renderPass = VK_NULL_HANDLE;
    }
}

bool Renderer::createShadowRenderPass() {
    if (!m_context) {
        return false;
    }
    const VkFormat shadowDepthFormat = findSupportedShadowDepthFormat();
    if (shadowDepthFormat == VK_FORMAT_UNDEFINED) {
        ORO_LOG_ERROR("No depth format supports depth attachment + sampled compare for shadows");
        return false;
    }
    m_shadowDepthFormat = shadowDepthFormat;

    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = shadowDepthFormat;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkAttachmentReference depthReference{};
    depthReference.attachment = 0;
    depthReference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 0;
    subpass.pDepthStencilAttachment = &depthReference;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependency.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &depthAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    const VkResult r = vkCreateRenderPass(m_context->device(), &renderPassInfo, nullptr, &m_shadowRenderPass);
    if (r != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed to create shadow render pass: %d", r);
        m_shadowRenderPass = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

void Renderer::destroyShadowRenderPass() {
    if (m_context && m_shadowRenderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(m_context->device(), m_shadowRenderPass, nullptr);
        m_shadowRenderPass = VK_NULL_HANDLE;
    }
}

bool Renderer::createImage(uint32_t width,
                           uint32_t height,
                           VkFormat format,
                           VkImageTiling tiling,
                           VkImageUsageFlags usage,
                           VkMemoryPropertyFlags properties,
                           VkImage& outImage,
                           VkDeviceMemory& outMemory) const {
    if (!m_context) {
        return false;
    }

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = tiling;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult r = vkCreateImage(m_context->device(), &imageInfo, nullptr, &outImage);
    if (r != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed to create Vulkan image: %d", r);
        return false;
    }

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(m_context->device(), outImage, &requirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = requirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, properties);
    if (allocInfo.memoryTypeIndex == UINT32_MAX) {
        ORO_LOG_ERROR("Failed to find suitable memory type for image");
        vkDestroyImage(m_context->device(), outImage, nullptr);
        outImage = VK_NULL_HANDLE;
        return false;
    }

    r = vkAllocateMemory(m_context->device(), &allocInfo, nullptr, &outMemory);
    if (r != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed to allocate image memory: %d", r);
        vkDestroyImage(m_context->device(), outImage, nullptr);
        outImage = VK_NULL_HANDLE;
        return false;
    }

    r = vkBindImageMemory(m_context->device(), outImage, outMemory, 0);
    if (r != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed to bind image memory: %d", r);
        vkFreeMemory(m_context->device(), outMemory, nullptr);
        outMemory = VK_NULL_HANDLE;
        vkDestroyImage(m_context->device(), outImage, nullptr);
        outImage = VK_NULL_HANDLE;
        return false;
    }

    return true;
}

bool Renderer::createDepthResources() {
    if (!m_context || m_depthFormat == VK_FORMAT_UNDEFINED) {
        return false;
    }
    const VkExtent2D extent = m_swapchain.extent();
    if (extent.width == 0 || extent.height == 0) {
        return false;
    }

    if (!createImage(extent.width,
                     extent.height,
                     m_depthFormat,
                     VK_IMAGE_TILING_OPTIMAL,
                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                     m_depthImage,
                     m_depthImageMemory)) {
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_depthImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = m_depthFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    const VkResult r = vkCreateImageView(m_context->device(), &viewInfo, nullptr, &m_depthImageView);
    if (r != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed to create depth image view: %d", r);
        destroyDepthResources();
        return false;
    }

    return true;
}

void Renderer::destroyDepthResources() {
    if (!m_context) {
        return;
    }
    if (m_depthImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_context->device(), m_depthImageView, nullptr);
        m_depthImageView = VK_NULL_HANDLE;
    }
    if (m_depthImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_context->device(), m_depthImage, nullptr);
        m_depthImage = VK_NULL_HANDLE;
    }
    if (m_depthImageMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_context->device(), m_depthImageMemory, nullptr);
        m_depthImageMemory = VK_NULL_HANDLE;
    }
}

bool Renderer::createShadowResources() {
    if (!m_context || m_shadowDepthFormat == VK_FORMAT_UNDEFINED) {
        return false;
    }

    if (!createImage(kShadowMapResolution,
                     kShadowMapResolution,
                     m_shadowDepthFormat,
                     VK_IMAGE_TILING_OPTIMAL,
                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                     m_shadowDepthImage,
                     m_shadowDepthImageMemory)) {
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_shadowDepthImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = m_shadowDepthFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    VkResult r = vkCreateImageView(m_context->device(), &viewInfo, nullptr, &m_shadowDepthImageView);
    if (r != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed to create shadow depth image view: %d", r);
        destroyShadowResources();
        return false;
    }

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 1.0f;
    r = vkCreateSampler(m_context->device(), &samplerInfo, nullptr, &m_shadowSampler);
    if (r != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed to create shadow sampler: %d", r);
        destroyShadowResources();
        return false;
    }
    if (m_context->portabilitySubsetEnabled()) {
        ORO_LOG_INFO("Using portability-safe shadow sampler path (manual depth compare)");
    }
    {
        std::ostringstream oss;
        oss << "{\"shadowDepthFormat\":" << static_cast<uint32_t>(m_shadowDepthFormat)
            << ",\"magFilter\":" << static_cast<uint32_t>(samplerInfo.magFilter)
            << ",\"minFilter\":" << static_cast<uint32_t>(samplerInfo.minFilter)
            << ",\"compareEnable\":" << (samplerInfo.compareEnable == VK_TRUE ? 1 : 0)
            << ",\"shadowMapResolution\":" << kShadowMapResolution << "}";
        // #region agent log
        appendRendererDebugLog("shadow-scales-pass2",
                               "H18_H19",
                               "engine/render/renderer.cpp:createShadowResources",
                               "Shadow resource configuration",
                               oss.str());
        // #endregion
    }
    return true;
}

void Renderer::destroyShadowResources() {
    if (!m_context) {
        return;
    }
    if (m_shadowSampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_context->device(), m_shadowSampler, nullptr);
        m_shadowSampler = VK_NULL_HANDLE;
    }
    if (m_shadowDepthImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_context->device(), m_shadowDepthImageView, nullptr);
        m_shadowDepthImageView = VK_NULL_HANDLE;
    }
    if (m_shadowDepthImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_context->device(), m_shadowDepthImage, nullptr);
        m_shadowDepthImage = VK_NULL_HANDLE;
    }
    if (m_shadowDepthImageMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_context->device(), m_shadowDepthImageMemory, nullptr);
        m_shadowDepthImageMemory = VK_NULL_HANDLE;
    }
}

bool Renderer::createFramebuffers() {
    if (!m_context || m_renderPass == VK_NULL_HANDLE || m_depthImageView == VK_NULL_HANDLE) {
        return false;
    }

    m_framebuffers.clear();
    const auto& imageViews = m_swapchain.imageViews();
    m_framebuffers.reserve(imageViews.size());

    for (VkImageView colorView : imageViews) {
        const std::array<VkImageView, 2> attachments = {colorView, m_depthImageView};
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = m_renderPass;
        framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        framebufferInfo.pAttachments = attachments.data();
        framebufferInfo.width = m_swapchain.extent().width;
        framebufferInfo.height = m_swapchain.extent().height;
        framebufferInfo.layers = 1;

        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        const VkResult r = vkCreateFramebuffer(m_context->device(), &framebufferInfo, nullptr, &framebuffer);
        if (r != VK_SUCCESS) {
            ORO_LOG_ERROR("Failed to create Vulkan framebuffer: %d", r);
            destroyFramebuffers();
            return false;
        }
        m_framebuffers.push_back(framebuffer);
    }

    return true;
}

bool Renderer::createShadowFramebuffer() {
    if (!m_context || m_shadowRenderPass == VK_NULL_HANDLE || m_shadowDepthImageView == VK_NULL_HANDLE) {
        return false;
    }

    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = m_shadowRenderPass;
    framebufferInfo.attachmentCount = 1;
    framebufferInfo.pAttachments = &m_shadowDepthImageView;
    framebufferInfo.width = kShadowMapResolution;
    framebufferInfo.height = kShadowMapResolution;
    framebufferInfo.layers = 1;

    const VkResult r = vkCreateFramebuffer(m_context->device(), &framebufferInfo, nullptr, &m_shadowFramebuffer);
    if (r != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed to create shadow framebuffer: %d", r);
        m_shadowFramebuffer = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

void Renderer::destroyShadowFramebuffer() {
    if (m_context && m_shadowFramebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(m_context->device(), m_shadowFramebuffer, nullptr);
        m_shadowFramebuffer = VK_NULL_HANDLE;
    }
}

void Renderer::destroyFramebuffers() {
    if (!m_context) {
        return;
    }
    for (VkFramebuffer framebuffer : m_framebuffers) {
        if (framebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(m_context->device(), framebuffer, nullptr);
        }
    }
    m_framebuffers.clear();
}

bool Renderer::createMaterialDescriptorResources() {
    if (!m_context) {
        return false;
    }

    VkDescriptorSetLayoutBinding materialBinding{};
    materialBinding.binding = 0;
    materialBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    materialBinding.descriptorCount = 1;
    materialBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutBinding sceneBinding{};
    sceneBinding.binding = 1;
    sceneBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sceneBinding.descriptorCount = 1;
    sceneBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutBinding shadowBinding{};
    shadowBinding.binding = 2;
    shadowBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    shadowBinding.descriptorCount = 1;
    shadowBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    const std::array<VkDescriptorSetLayoutBinding, 3> bindings = {
        materialBinding,
        sceneBinding,
        shadowBinding,
    };

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    VkResult r = vkCreateDescriptorSetLayout(
        m_context->device(), &layoutInfo, nullptr, &m_materialDescriptorSetLayout);
    if (r != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed to create material descriptor set layout: %d", r);
        return false;
    }

    VkDescriptorPoolSize materialPoolSize{};
    materialPoolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    materialPoolSize.descriptorCount = kMaxFramesInFlight;

    VkDescriptorPoolSize scenePoolSize{};
    scenePoolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    scenePoolSize.descriptorCount = kMaxFramesInFlight;

    VkDescriptorPoolSize shadowPoolSize{};
    shadowPoolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    shadowPoolSize.descriptorCount = kMaxFramesInFlight;

    const std::array<VkDescriptorPoolSize, 3> poolSizes = {
        materialPoolSize,
        scenePoolSize,
        shadowPoolSize,
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = kMaxFramesInFlight;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    r = vkCreateDescriptorPool(m_context->device(), &poolInfo, nullptr, &m_materialDescriptorPool);
    if (r != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed to create material descriptor pool: %d", r);
        destroyMaterialDescriptorResources();
        return false;
    }

    std::array<VkDescriptorSetLayout, kMaxFramesInFlight> layouts{};
    layouts.fill(m_materialDescriptorSetLayout);
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_materialDescriptorPool;
    allocInfo.descriptorSetCount = kMaxFramesInFlight;
    allocInfo.pSetLayouts = layouts.data();
    r = vkAllocateDescriptorSets(m_context->device(), &allocInfo, m_materialDescriptorSets.data());
    if (r != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed to allocate material descriptor sets: %d", r);
        destroyMaterialDescriptorResources();
        return false;
    }

    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        if (!createBuffer(sizeof(SceneUniforms),
                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          m_sceneUniformBuffers[i])) {
            ORO_LOG_ERROR("Failed to create scene UBO for frame %u", i);
            destroyMaterialDescriptorResources();
            return false;
        }
    }

    return true;
}

void Renderer::destroyMaterialDescriptorResources() {
    if (!m_context) {
        return;
    }

    for (GpuBuffer& sceneBuffer : m_sceneUniformBuffers) {
        destroyBuffer(sceneBuffer);
    }
    m_materialDescriptorSets.fill(VK_NULL_HANDLE);

    if (m_materialDescriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_context->device(), m_materialDescriptorPool, nullptr);
        m_materialDescriptorPool = VK_NULL_HANDLE;
    }
    if (m_materialDescriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_context->device(), m_materialDescriptorSetLayout, nullptr);
        m_materialDescriptorSetLayout = VK_NULL_HANDLE;
    }
}

bool Renderer::updateMaterialDescriptorSets() {
    if (!m_context ||
        m_materialDescriptorPool == VK_NULL_HANDLE ||
        m_materialDescriptorSetLayout == VK_NULL_HANDLE ||
        m_materialBuffer.handle == VK_NULL_HANDLE ||
        m_shadowSampler == VK_NULL_HANDLE ||
        m_shadowDepthImageView == VK_NULL_HANDLE) {
        return false;
    }

    std::array<VkWriteDescriptorSet, kMaxFramesInFlight * 3> writes{};
    std::array<VkDescriptorBufferInfo, kMaxFramesInFlight> bufferInfos{};
    std::array<VkDescriptorBufferInfo, kMaxFramesInFlight> sceneBufferInfos{};
    std::array<VkDescriptorImageInfo, kMaxFramesInFlight> shadowImageInfos{};
    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        if (m_sceneUniformBuffers[i].handle == VK_NULL_HANDLE) {
            ORO_LOG_ERROR("Scene UBO is not allocated for frame %u", i);
            return false;
        }

        bufferInfos[i].buffer = m_materialBuffer.handle;
        bufferInfos[i].offset = 0;
        bufferInfos[i].range = m_materialBuffer.size;

        sceneBufferInfos[i].buffer = m_sceneUniformBuffers[i].handle;
        sceneBufferInfos[i].offset = 0;
        sceneBufferInfos[i].range = sizeof(SceneUniforms);

        shadowImageInfos[i].sampler = m_shadowSampler;
        shadowImageInfos[i].imageView = m_shadowDepthImageView;
        shadowImageInfos[i].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet& materialWrite = writes[(i * 3) + 0];
        materialWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        materialWrite.dstSet = m_materialDescriptorSets[i];
        materialWrite.dstBinding = 0;
        materialWrite.descriptorCount = 1;
        materialWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        materialWrite.pBufferInfo = &bufferInfos[i];

        VkWriteDescriptorSet& sceneWrite = writes[(i * 3) + 1];
        sceneWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        sceneWrite.dstSet = m_materialDescriptorSets[i];
        sceneWrite.dstBinding = 1;
        sceneWrite.descriptorCount = 1;
        sceneWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        sceneWrite.pBufferInfo = &sceneBufferInfos[i];

        VkWriteDescriptorSet& shadowWrite = writes[(i * 3) + 2];
        shadowWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        shadowWrite.dstSet = m_materialDescriptorSets[i];
        shadowWrite.dstBinding = 2;
        shadowWrite.descriptorCount = 1;
        shadowWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        shadowWrite.pImageInfo = &shadowImageInfos[i];
    }

    vkUpdateDescriptorSets(
        m_context->device(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    return true;
}

bool Renderer::createGraphicsPipeline() {
    if (!m_context || m_renderPass == VK_NULL_HANDLE || m_materialDescriptorSetLayout == VK_NULL_HANDLE) {
        return false;
    }

    static_assert(std::is_standard_layout_v<oro::voxel::MeshVertex>, "MeshVertex must be standard layout");
    static_assert(sizeof(oro::voxel::MeshVertex::materialId) == sizeof(uint16_t), "materialId must be 16-bit");

    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_materialDescriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

    VkResult r = vkCreatePipelineLayout(m_context->device(), &pipelineLayoutInfo, nullptr, &m_pipelineLayout);
    if (r != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed to create Vulkan pipeline layout: %d", r);
        return false;
    }

    if (!createPipeline(
            VK_POLYGON_MODE_FILL, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_CULL_MODE_BACK_BIT, m_pipelineLayout, &m_graphicsPipeline)) {
        ORO_LOG_ERROR("Failed to create solid graphics pipeline");
        destroyGraphicsPipeline();
        return false;
    }
    if (!createPipeline(VK_POLYGON_MODE_FILL,
                        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                        VK_CULL_MODE_NONE,
                        m_pipelineLayout,
                        &m_graphicsNoCullPipeline)) {
        ORO_LOG_ERROR("Failed to create fallback no-cull graphics pipeline");
        destroyGraphicsPipeline();
        return false;
    }

    if (m_context->supportsNonSolidFill()) {
        if (!createPipeline(VK_POLYGON_MODE_LINE,
                            VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                            VK_CULL_MODE_BACK_BIT,
                            m_pipelineLayout,
                            &m_wireframePipeline)) {
            ORO_LOG_WARN("Failed to create wireframe pipeline; wireframe modes will fall back to lit");
        }
    }

    if (!createPipeline(
            VK_POLYGON_MODE_FILL, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_CULL_MODE_NONE, m_pipelineLayout, &m_chunkLinePipeline)) {
        ORO_LOG_ERROR("Failed to create chunk line pipeline");
        destroyGraphicsPipeline();
        return false;
    }

    return true;
}

bool Renderer::createShadowPipeline() {
    if (!m_context || m_shadowRenderPass == VK_NULL_HANDLE) {
        return false;
    }

    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(ShadowPushConstants);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 0;
    pipelineLayoutInfo.pSetLayouts = nullptr;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

    VkResult r = vkCreatePipelineLayout(m_context->device(), &pipelineLayoutInfo, nullptr, &m_shadowPipelineLayout);
    if (r != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed to create shadow pipeline layout: %d", r);
        return false;
    }

    const std::vector<char> vertCode = loadShaderBinary({
        "shaders/shadow_depth.vert.spv",
        "build/shaders/shadow_depth.vert.spv",
        "engine/render/shaders/shadow_depth.vert.spv"});
    const std::vector<char> fragCode = loadShaderBinary({
        "shaders/shadow_depth.frag.spv",
        "build/shaders/shadow_depth.frag.spv",
        "engine/render/shaders/shadow_depth.frag.spv"});
    if (vertCode.empty() || fragCode.empty()) {
        return false;
    }

    VkShaderModule vertShaderModule = VK_NULL_HANDLE;
    VkShaderModule fragShaderModule = VK_NULL_HANDLE;
    if (!createShaderModule(m_context->device(), vertCode, vertShaderModule) ||
        !createShaderModule(m_context->device(), fragCode, fragShaderModule)) {
        if (vertShaderModule != VK_NULL_HANDLE) {
            vkDestroyShaderModule(m_context->device(), vertShaderModule, nullptr);
        }
        if (fragShaderModule != VK_NULL_HANDLE) {
            vkDestroyShaderModule(m_context->device(), fragShaderModule, nullptr);
        }
        return false;
    }

    VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
    vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertShaderStageInfo.module = vertShaderModule;
    vertShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
    fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragShaderStageInfo.module = fragShaderModule;
    fragShaderStageInfo.pName = "main";

    const std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages = {
        vertShaderStageInfo,
        fragShaderStageInfo};

    VkVertexInputBindingDescription bindingDescription{};
    bindingDescription.binding = 0;
    bindingDescription.stride = static_cast<uint32_t>(sizeof(oro::voxel::MeshVertex));
    bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription positionAttribute{};
    positionAttribute.location = 0;
    positionAttribute.binding = 0;
    positionAttribute.format = VK_FORMAT_R32G32B32_SFLOAT;
    positionAttribute.offset = static_cast<uint32_t>(offsetof(oro::voxel::MeshVertex, position));

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
    vertexInputInfo.vertexAttributeDescriptionCount = 1;
    vertexInputInfo.pVertexAttributeDescriptions = &positionAttribute;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;
    rasterizer.depthBiasEnable = VK_TRUE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 0;
    colorBlending.pAttachments = nullptr;

    const std::array<VkDynamicState, 3> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_DEPTH_BIAS,
    };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
    pipelineInfo.pStages = shaderStages.data();
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_shadowPipelineLayout;
    pipelineInfo.renderPass = m_shadowRenderPass;
    pipelineInfo.subpass = 0;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;

    r = vkCreateGraphicsPipelines(m_context->device(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_shadowPipeline);
    vkDestroyShaderModule(m_context->device(), fragShaderModule, nullptr);
    vkDestroyShaderModule(m_context->device(), vertShaderModule, nullptr);
    if (r != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed to create shadow pipeline: %d", r);
        return false;
    }
    return true;
}

bool Renderer::createPipeline(VkPolygonMode polygonMode,
                              VkPrimitiveTopology topology,
                              VkCullModeFlags cullMode,
                              VkPipelineLayout pipelineLayout,
                              VkPipeline* outPipeline) {
    if (!m_context || !outPipeline || m_renderPass == VK_NULL_HANDLE || pipelineLayout == VK_NULL_HANDLE) {
        return false;
    }

    const std::vector<char> vertCode = loadShaderBinary({
        "shaders/voxel_mesh.vert.spv",
        "build/shaders/voxel_mesh.vert.spv",
        "engine/render/shaders/voxel_mesh.vert.spv"});
    const std::vector<char> fragCode = loadShaderBinary({
        "shaders/voxel_mesh.frag.spv",
        "build/shaders/voxel_mesh.frag.spv",
        "engine/render/shaders/voxel_mesh.frag.spv"});
    if (vertCode.empty() || fragCode.empty()) {
        return false;
    }

    VkShaderModule vertShaderModule = VK_NULL_HANDLE;
    VkShaderModule fragShaderModule = VK_NULL_HANDLE;
    if (!createShaderModule(m_context->device(), vertCode, vertShaderModule) ||
        !createShaderModule(m_context->device(), fragCode, fragShaderModule)) {
        if (vertShaderModule != VK_NULL_HANDLE) {
            vkDestroyShaderModule(m_context->device(), vertShaderModule, nullptr);
        }
        if (fragShaderModule != VK_NULL_HANDLE) {
            vkDestroyShaderModule(m_context->device(), fragShaderModule, nullptr);
        }
        return false;
    }

    VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
    vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertShaderStageInfo.module = vertShaderModule;
    vertShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
    fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragShaderStageInfo.module = fragShaderModule;
    fragShaderStageInfo.pName = "main";

    const std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages = {
        vertShaderStageInfo,
        fragShaderStageInfo};

    VkVertexInputBindingDescription bindingDescription{};
    bindingDescription.binding = 0;
    bindingDescription.stride = static_cast<uint32_t>(sizeof(oro::voxel::MeshVertex));
    bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    const std::array<VkVertexInputAttributeDescription, 3> attributeDescriptions = {
        VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(oro::voxel::MeshVertex, position))},
        VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(oro::voxel::MeshVertex, normal))},
        VkVertexInputAttributeDescription{2, 0, VK_FORMAT_R16_UINT, static_cast<uint32_t>(offsetof(oro::voxel::MeshVertex, materialId))},
    };

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
    vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = topology;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = polygonMode;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = cullMode;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;
    static std::atomic<uint32_t> pipelineCullProbeLogCount{0};
    const uint32_t loggedCullProbePipelines = pipelineCullProbeLogCount.fetch_add(1U, std::memory_order_relaxed);
    if (loggedCullProbePipelines < 6U) {
        std::ostringstream oss;
        oss << "{\"topology\":" << static_cast<uint32_t>(topology)
            << ",\"polygonMode\":" << static_cast<uint32_t>(polygonMode)
            << ",\"cullMode\":" << static_cast<uint32_t>(rasterizer.cullMode)
            << ",\"frontFace\":" << static_cast<uint32_t>(rasterizer.frontFace) << "}";
        // #region agent log
        appendRendererDebugLog("gap-pass1",
                               "H26",
                               "engine/render/renderer.cpp:createPipeline",
                               "Main pipeline culling configuration for gap probe",
                               oss.str());
        // #endregion
    }

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = (topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST) ? VK_FALSE : VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    const std::array<VkDynamicState, 2> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
    pipelineInfo.pStages = shaderStages.data();
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipelineLayout;
    pipelineInfo.renderPass = m_renderPass;
    pipelineInfo.subpass = 0;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;

    const VkResult r = vkCreateGraphicsPipelines(m_context->device(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, outPipeline);
    vkDestroyShaderModule(m_context->device(), fragShaderModule, nullptr);
    vkDestroyShaderModule(m_context->device(), vertShaderModule, nullptr);
    return r == VK_SUCCESS;
}

void Renderer::destroyGraphicsPipeline() {
    if (!m_context) {
        return;
    }
    if (m_graphicsPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_context->device(), m_graphicsPipeline, nullptr);
        m_graphicsPipeline = VK_NULL_HANDLE;
    }
    if (m_graphicsNoCullPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_context->device(), m_graphicsNoCullPipeline, nullptr);
        m_graphicsNoCullPipeline = VK_NULL_HANDLE;
    }
    if (m_wireframePipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_context->device(), m_wireframePipeline, nullptr);
        m_wireframePipeline = VK_NULL_HANDLE;
    }
    if (m_chunkLinePipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_context->device(), m_chunkLinePipeline, nullptr);
        m_chunkLinePipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_context->device(), m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
}

void Renderer::destroyShadowPipeline() {
    if (!m_context) {
        return;
    }
    if (m_shadowPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_context->device(), m_shadowPipeline, nullptr);
        m_shadowPipeline = VK_NULL_HANDLE;
    }
    if (m_shadowPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_context->device(), m_shadowPipelineLayout, nullptr);
        m_shadowPipelineLayout = VK_NULL_HANDLE;
    }
}

bool Renderer::recreateSwapchain() {
    if (!m_context) {
        return false;
    }
    vkDeviceWaitIdle(m_context->device());

    destroyFramebuffers();
    destroyDepthResources();
    destroyShadowFramebuffer();
    destroyShadowPipeline();
    destroyGraphicsPipeline();
    destroyMaterialDescriptorResources();
    destroyShadowResources();
    destroyShadowRenderPass();
    destroyRenderPass();
    destroySwapchainSemaphores();
    m_swapchain.shutdown();

    if (!m_swapchain.init(*m_context) ||
        !createSwapchainSemaphores() ||
        !createRenderPass() ||
        !createShadowRenderPass() ||
        !createShadowResources() ||
        !createShadowFramebuffer() ||
        !createMaterialDescriptorResources() ||
        !createGraphicsPipeline() ||
        !createShadowPipeline() ||
        !createDepthResources() ||
        !createFramebuffers()) {
        ORO_LOG_ERROR("Failed to recreate swapchain-dependent renderer resources");
        return false;
    }

    if (m_materialBuffer.handle != VK_NULL_HANDLE && !updateMaterialDescriptorSets()) {
        ORO_LOG_ERROR("Failed to refresh descriptors after swapchain recreation");
        return false;
    }

    m_imagesInFlight.assign(m_swapchain.imageCount(), VK_NULL_HANDLE);
    ORO_LOG_INFO("Swapchain recreation complete: extent=%ux%u images=%u",
                 m_swapchain.extent().width,
                 m_swapchain.extent().height,
                 m_swapchain.imageCount());
    return true;
}

bool Renderer::waitForFrameFence(VkFence fence) const {
    if (!m_context || fence == VK_NULL_HANDLE) {
        return false;
    }
    const VkResult r = vkWaitForFences(m_context->device(), 1, &fence, VK_TRUE, UINT64_MAX);
    if (r != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed to wait on Vulkan fence: %d", r);
        return false;
    }
    return true;
}

bool Renderer::recordDrawCommands(VkCommandBuffer commandBuffer,
                                  uint32_t imageIndex,
                                  const Camera& camera,
                                  DebugViewMode debugMode,
                                  uint32_t overlayFlags) {
    if (!m_context ||
        m_renderPass == VK_NULL_HANDLE ||
        m_graphicsPipeline == VK_NULL_HANDLE ||
        m_graphicsNoCullPipeline == VK_NULL_HANDLE ||
        m_shadowRenderPass == VK_NULL_HANDLE ||
        m_shadowFramebuffer == VK_NULL_HANDLE ||
        m_shadowPipeline == VK_NULL_HANDLE ||
        imageIndex >= m_framebuffers.size()) {
        return false;
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkResult r = vkBeginCommandBuffer(commandBuffer, &beginInfo);
    if (r != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed to begin draw command buffer: %d", r);
        return false;
    }

    if (m_sceneUniformBuffers[m_currentFrame].memory == VK_NULL_HANDLE) {
        ORO_LOG_ERROR("Scene UBO memory is unavailable for frame %u", m_currentFrame);
        return false;
    }

    const glm::mat4 lightSpaceMatrix = computeDynamicLightSpaceMatrix(kDefaultLightDirection);
    const ShadowPushConstants shadowPushConstants{lightSpaceMatrix};

    VkClearValue shadowClearValue{};
    shadowClearValue.depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo shadowPassInfo{};
    shadowPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    shadowPassInfo.renderPass = m_shadowRenderPass;
    shadowPassInfo.framebuffer = m_shadowFramebuffer;
    shadowPassInfo.renderArea.offset = {0, 0};
    shadowPassInfo.renderArea.extent = {kShadowMapResolution, kShadowMapResolution};
    shadowPassInfo.clearValueCount = 1;
    shadowPassInfo.pClearValues = &shadowClearValue;

    vkCmdBeginRenderPass(commandBuffer, &shadowPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipeline);

    VkViewport shadowViewport{};
    shadowViewport.x = 0.0f;
    shadowViewport.y = 0.0f;
    shadowViewport.width = static_cast<float>(kShadowMapResolution);
    shadowViewport.height = static_cast<float>(kShadowMapResolution);
    shadowViewport.minDepth = 0.0f;
    shadowViewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &shadowViewport);

    VkRect2D shadowScissor{};
    shadowScissor.offset = {0, 0};
    shadowScissor.extent = {kShadowMapResolution, kShadowMapResolution};
    vkCmdSetScissor(commandBuffer, 0, 1, &shadowScissor);
    vkCmdSetDepthBias(commandBuffer, 1.5f, 0.0f, 1.75f);
    vkCmdPushConstants(commandBuffer,
                       m_shadowPipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT,
                       0,
                       sizeof(ShadowPushConstants),
                       &shadowPushConstants);
    static std::atomic<uint32_t> shadowPassLogCount{0};
    const uint32_t loggedShadowPasses = shadowPassLogCount.fetch_add(1U, std::memory_order_relaxed);
    if (loggedShadowPasses < 8U) {
        std::size_t totalIndices = 0;
        std::size_t totalVertices = 0;
        for (const auto& [coord, entry] : m_chunkMeshCache) {
            (void)coord;
            totalIndices += entry.resources.indexCount;
            totalVertices += entry.resources.vertexCount;
        }
        std::ostringstream oss;
        oss << "{\"chunkCount\":" << m_chunkMeshCache.size()
            << ",\"totalVertices\":" << totalVertices
            << ",\"totalIndices\":" << totalIndices
            << ",\"shadowDepthBiasConstant\":1.5"
            << ",\"shadowDepthBiasSlope\":1.75"
            << ",\"shadowCullMode\":" << static_cast<uint32_t>(VK_CULL_MODE_BACK_BIT) << "}";
        // #region agent log
        appendRendererDebugLog("shadow-scales-pass2",
                               "H18_H19",
                               "engine/render/renderer.cpp:recordDrawCommands",
                               "Shadow pass draw-state metrics",
                               oss.str());
        // #endregion
    }
    for (const auto& [coord, entry] : m_chunkMeshCache) {
        (void)coord;
        const MeshGpuResources& mesh = entry.resources;
        if (mesh.indexCount == 0) {
            continue;
        }
        const VkBuffer vertexBuffer = mesh.vertexBuffer.handle;
        const VkDeviceSize vertexOffset = 0;
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer, &vertexOffset);
        vkCmdBindIndexBuffer(commandBuffer, mesh.indexBuffer.handle, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(commandBuffer, static_cast<uint32_t>(mesh.indexCount), 1, 0, 0, 0);
    }
    vkCmdEndRenderPass(commandBuffer);

    VkImageMemoryBarrier shadowReadBarrier{};
    shadowReadBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    shadowReadBarrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    shadowReadBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    shadowReadBarrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    shadowReadBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    shadowReadBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    shadowReadBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    shadowReadBarrier.image = m_shadowDepthImage;
    shadowReadBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    shadowReadBarrier.subresourceRange.baseMipLevel = 0;
    shadowReadBarrier.subresourceRange.levelCount = 1;
    shadowReadBarrier.subresourceRange.baseArrayLayer = 0;
    shadowReadBarrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0,
                         0,
                         nullptr,
                         0,
                         nullptr,
                         1,
                         &shadowReadBarrier);

    SceneUniforms sceneUniforms{};
    sceneUniforms.cameraPosition = camera.position();
    sceneUniforms.lightDirection = kDefaultLightDirection;
    sceneUniforms.lightColor = kDefaultLightColor;
    sceneUniforms.lightIntensity = kDefaultLightIntensity;
    sceneUniforms.ambientColor = kDefaultAmbientColor;
    sceneUniforms.ambientIntensity = kDefaultAmbientIntensity;
    sceneUniforms.lightSpaceMatrix = lightSpaceMatrix;

    void* mappedScene = nullptr;
    r = vkMapMemory(m_context->device(),
                    m_sceneUniformBuffers[m_currentFrame].memory,
                    0,
                    sizeof(SceneUniforms),
                    0,
                    &mappedScene);
    if (r != VK_SUCCESS || mappedScene == nullptr) {
        ORO_LOG_ERROR("Failed to map scene UBO memory for frame %u: %d", m_currentFrame, r);
        return false;
    }
    std::memcpy(mappedScene, &sceneUniforms, sizeof(SceneUniforms));
    vkUnmapMemory(m_context->device(), m_sceneUniformBuffers[m_currentFrame].memory);

    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color = {{0.02f, 0.03f, 0.05f, 1.0f}};
    clearValues[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_renderPass;
    renderPassInfo.framebuffer = m_framebuffers[imageIndex];
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = m_swapchain.extent();
    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    const bool wantsWireframe = debugMode == DebugViewMode::Wireframe || debugMode == DebugViewMode::WireframeOverlay;
    const bool wireframeAvailable = m_wireframePipeline != VK_NULL_HANDLE;
    const bool drawWireframeOnly = debugMode == DebugViewMode::Wireframe && wireframeAvailable;
    const bool drawWireframeOverlay = debugMode == DebugViewMode::WireframeOverlay && wireframeAvailable;
    VkPipeline basePipeline = drawWireframeOnly ? m_wireframePipeline : m_graphicsPipeline;
    VkPipeline activePipeline = basePipeline;
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, activePipeline);
    if (m_materialDescriptorSets[m_currentFrame] != VK_NULL_HANDLE) {
        vkCmdBindDescriptorSets(commandBuffer,
                                VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_pipelineLayout,
                                0,
                                1,
                                &m_materialDescriptorSets[m_currentFrame],
                                0,
                                nullptr);
    }

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(m_swapchain.extent().width);
    viewport.height = static_cast<float>(m_swapchain.extent().height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = m_swapchain.extent();
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    const float width = static_cast<float>(m_swapchain.extent().width);
    const float height = static_cast<float>(m_swapchain.extent().height);
    const float safeHeight = height > 0.0f ? height : 1.0f;
    const float aspect = width / safeHeight;
    const glm::mat4 model = glm::mat4(1.0f);
    const glm::mat4 mvp = camera.projectionMatrix(aspect) * camera.viewMatrix() * model;
    const PushConstants pushConstants{
        mvp,
        (wantsWireframe && !wireframeAvailable) ? static_cast<uint32_t>(DebugViewMode::Lit)
                                                : static_cast<uint32_t>(debugMode)};
    const PushConstants overlayPushConstants{mvp, static_cast<uint32_t>(DebugViewMode::MaterialId)};
    static std::atomic<uint32_t> frameLightingLogCount{0};
    const uint32_t loggedLightingFrames = frameLightingLogCount.fetch_add(1U, std::memory_order_relaxed);
    if (loggedLightingFrames < 10U) {
        std::ostringstream oss;
        oss << "{\"debugMode\":" << pushConstants.debugMode
            << ",\"lightDirection\":[" << sceneUniforms.lightDirection.x << "," << sceneUniforms.lightDirection.y << ","
            << sceneUniforms.lightDirection.z << "]"
            << ",\"lightIntensity\":" << sceneUniforms.lightIntensity
            << ",\"ambientIntensity\":" << sceneUniforms.ambientIntensity << "}";
        // #region agent log
        appendRendererDebugLog("lighting-sign-pass1",
                               "H22_H25",
                               "engine/render/renderer.cpp:recordDrawCommands",
                               "Frame lighting uniforms and debug mode",
                               oss.str());
        // #endregion
    }

    uint64_t fallbackChunkDraws = 0;
    uint64_t fallbackIndexDraws = 0;
    for (const auto& [coord, entry] : m_chunkMeshCache) {
        (void)coord;
        const MeshGpuResources& mesh = entry.resources;
        if (mesh.indexCount > 0) {
            VkPipeline chunkPipeline = basePipeline;
            if (!drawWireframeOnly && entry.forceTwoSidedFallback) {
                chunkPipeline = m_graphicsNoCullPipeline;
                ++fallbackChunkDraws;
                fallbackIndexDraws += static_cast<uint64_t>(mesh.indexCount);
            }
            if (chunkPipeline != activePipeline) {
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, chunkPipeline);
                activePipeline = chunkPipeline;
            }
            const VkBuffer vertexBuffer = mesh.vertexBuffer.handle;
            const VkDeviceSize vertexOffset = 0;
            vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer, &vertexOffset);
            vkCmdBindIndexBuffer(commandBuffer, mesh.indexBuffer.handle, 0, VK_INDEX_TYPE_UINT32);
            vkCmdPushConstants(commandBuffer,
                               m_pipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0,
                               sizeof(PushConstants),
                               &pushConstants);
            const uint32_t indexCount = static_cast<uint32_t>(mesh.indexCount);
            vkCmdDrawIndexed(commandBuffer, indexCount, 1, 0, 0, 0);

            if (drawWireframeOverlay) {
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_wireframePipeline);
                vkCmdPushConstants(commandBuffer,
                                   m_pipelineLayout,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0,
                                   sizeof(PushConstants),
                                   &pushConstants);
                vkCmdDrawIndexed(commandBuffer, indexCount, 1, 0, 0, 0);
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, chunkPipeline);
                activePipeline = chunkPipeline;
            }
        }

        if ((overlayFlags & DebugOverlayChunkBounds) != 0u &&
            m_chunkLinePipeline != VK_NULL_HANDLE &&
            mesh.lineIndexCount > 0) {
            const VkBuffer lineVertexBuffer = mesh.lineVertexBuffer.handle;
            const VkDeviceSize lineVertexOffset = 0;
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_chunkLinePipeline);
            vkCmdBindVertexBuffers(commandBuffer, 0, 1, &lineVertexBuffer, &lineVertexOffset);
            vkCmdBindIndexBuffer(commandBuffer, mesh.lineIndexBuffer.handle, 0, VK_INDEX_TYPE_UINT32);
            vkCmdPushConstants(commandBuffer,
                               m_pipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0,
                               sizeof(PushConstants),
                               &overlayPushConstants);
            vkCmdDrawIndexed(commandBuffer, static_cast<uint32_t>(mesh.lineIndexCount), 1, 0, 0, 0);
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, activePipeline);
        }
    }
    m_lastFrameFallbackChunkDraws = fallbackChunkDraws;
    m_lastFrameFallbackIndexDraws = fallbackIndexDraws;

    vkCmdEndRenderPass(commandBuffer);
    r = vkEndCommandBuffer(commandBuffer);
    if (r != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed to end draw command buffer: %d", r);
        return false;
    }
    return true;
}

uint32_t Renderer::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required) const {
    if (!m_context) {
        return UINT32_MAX;
    }
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(m_context->physicalDevice(), &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        const bool typeSupported = (typeBits & (1u << i)) != 0u;
        const bool propertiesMatch = (memProps.memoryTypes[i].propertyFlags & required) == required;
        if (typeSupported && propertiesMatch) {
            return i;
        }
    }
    return UINT32_MAX;
}

bool Renderer::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, GpuBuffer& outBuffer) {
    if (!m_context) {
        return false;
    }

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult r = vkCreateBuffer(m_context->device(), &bci, nullptr, &outBuffer.handle);
    if (r != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed to create Vulkan buffer: %d", r);
        return false;
    }

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(m_context->device(), outBuffer.handle, &req);
    const uint32_t memoryType = findMemoryType(req.memoryTypeBits, properties);
    if (memoryType == UINT32_MAX) {
        ORO_LOG_ERROR("Failed to find memory type for buffer allocation");
        vkDestroyBuffer(m_context->device(), outBuffer.handle, nullptr);
        outBuffer.handle = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memoryType;
    r = vkAllocateMemory(m_context->device(), &mai, nullptr, &outBuffer.memory);
    if (r != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed to allocate Vulkan buffer memory: %d", r);
        vkDestroyBuffer(m_context->device(), outBuffer.handle, nullptr);
        outBuffer.handle = VK_NULL_HANDLE;
        return false;
    }

    r = vkBindBufferMemory(m_context->device(), outBuffer.handle, outBuffer.memory, 0);
    if (r != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed to bind Vulkan buffer memory: %d", r);
        vkFreeMemory(m_context->device(), outBuffer.memory, nullptr);
        outBuffer.memory = VK_NULL_HANDLE;
        vkDestroyBuffer(m_context->device(), outBuffer.handle, nullptr);
        outBuffer.handle = VK_NULL_HANDLE;
        return false;
    }

    outBuffer.size = req.size;
    return true;
}

void Renderer::destroyBuffer(GpuBuffer& buffer) {
    if (!m_context) {
        return;
    }
    if (buffer.handle != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_context->device(), buffer.handle, nullptr);
        buffer.handle = VK_NULL_HANDLE;
    }
    if (buffer.memory != VK_NULL_HANDLE) {
        vkFreeMemory(m_context->device(), buffer.memory, nullptr);
        buffer.memory = VK_NULL_HANDLE;
    }
    buffer.size = 0;
}

bool Renderer::copyBuffer(const GpuBuffer& src, const GpuBuffer& dst, VkDeviceSize size) {
    if (!m_context || m_commandPool == VK_NULL_HANDLE || m_uploadFence == VK_NULL_HANDLE) {
        return false;
    }

    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = m_commandPool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkResult r = vkAllocateCommandBuffers(m_context->device(), &alloc, &commandBuffer);
    if (r != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed to allocate upload command buffer: %d", r);
        return false;
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    r = vkBeginCommandBuffer(commandBuffer, &beginInfo);
    if (r != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed to begin upload command buffer: %d", r);
        vkFreeCommandBuffers(m_context->device(), m_commandPool, 1, &commandBuffer);
        return false;
    }

    VkBufferCopy copyRegion{};
    copyRegion.srcOffset = 0;
    copyRegion.dstOffset = 0;
    copyRegion.size = size;
    vkCmdCopyBuffer(commandBuffer, src.handle, dst.handle, 1, &copyRegion);

    r = vkEndCommandBuffer(commandBuffer);
    if (r != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed to end upload command buffer: %d", r);
        vkFreeCommandBuffers(m_context->device(), m_commandPool, 1, &commandBuffer);
        return false;
    }

    vkResetFences(m_context->device(), 1, &m_uploadFence);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commandBuffer;

    r = vkQueueSubmit(m_context->graphicsQueue(), 1, &submit, m_uploadFence);
    if (r != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed to submit upload command buffer: %d", r);
        vkFreeCommandBuffers(m_context->device(), m_commandPool, 1, &commandBuffer);
        return false;
    }

    r = vkWaitForFences(m_context->device(), 1, &m_uploadFence, VK_TRUE, UINT64_MAX);
    vkFreeCommandBuffers(m_context->device(), m_commandPool, 1, &commandBuffer);
    if (r != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed while waiting for upload fence: %d", r);
        return false;
    }
    return true;
}

bool Renderer::uploadMaterialTable(const std::vector<oro::material::GpuMaterialEntry>& materialTable) {
    if (!m_context || m_materialDescriptorPool == VK_NULL_HANDLE) {
        return false;
    }

    std::vector<oro::material::GpuMaterialEntry> uploadData = materialTable;
    if (uploadData.empty()) {
        uploadData.push_back(oro::material::GpuMaterialEntry{});
    }

    const VkDeviceSize uploadSize = static_cast<VkDeviceSize>(uploadData.size() * sizeof(uploadData[0]));
    GpuBuffer staging{};
    if (!createBuffer(uploadSize,
                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      staging)) {
        ORO_LOG_ERROR("Failed to allocate staging buffer for material table");
        return false;
    }

    void* mapped = nullptr;
    VkResult mapResult = vkMapMemory(m_context->device(), staging.memory, 0, uploadSize, 0, &mapped);
    if (mapResult != VK_SUCCESS || mapped == nullptr) {
        ORO_LOG_ERROR("Failed to map material staging buffer memory: %d", mapResult);
        destroyBuffer(staging);
        return false;
    }
    std::memcpy(mapped, uploadData.data(), static_cast<std::size_t>(uploadSize));
    vkUnmapMemory(m_context->device(), staging.memory);

    GpuBuffer gpuBuffer{};
    if (!createBuffer(uploadSize,
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                      gpuBuffer)) {
        ORO_LOG_ERROR("Failed to allocate device-local material buffer");
        destroyBuffer(staging);
        return false;
    }
    if (!copyBuffer(staging, gpuBuffer, uploadSize)) {
        ORO_LOG_ERROR("Failed to upload material table to device-local buffer");
        destroyBuffer(staging);
        destroyBuffer(gpuBuffer);
        return false;
    }
    destroyBuffer(staging);

    destroyBuffer(m_materialBuffer);
    m_materialBuffer = gpuBuffer;
    if (!updateMaterialDescriptorSets()) {
        ORO_LOG_ERROR("Failed to update material descriptor sets");
        return false;
    }

    ORO_LOG_INFO("Uploaded material table entries=%zu bytes=%llu",
                 uploadData.size(),
                 static_cast<unsigned long long>(uploadSize));
    return true;
}

bool Renderer::uploadMeshBuffers(const oro::voxel::MeshBuffers& mesh, MeshGpuResources& uploaded) {
    if (!m_context || mesh.vertices.empty() || mesh.indices.empty()) {
        return false;
    }
    const VkDeviceSize vertexBytes = static_cast<VkDeviceSize>(mesh.vertices.size() * sizeof(oro::voxel::MeshVertex));
    const VkDeviceSize indexBytes = static_cast<VkDeviceSize>(mesh.indices.size() * sizeof(uint32_t));
    uploaded.sourceVersion = mesh.sourceTopologyVersion;
    uploaded.vertexCount = mesh.vertices.size();
    uploaded.indexCount = mesh.indices.size();
    GpuBuffer vertexStaging{};
    GpuBuffer indexStaging{};
    if (!createBuffer(vertexBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, vertexStaging) ||
        !createBuffer(indexBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, indexStaging)) {
        destroyBuffer(vertexStaging);
        destroyBuffer(indexStaging);
        return false;
    }

    void* mapped = nullptr;
    VkResult r = vkMapMemory(m_context->device(), vertexStaging.memory, 0, vertexBytes, 0, &mapped);
    if (r != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed to map vertex staging memory: %d", r);
        destroyBuffer(vertexStaging);
        destroyBuffer(indexStaging);
        return false;
    }
    std::memcpy(mapped, mesh.vertices.data(), static_cast<std::size_t>(vertexBytes));
    vkUnmapMemory(m_context->device(), vertexStaging.memory);

    mapped = nullptr;
    r = vkMapMemory(m_context->device(), indexStaging.memory, 0, indexBytes, 0, &mapped);
    if (r != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed to map index staging memory: %d", r);
        destroyBuffer(vertexStaging);
        destroyBuffer(indexStaging);
        return false;
    }
    std::memcpy(mapped, mesh.indices.data(), static_cast<std::size_t>(indexBytes));
    vkUnmapMemory(m_context->device(), indexStaging.memory);

    if (!createBuffer(vertexBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, uploaded.vertexBuffer) ||
        !createBuffer(indexBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, uploaded.indexBuffer)) {
        destroyBuffer(vertexStaging);
        destroyBuffer(indexStaging);
        destroyBuffer(uploaded.vertexBuffer);
        destroyBuffer(uploaded.indexBuffer);
        return false;
    }

    if (!copyBuffer(vertexStaging, uploaded.vertexBuffer, vertexBytes) ||
        !copyBuffer(indexStaging, uploaded.indexBuffer, indexBytes)) {
        ORO_LOG_ERROR("Failed to copy mesh payload to device-local buffers for version=%llu",
                      static_cast<unsigned long long>(mesh.sourceTopologyVersion));
        destroyBuffer(vertexStaging);
        destroyBuffer(indexStaging);
        destroyBuffer(uploaded.vertexBuffer);
        destroyBuffer(uploaded.indexBuffer);
        return false;
    }

    destroyBuffer(vertexStaging);
    destroyBuffer(indexStaging);
    return true;
}

bool Renderer::uploadChunkLineOverlay(const oro::voxel::ChunkCoord& coord, float voxelSize, MeshGpuResources& uploaded) {
    const float chunkWorldEdge = static_cast<float>(oro::voxel::kChunkEdge) * voxelSize;
    if (chunkWorldEdge <= 0.0f) {
        return true;
    }
    std::vector<oro::voxel::MeshVertex> lineVertices;
    std::vector<uint32_t> lineIndices;
    lineVertices.reserve(8u);
    lineIndices.reserve(24u);

    const float x0 = static_cast<float>(coord.x) * chunkWorldEdge;
    const float y0 = static_cast<float>(coord.y) * chunkWorldEdge;
    const float z0 = static_cast<float>(coord.z) * chunkWorldEdge;
    const float x1 = x0 + chunkWorldEdge;
    const float y1 = y0 + chunkWorldEdge;
    const float z1 = z0 + chunkWorldEdge;

    const std::array<std::array<float, 3>, 8> corners = {{
        {x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0},
        {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1},
    }};
    const std::array<std::array<uint32_t, 2>, 12> edges = {{
        {0u, 1u}, {1u, 2u}, {2u, 3u}, {3u, 0u},
        {4u, 5u}, {5u, 6u}, {6u, 7u}, {7u, 4u},
        {0u, 4u}, {1u, 5u}, {2u, 6u}, {3u, 7u},
    }};

    const uint32_t baseVertex = static_cast<uint32_t>(lineVertices.size());
    for (const auto& corner : corners) {
        oro::voxel::MeshVertex vertex{};
        vertex.position = corner;
        vertex.normal = {0.0f, 1.0f, 0.0f};
        vertex.materialId = 3u;
        lineVertices.push_back(vertex);
    }
    for (const auto& edge : edges) {
        lineIndices.push_back(baseVertex + edge[0]);
        lineIndices.push_back(baseVertex + edge[1]);
    }

    if (lineVertices.empty() || lineIndices.empty()) {
        return true;
    }

    const VkDeviceSize vertexBytes = static_cast<VkDeviceSize>(lineVertices.size() * sizeof(oro::voxel::MeshVertex));
    const VkDeviceSize indexBytes = static_cast<VkDeviceSize>(lineIndices.size() * sizeof(uint32_t));
    GpuBuffer vertexStaging{};
    GpuBuffer indexStaging{};
    if (!createBuffer(vertexBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, vertexStaging) ||
        !createBuffer(indexBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, indexStaging)) {
        destroyBuffer(vertexStaging);
        destroyBuffer(indexStaging);
        return false;
    }

    void* mapped = nullptr;
    VkResult r = vkMapMemory(m_context->device(), vertexStaging.memory, 0, vertexBytes, 0, &mapped);
    if (r != VK_SUCCESS) {
        destroyBuffer(vertexStaging);
        destroyBuffer(indexStaging);
        return false;
    }
    std::memcpy(mapped, lineVertices.data(), static_cast<std::size_t>(vertexBytes));
    vkUnmapMemory(m_context->device(), vertexStaging.memory);

    mapped = nullptr;
    r = vkMapMemory(m_context->device(), indexStaging.memory, 0, indexBytes, 0, &mapped);
    if (r != VK_SUCCESS) {
        destroyBuffer(vertexStaging);
        destroyBuffer(indexStaging);
        return false;
    }
    std::memcpy(mapped, lineIndices.data(), static_cast<std::size_t>(indexBytes));
    vkUnmapMemory(m_context->device(), indexStaging.memory);

    if (!createBuffer(vertexBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, uploaded.lineVertexBuffer) ||
        !createBuffer(indexBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, uploaded.lineIndexBuffer)) {
        destroyBuffer(vertexStaging);
        destroyBuffer(indexStaging);
        destroyBuffer(uploaded.lineVertexBuffer);
        destroyBuffer(uploaded.lineIndexBuffer);
        return false;
    }

    if (!copyBuffer(vertexStaging, uploaded.lineVertexBuffer, vertexBytes) ||
        !copyBuffer(indexStaging, uploaded.lineIndexBuffer, indexBytes)) {
        destroyBuffer(vertexStaging);
        destroyBuffer(indexStaging);
        destroyBuffer(uploaded.lineVertexBuffer);
        destroyBuffer(uploaded.lineIndexBuffer);
        return false;
    }

    destroyBuffer(vertexStaging);
    destroyBuffer(indexStaging);

    uploaded.lineVertexCount = lineVertices.size();
    uploaded.lineIndexCount = lineIndices.size();
    return true;
}

void Renderer::destroyMeshResources(MeshGpuResources& resources) {
    destroyBuffer(resources.vertexBuffer);
    destroyBuffer(resources.indexBuffer);
    destroyBuffer(resources.lineVertexBuffer);
    destroyBuffer(resources.lineIndexBuffer);
}

bool Renderer::applyPendingPatchBatch() {
    if (!m_context || m_pendingMeshSnapshots.empty()) {
        return false;
    }
    const PendingSnapshot& pending = m_pendingMeshSnapshots.front();
    struct StagedUpdate {
        oro::voxel::ChunkCoord coord{};
        std::optional<ChunkCacheEntry> entry;
    };
    std::vector<StagedUpdate> staged;
    staged.reserve(pending.patchBatch.patches.size());
    const bool allowDestructiveRemovals = pending.patchBatch.diagnostics.droppedIncompleteQuads == 0U;

    for (const oro::voxel::ChunkMeshPatch& patch : pending.patchBatch.patches) {
        auto existing = std::find_if(staged.begin(), staged.end(), [&patch](const StagedUpdate& update) {
            return update.coord == patch.coord;
        });
        if (patch.remove || patch.mesh.vertices.empty() || patch.mesh.indices.empty()) {
            if (!allowDestructiveRemovals) {
                continue;
            }
            if (existing != staged.end()) {
                if (existing->entry.has_value()) {
                    destroyMeshResources(existing->entry->resources);
                }
                existing->entry.reset();
            } else {
                staged.push_back({patch.coord, std::nullopt});
            }
            continue;
        }

        ChunkCacheEntry newEntry{};
        newEntry.voxelSize = pending.voxelSize;
        newEntry.forceTwoSidedFallback = patch.forceTwoSidedFallback;
        if (!uploadMeshBuffers(patch.mesh, newEntry.resources) ||
            !uploadChunkLineOverlay(patch.coord, pending.voxelSize, newEntry.resources)) {
            destroyMeshResources(newEntry.resources);
            for (StagedUpdate& update : staged) {
                if (update.entry.has_value()) {
                    destroyMeshResources(update.entry->resources);
                }
            }
            return false;
        }
        if (existing != staged.end()) {
            if (existing->entry.has_value()) {
                destroyMeshResources(existing->entry->resources);
            }
            existing->entry = std::move(newEntry);
        } else {
            staged.push_back({patch.coord, std::move(newEntry)});
        }
    }

    for (StagedUpdate& update : staged) {
        const auto cacheIt = m_chunkMeshCache.find(update.coord);
        if (cacheIt != m_chunkMeshCache.end()) {
            destroyMeshResources(cacheIt->second.resources);
            m_chunkMeshCache.erase(cacheIt);
        }
        if (update.entry.has_value()) {
            m_chunkMeshCache.emplace(update.coord, std::move(*update.entry));
        }
    }
    m_visibleMeshVersion = pending.patchBatch.sourceTopologyVersion;
    m_activatedMeshVersions.push_back(m_visibleMeshVersion);
    uint32_t flaggedChunks = 0;
    for (const auto& [coord, entry] : m_chunkMeshCache) {
        (void)coord;
        if (entry.forceTwoSidedFallback) {
            ++flaggedChunks;
        }
    }
    ORO_LOG_INFO("Renderer applied chunk patch batch version=%llu patches=%zu cachedChunks=%zu fallbackChunks=%u",
                 static_cast<unsigned long long>(m_visibleMeshVersion),
                 pending.patchBatch.patches.size(),
                 m_chunkMeshCache.size(),
                 flaggedChunks);
    m_pendingMeshSnapshots.pop_front();
    return true;
}

std::optional<uint64_t> Renderer::popActivatedMeshVersion() {
    if (m_activatedMeshVersions.empty()) {
        return std::nullopt;
    }
    const uint64_t version = m_activatedMeshVersions.front();
    m_activatedMeshVersions.pop_front();
    return version;
}

void Renderer::drawFrame(const Camera& camera, DebugViewMode debugMode, uint32_t overlayFlags) {
    if (!m_context || m_swapchain.m_swapchain == VK_NULL_HANDLE) {
        return;
    }
    if (!m_pendingMeshSnapshots.empty()) {
        const uint64_t pendingVersion = m_pendingMeshSnapshots.front().patchBatch.sourceTopologyVersion;
        if (!applyPendingPatchBatch()) {
            ORO_LOG_WARN("Retaining chunk cache after failed patch batch apply for version=%llu",
                         static_cast<unsigned long long>(pendingVersion));
            if (!m_pendingMeshSnapshots.empty() &&
                m_pendingMeshSnapshots.front().patchBatch.sourceTopologyVersion == pendingVersion) {
                m_pendingMeshSnapshots.pop_front();
            }
        }
    }

    FrameResources& frame = m_frames[m_currentFrame];
    if (!waitForFrameFence(frame.inFlightFence)) {
        return;
    }

    uint32_t imageIndex = 0;
    VkResult acquireResult = vkAcquireNextImageKHR(
        m_context->device(),
        m_swapchain.m_swapchain,
        UINT64_MAX,
        frame.imageAvailableSemaphore,
        VK_NULL_HANDLE,
        &imageIndex);
    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
        (void)recreateSwapchain();
        return;
    }
    if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) {
        ORO_LOG_ERROR("Failed to acquire swapchain image: %d", acquireResult);
        return;
    }

    if (imageIndex >= m_imagesInFlight.size()) {
        ORO_LOG_ERROR("Swapchain image index out of range: %u (max=%zu)", imageIndex, m_imagesInFlight.size());
        return;
    }
    if (imageIndex >= m_renderFinishedSemaphores.size()) {
        ORO_LOG_ERROR("Render-finished semaphore index out of range: %u (max=%zu)",
                      imageIndex,
                      m_renderFinishedSemaphores.size());
        return;
    }
    if (m_imagesInFlight[imageIndex] != VK_NULL_HANDLE) {
        if (!waitForFrameFence(m_imagesInFlight[imageIndex])) {
            return;
        }
    }
    m_imagesInFlight[imageIndex] = frame.inFlightFence;
    VkSemaphore renderFinishedSemaphore = m_renderFinishedSemaphores[imageIndex];

    vkResetFences(m_context->device(), 1, &frame.inFlightFence);
    vkResetCommandBuffer(frame.commandBuffer, 0);
    if (!recordDrawCommands(frame.commandBuffer, imageIndex, camera, debugMode, overlayFlags)) {
        ORO_LOG_ERROR("Failed to record draw commands for frame %u", m_currentFrame);
        return;
    }

    const std::array<VkSemaphore, 1> waitSemaphores = {frame.imageAvailableSemaphore};
    const std::array<VkPipelineStageFlags, 1> waitStages = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    const std::array<VkSemaphore, 1> signalSemaphores = {renderFinishedSemaphore};
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size());
    submitInfo.pWaitSemaphores = waitSemaphores.data();
    submitInfo.pWaitDstStageMask = waitStages.data();
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &frame.commandBuffer;
    submitInfo.signalSemaphoreCount = static_cast<uint32_t>(signalSemaphores.size());
    submitInfo.pSignalSemaphores = signalSemaphores.data();

    VkResult r = vkQueueSubmit(m_context->graphicsQueue(), 1, &submitInfo, frame.inFlightFence);
    if (r != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed to submit draw command buffer: %d", r);
        return;
    }

    VkSwapchainKHR swapchainHandle = m_swapchain.m_swapchain;
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = static_cast<uint32_t>(signalSemaphores.size());
    presentInfo.pWaitSemaphores = signalSemaphores.data();
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchainHandle;
    presentInfo.pImageIndices = &imageIndex;

    r = vkQueuePresentKHR(m_context->graphicsQueue(), &presentInfo);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        (void)recreateSwapchain();
        return;
    }
    if (r != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed to present swapchain image: %d", r);
        return;
    }

    m_currentFrame = (m_currentFrame + 1) % kMaxFramesInFlight;
}

bool Renderer::ingestVoxelMeshSnapshot(oro::voxel::VoxelRuntime::PublishedMeshSnapshot snapshot) {
    const uint64_t sourceVersion = snapshot.patchBatch.sourceTopologyVersion;
    if (sourceVersion <= m_ingestedMeshVersion) {
        ORO_LOG_WARN("Rejected voxel mesh ingestion for non-incrementing version=%llu (current=%llu)",
                     static_cast<unsigned long long>(sourceVersion),
                     static_cast<unsigned long long>(m_ingestedMeshVersion));
        return false;
    }
    m_ingestedMeshVersion = sourceVersion;
    PendingSnapshot pending{};
    pending.patchBatch = std::move(snapshot.patchBatch);
    pending.voxelSize = snapshot.voxelSize;
    std::size_t vertexCount = 0;
    std::size_t indexCount = 0;
    for (const oro::voxel::ChunkMeshPatch& patch : pending.patchBatch.patches) {
        vertexCount += patch.mesh.vertices.size();
        indexCount += patch.mesh.indices.size();
    }
    const std::size_t patchCount = pending.patchBatch.patches.size();
    m_pendingMeshSnapshots.push_back(std::move(pending));
    ORO_LOG_DEBUG("Renderer ingested voxel patch batch version=%llu patches=%zu vertices=%zu indices=%zu",
                  static_cast<unsigned long long>(sourceVersion), patchCount, vertexCount, indexCount);
    return true;
}

void Renderer::shutdown() {
    if (m_context) {
        vkDeviceWaitIdle(m_context->device());
    }

    for (auto& [coord, entry] : m_chunkMeshCache) {
        (void)coord;
        destroyMeshResources(entry.resources);
    }
    m_chunkMeshCache.clear();
    m_pendingMeshSnapshots.clear();
    m_activatedMeshVersions.clear();
    m_imagesInFlight.clear();
    destroyBuffer(m_materialBuffer);

    destroyFramebuffers();
    destroyDepthResources();
    destroyGraphicsPipeline();
    destroyMaterialDescriptorResources();
    destroyRenderPass();
    destroySwapchainSemaphores();
    destroyFrameResources();
    destroyCommandPool();

    if (m_context && m_uploadFence != VK_NULL_HANDLE) {
        vkDestroyFence(m_context->device(), m_uploadFence, nullptr);
        m_uploadFence = VK_NULL_HANDLE;
    }

    m_swapchain.shutdown();
    m_depthFormat = VK_FORMAT_UNDEFINED;
    m_shadowDepthFormat = VK_FORMAT_UNDEFINED;
    m_currentFrame = 0;
    m_context = nullptr;
}
