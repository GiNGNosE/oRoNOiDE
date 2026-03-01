#include "voxel/edit_classifier.h"

#include <algorithm>

namespace oro::voxel {

namespace {

EditSizeClass maxSeverity(EditSizeClass a, EditSizeClass b) {
    return (static_cast<int>(a) > static_cast<int>(b)) ? a : b;
}

}  // namespace

EditClassifier::EditClassifier(EditClassThresholds thresholds)
    : m_thresholds(thresholds) {}

EditSizeClass EditClassifier::classify(const DirtyMetrics& metrics) const {
    const EditSizeClass chunksClass = classifyByChunks(metrics.chunksDirty, m_thresholds);
    const EditSizeClass cellsClass = classifyByCells(metrics.cellsDirty, m_thresholds);
    const EditSizeClass surfaceClass = classifyBySurfaceCells(metrics.surfaceCellsDirty, m_thresholds);
    return maxSeverity(maxSeverity(chunksClass, cellsClass), surfaceClass);
}

std::string_view EditClassifier::toString(EditSizeClass value) const {
    switch (value) {
        case EditSizeClass::Small:
            return "small";
        case EditSizeClass::Medium:
            return "medium";
        case EditSizeClass::LargeStress:
            return "large_stress";
        case EditSizeClass::XLarge:
            return "xlarge";
    }
    return "unknown";
}

EditSizeClass EditClassifier::classifyByChunks(uint32_t chunks, const EditClassThresholds& thresholds) {
    if (chunks <= thresholds.smallChunksMax) {
        return EditSizeClass::Small;
    }
    if (chunks <= thresholds.mediumChunksMax) {
        return EditSizeClass::Medium;
    }
    if (chunks <= thresholds.largeChunksMax) {
        return EditSizeClass::LargeStress;
    }
    return EditSizeClass::XLarge;
}

EditSizeClass EditClassifier::classifyByCells(uint32_t cells, const EditClassThresholds& thresholds) {
    if (cells <= thresholds.smallCellsMax) {
        return EditSizeClass::Small;
    }
    if (cells <= thresholds.mediumCellsMax) {
        return EditSizeClass::Medium;
    }
    if (cells <= thresholds.largeCellsMax) {
        return EditSizeClass::LargeStress;
    }
    return EditSizeClass::XLarge;
}

EditSizeClass EditClassifier::classifyBySurfaceCells(uint32_t surfaceCells, const EditClassThresholds& thresholds) {
    if (surfaceCells <= thresholds.smallSurfaceCellsMax) {
        return EditSizeClass::Small;
    }
    if (surfaceCells <= thresholds.mediumSurfaceCellsMax) {
        return EditSizeClass::Medium;
    }
    if (surfaceCells <= thresholds.largeSurfaceCellsMax) {
        return EditSizeClass::LargeStress;
    }
    return EditSizeClass::XLarge;
}

}  // namespace oro::voxel
