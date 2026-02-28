#pragma once

struct SDL_Window;

class App {
public:
    bool init(int width, int height, const char* title);
    void run();
    void shutdown();

    SDL_Window* window() const { return m_window; }

private:
    SDL_Window* m_window = nullptr;
    bool m_running = false;
    VulkanContext m_context;
};