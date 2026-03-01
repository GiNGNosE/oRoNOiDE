#pragma once

#include "voxel/dual_contouring.h"
#include "voxel/edit_classifier.h"
#include "voxel/voxel_world.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace oro::voxel {

struct LatencySample {
    double transactionMs = 0.0;
    double remeshMs = 0.0;
    double publishMs = 0.0;
    double collisionMs = 0.0;
    double endToEndMs = 0.0;
};

struct Percentiles {
    double p50 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
};

struct LatencyGate {
    double p95Max = 0.0;
    double p99MultiplierMax = 2.0;
    double p99AbsoluteMax = 0.0;
};

struct ClassLatencyGates {
    LatencyGate transaction;
    LatencyGate remesh;
    LatencyGate publish;
    LatencyGate collision;
    LatencyGate endToEnd;
};

struct GateConfig {
    std::array<ClassLatencyGates, 4> perClass{};
};

struct GateViolation {
    bool hardFail = false;
    const char* metric = "";
    const char* reason = "";
};

enum class DiagnosticSeverity : uint8_t {
    Info = 0,
    Warn = 1,
    Error = 2,
};

enum class DiagnosticsTier : uint8_t {
    Compact = 0,
    Expanded = 1,
    Full = 2,
};

struct DiagnosticPayload {
    uint32_t chunkCount = 0;
    uint64_t chunkHash = 0;
    std::vector<ChunkCoord> sampleChunks;
};

struct RuntimeDiagnostic {
    const char* phase = "";
    const char* invariantOrPolicyCode = "";
    DiagnosticSeverity severity = DiagnosticSeverity::Info;
    uint64_t topologyVersion = 0;
    uint64_t meshProducedVersion = 0;
    uint64_t meshVisibleVersion = 0;
    uint64_t collisionVersion = 0;
    const char* actionTaken = "";
    const char* purgeMode = "";
    DiagnosticPayload payload{};
};

class QualityGateEvaluator {
public:
    static GateConfig makeDefaultConfig();
    static Percentiles computePercentiles(std::vector<double> values);

    std::vector<GateViolation> evaluateLatency(EditSizeClass klass, const std::vector<LatencySample>& samples) const;
    std::vector<GateViolation> evaluateStructural(const MeshStructuralStats& stats) const;
    RuntimeDiagnostic buildDiagnostic(DiagnosticsTier tier, RuntimeDiagnostic base,
                                      const std::vector<ChunkCoord>& chunks,
                                      std::size_t expandedLimit = 8U) const;

private:
    explicit QualityGateEvaluator(GateConfig config);
    GateConfig m_config;

public:
    static QualityGateEvaluator createDefault() { return QualityGateEvaluator(makeDefaultConfig()); }
};

}  // namespace oro::voxel
