#pragma once

#include <cstdint>

enum class DebugViewMode : uint32_t {
    Lit = 0,
    Wireframe = 1,
    WireframeOverlay = 2,
    Normals = 3,
    FlatShading = 4,
    MaterialId = 5,
};

enum DebugOverlayFlags : uint32_t {
    DebugOverlayNone = 0,
    DebugOverlayChunkBounds = 1u << 0u,
};
