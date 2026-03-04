#include "voxel/ci_protocol.h"

#include <random>

namespace oro::voxel {

CiProtocol::CiProtocol(CiProtocolConfig config)
    : m_config(config) {}

EditCommand CiProtocol::makeCommand(EditSizeClass klass, uint32_t seed) const {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> posDist(-2.0F, 2.0F);
    std::uniform_real_distribution<float> axisJitter(0.75F, 1.35F);
    EditCommand command{};
    command.mode = (seed % 2U == 0U) ? EditMode::Carve : EditMode::Fill;
    command.criticality = (seed % 3U == 0U) ? EditCriticality::Hard : EditCriticality::Soft;
    command.center = {posDist(rng), posDist(rng), posDist(rng)};
    command.shape = static_cast<EditShape>(seed % 3U);
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
    command.radii = {
        std::max(0.2F, command.radius * axisJitter(rng)),
        std::max(0.2F, command.radius * axisJitter(rng)),
        std::max(0.2F, command.radius * axisJitter(rng)),
    };
    command.noiseAmplitude = 0.12F + (0.04F * static_cast<float>((seed / 3U) % 4U));
    command.noiseFrequency = 1.5F + (0.35F * static_cast<float>((seed / 5U) % 5U));
    command.noiseSeed = seed * 17U;
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
