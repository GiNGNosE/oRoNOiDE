#include "voxel/quality_gates.h"

#include <algorithm>

namespace oro::voxel {

namespace {

double percentileAt(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) {
        return 0.0;
    }
    const double clamped = std::clamp(p, 0.0, 1.0);
    const double idx = clamped * static_cast<double>(sorted.size() - 1U);
    const std::size_t lo = static_cast<std::size_t>(idx);
    const std::size_t hi = std::min(lo + 1U, sorted.size() - 1U);
    const double t = idx - static_cast<double>(lo);
    return (sorted[lo] * (1.0 - t)) + (sorted[hi] * t);
}

ClassLatencyGates buildClass(double tx95, double tx99Abs,
                             double remesh95, double remesh99Abs,
                             double publish95, double publish99Abs,
                             double coll95, double coll99Abs,
                             double end95, double end99Abs) {
    ClassLatencyGates c{};
    c.transaction = {tx95, 2.0, tx99Abs};
    c.remesh = {remesh95, 2.0, remesh99Abs};
    c.publish = {publish95, 1.5, publish99Abs};
    c.collision = {coll95, 2.0, coll99Abs};
    c.endToEnd = {end95, 2.0, end99Abs};
    return c;
}

void appendLatencyViolations(std::vector<GateViolation>& out, const char* name, const Percentiles& pct, const LatencyGate& gate) {
    if (pct.p95 > gate.p95Max) {
        out.push_back({true, name, "p95 exceeds budget"});
    }
    if (gate.p99AbsoluteMax > 0.0 && pct.p99 > gate.p99AbsoluteMax) {
        out.push_back({true, name, "p99 absolute cap exceeded"});
    }
    if (pct.p95 > 0.0 && (pct.p99 / pct.p95) > gate.p99MultiplierMax) {
        out.push_back({true, name, "p99 multiplier exceeded"});
    }
}

}  // namespace

GateConfig QualityGateEvaluator::makeDefaultConfig() {
    GateConfig cfg{};
    cfg.perClass[static_cast<std::size_t>(EditSizeClass::Small)] =
        buildClass(2.0, 4.0, 8.0, 16.0, 1.0, 2.0, 15.0, 30.0, 25.0, 50.0);
    cfg.perClass[static_cast<std::size_t>(EditSizeClass::Medium)] =
        buildClass(6.0, 12.0, 20.0, 40.0, 1.0, 2.0, 15.0, 30.0, 60.0, 120.0);
    cfg.perClass[static_cast<std::size_t>(EditSizeClass::LargeStress)] =
        buildClass(15.0, 30.0, 60.0, 120.0, 1.0, 2.0, 80.0, 160.0, 150.0, 300.0);
    cfg.perClass[static_cast<std::size_t>(EditSizeClass::XLarge)] =
        buildClass(15.0, 30.0, 60.0, 120.0, 1.0, 2.0, 80.0, 160.0, 150.0, 300.0);
    return cfg;
}

Percentiles QualityGateEvaluator::computePercentiles(std::vector<double> values) {
    Percentiles pct{};
    if (values.empty()) {
        return pct;
    }
    std::sort(values.begin(), values.end());
    pct.p50 = percentileAt(values, 0.50);
    pct.p95 = percentileAt(values, 0.95);
    pct.p99 = percentileAt(values, 0.99);
    return pct;
}

QualityGateEvaluator::QualityGateEvaluator(GateConfig config)
    : m_config(config) {}

std::vector<GateViolation> QualityGateEvaluator::evaluateLatency(EditSizeClass klass,
                                                                 const std::vector<LatencySample>& samples) const {
    std::vector<GateViolation> violations;
    if (samples.empty()) {
        return violations;
    }
    std::vector<double> tx;
    std::vector<double> remesh;
    std::vector<double> publish;
    std::vector<double> collision;
    std::vector<double> end;
    tx.reserve(samples.size());
    remesh.reserve(samples.size());
    publish.reserve(samples.size());
    collision.reserve(samples.size());
    end.reserve(samples.size());
    for (const LatencySample& s : samples) {
        tx.push_back(s.transactionMs);
        remesh.push_back(s.remeshMs);
        publish.push_back(s.publishMs);
        collision.push_back(s.collisionMs);
        end.push_back(s.endToEndMs);
    }
    const ClassLatencyGates& gates = m_config.perClass[static_cast<std::size_t>(klass)];
    appendLatencyViolations(violations, "transaction", computePercentiles(std::move(tx)), gates.transaction);
    appendLatencyViolations(violations, "remesh", computePercentiles(std::move(remesh)), gates.remesh);
    appendLatencyViolations(violations, "publish", computePercentiles(std::move(publish)), gates.publish);
    appendLatencyViolations(violations, "collision", computePercentiles(std::move(collision)), gates.collision);
    appendLatencyViolations(violations, "end_to_end", computePercentiles(std::move(end)), gates.endToEnd);
    return violations;
}

std::vector<GateViolation> QualityGateEvaluator::evaluateStructural(const MeshStructuralStats& stats) const {
    std::vector<GateViolation> violations;
    if (stats.nanCount > 0) {
        violations.push_back({true, "mesh_structural", "NaN/Inf count > 0"});
    }
    if (stats.invalidIndexCount > 0) {
        violations.push_back({true, "mesh_structural", "Invalid index count > 0"});
    }
    if (stats.degenerateCount > 0) {
        violations.push_back({true, "mesh_structural", "Degenerate triangle count > 0"});
    }
    return violations;
}

RuntimeDiagnostic QualityGateEvaluator::buildDiagnostic(DiagnosticsTier tier, RuntimeDiagnostic base,
                                                        const std::vector<ChunkCoord>& chunks,
                                                        std::size_t expandedLimit) const {
    uint64_t hash = 1469598103934665603ULL;
    constexpr uint64_t prime = 1099511628211ULL;
    for (const ChunkCoord& c : chunks) {
        hash ^= static_cast<uint64_t>(static_cast<uint32_t>(c.x));
        hash *= prime;
        hash ^= static_cast<uint64_t>(static_cast<uint32_t>(c.y));
        hash *= prime;
        hash ^= static_cast<uint64_t>(static_cast<uint32_t>(c.z));
        hash *= prime;
    }
    base.payload.chunkCount = static_cast<uint32_t>(chunks.size());
    base.payload.chunkHash = hash;

    if (tier == DiagnosticsTier::Expanded || tier == DiagnosticsTier::Full) {
        const std::size_t cap = tier == DiagnosticsTier::Full ? chunks.size() : std::min(chunks.size(), expandedLimit);
        base.payload.sampleChunks.assign(chunks.begin(), chunks.begin() + static_cast<std::ptrdiff_t>(cap));
    }
    return base;
}

}  // namespace oro::voxel
