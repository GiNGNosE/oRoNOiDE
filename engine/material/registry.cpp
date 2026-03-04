#include "material/registry.h"

#include <cassert>
#include <limits>
#include <stdexcept>

namespace oro::material {

uint16_t MaterialRegistry::registerMaterial(const MaterialProperties& properties) {
    if (m_materials.size() >= static_cast<std::size_t>(std::numeric_limits<uint16_t>::max()) + 1U) {
        throw std::runtime_error("MaterialRegistry exceeded uint16_t material ID range");
    }
    m_materials.push_back(properties);
    m_gpuDirty = true;
    return static_cast<uint16_t>(m_materials.size() - 1U);
}

const MaterialProperties& MaterialRegistry::get(uint16_t materialId) const {
    assert(materialId < m_materials.size() && "Material ID out of range");
    if (materialId >= m_materials.size()) {
        return m_materials.front();
    }
    return m_materials[materialId];
}

std::vector<GpuMaterialEntry> MaterialRegistry::buildGpuTable() const {
    std::vector<GpuMaterialEntry> table;
    table.reserve(m_materials.size());
    for (const MaterialProperties& material : m_materials) {
        GpuMaterialEntry gpuEntry{};
        gpuEntry.albedoRoughness = {
            material.albedo[0], material.albedo[1], material.albedo[2], material.roughness};
        gpuEntry.emissiveMetallic = {
            material.emissive[0], material.emissive[1], material.emissive[2], material.metallic};
        table.push_back(gpuEntry);
    }
    return table;
}

void MaterialRegistry::initDefaults() {
    m_materials.clear();
    m_gpuDirty = false;

    (void)registerMaterial(MaterialProperties{
        "Stone",
        {0.75F, 0.74F, 0.72F},
        0.92F,
        0.0F,
        {0.0F, 0.0F, 0.0F}});
    (void)registerMaterial(MaterialProperties{
        "Dirt",
        {0.46F, 0.42F, 0.37F},
        0.95F,
        0.0F,
        {0.0F, 0.0F, 0.0F}});
    (void)registerMaterial(MaterialProperties{
        "Grass",
        {0.32F, 0.47F, 0.30F},
        0.90F,
        0.0F,
        {0.0F, 0.0F, 0.0F}});
    (void)registerMaterial(MaterialProperties{
        "Iron",
        {0.40F, 0.42F, 0.52F},
        0.25F,
        1.0F,
        {0.0F, 0.0F, 0.0F}});
    (void)registerMaterial(MaterialProperties{
        "Sandstone",
        {0.68F, 0.58F, 0.42F},
        0.55F,
        0.0F,
        {0.0F, 0.0F, 0.0F}});
}

}  // namespace oro::material
