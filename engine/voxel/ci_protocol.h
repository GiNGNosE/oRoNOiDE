#pragma once

#include "voxel/edit_classifier.h"
#include "voxel/edit_types.h"

#include <array>
#include <cstdint>
#include <vector>

namespace oro::voxel {

struct CiProtocolConfig {
    uint32_t seedsPerClass = 20;
    uint32_t warmupIterations = 10;
    uint32_t workerThreads = 4;
    std::array<uint32_t, 4> classSeedsBase = {100U, 200U, 300U, 400U};
};

struct CiScenario {
    EditSizeClass klass = EditSizeClass::Small;
    uint32_t seed = 0;
    EditCommand command{};
};

class CiProtocol {
public:
    explicit CiProtocol(CiProtocolConfig config = {});

    std::vector<CiScenario> buildDeterministicScenarios() const;
    const CiProtocolConfig& config() const { return m_config; }

private:
    EditCommand makeCommand(EditSizeClass klass, uint32_t seed) const;
    CiProtocolConfig m_config;
};

}  // namespace oro::voxel
