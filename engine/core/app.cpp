#include "core/app.h"
#include "core/log.h"
#include <SDL3/SDL.h>

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

    ORO_LOG_INFO("Window created: %dx%d", width, height);
    return true;
}

void App::run() {
    m_running = true;
    while (m_running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) 
                m_running = false;
        }
    }
    
}

void App::shutdown() {
    m_context.shutdown();

    if (m_window) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }
    
    SDL_Quit();
    ORO_LOG_INFO("App shutdown complete");
}
