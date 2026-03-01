#include "render/renderer.h"

#include "core/log.h"
#include "rhi/vulkan/context.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <type_traits>
#include <vector>

namespace {

struct PushConstants {
    glm::mat4 mvp{1.0f};
};

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
        !createGraphicsPipeline() ||
        !createDepthResources() ||
        !createFramebuffers()) {
        ORO_LOG_ERROR("Renderer initialization failed while creating frame resources");
        shutdown();
        return false;
    }

    m_imagesInFlight.assign(m_swapchain.imageCount(), VK_NULL_HANDLE);
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

bool Renderer::createGraphicsPipeline() {
    if (!m_context || m_renderPass == VK_NULL_HANDLE) {
        return false;
    }

    static_assert(std::is_standard_layout_v<oro::voxel::MeshVertex>, "MeshVertex must be standard layout");
    static_assert(sizeof(oro::voxel::MeshVertex::materialId) == sizeof(uint16_t), "materialId must be 16-bit");

    const std::vector<char> vertCode = loadShaderBinary({
        "shaders/voxel_mesh.vert.spv",
        "build/shaders/voxel_mesh.vert.spv",
        "engine/render/shaders/voxel_mesh.vert.spv"});
    const std::vector<char> fragCode = loadShaderBinary({
        "shaders/voxel_mesh.frag.spv",
        "build/shaders/voxel_mesh.frag.spv",
        "engine/render/shaders/voxel_mesh.frag.spv"});
    if (vertCode.empty() || fragCode.empty()) {
        ORO_LOG_ERROR("Failed to load shader binaries for graphics pipeline");
        return false;
    }

    VkShaderModule vertShaderModule = VK_NULL_HANDLE;
    VkShaderModule fragShaderModule = VK_NULL_HANDLE;
    if (!createShaderModule(m_context->device(), vertCode, vertShaderModule) ||
        !createShaderModule(m_context->device(), fragCode, fragShaderModule)) {
        ORO_LOG_ERROR("Failed to create shader modules");
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
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
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

    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

    VkResult r = vkCreatePipelineLayout(m_context->device(), &pipelineLayoutInfo, nullptr, &m_pipelineLayout);
    if (r != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed to create Vulkan pipeline layout: %d", r);
        vkDestroyShaderModule(m_context->device(), vertShaderModule, nullptr);
        vkDestroyShaderModule(m_context->device(), fragShaderModule, nullptr);
        return false;
    }

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
    pipelineInfo.layout = m_pipelineLayout;
    pipelineInfo.renderPass = m_renderPass;
    pipelineInfo.subpass = 0;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;

    r = vkCreateGraphicsPipelines(m_context->device(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_graphicsPipeline);
    vkDestroyShaderModule(m_context->device(), fragShaderModule, nullptr);
    vkDestroyShaderModule(m_context->device(), vertShaderModule, nullptr);
    if (r != VK_SUCCESS) {
        ORO_LOG_ERROR("Failed to create Vulkan graphics pipeline: %d", r);
        destroyGraphicsPipeline();
        return false;
    }

    return true;
}

void Renderer::destroyGraphicsPipeline() {
    if (!m_context) {
        return;
    }
    if (m_graphicsPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_context->device(), m_graphicsPipeline, nullptr);
        m_graphicsPipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_context->device(), m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
}

bool Renderer::recreateSwapchain() {
    if (!m_context) {
        return false;
    }
    vkDeviceWaitIdle(m_context->device());

    destroyFramebuffers();
    destroyDepthResources();
    destroyGraphicsPipeline();
    destroyRenderPass();
    destroySwapchainSemaphores();
    m_swapchain.shutdown();

    if (!m_swapchain.init(*m_context) ||
        !createSwapchainSemaphores() ||
        !createRenderPass() ||
        !createGraphicsPipeline() ||
        !createDepthResources() ||
        !createFramebuffers()) {
        ORO_LOG_ERROR("Failed to recreate swapchain-dependent renderer resources");
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

bool Renderer::recordDrawCommands(VkCommandBuffer commandBuffer, uint32_t imageIndex) {
    if (!m_context ||
        m_renderPass == VK_NULL_HANDLE ||
        m_graphicsPipeline == VK_NULL_HANDLE ||
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
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_graphicsPipeline);

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

    if (m_activeMesh.has_value() && m_activeMesh->indexCount > 0) {
        const VkBuffer vertexBuffer = m_activeMesh->vertexBuffer.handle;
        const VkDeviceSize vertexOffset = 0;
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer, &vertexOffset);
        vkCmdBindIndexBuffer(commandBuffer, m_activeMesh->indexBuffer.handle, 0, VK_INDEX_TYPE_UINT32);

        const float width = static_cast<float>(m_swapchain.extent().width);
        const float height = static_cast<float>(m_swapchain.extent().height);
        const float safeHeight = height > 0.0f ? height : 1.0f;
        const float aspect = width / safeHeight;
        const glm::mat4 model = glm::mat4(1.0f);
        const glm::mat4 view = glm::lookAt(glm::vec3(4.0f, 4.0f, 6.0f),
                                           glm::vec3(1.6f, 1.6f, 1.6f),
                                           glm::vec3(0.0f, 1.0f, 0.0f));
        glm::mat4 projection = glm::perspective(glm::radians(50.0f), aspect, 0.1f, 512.0f);
        projection[1][1] *= -1.0f;
        const PushConstants pushConstants{projection * view * model};
        vkCmdPushConstants(commandBuffer,
                           m_pipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT,
                           0,
                           sizeof(PushConstants),
                           &pushConstants);

        const uint32_t indexCount = static_cast<uint32_t>(m_activeMesh->indexCount);
        vkCmdDrawIndexed(commandBuffer, indexCount, 1, 0, 0, 0);
    }

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

bool Renderer::uploadPendingMesh() {
    if (!m_context || m_pendingMeshSnapshots.empty()) {
        return false;
    }

    const oro::voxel::MeshBuffers& mesh = m_pendingMeshSnapshots.front();
    if (mesh.vertices.empty() || mesh.indices.empty()) {
        ORO_LOG_WARN("Rejected upload of empty mesh payload version=%llu",
                     static_cast<unsigned long long>(mesh.sourceTopologyVersion));
        m_pendingMeshSnapshots.pop_front();
        return false;
    }

    const VkDeviceSize vertexBytes = static_cast<VkDeviceSize>(mesh.vertices.size() * sizeof(oro::voxel::MeshVertex));
    const VkDeviceSize indexBytes = static_cast<VkDeviceSize>(mesh.indices.size() * sizeof(uint32_t));

    MeshGpuResources uploaded{};
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
    m_uploadedMesh = std::move(uploaded);
    m_pendingMeshSnapshots.pop_front();
    return true;
}

void Renderer::swapInUploadedMesh() {
    if (!m_uploadedMesh.has_value()) {
        return;
    }
    if (m_activeMesh.has_value()) {
        destroyBuffer(m_activeMesh->vertexBuffer);
        destroyBuffer(m_activeMesh->indexBuffer);
    }
    m_activeMesh = std::move(m_uploadedMesh);
    m_uploadedMesh.reset();
    m_visibleMeshVersion = m_activeMesh->sourceVersion;
    m_activatedMeshVersions.push_back(m_visibleMeshVersion);
    ORO_LOG_INFO("Renderer activated voxel mesh version=%llu vertices=%zu indices=%zu",
                 static_cast<unsigned long long>(m_activeMesh->sourceVersion),
                 m_activeMesh->vertexCount,
                 m_activeMesh->indexCount);
}

std::optional<uint64_t> Renderer::popActivatedMeshVersion() {
    if (m_activatedMeshVersions.empty()) {
        return std::nullopt;
    }
    const uint64_t version = m_activatedMeshVersions.front();
    m_activatedMeshVersions.pop_front();
    return version;
}

void Renderer::drawFrame() {
    if (!m_context || m_swapchain.m_swapchain == VK_NULL_HANDLE) {
        return;
    }
    if (!m_pendingMeshSnapshots.empty()) {
        const uint64_t pendingVersion = m_pendingMeshSnapshots.front().sourceTopologyVersion;
        if (!uploadPendingMesh()) {
            ORO_LOG_WARN("Retaining active mesh after failed upload for version=%llu",
                         static_cast<unsigned long long>(pendingVersion));
            if (!m_pendingMeshSnapshots.empty() &&
                m_pendingMeshSnapshots.front().sourceTopologyVersion == pendingVersion) {
                m_pendingMeshSnapshots.pop_front();
            }
        } else {
            swapInUploadedMesh();
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
    if (!recordDrawCommands(frame.commandBuffer, imageIndex)) {
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

bool Renderer::ingestVoxelMeshSnapshot(oro::voxel::MeshBuffers snapshot) {
    const uint64_t sourceVersion = snapshot.sourceTopologyVersion;
    if (sourceVersion <= m_ingestedMeshVersion) {
        ORO_LOG_WARN("Rejected voxel mesh ingestion for non-incrementing version=%llu (current=%llu)",
                     static_cast<unsigned long long>(sourceVersion),
                     static_cast<unsigned long long>(m_ingestedMeshVersion));
        return false;
    }
    m_ingestedMeshVersion = sourceVersion;
    const std::size_t vertexCount = snapshot.vertices.size();
    const std::size_t indexCount = snapshot.indices.size();
    m_pendingMeshSnapshots.push_back(std::move(snapshot));
    ORO_LOG_DEBUG("Renderer ingested voxel mesh payload version=%llu vertices=%zu indices=%zu",
                  static_cast<unsigned long long>(sourceVersion), vertexCount, indexCount);
    return true;
}

void Renderer::shutdown() {
    if (m_context) {
        vkDeviceWaitIdle(m_context->device());
    }

    if (m_activeMesh.has_value()) {
        destroyBuffer(m_activeMesh->vertexBuffer);
        destroyBuffer(m_activeMesh->indexBuffer);
        m_activeMesh.reset();
    }
    if (m_uploadedMesh.has_value()) {
        destroyBuffer(m_uploadedMesh->vertexBuffer);
        destroyBuffer(m_uploadedMesh->indexBuffer);
        m_uploadedMesh.reset();
    }
    m_pendingMeshSnapshots.clear();
    m_activatedMeshVersions.clear();
    m_imagesInFlight.clear();

    destroyFramebuffers();
    destroyDepthResources();
    destroyGraphicsPipeline();
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
    m_currentFrame = 0;
    m_context = nullptr;
}
