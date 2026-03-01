#include "core/app.h"
#include "core/log.h"
#include <SDL3/SDL.h>
#include <chrono>
#include <utility>

bool App::init(int width, int height, const char* title) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        ORO_LOG_ERROR("SDL_Init failed: %s", SDL_GetError());
        return false;
    }

    m_window = SDL_CreateWindow(title, width, height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);

    if (!m_window) {
        ORO_LOG_ERROR("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return false;
    }

    if (!m_context.init(m_window)) {
        ORO_LOG_ERROR("Failed to initiate Vulkan context");
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
        SDL_Quit();
        return false;
    }

    if (!m_renderer.init(m_context)) {
        ORO_LOG_ERROR("Failed to initiate renderer");
        m_context.shutdown();
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
        SDL_Quit();
        return false;
    }

    m_voxelRuntime.initialize();
    if (!m_voxelRuntime.runDeterministicGateSmoke()) {
        ORO_LOG_WARN("Voxel runtime smoke gates reported failures");
    }

    ORO_LOG_INFO("Window created: %dx%d", width, height);
    return true;
}

void App::run() {
    m_running = true;
    auto lastTick = std::chrono::steady_clock::now();
    while (m_running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) m_running = false;
        }
        const auto now = std::chrono::steady_clock::now();
        const auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTick);
        lastTick = now;
        m_voxelRuntime.tick(static_cast<uint64_t>(delta.count()));
        while (auto meshSnapshot = m_voxelRuntime.popPublishedMeshSnapshot()) {
            (void)m_renderer.ingestVoxelMeshSnapshot(std::move(meshSnapshot->mesh));
        }
        m_renderer.drawFrame();
        while (auto visibleVersion = m_renderer.popActivatedMeshVersion()) {
            (void)m_voxelRuntime.onRendererMeshVisible(*visibleVersion);
        }
    }
    
}

void App::shutdown() {
    m_renderer.shutdown();
    m_context.shutdown();

    if (m_window) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }

    SDL_Quit();
    ORO_LOG_INFO("App shutdown complete");
}
