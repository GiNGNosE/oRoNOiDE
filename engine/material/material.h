#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace oro::material {

struct MaterialProperties {
    std::string name;
    std::array<float, 3> albedo{0.7F, 0.7F, 0.7F};
    float roughness = 0.8F;
    float metallic = 0.0F;
    std::array<float, 3> emissive{0.0F, 0.0F, 0.0F};
};

struct GpuMaterialEntry {
    std::array<float, 4> albedoRoughness{0.7F, 0.7F, 0.7F, 0.8F};
    std::array<float, 4> emissiveMetallic{0.0F, 0.0F, 0.0F, 0.0F};
};

static_assert(sizeof(GpuMaterialEntry) == 32, "GpuMaterialEntry must be 32 bytes");
static_assert(alignof(GpuMaterialEntry) == alignof(float), "GpuMaterialEntry must be tightly packed");

}  // namespace oro::material
