#include "voxel/ci_protocol.h"

#include <random>

namespace oro::voxel {

CiProtocol::CiProtocol(CiProtocolConfig config)
    : m_config(config) {}

EditCommand CiProtocol::makeCommand(EditSizeClass klass, uint32_t seed) const {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> posDist(-2.0F, 2.0F);
    EditCommand command{};
    command.mode = (seed % 2U == 0U) ? EditMode::Carve : EditMode::Fill;
    command.criticality = (seed % 3U == 0U) ? EditCriticality::Hard : EditCriticality::Soft;
    command.center = {posDist(rng), posDist(rng), posDist(rng)};
    switch (klass) {
        case EditSizeClass::Small:
            command.radius = 0.5F;
            break;
        case EditSizeClass::Medium:
            command.radius = 1.5F;
            break;
        case EditSizeClass::LargeStress:
            command.radius = 3.0F;
            break;
        case EditSizeClass::XLarge:
            command.radius = 5.0F;
            break;
    }
    return command;
}

std::vector<CiScenario> CiProtocol::buildDeterministicScenarios() const {
    std::vector<CiScenario> scenarios;
    for (std::size_t classIdx = 0; classIdx < m_config.classSeedsBase.size(); ++classIdx) {
        const EditSizeClass klass = static_cast<EditSizeClass>(classIdx);
        const uint32_t seedBase = m_config.classSeedsBase[classIdx];
        for (uint32_t i = 0; i < m_config.seedsPerClass; ++i) {
            const uint32_t seed = seedBase + i;
            scenarios.push_back({klass, seed, makeCommand(klass, seed)});
        }
    }
    return scenarios;
}

}  // namespace oro::voxel
