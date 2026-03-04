#pragma once
#include "core/camera.h"
#include "material/registry.h"
#include "rhi/vulkan/context.h"
#include "render/debug_view.h"
#include "render/renderer.h"
#include "voxel/runtime.h"

struct SDL_Window;

class App {
public:
    bool init(int width, int height, const char* title);
    void run();
    void shutdown();

    SDL_Window* window() const { return m_window; }

private:
    static DebugViewMode nextDebugViewMode(DebugViewMode current);
    void setMouseCapture(bool captured);

    SDL_Window* m_window = nullptr;
    bool m_running = false;
    bool m_mouseCaptured = false;
    Camera m_camera;
    Camera::MovementInput m_cameraInput{};
    DebugViewMode m_debugViewMode = DebugViewMode::Lit;
    uint32_t m_debugOverlayFlags = DebugOverlayNone;
    VulkanContext m_context;
    Renderer m_renderer;
    oro::material::MaterialRegistry m_materialRegistry;
    oro::voxel::VoxelRuntime m_voxelRuntime;
};