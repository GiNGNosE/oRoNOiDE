#pragma once

class VulkanContext;

class Renderer {
public:
    bool init(VulkanContext& context);
    void shutdown();
    void drawFrame();

private:
    VulkanContext* m_context = nullptr;
};