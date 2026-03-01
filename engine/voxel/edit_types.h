#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace oro::voxel {

enum class EditMode : uint8_t {
    Carve = 0,
    Fill = 1,
};

enum class EditCriticality : uint8_t {
    Soft = 0,
    Hard = 1,
};

struct EditCommand {
    EditMode mode = EditMode::Carve;
    EditCriticality criticality = EditCriticality::Soft;
    std::array<float, 3> center = {0.0F, 0.0F, 0.0F};
    float radius = 1.0F;
};

enum class InvariantCode : uint8_t {
    None = 0,
    InvalidPhi,
    UninitializedMaterial,
    ActiveCountMismatch,
    SeamOccupancyMismatch,
    VersionParityMismatch,
};

struct InvariantFailure {
    InvariantCode code = InvariantCode::None;
    int chunkX = 0;
    int chunkY = 0;
    int chunkZ = 0;
    std::string_view message = {};
};

}  // namespace oro::voxel
