#include "core/app.h"
#include "core/log.h"
#include <SDL3/SDL.h>
#include <chrono>
#include <utility>

namespace {

void updateMovementKey(Camera::MovementInput& input, SDL_Scancode scancode, bool pressed) {
    switch (scancode) {
        case SDL_SCANCODE_W:
            input.moveForward = pressed;
            break;
        case SDL_SCANCODE_S:
            input.moveBackward = pressed;
            break;
        case SDL_SCANCODE_A:
            input.moveLeft = pressed;
            break;
        case SDL_SCANCODE_D:
            input.moveRight = pressed;
            break;
        case SDL_SCANCODE_Q:
            input.moveDown = pressed;
            break;
        case SDL_SCANCODE_E:
            input.moveUp = pressed;
            break;
        case SDL_SCANCODE_LSHIFT:
        case SDL_SCANCODE_RSHIFT:
            input.sprint = pressed;
            break;
        default:
            break;
    }
}

}  // namespace

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

    m_materialRegistry.initDefaults();
    if (!m_renderer.uploadMaterialTable(m_materialRegistry.buildGpuTable())) {
        ORO_LOG_ERROR("Failed to upload initial material table");
        m_renderer.shutdown();
        m_context.shutdown();
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
        SDL_Quit();
        return false;
    }

    m_voxelRuntime.initialize();
    if (!m_voxelRuntime.waitForSeedBootstrapPublish(1200U)) {
        ORO_LOG_WARN("Seed bootstrap publish did not complete before timeout");
    }

    ORO_LOG_INFO("Window created: %dx%d", width, height);
    setMouseCapture(false);
    return true;
}

DebugViewMode App::nextDebugViewMode(DebugViewMode current) {
    switch (current) {
        case DebugViewMode::Lit:
            return DebugViewMode::Wireframe;
        case DebugViewMode::Wireframe:
            return DebugViewMode::WireframeOverlay;
        case DebugViewMode::WireframeOverlay:
            return DebugViewMode::Normals;
        case DebugViewMode::Normals:
            return DebugViewMode::FlatShading;
        case DebugViewMode::FlatShading:
            return DebugViewMode::MaterialId;
        case DebugViewMode::MaterialId:
        default:
            return DebugViewMode::Lit;
    }
}

void App::setMouseCapture(bool captured) {
    m_mouseCaptured = captured;
    if (m_window) {
        (void)SDL_SetWindowRelativeMouseMode(m_window, captured);
    }
}

void App::run() {
    m_running = true;
    auto lastTick = std::chrono::steady_clock::now();
    while (m_running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                m_running = false;
            } else if (ev.type == SDL_EVENT_MOUSE_MOTION && m_mouseCaptured) {
                m_camera.processMouseMotion(ev.motion.xrel, ev.motion.yrel);
            } else if (ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_KEY_UP) {
                const bool keyDown = ev.type == SDL_EVENT_KEY_DOWN;
                const bool repeat = ev.key.repeat;
                const SDL_Scancode scancode = ev.key.scancode;

                if (keyDown && !repeat && scancode == SDL_SCANCODE_ESCAPE) {
                    setMouseCapture(!m_mouseCaptured);
                    continue;
                }
                if (keyDown && !repeat && scancode == SDL_SCANCODE_F1) {
                    m_debugViewMode = nextDebugViewMode(m_debugViewMode);
                    continue;
                }
                if (keyDown && !repeat && scancode == SDL_SCANCODE_F2) {
                    m_debugOverlayFlags ^= DebugOverlayChunkBounds;
                    continue;
                }

                updateMovementKey(m_cameraInput, scancode, keyDown);
            }
        }
        const auto now = std::chrono::steady_clock::now();
        const auto delta = now - lastTick;
        lastTick = now;
        const auto deltaMs = std::chrono::duration_cast<std::chrono::milliseconds>(delta);
        const float deltaSeconds = std::chrono::duration<float>(delta).count();
        m_camera.update(deltaSeconds, m_cameraInput);
        m_voxelRuntime.tick(static_cast<uint64_t>(deltaMs.count()));
        while (auto meshSnapshot = m_voxelRuntime.popPublishedMeshSnapshot()) {
            (void)m_renderer.ingestVoxelMeshSnapshot(std::move(*meshSnapshot));
        }
        m_renderer.drawFrame(m_camera, m_debugViewMode, m_debugOverlayFlags);
        while (auto visibleVersion = m_renderer.popActivatedMeshVersion()) {
            (void)m_voxelRuntime.onRendererMeshVisible(*visibleVersion);
        }
    }
}

void App::shutdown() {
    setMouseCapture(false);
    m_renderer.shutdown();
    m_context.shutdown();

    if (m_window) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }

    SDL_Quit();
    ORO_LOG_INFO("App shutdown complete");
}
