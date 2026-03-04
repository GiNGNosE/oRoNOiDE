#pragma once

#include "material/material.h"

#include <cstdint>
#include <vector>

namespace oro::material {

class MaterialRegistry {
public:
    void initDefaults();
    uint16_t registerMaterial(const MaterialProperties& properties);
    const MaterialProperties& get(uint16_t materialId) const;

    std::vector<GpuMaterialEntry> buildGpuTable() const;

    bool gpuDirty() const { return m_gpuDirty; }
    void clearGpuDirty() { m_gpuDirty = false; }

private:
    std::vector<MaterialProperties> m_materials;
    bool m_gpuDirty = false;
};

}  // namespace oro::material
