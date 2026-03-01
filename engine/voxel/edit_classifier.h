#pragma once

#include "voxel/voxel_world.h"

#include <string_view>

namespace oro::voxel {

enum class EditSizeClass {
    Small = 0,
    Medium = 1,
    LargeStress = 2,
    XLarge = 3,
};

struct EditClassThresholds {
    uint32_t smallChunksMax = 4;
    uint32_t smallCellsMax = 40000;
    uint32_t smallSurfaceCellsMax = 12000;

    uint32_t mediumChunksMax = 16;
    uint32_t mediumCellsMax = 180000;
    uint32_t mediumSurfaceCellsMax = 60000;

    uint32_t largeChunksMax = 64;
    uint32_t largeCellsMax = 700000;
    uint32_t largeSurfaceCellsMax = 220000;
};

class EditClassifier {
public:
    explicit EditClassifier(EditClassThresholds thresholds = {});

    EditSizeClass classify(const DirtyMetrics& metrics) const;
    std::string_view toString(EditSizeClass value) const;
    const EditClassThresholds& thresholds() const { return m_thresholds; }

private:
    static EditSizeClass classifyByChunks(uint32_t chunks, const EditClassThresholds& thresholds);
    static EditSizeClass classifyByCells(uint32_t cells, const EditClassThresholds& thresholds);
    static EditSizeClass classifyBySurfaceCells(uint32_t surfaceCells, const EditClassThresholds& thresholds);

    EditClassThresholds m_thresholds;
};

}  // namespace oro::voxel
