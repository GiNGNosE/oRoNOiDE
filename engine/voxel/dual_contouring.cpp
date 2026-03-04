#include "voxel/dual_contouring.h"

#include "core/log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace oro::voxel {

namespace {

constexpr float kGradientEpsilon = 1.0e-6F;
constexpr float kQefRegularization = 1.0e-4F;
constexpr float kQefPivotEpsilon = 1.0e-8F;
constexpr float kQefConditionMinDiag = 1.0e-5F;
constexpr float kQefConditionRatioMax = 1.0e4F;
constexpr float kQefResidualThresholdScale = 0.05F;
constexpr float kQefOutOfCellRejectScaleIllConditioned = 0.5F;
constexpr float kQefOutOfCellRejectScaleWellConditioned = 0.75F;
constexpr float kQefAcceptedOutOfCellPullStartScale = 0.15F;
constexpr float kQefAcceptedOutOfCellMaxBlend = 0.65F;
constexpr float kLocalRadialProtrusionClampScale = 0.25F;
constexpr float kLocalRadialProtrusionTargetScale = 0.18F;
constexpr float kLocalRadialProtrusionClampBlend = 0.7F;
constexpr int kCellsPerAxis = kChunkEdge;
constexpr uint32_t kInvalidVertexIndex = std::numeric_limits<uint32_t>::max();

std::string escapeJson(const char* text) {
    if (text == nullptr) {
        return "";
    }
    std::string out;
    out.reserve(std::char_traits<char>::length(text));
    for (const char ch : std::string(text)) {
        switch (ch) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += ch;
            break;
        }
    }
    return out;
}

void appendDebugLog(const char* runId,
                    const char* hypothesisId,
                    const char* location,
                    const char* message,
                    const std::string& dataJson) {
    static std::mutex sLogMutex;
    static std::atomic<bool> sOpenFailureReported{false};
    std::lock_guard<std::mutex> lock(sLogMutex);
    std::error_code ec;
    std::filesystem::create_directories("/Users/gingnose/dev/oRoNOiDE/.cursor", ec);
    std::ofstream logFile("/Users/gingnose/dev/oRoNOiDE/.cursor/debug-8c2e28.log", std::ios::app);
    if (!logFile.is_open()) {
        if (!sOpenFailureReported.exchange(true)) {
            ORO_LOG_WARN("Debug log append failed at %s (path=%s)",
                         location != nullptr ? location : "unknown",
                         "/Users/gingnose/dev/oRoNOiDE/.cursor/debug-8c2e28.log");
        }
        return;
    }
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    logFile << "{\"sessionId\":\"8c2e28\""
            << ",\"runId\":\"" << escapeJson(runId) << "\""
            << ",\"hypothesisId\":\"" << escapeJson(hypothesisId) << "\""
            << ",\"location\":\"" << escapeJson(location) << "\""
            << ",\"message\":\"" << escapeJson(message) << "\""
            << ",\"data\":" << dataJson
            << ",\"timestamp\":" << nowMs
            << "}\n";
}

enum class Axis : uint8_t {
    X = 0,
    Y = 1,
    Z = 2,
};

struct LatticeSample {
    float phi = 1.0F;
    uint16_t materialId = 1;
};

struct CellCoord {
    int x = 0;
    int y = 0;
    int z = 0;
};

struct QefAccumulator {
    std::array<std::array<float, 3>, 3> ata{};
    std::array<float, 3> atb{};
    std::array<float, 3> massPoint{};
    std::array<float, 3> normalSum{};
    uint32_t sampleCount = 0;
    uint16_t materialId = 1;

    void addSample(const HermiteSample& sample) {
        const auto& n = sample.normal;
        const float b = (n[0] * sample.intersection[0]) + (n[1] * sample.intersection[1]) + (n[2] * sample.intersection[2]);
        ata[0][0] += n[0] * n[0];
        ata[0][1] += n[0] * n[1];
        ata[0][2] += n[0] * n[2];
        ata[1][0] += n[1] * n[0];
        ata[1][1] += n[1] * n[1];
        ata[1][2] += n[1] * n[2];
        ata[2][0] += n[2] * n[0];
        ata[2][1] += n[2] * n[1];
        ata[2][2] += n[2] * n[2];
        atb[0] += n[0] * b;
        atb[1] += n[1] * b;
        atb[2] += n[2] * b;
        massPoint[0] += sample.intersection[0];
        massPoint[1] += sample.intersection[1];
        massPoint[2] += sample.intersection[2];
        normalSum[0] += sample.normal[0];
        normalSum[1] += sample.normal[1];
        normalSum[2] += sample.normal[2];
        if (sampleCount == 0U) {
            materialId = sample.materialId;
        }
        ++sampleCount;
    }
};

struct CellWork {
    QefAccumulator qef{};
    bool active = false;
    uint32_t vertexIndex = kInvalidVertexIndex;
};

struct CrossingEdge {
    Axis axis = Axis::X;
    int x = 0;
    int y = 0;
    int z = 0;
    std::array<float, 3> normal{};
};

struct QefReasonStats {
    uint32_t illByMinDiag = 0;
    uint32_t illByRatio = 0;
    uint32_t solveFailed = 0;
    uint32_t residualRejected = 0;
    uint32_t outOfCellRejected = 0;
    uint32_t outOfCellRejectedIllConditioned = 0;
    uint32_t outOfCellRejectedWellConditioned = 0;
};

struct VertexPlacementStats {
    uint32_t solvedVertices = 0;
    uint32_t clampedVertices = 0;
    uint32_t boundaryPinnedVertices = 0;
    uint32_t outOfCellVertices = 0;
    uint32_t rejectedOutOfCellVertices = 0;
    float clampDisplacementAccum = 0.0F;
    float maxClampDisplacement = 0.0F;
    float massPointDistanceAccum = 0.0F;
    float maxMassPointDistance = 0.0F;
    float outOfCellDistanceAccum = 0.0F;
    float maxOutOfCellDistance = 0.0F;
    float rejectedOutOfCellDistanceAccum = 0.0F;
    float maxRejectedOutOfCellDistance = 0.0F;
    uint32_t acceptedOutOfCellIllConditioned = 0;
    uint32_t acceptedOutOfCellWellConditioned = 0;
    float acceptedOutOfCellIllDistanceAccum = 0.0F;
    float acceptedOutOfCellWellDistanceAccum = 0.0F;
    float maxAcceptedOutOfCellIllDistance = 0.0F;
    float maxAcceptedOutOfCellWellDistance = 0.0F;
    uint32_t acceptedOutWellLeQuarterVoxel = 0;
    uint32_t acceptedOutWellLeHalfVoxel = 0;
    uint32_t acceptedOutWellLeThreeQuarterVoxel = 0;
    uint32_t acceptedOutWellLeFullVoxel = 0;
    uint32_t acceptedOutOfCellSoftPulledVertices = 0;
    float acceptedOutOfCellSoftPullBlendAccum = 0.0F;
    float maxAcceptedOutOfCellSoftPullBlend = 0.0F;
};

int floorDiv(int value, int divisor) {
    int quotient = value / divisor;
    const int remainder = value % divisor;
    if (remainder != 0 && ((remainder > 0) != (divisor > 0))) {
        --quotient;
    }
    return quotient;
}

int floorMod(int value, int divisor) {
    const int remainder = value % divisor;
    return remainder < 0 ? remainder + divisor : remainder;
}

std::array<float, 3> add(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

std::array<float, 3> sub(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

std::array<float, 3> scale(const std::array<float, 3>& v, float s) {
    return {v[0] * s, v[1] * s, v[2] * s};
}

float dot(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
}

std::array<float, 3> cross(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return {(a[1] * b[2]) - (a[2] * b[1]), (a[2] * b[0]) - (a[0] * b[2]), (a[0] * b[1]) - (a[1] * b[0])};
}

float length(const std::array<float, 3>& v) {
    return std::sqrt(dot(v, v));
}

float triangleAreaFromPositions(const std::array<float, 3>& a, const std::array<float, 3>& b, const std::array<float, 3>& c) {
    return 0.5F * length(cross(sub(b, a), sub(c, a)));
}

float triangleAspectRatioFromPositions(const std::array<float, 3>& a,
                                       const std::array<float, 3>& b,
                                       const std::array<float, 3>& c) {
    const float edgeAB = length(sub(b, a));
    const float edgeBC = length(sub(c, b));
    const float edgeCA = length(sub(a, c));
    const float maxEdge = std::max({edgeAB, edgeBC, edgeCA});
    const float minEdge = std::max(std::min({edgeAB, edgeBC, edgeCA}), kGradientEpsilon);
    return maxEdge / minEdge;
}

std::array<float, 3> normalizeVec(const std::array<float, 3>& v) {
    const float len = length(v);
    if (len < kGradientEpsilon) {
        return {0.0F, 1.0F, 0.0F};
    }
    const float inv = 1.0F / len;
    return {v[0] * inv, v[1] * inv, v[2] * inv};
}

template <typename ChunkLookup>
LatticeSample sampleLattice(const ChunkCoord& ownerCoord, int localX, int localY, int localZ, const ChunkLookup& chunkLookup) {
    const int globalX = (ownerCoord.x * kChunkEdge) + localX;
    const int globalY = (ownerCoord.y * kChunkEdge) + localY;
    const int globalZ = (ownerCoord.z * kChunkEdge) + localZ;
    const ChunkCoord targetCoord{
        floorDiv(globalX, kChunkEdge),
        floorDiv(globalY, kChunkEdge),
        floorDiv(globalZ, kChunkEdge),
    };
    const int tx = floorMod(globalX, kChunkEdge);
    const int ty = floorMod(globalY, kChunkEdge);
    const int tz = floorMod(globalZ, kChunkEdge);
    const VoxelChunk* chunk = chunkLookup(targetCoord);
    if (chunk == nullptr) {
        return {};
    }
    const VoxelCell& cell = chunk->cells[VoxelWorld::flatIndex(tx, ty, tz)];
    return {cell.phi, cell.material.materialId};
}

std::array<float, 3> latticePointWorld(const ChunkCoord& ownerCoord, int localX, int localY, int localZ, float voxelSize) {
    const float gx = static_cast<float>((ownerCoord.x * kChunkEdge) + localX) + 0.5F;
    const float gy = static_cast<float>((ownerCoord.y * kChunkEdge) + localY) + 0.5F;
    const float gz = static_cast<float>((ownerCoord.z * kChunkEdge) + localZ) + 0.5F;
    return {gx * voxelSize, gy * voxelSize, gz * voxelSize};
}

bool solveLinear3x3(const std::array<std::array<float, 3>, 3>& matrix, const std::array<float, 3>& rhs, std::array<float, 3>& out) {
    std::array<std::array<float, 4>, 3> augmented{};
    for (std::size_t r = 0; r < 3; ++r) {
        augmented[r][0] = matrix[r][0];
        augmented[r][1] = matrix[r][1];
        augmented[r][2] = matrix[r][2];
        augmented[r][3] = rhs[r];
    }

    for (std::size_t pivot = 0; pivot < 3; ++pivot) {
        std::size_t best = pivot;
        float bestAbs = std::abs(augmented[pivot][pivot]);
        for (std::size_t row = pivot + 1; row < 3; ++row) {
            const float candidate = std::abs(augmented[row][pivot]);
            if (candidate > bestAbs) {
                bestAbs = candidate;
                best = row;
            }
        }
        if (bestAbs < kQefPivotEpsilon) {
            return false;
        }
        if (best != pivot) {
            std::swap(augmented[pivot], augmented[best]);
        }
        const float invPivot = 1.0F / augmented[pivot][pivot];
        for (std::size_t col = pivot; col < 4; ++col) {
            augmented[pivot][col] *= invPivot;
        }
        for (std::size_t row = 0; row < 3; ++row) {
            if (row == pivot) {
                continue;
            }
            const float factor = augmented[row][pivot];
            for (std::size_t col = pivot; col < 4; ++col) {
                augmented[row][col] -= factor * augmented[pivot][col];
            }
        }
    }

    out[0] = augmented[0][3];
    out[1] = augmented[1][3];
    out[2] = augmented[2][3];
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

template <typename ChunkLookup>
std::array<float, 3> gradientAt(const ChunkCoord& ownerCoord, int localX, int localY, int localZ, float voxelSize,
                                const ChunkLookup& chunkLookup) {
    const auto samplePhi = [&](int x, int y, int z) {
        return sampleLattice(ownerCoord, x, y, z, chunkLookup).phi;
    };
    const float sx = samplePhi(localX + 1, localY, localZ) - samplePhi(localX - 1, localY, localZ);
    const float sy = samplePhi(localX, localY + 1, localZ) - samplePhi(localX, localY - 1, localZ);
    const float sz = samplePhi(localX, localY, localZ + 1) - samplePhi(localX, localY, localZ - 1);
    const float invDx = 0.5F / voxelSize;
    return normalizeVec({sx * invDx, sy * invDx, sz * invDx});
}

template <typename ChunkLookup>
float samplePhiGlobal(int gx, int gy, int gz, const ChunkLookup& chunkLookup) {
    const ChunkCoord coord{
        floorDiv(gx, kChunkEdge),
        floorDiv(gy, kChunkEdge),
        floorDiv(gz, kChunkEdge),
    };
    const int lx = floorMod(gx, kChunkEdge);
    const int ly = floorMod(gy, kChunkEdge);
    const int lz = floorMod(gz, kChunkEdge);
    const VoxelChunk* chunk = chunkLookup(coord);
    if (chunk == nullptr) {
        return 1.0F;
    }
    return chunk->cells[VoxelWorld::flatIndex(lx, ly, lz)].phi;
}

template <typename ChunkLookup>
float samplePhiWorld(const std::array<float, 3>& worldPos, float voxelSize, const ChunkLookup& chunkLookup) {
    const float fx = (worldPos[0] / voxelSize) - 0.5F;
    const float fy = (worldPos[1] / voxelSize) - 0.5F;
    const float fz = (worldPos[2] / voxelSize) - 0.5F;

    const int x0 = static_cast<int>(std::floor(fx));
    const int y0 = static_cast<int>(std::floor(fy));
    const int z0 = static_cast<int>(std::floor(fz));
    const int x1 = x0 + 1;
    const int y1 = y0 + 1;
    const int z1 = z0 + 1;

    const float tx = std::clamp(fx - static_cast<float>(x0), 0.0F, 1.0F);
    const float ty = std::clamp(fy - static_cast<float>(y0), 0.0F, 1.0F);
    const float tz = std::clamp(fz - static_cast<float>(z0), 0.0F, 1.0F);

    const float c000 = samplePhiGlobal(x0, y0, z0, chunkLookup);
    const float c100 = samplePhiGlobal(x1, y0, z0, chunkLookup);
    const float c010 = samplePhiGlobal(x0, y1, z0, chunkLookup);
    const float c110 = samplePhiGlobal(x1, y1, z0, chunkLookup);
    const float c001 = samplePhiGlobal(x0, y0, z1, chunkLookup);
    const float c101 = samplePhiGlobal(x1, y0, z1, chunkLookup);
    const float c011 = samplePhiGlobal(x0, y1, z1, chunkLookup);
    const float c111 = samplePhiGlobal(x1, y1, z1, chunkLookup);

    const float c00 = (c000 * (1.0F - tx)) + (c100 * tx);
    const float c10 = (c010 * (1.0F - tx)) + (c110 * tx);
    const float c01 = (c001 * (1.0F - tx)) + (c101 * tx);
    const float c11 = (c011 * (1.0F - tx)) + (c111 * tx);
    const float c0 = (c00 * (1.0F - ty)) + (c10 * ty);
    const float c1 = (c01 * (1.0F - ty)) + (c11 * ty);
    return (c0 * (1.0F - tz)) + (c1 * tz);
}

template <typename ChunkLookup>
std::array<float, 3> gradientAtPosition(const std::array<float, 3>& worldPos, float voxelSize, const ChunkLookup& chunkLookup) {
    const std::array<float, 3> dx{voxelSize, 0.0F, 0.0F};
    const std::array<float, 3> dy{0.0F, voxelSize, 0.0F};
    const std::array<float, 3> dz{0.0F, 0.0F, voxelSize};
    const float sx = samplePhiWorld(add(worldPos, dx), voxelSize, chunkLookup) - samplePhiWorld(sub(worldPos, dx), voxelSize, chunkLookup);
    const float sy = samplePhiWorld(add(worldPos, dy), voxelSize, chunkLookup) - samplePhiWorld(sub(worldPos, dy), voxelSize, chunkLookup);
    const float sz = samplePhiWorld(add(worldPos, dz), voxelSize, chunkLookup) - samplePhiWorld(sub(worldPos, dz), voxelSize, chunkLookup);
    const float invDx = 0.5F / voxelSize;
    return normalizeVec({sx * invDx, sy * invDx, sz * invDx});
}

bool isValidCell(const CellCoord& c) {
    return c.x >= 0 && c.x < kCellsPerAxis &&
           c.y >= 0 && c.y < kCellsPerAxis &&
           c.z >= 0 && c.z < kCellsPerAxis;
}

std::size_t flatCellIndex(const CellCoord& c) {
    return static_cast<std::size_t>((c.z * kCellsPerAxis * kCellsPerAxis) + (c.y * kCellsPerAxis) + c.x);
}

int floorDivInt(int value, int divisor) {
    int q = value / divisor;
    int r = value % divisor;
    if (r != 0 && ((r < 0) != (divisor < 0))) {
        --q;
    }
    return q;
}

std::array<CellCoord, 4> incidentCells(Axis axis, int x, int y, int z) {
    if (axis == Axis::X) {
        return {CellCoord{x, y - 1, z - 1}, CellCoord{x, y, z - 1}, CellCoord{x, y, z}, CellCoord{x, y - 1, z}};
    }
    if (axis == Axis::Y) {
        return {CellCoord{x - 1, y, z - 1}, CellCoord{x, y, z - 1}, CellCoord{x, y, z}, CellCoord{x - 1, y, z}};
    }
    return {CellCoord{x - 1, y - 1, z}, CellCoord{x, y - 1, z}, CellCoord{x, y, z}, CellCoord{x - 1, y, z}};
}

bool ownsCrossing(Axis axis, const ChunkCoord& coord, int x, int y, int z) {
    if (axis == Axis::X) {
        if (!(x >= 0 && x < kChunkEdge && y > 0 && y < kChunkEdge && z > 0 && z < kChunkEdge)) {
            return false;
        }
        const int globalX = (coord.x * kChunkEdge) + x;
        const int globalY = (coord.y * kChunkEdge) + y;
        const int globalZ = (coord.z * kChunkEdge) + z;
        const ChunkCoord owner{
            floorDivInt(globalX, kChunkEdge),
            floorDivInt(globalY - 1, kChunkEdge),
            floorDivInt(globalZ - 1, kChunkEdge),
        };
        return owner == coord;
    }
    if (axis == Axis::Y) {
        if (!(y >= 0 && y < kChunkEdge && x > 0 && x < kChunkEdge && z > 0 && z < kChunkEdge)) {
            return false;
        }
        const int globalX = (coord.x * kChunkEdge) + x;
        const int globalY = (coord.y * kChunkEdge) + y;
        const int globalZ = (coord.z * kChunkEdge) + z;
        const ChunkCoord owner{
            floorDivInt(globalX - 1, kChunkEdge),
            floorDivInt(globalY, kChunkEdge),
            floorDivInt(globalZ - 1, kChunkEdge),
        };
        return owner == coord;
    }
    if (!(z >= 0 && z < kChunkEdge && x > 0 && x < kChunkEdge && y > 0 && y < kChunkEdge)) {
        return false;
    }
    const int globalX = (coord.x * kChunkEdge) + x;
    const int globalY = (coord.y * kChunkEdge) + y;
    const int globalZ = (coord.z * kChunkEdge) + z;
    const ChunkCoord owner{
        floorDivInt(globalX - 1, kChunkEdge),
        floorDivInt(globalY - 1, kChunkEdge),
        floorDivInt(globalZ, kChunkEdge),
    };
    return owner == coord;
}

template <typename ChunkLookup>
void accumulateEdgeCrossingsForAxis(Axis axis, const ChunkCoord& coord, float voxelSize, const ChunkLookup& chunkLookup,
                                    std::vector<CellWork>& cells, std::vector<CrossingEdge>& crossings,
                                    MeshPatchBatch::Diagnostics& diagnostics) {
    const int xMax = axis == Axis::X ? kChunkEdge - 1 : kChunkEdge;
    const int yMax = axis == Axis::Y ? kChunkEdge - 1 : kChunkEdge;
    const int zMax = axis == Axis::Z ? kChunkEdge - 1 : kChunkEdge;

    for (int z = 0; z <= zMax; ++z) {
        for (int y = 0; y <= yMax; ++y) {
            for (int x = 0; x <= xMax; ++x) {
                if (!ownsCrossing(axis, coord, x, y, z)) {
                    continue;
                }
                const int nx = x + (axis == Axis::X ? 1 : 0);
                const int ny = y + (axis == Axis::Y ? 1 : 0);
                const int nz = z + (axis == Axis::Z ? 1 : 0);
                const LatticeSample s0 = sampleLattice(coord, x, y, z, chunkLookup);
                const LatticeSample s1 = sampleLattice(coord, nx, ny, nz, chunkLookup);
                const bool crossing = (s0.phi < kIsoValue && s1.phi >= kIsoValue) || (s0.phi >= kIsoValue && s1.phi < kIsoValue);
                if (!crossing) {
                    continue;
                }
                const auto p0 = latticePointWorld(coord, x, y, z, voxelSize);
                const auto p1 = latticePointWorld(coord, nx, ny, nz, voxelSize);
                const float denom = s0.phi - s1.phi;
                const float t = std::abs(denom) < kGradientEpsilon ? 0.5F : std::clamp(s0.phi / denom, 0.0F, 1.0F);

                HermiteSample sample{};
                sample.intersection = add(p0, scale(sub(p1, p0), t));
                sample.normal = gradientAtPosition(sample.intersection, voxelSize, chunkLookup);
                if (!std::isfinite(sample.normal[0]) || !std::isfinite(sample.normal[1]) || !std::isfinite(sample.normal[2])) {
                    sample.normal = gradientAt(coord, x, y, z, voxelSize, chunkLookup);
                }
                sample.materialId = s0.phi < kIsoValue ? s0.materialId : s1.materialId;

                const std::array<CellCoord, 4> ring = incidentCells(axis, x, y, z);
                for (const CellCoord& c : ring) {
                    if (!isValidCell(c)) {
                        ++diagnostics.droppedOutOfDomainQuads;
                        goto next_crossing;
                    }
                    CellWork& cell = cells[flatCellIndex(c)];
                    cell.active = true;
                    cell.qef.addSample(sample);
                }
                crossings.push_back({axis, x, y, z, sample.normal});
            next_crossing:
                continue;
            }
        }
    }
}

MeshVertex solveCellVertex(const ChunkCoord& coord, const CellCoord& cellCoord, float voxelSize, const QefAccumulator& qef,
                          MeshPatchBatch::Diagnostics& diagnostics, QefReasonStats& reasonStats, VertexPlacementStats& placementStats) {
    MeshVertex vertex{};
    if (qef.sampleCount == 0U) {
        return vertex;
    }
    const float invMass = 1.0F / static_cast<float>(qef.sampleCount);
    const std::array<float, 3> massPoint{qef.massPoint[0] * invMass, qef.massPoint[1] * invMass, qef.massPoint[2] * invMass};

    std::array<std::array<float, 3>, 3> regularized = qef.ata;
    regularized[0][0] += kQefRegularization;
    regularized[1][1] += kQefRegularization;
    regularized[2][2] += kQefRegularization;

    const float d0 = std::abs(qef.ata[0][0]);
    const float d1 = std::abs(qef.ata[1][1]);
    const float d2 = std::abs(qef.ata[2][2]);
    const float maxDiag = std::max({d0, d1, d2});
    const float minDiag = std::min({d0, d1, d2});
    const bool illByMinDiag = minDiag < kQefConditionMinDiag;
    const bool illByRatio = (maxDiag / std::max(minDiag, kGradientEpsilon)) > kQefConditionRatioMax;
    const bool illConditioned = illByMinDiag || illByRatio;
    if (illConditioned) {
        ++diagnostics.illConditionedQef;
        if (illByMinDiag) {
            ++reasonStats.illByMinDiag;
        }
        if (illByRatio) {
            ++reasonStats.illByRatio;
        }
    }

    std::array<float, 3> solved{};
    const bool solveSucceeded = solveLinear3x3(regularized, qef.atb, solved);
    bool solvedOk = solveSucceeded;
    if (!solveSucceeded) {
        ++reasonStats.solveFailed;
    }
    if (solveSucceeded) {
        const float rx = (qef.ata[0][0] * solved[0]) + (qef.ata[0][1] * solved[1]) + (qef.ata[0][2] * solved[2]) - qef.atb[0];
        const float ry = (qef.ata[1][0] * solved[0]) + (qef.ata[1][1] * solved[1]) + (qef.ata[1][2] * solved[2]) - qef.atb[1];
        const float rz = (qef.ata[2][0] * solved[0]) + (qef.ata[2][1] * solved[1]) + (qef.ata[2][2] * solved[2]) - qef.atb[2];
        const float residual = std::sqrt((rx * rx) + (ry * ry) + (rz * rz));
        const float residualThreshold = kQefResidualThresholdScale * static_cast<float>(qef.sampleCount);
        const bool residualAccept = std::isfinite(residual) && residual <= residualThreshold;
        solvedOk = residualAccept;
        if (!residualAccept) {
            ++reasonStats.residualRejected;
        }
    }
    if (solvedOk) {
        const auto cellMin = latticePointWorld(coord, cellCoord.x, cellCoord.y, cellCoord.z, voxelSize);
        const auto cellMax = latticePointWorld(coord, cellCoord.x + 1, cellCoord.y + 1, cellCoord.z + 1, voxelSize);
        const auto minX = std::min(cellMin[0], cellMax[0]);
        const auto minY = std::min(cellMin[1], cellMax[1]);
        const auto minZ = std::min(cellMin[2], cellMax[2]);
        const auto maxX = std::max(cellMin[0], cellMax[0]);
        const auto maxY = std::max(cellMin[1], cellMax[1]);
        const auto maxZ = std::max(cellMin[2], cellMax[2]);
        const float outX = std::max({minX - solved[0], 0.0F, solved[0] - maxX});
        const float outY = std::max({minY - solved[1], 0.0F, solved[1] - maxY});
        const float outZ = std::max({minZ - solved[2], 0.0F, solved[2] - maxZ});
        const float outOfCellDistance = std::sqrt((outX * outX) + (outY * outY) + (outZ * outZ));
        if (outOfCellDistance > 0.0F) {
            ++placementStats.outOfCellVertices;
            placementStats.outOfCellDistanceAccum += outOfCellDistance;
            placementStats.maxOutOfCellDistance = std::max(placementStats.maxOutOfCellDistance, outOfCellDistance);
        }
        const float rejectScale = illConditioned ? kQefOutOfCellRejectScaleIllConditioned
                                                 : kQefOutOfCellRejectScaleWellConditioned;
        if (outOfCellDistance > (rejectScale * voxelSize)) {
            solvedOk = false;
            ++reasonStats.outOfCellRejected;
            if (illConditioned) {
                ++reasonStats.outOfCellRejectedIllConditioned;
            } else {
                ++reasonStats.outOfCellRejectedWellConditioned;
            }
            ++placementStats.rejectedOutOfCellVertices;
            placementStats.rejectedOutOfCellDistanceAccum += outOfCellDistance;
            placementStats.maxRejectedOutOfCellDistance = std::max(placementStats.maxRejectedOutOfCellDistance, outOfCellDistance);
        } else if (outOfCellDistance > 0.0F) {
            if (illConditioned) {
                ++placementStats.acceptedOutOfCellIllConditioned;
                placementStats.acceptedOutOfCellIllDistanceAccum += outOfCellDistance;
                placementStats.maxAcceptedOutOfCellIllDistance =
                    std::max(placementStats.maxAcceptedOutOfCellIllDistance, outOfCellDistance);
            } else {
                ++placementStats.acceptedOutOfCellWellConditioned;
                placementStats.acceptedOutOfCellWellDistanceAccum += outOfCellDistance;
                placementStats.maxAcceptedOutOfCellWellDistance =
                    std::max(placementStats.maxAcceptedOutOfCellWellDistance, outOfCellDistance);
                const float normalized = outOfCellDistance / std::max(voxelSize, kGradientEpsilon);
                if (normalized <= 0.25F) {
                    ++placementStats.acceptedOutWellLeQuarterVoxel;
                } else if (normalized <= 0.5F) {
                    ++placementStats.acceptedOutWellLeHalfVoxel;
                } else if (normalized <= 0.75F) {
                    ++placementStats.acceptedOutWellLeThreeQuarterVoxel;
                } else {
                    ++placementStats.acceptedOutWellLeFullVoxel;
                }
            }
            if (outOfCellDistance > (kQefAcceptedOutOfCellPullStartScale * voxelSize)) {
                const float denom = std::max((rejectScale - kQefAcceptedOutOfCellPullStartScale) * voxelSize, kGradientEpsilon);
                const float t = std::clamp((outOfCellDistance - (kQefAcceptedOutOfCellPullStartScale * voxelSize)) / denom, 0.0F, 1.0F);
                const float blend = kQefAcceptedOutOfCellMaxBlend * t;
                solved = add(scale(solved, 1.0F - blend), scale(massPoint, blend));
                ++placementStats.acceptedOutOfCellSoftPulledVertices;
                placementStats.acceptedOutOfCellSoftPullBlendAccum += blend;
                placementStats.maxAcceptedOutOfCellSoftPullBlend =
                    std::max(placementStats.maxAcceptedOutOfCellSoftPullBlend, blend);
            }
        }
    }
    if (!solvedOk) {
        solved = massPoint;
        ++diagnostics.qefFallbacks;
    }

    const std::array<float, 3> unclamped = solved;
    const auto cellMin = latticePointWorld(coord, cellCoord.x, cellCoord.y, cellCoord.z, voxelSize);
    const auto cellMax = latticePointWorld(coord, cellCoord.x + 1, cellCoord.y + 1, cellCoord.z + 1, voxelSize);
    solved[0] = std::clamp(solved[0], std::min(cellMin[0], cellMax[0]), std::max(cellMin[0], cellMax[0]));
    solved[1] = std::clamp(solved[1], std::min(cellMin[1], cellMax[1]), std::max(cellMin[1], cellMax[1]));
    solved[2] = std::clamp(solved[2], std::min(cellMin[2], cellMax[2]), std::max(cellMin[2], cellMax[2]));

    ++placementStats.solvedVertices;
    const float clampDx = solved[0] - unclamped[0];
    const float clampDy = solved[1] - unclamped[1];
    const float clampDz = solved[2] - unclamped[2];
    const float clampDisplacement = std::sqrt((clampDx * clampDx) + (clampDy * clampDy) + (clampDz * clampDz));
    placementStats.clampDisplacementAccum += clampDisplacement;
    placementStats.maxClampDisplacement = std::max(placementStats.maxClampDisplacement, clampDisplacement);
    if (clampDisplacement > (1.0e-6F * voxelSize)) {
        ++placementStats.clampedVertices;
    }
    const float boundaryPinEpsilon = 1.0e-5F * voxelSize;
    const bool pinnedX = (std::abs(solved[0] - cellMin[0]) <= boundaryPinEpsilon) || (std::abs(solved[0] - cellMax[0]) <= boundaryPinEpsilon);
    const bool pinnedY = (std::abs(solved[1] - cellMin[1]) <= boundaryPinEpsilon) || (std::abs(solved[1] - cellMax[1]) <= boundaryPinEpsilon);
    const bool pinnedZ = (std::abs(solved[2] - cellMin[2]) <= boundaryPinEpsilon) || (std::abs(solved[2] - cellMax[2]) <= boundaryPinEpsilon);
    if (pinnedX || pinnedY || pinnedZ) {
        ++placementStats.boundaryPinnedVertices;
    }
    const float massDx = solved[0] - massPoint[0];
    const float massDy = solved[1] - massPoint[1];
    const float massDz = solved[2] - massPoint[2];
    const float massDistance = std::sqrt((massDx * massDx) + (massDy * massDy) + (massDz * massDz));
    placementStats.massPointDistanceAccum += massDistance;
    placementStats.maxMassPointDistance = std::max(placementStats.maxMassPointDistance, massDistance);

    vertex.position = solved;
    const float inv = 1.0F / static_cast<float>(qef.sampleCount);
    vertex.normal = normalizeVec({qef.normalSum[0] * inv, qef.normalSum[1] * inv, qef.normalSum[2] * inv});
    vertex.materialId = qef.materialId;
    return vertex;
}

struct TriangleCandidate {
    uint32_t a = 0;
    uint32_t b = 0;
    uint32_t c = 0;
};

bool buildQuadCandidates(const std::array<uint32_t, 4>& quad,
                         const std::array<float, 3>& normalHint,
                         const std::vector<MeshVertex>& vertices,
                         std::array<TriangleCandidate, 2>& outTriangles,
                         float* outFaceHintDot,
                         bool* outAligned,
                         bool* outUsedAlternateDiagonal) {
    if (quad[0] == quad[1] || quad[1] == quad[2] || quad[2] == quad[3] || quad[3] == quad[0] || quad[0] == quad[2] || quad[1] == quad[3]) {
        return false;
    }
    const auto& v0 = vertices[quad[0]].position;
    const auto& v1 = vertices[quad[1]].position;
    const auto& v2 = vertices[quad[2]].position;
    const auto& v3 = vertices[quad[3]].position;
    const auto faceA0 = cross(sub(v1, v0), sub(v2, v0));
    const auto faceA1 = cross(sub(v2, v0), sub(v3, v0));
    const float agreeA = dot(faceA0, faceA1);
    const float minAreaA = std::min(length(faceA0), length(faceA1));

    const auto faceB0 = cross(sub(v2, v1), sub(v3, v1));
    const auto faceB1 = cross(sub(v3, v1), sub(v0, v1));
    const float agreeB = dot(faceB0, faceB1);
    const float minAreaB = std::min(length(faceB0), length(faceB1));
    const float worstAspectA =
        std::max(triangleAspectRatioFromPositions(v0, v1, v2), triangleAspectRatioFromPositions(v0, v2, v3));
    const float worstAspectB =
        std::max(triangleAspectRatioFromPositions(v1, v2, v3), triangleAspectRatioFromPositions(v1, v3, v0));

    bool useAlt = false;
    const bool consistentA = agreeA >= -kGradientEpsilon;
    const bool consistentB = agreeB >= -kGradientEpsilon;
    constexpr float kAspectPreferenceDelta = 0.25F;
    if (consistentA != consistentB) {
        useAlt = consistentB;
    } else if (worstAspectB + kAspectPreferenceDelta < worstAspectA) {
        useAlt = true;
    } else if (worstAspectA + kAspectPreferenceDelta < worstAspectB) {
        useAlt = false;
    } else if (agreeB > agreeA + kGradientEpsilon) {
        useAlt = true;
    } else if (std::abs(agreeB - agreeA) <= kGradientEpsilon && minAreaB > minAreaA + kGradientEpsilon) {
        useAlt = true;
    }
    if (outUsedAlternateDiagonal != nullptr) {
        *outUsedAlternateDiagonal = useAlt;
    }

    const auto face = useAlt ? add(faceB0, faceB1) : add(faceA0, faceA1);
    const float faceHintDot = dot(face, normalHint);
    const bool aligned = faceHintDot >= 0.0F;
    if (outFaceHintDot != nullptr) {
        *outFaceHintDot = faceHintDot;
    }
    if (outAligned != nullptr) {
        *outAligned = aligned;
    }
    if (aligned) {
        if (!useAlt) {
            outTriangles[0] = TriangleCandidate{quad[0], quad[1], quad[2]};
            outTriangles[1] = TriangleCandidate{quad[0], quad[2], quad[3]};
        } else {
            outTriangles[0] = TriangleCandidate{quad[1], quad[2], quad[3]};
            outTriangles[1] = TriangleCandidate{quad[1], quad[3], quad[0]};
        }
        return true;
    }
    if (!useAlt) {
        outTriangles[0] = TriangleCandidate{quad[0], quad[3], quad[2]};
        outTriangles[1] = TriangleCandidate{quad[0], quad[2], quad[1]};
    } else {
        outTriangles[0] = TriangleCandidate{quad[1], quad[0], quad[3]};
        outTriangles[1] = TriangleCandidate{quad[1], quad[3], quad[2]};
    }
    return true;
}

template <typename ChunkLookup>
void meshSingleChunk(const ChunkCoord& coord,
                     float voxelSize,
                     const ChunkLookup& chunkLookup,
                     MeshBuffers& output,
                     MeshPatchBatch::Diagnostics& diagnostics,
                     bool* outForceTwoSidedFallback) {
    const std::size_t chunkVertexStart = output.vertices.size();
    const std::size_t chunkIndexStart = output.indices.size();
    std::vector<CellWork> cells(static_cast<std::size_t>(kCellsPerAxis * kCellsPerAxis * kCellsPerAxis));
    std::vector<CrossingEdge> crossings;
    crossings.reserve(4096);
    uint32_t alignedQuads = 0;
    uint32_t flippedQuads = 0;
    uint32_t selectedAlternateDiagonalQuads = 0;
    uint32_t emittedQuads = 0;
    uint32_t twistedCurrentDiagonalQuads = 0;
    uint32_t twistedAlternateDiagonalQuads = 0;
    uint32_t alternateDiagonalHigherAreaQuads = 0;
    uint32_t activeCellCount = 0;
    uint32_t cellsWithOneSample = 0;
    uint32_t cellsWithTwoSamples = 0;
    uint32_t cellsWithThreeOrMoreSamples = 0;
    const uint32_t diagnosticsIllStart = diagnostics.illConditionedQef;
    const uint32_t diagnosticsFallbackStart = diagnostics.qefFallbacks;
    QefReasonStats qefReasonStats{};
    VertexPlacementStats placementStats{};
    uint32_t orientationComponentCount = 0;
    uint32_t centroidHeuristicFlipCount = 0;
    uint32_t centroidHeuristicWeakSignalCount = 0;
    float minCentroidHeuristicSignal = std::numeric_limits<float>::max();
    float maxCentroidHeuristicSignal = std::numeric_limits<float>::lowest();
    float minFaceHintDot = std::numeric_limits<float>::max();
    float maxFaceHintDot = std::numeric_limits<float>::lowest();

    accumulateEdgeCrossingsForAxis(Axis::X, coord, voxelSize, chunkLookup, cells, crossings, diagnostics);
    accumulateEdgeCrossingsForAxis(Axis::Y, coord, voxelSize, chunkLookup, cells, crossings, diagnostics);
    accumulateEdgeCrossingsForAxis(Axis::Z, coord, voxelSize, chunkLookup, cells, crossings, diagnostics);

    for (int z = 0; z < kCellsPerAxis; ++z) {
        for (int y = 0; y < kCellsPerAxis; ++y) {
            for (int x = 0; x < kCellsPerAxis; ++x) {
                const CellCoord cellCoord{x, y, z};
                CellWork& cell = cells[flatCellIndex(cellCoord)];
                if (!cell.active || cell.qef.sampleCount == 0U) {
                    continue;
                }
                ++activeCellCount;
                if (cell.qef.sampleCount == 1U) {
                    ++cellsWithOneSample;
                } else if (cell.qef.sampleCount == 2U) {
                    ++cellsWithTwoSamples;
                } else {
                    ++cellsWithThreeOrMoreSamples;
                }
                MeshVertex vertex = solveCellVertex(coord, cellCoord, voxelSize, cell.qef, diagnostics, qefReasonStats, placementStats);
                cell.vertexIndex = static_cast<uint32_t>(output.vertices.size());
                output.vertices.push_back(vertex);
            }
        }
    }

    std::vector<TriangleCandidate> chunkTriangles;
    chunkTriangles.reserve(crossings.size() * 2U);
    for (const CrossingEdge& crossing : crossings) {
        const std::array<CellCoord, 4> ring = incidentCells(crossing.axis, crossing.x, crossing.y, crossing.z);
        std::array<uint32_t, 4> quad{};
        bool complete = true;
        for (std::size_t i = 0; i < ring.size(); ++i) {
            if (!isValidCell(ring[i])) {
                complete = false;
                break;
            }
            const uint32_t idx = cells[flatCellIndex(ring[i])].vertexIndex;
            if (idx == kInvalidVertexIndex) {
                complete = false;
                break;
            }
            quad[i] = idx;
        }
        if (!complete) {
            ++diagnostics.droppedIncompleteQuads;
            continue;
        }
        const auto& v0 = output.vertices[quad[0]].position;
        const auto& v1 = output.vertices[quad[1]].position;
        const auto& v2 = output.vertices[quad[2]].position;
        const auto& v3 = output.vertices[quad[3]].position;
        const auto nA = cross(sub(v1, v0), sub(v2, v0));
        const auto nB = cross(sub(v2, v0), sub(v3, v0));
        const auto mA = cross(sub(v2, v1), sub(v3, v1));
        const auto mB = cross(sub(v3, v1), sub(v0, v1));
        if (dot(nA, nB) < 0.0F) {
            ++twistedCurrentDiagonalQuads;
        }
        if (dot(mA, mB) < 0.0F) {
            ++twistedAlternateDiagonalQuads;
        }
        const float currentDiagonalArea =
            triangleAreaFromPositions(v0, v1, v2) + triangleAreaFromPositions(v0, v2, v3);
        const float alternateDiagonalArea =
            triangleAreaFromPositions(v1, v2, v3) + triangleAreaFromPositions(v1, v3, v0);
        if (alternateDiagonalArea > currentDiagonalArea + (1.0e-6F * voxelSize * voxelSize)) {
            ++alternateDiagonalHigherAreaQuads;
        }
        float faceHintDot = 0.0F;
        bool aligned = false;
        bool usedAlternateDiagonal = false;
        std::array<TriangleCandidate, 2> quadTriangles{};
        if (!buildQuadCandidates(
                quad, crossing.normal, output.vertices, quadTriangles, &faceHintDot, &aligned, &usedAlternateDiagonal)) {
            ++diagnostics.droppedDuplicateQuads;
        } else {
            chunkTriangles.push_back(quadTriangles[0]);
            chunkTriangles.push_back(quadTriangles[1]);
            ++emittedQuads;
            if (usedAlternateDiagonal) {
                ++selectedAlternateDiagonalQuads;
            }
            if (aligned) {
                ++alignedQuads;
            } else {
                ++flippedQuads;
            }
            minFaceHintDot = std::min(minFaceHintDot, faceHintDot);
            maxFaceHintDot = std::max(maxFaceHintDot, faceHintDot);
        }
    }

    if (outForceTwoSidedFallback != nullptr) {
        *outForceTwoSidedFallback = false;
    }

    struct AdjacencyEdgeRef {
        uint32_t triIndex = 0;
        uint32_t from = 0;
        uint32_t to = 0;
    };
    struct TriNeighbor {
        uint32_t triIndex = 0;
        bool requiresOppositeFlip = false;
    };
    const auto edgeKey = [](uint32_t a, uint32_t b) -> uint64_t {
        const uint32_t lo = std::min(a, b);
        const uint32_t hi = std::max(a, b);
        return (static_cast<uint64_t>(lo) << 32U) | static_cast<uint64_t>(hi);
    };
    std::unordered_map<uint64_t, std::vector<AdjacencyEdgeRef>> adjacencyByEdge;
    adjacencyByEdge.reserve(chunkTriangles.size() * 3U);
    for (uint32_t triIndex = 0; triIndex < static_cast<uint32_t>(chunkTriangles.size()); ++triIndex) {
        const TriangleCandidate& tri = chunkTriangles[triIndex];
        adjacencyByEdge[edgeKey(tri.a, tri.b)].push_back({triIndex, tri.a, tri.b});
        adjacencyByEdge[edgeKey(tri.b, tri.c)].push_back({triIndex, tri.b, tri.c});
        adjacencyByEdge[edgeKey(tri.c, tri.a)].push_back({triIndex, tri.c, tri.a});
    }
    std::vector<std::vector<TriNeighbor>> triNeighbors(chunkTriangles.size());
    for (const auto& [unusedEdge, refs] : adjacencyByEdge) {
        (void)unusedEdge;
        if (refs.size() < 2U) {
            continue;
        }
        for (std::size_t i = 0; i < refs.size(); ++i) {
            for (std::size_t j = i + 1; j < refs.size(); ++j) {
                const AdjacencyEdgeRef& a = refs[i];
                const AdjacencyEdgeRef& b = refs[j];
                const bool sameDirection = a.from == b.from && a.to == b.to;
                triNeighbors[a.triIndex].push_back({b.triIndex, sameDirection});
                triNeighbors[b.triIndex].push_back({a.triIndex, sameDirection});
            }
        }
    }
    for (std::vector<TriNeighbor>& neighbors : triNeighbors) {
        std::sort(neighbors.begin(),
                  neighbors.end(),
                  [](const TriNeighbor& a, const TriNeighbor& b) { return a.triIndex < b.triIndex; });
    }

    std::array<float, 3> vertexCentroid{0.0F, 0.0F, 0.0F};
    const std::size_t chunkVertexCount = output.vertices.size() - chunkVertexStart;
    for (std::size_t i = chunkVertexStart; i < output.vertices.size(); ++i) {
        const MeshVertex& v = output.vertices[i];
        vertexCentroid[0] += v.position[0];
        vertexCentroid[1] += v.position[1];
        vertexCentroid[2] += v.position[2];
    }
    if (chunkVertexCount > 0U) {
        const float inv = 1.0F / static_cast<float>(chunkVertexCount);
        vertexCentroid[0] *= inv;
        vertexCentroid[1] *= inv;
        vertexCentroid[2] *= inv;
    }

    std::vector<int8_t> triFlipState(chunkTriangles.size(), -1);
    std::vector<uint32_t> bfsQueue;
    bfsQueue.reserve(chunkTriangles.size());
    for (uint32_t seed = 0; seed < static_cast<uint32_t>(chunkTriangles.size()); ++seed) {
        if (triFlipState[seed] != -1) {
            continue;
        }
        ++orientationComponentCount;
        bfsQueue.clear();
        std::vector<uint32_t> component;
        triFlipState[seed] = 0;
        bfsQueue.push_back(seed);
        component.push_back(seed);
        for (std::size_t q = 0; q < bfsQueue.size(); ++q) {
            const uint32_t triIndex = bfsQueue[q];
            const int8_t flip = triFlipState[triIndex];
            for (const TriNeighbor& neighbor : triNeighbors[triIndex]) {
                const int8_t required = static_cast<int8_t>(flip ^ (neighbor.requiresOppositeFlip ? 1 : 0));
                if (triFlipState[neighbor.triIndex] == -1) {
                    triFlipState[neighbor.triIndex] = required;
                    bfsQueue.push_back(neighbor.triIndex);
                    component.push_back(neighbor.triIndex);
                }
            }
        }

        std::array<float, 3> componentNormalSum{0.0F, 0.0F, 0.0F};
        std::array<float, 3> componentCentroid{0.0F, 0.0F, 0.0F};
        for (uint32_t triIndex : component) {
            TriangleCandidate tri = chunkTriangles[triIndex];
            if (triFlipState[triIndex] == 1) {
                std::swap(tri.b, tri.c);
            }
            const auto& va = output.vertices[tri.a].position;
            const auto& vb = output.vertices[tri.b].position;
            const auto& vc = output.vertices[tri.c].position;
            componentNormalSum = add(componentNormalSum, cross(sub(vb, va), sub(vc, va)));
            componentCentroid = add(componentCentroid, scale(add(add(va, vb), vc), 1.0F / 3.0F));
        }
        if (!component.empty()) {
            const float inv = 1.0F / static_cast<float>(component.size());
            componentCentroid = scale(componentCentroid, inv);
            const std::array<float, 3> radial = sub(componentCentroid, vertexCentroid);
            const float orientationSignal = dot(componentNormalSum, radial);
            minCentroidHeuristicSignal = std::min(minCentroidHeuristicSignal, orientationSignal);
            maxCentroidHeuristicSignal = std::max(maxCentroidHeuristicSignal, orientationSignal);
            if (std::abs(orientationSignal) <= (1.0e-8F * voxelSize * voxelSize)) {
                ++centroidHeuristicWeakSignalCount;
            }
            if (orientationSignal < 0.0F) {
                ++centroidHeuristicFlipCount;
                for (uint32_t triIndex : component) {
                    triFlipState[triIndex] ^= 1;
                }
            }
        }
    }
    for (uint32_t triIndex = 0; triIndex < static_cast<uint32_t>(chunkTriangles.size()); ++triIndex) {
        if (triFlipState[triIndex] == 1) {
            std::swap(chunkTriangles[triIndex].b, chunkTriangles[triIndex].c);
            ++diagnostics.componentOrientationFlips;
        }
    }

    const float cleanupAreaThreshold = 1.0e-12F * voxelSize * voxelSize;
    uint32_t localDroppedInvalidTriangles = 0;
    uint32_t localDroppedDegenerateTriangles = 0;
    uint32_t localDroppedDuplicateTriangles = 0;
    uint32_t trianglesFlippedToMatchVertexNormals = 0;
    uint32_t trianglesFlippedToMatchRadialOrientation = 0;
    std::unordered_map<uint64_t, uint8_t> seenTriangles;
    seenTriangles.reserve(chunkTriangles.size());
    std::vector<TriangleCandidate> cleanedTriangles;
    cleanedTriangles.reserve(chunkTriangles.size());
    for (const TriangleCandidate& tri : chunkTriangles) {
        if (tri.a >= output.vertices.size() || tri.b >= output.vertices.size() || tri.c >= output.vertices.size() ||
            tri.a == tri.b || tri.b == tri.c || tri.c == tri.a) {
            ++diagnostics.droppedInvalidTriangles;
            ++localDroppedInvalidTriangles;
            continue;
        }
        const float area =
            triangleAreaFromPositions(output.vertices[tri.a].position, output.vertices[tri.b].position, output.vertices[tri.c].position);
        if (!std::isfinite(area) || area < cleanupAreaThreshold) {
            ++diagnostics.droppedDegenerateTriangles;
            ++localDroppedDegenerateTriangles;
            continue;
        }
        std::array<uint32_t, 3> sorted = {tri.a, tri.b, tri.c};
        std::sort(sorted.begin(), sorted.end());
        const uint64_t triKey = (static_cast<uint64_t>(sorted[0]) << 42U) ^
                                (static_cast<uint64_t>(sorted[1]) << 21U) ^
                                static_cast<uint64_t>(sorted[2]);
        if (seenTriangles.contains(triKey)) {
            ++diagnostics.droppedDuplicateTriangles;
            ++localDroppedDuplicateTriangles;
            continue;
        }
        seenTriangles[triKey] = 1U;
        TriangleCandidate orientedTri = tri;
        const auto face = cross(sub(output.vertices[orientedTri.b].position, output.vertices[orientedTri.a].position),
                                sub(output.vertices[orientedTri.c].position, output.vertices[orientedTri.a].position));
        const float faceLen = length(face);
        if (faceLen > kGradientEpsilon) {
            const auto faceUnit = scale(face, 1.0F / faceLen);
            const auto na = normalizeVec(output.vertices[orientedTri.a].normal);
            const auto nb = normalizeVec(output.vertices[orientedTri.b].normal);
            const auto nc = normalizeVec(output.vertices[orientedTri.c].normal);
            const float avgVertexNormalDotFace = (dot(na, faceUnit) + dot(nb, faceUnit) + dot(nc, faceUnit)) / 3.0F;
            if (avgVertexNormalDotFace < 0.0F) {
                std::swap(orientedTri.b, orientedTri.c);
                ++trianglesFlippedToMatchVertexNormals;
            }
        }
        const auto ta = output.vertices[orientedTri.a].position;
        const auto tb = output.vertices[orientedTri.b].position;
        const auto tc = output.vertices[orientedTri.c].position;
        const auto triCenter = scale(add(add(ta, tb), tc), 1.0F / 3.0F);
        const auto radial = sub(triCenter, vertexCentroid);
        const auto orientedFace = cross(sub(tb, ta), sub(tc, ta));
        const float radialOrientationSignal = dot(orientedFace, radial);
        if (radialOrientationSignal < 0.0F) {
            std::swap(orientedTri.b, orientedTri.c);
            ++trianglesFlippedToMatchRadialOrientation;
        }
        cleanedTriangles.push_back(orientedTri);
    }
    output.indices.reserve(output.indices.size() + (cleanedTriangles.size() * 3U));
    for (const TriangleCandidate& tri : cleanedTriangles) {
        output.indices.push_back(tri.a);
        output.indices.push_back(tri.b);
        output.indices.push_back(tri.c);
    }
    uint32_t localRadialClampVertices = 0;
    float localRadialClampDisplacementAccum = 0.0F;
    float maxLocalRadialClampDisplacement = 0.0F;
    if (chunkVertexCount > 0U) {
        std::vector<std::vector<uint32_t>> vertexNeighbors(chunkVertexCount);
        auto addNeighbor = [&](uint32_t a, uint32_t b) {
            if (a < chunkVertexStart || b < chunkVertexStart) {
                return;
            }
            const uint32_t la = a - static_cast<uint32_t>(chunkVertexStart);
            const uint32_t lb = b - static_cast<uint32_t>(chunkVertexStart);
            if (la >= chunkVertexCount || lb >= chunkVertexCount || la == lb) {
                return;
            }
            vertexNeighbors[la].push_back(lb);
            vertexNeighbors[lb].push_back(la);
        };
        for (std::size_t i = chunkIndexStart; i + 2 < output.indices.size(); i += 3) {
            const uint32_t ia = output.indices[i];
            const uint32_t ib = output.indices[i + 1];
            const uint32_t ic = output.indices[i + 2];
            addNeighbor(ia, ib);
            addNeighbor(ib, ic);
            addNeighbor(ic, ia);
        }
        std::vector<std::array<float, 3>> snapshotPositions(chunkVertexCount);
        for (uint32_t li = 0; li < static_cast<uint32_t>(chunkVertexCount); ++li) {
            snapshotPositions[li] = output.vertices[chunkVertexStart + li].position;
        }
        const float chunkMinXClamp = static_cast<float>(coord.x * kChunkEdge) * voxelSize;
        const float chunkMinYClamp = static_cast<float>(coord.y * kChunkEdge) * voxelSize;
        const float chunkMinZClamp = static_cast<float>(coord.z * kChunkEdge) * voxelSize;
        const float chunkMaxXClamp = chunkMinXClamp + (static_cast<float>(kChunkEdge) * voxelSize);
        const float chunkMaxYClamp = chunkMinYClamp + (static_cast<float>(kChunkEdge) * voxelSize);
        const float chunkMaxZClamp = chunkMinZClamp + (static_cast<float>(kChunkEdge) * voxelSize);
        const float seamGuardEpsilon = 1.25F * voxelSize;
        const float protrusionThreshold = kLocalRadialProtrusionClampScale * voxelSize;
        const float protrusionTarget = kLocalRadialProtrusionTargetScale * voxelSize;
        for (uint32_t li = 0; li < static_cast<uint32_t>(chunkVertexCount); ++li) {
            auto& nbrs = vertexNeighbors[li];
            if (nbrs.size() < 3U) {
                continue;
            }
            std::sort(nbrs.begin(), nbrs.end());
            nbrs.erase(std::unique(nbrs.begin(), nbrs.end()), nbrs.end());
            std::array<float, 3> neighborMean{0.0F, 0.0F, 0.0F};
            for (const uint32_t ln : nbrs) {
                neighborMean = add(neighborMean, snapshotPositions[ln]);
            }
            neighborMean = scale(neighborMean, 1.0F / static_cast<float>(nbrs.size()));
            const auto selfPos = snapshotPositions[li];
            const bool seamAdjacent =
                (std::abs(selfPos[0] - chunkMinXClamp) <= seamGuardEpsilon) ||
                (std::abs(selfPos[1] - chunkMinYClamp) <= seamGuardEpsilon) ||
                (std::abs(selfPos[2] - chunkMinZClamp) <= seamGuardEpsilon) ||
                (std::abs(selfPos[0] - chunkMaxXClamp) <= seamGuardEpsilon) ||
                (std::abs(selfPos[1] - chunkMaxYClamp) <= seamGuardEpsilon) ||
                (std::abs(selfPos[2] - chunkMaxZClamp) <= seamGuardEpsilon);
            if (seamAdjacent) {
                continue;
            }
            const auto delta = sub(selfPos, neighborMean);
            const auto radial = sub(selfPos, vertexCentroid);
            const float radialLen = length(radial);
            if (radialLen <= kGradientEpsilon) {
                continue;
            }
            const auto radialDir = scale(radial, 1.0F / radialLen);
            const float radialDeviation = dot(delta, radialDir);
            if (radialDeviation <= protrusionThreshold) {
                continue;
            }
            const float excess = radialDeviation - protrusionTarget;
            const float pullDistance = std::max(0.0F, excess * kLocalRadialProtrusionClampBlend);
            const auto newPos = add(selfPos, scale(radialDir, -pullDistance));
            const float displacement = length(sub(newPos, selfPos));
            output.vertices[chunkVertexStart + li].position = newPos;
            ++localRadialClampVertices;
            localRadialClampDisplacementAccum += displacement;
            maxLocalRadialClampDisplacement = std::max(maxLocalRadialClampDisplacement, displacement);
        }
    }
    uint32_t recomputedVertexNormals = 0;
    uint32_t unresolvedVertexNormals = 0;
    uint32_t normalContributionFaces = 0;
    if (chunkVertexCount > 0U) {
        std::vector<std::array<float, 3>> normalAccum(chunkVertexCount, {0.0F, 0.0F, 0.0F});
        for (std::size_t i = chunkIndexStart; i + 2 < output.indices.size(); i += 3) {
            const uint32_t ia = output.indices[i];
            const uint32_t ib = output.indices[i + 1];
            const uint32_t ic = output.indices[i + 2];
            if (ia < chunkVertexStart || ib < chunkVertexStart || ic < chunkVertexStart) {
                continue;
            }
            const uint32_t la = ia - static_cast<uint32_t>(chunkVertexStart);
            const uint32_t lb = ib - static_cast<uint32_t>(chunkVertexStart);
            const uint32_t lc = ic - static_cast<uint32_t>(chunkVertexStart);
            if (la >= chunkVertexCount || lb >= chunkVertexCount || lc >= chunkVertexCount) {
                continue;
            }
            const auto& pa = output.vertices[ia].position;
            const auto& pb = output.vertices[ib].position;
            const auto& pc = output.vertices[ic].position;
            const auto face = cross(sub(pb, pa), sub(pc, pa));
            if (length(face) <= kGradientEpsilon) {
                continue;
            }
            normalAccum[la] = add(normalAccum[la], face);
            normalAccum[lb] = add(normalAccum[lb], face);
            normalAccum[lc] = add(normalAccum[lc], face);
            ++normalContributionFaces;
        }
        for (uint32_t li = 0; li < static_cast<uint32_t>(chunkVertexCount); ++li) {
            const float len = length(normalAccum[li]);
            if (len > kGradientEpsilon) {
                output.vertices[chunkVertexStart + li].normal = scale(normalAccum[li], 1.0F / len);
                ++recomputedVertexNormals;
            } else {
                ++unresolvedVertexNormals;
            }
        }
    }

    uint32_t degenerateTriangles = 0;
    float minTriangleArea = std::numeric_limits<float>::max();
    float maxTriangleArea = 0.0F;
    uint32_t outwardOrientedTriangles = 0;
    uint32_t inwardOrientedTriangles = 0;
    uint32_t lightFacingPositiveDirTriangles = 0;
    uint32_t lightFacingNegativeDirTriangles = 0;
    float avgLightDotPositiveDirAccum = 0.0F;
    float avgLightDotNegativeDirAccum = 0.0F;
    uint32_t lightDotSampleCount = 0;
    uint32_t invertedVertexNormalTriangles = 0;
    float minVertexNormalFaceDot = 1.0F;
    float avgVertexNormalFaceDotAccum = 0.0F;
    uint32_t avgVertexNormalFaceDotCount = 0;
    uint32_t skinnyTriangles = 0;
    uint32_t skinnyBoundaryTriangles = 0;
    uint32_t skinnyInvertedNormalTriangles = 0;
    float avgTriangleAspectRatioAccum = 0.0F;
    float maxTriangleAspectRatio = 0.0F;
    uint32_t boundaryEdgeCount = 0;
    uint32_t nonManifoldEdgeCount = 0;
    uint32_t boundaryVertexCount = 0;
    uint32_t nonFiniteVertexPositionCount = 0;
    uint32_t nonFiniteVertexNormalCount = 0;
    uint32_t nearZeroVertexNormalCount = 0;
    std::vector<float> radialDistances;
    radialDistances.reserve(chunkVertexCount);
    const float chunkMinX = static_cast<float>(coord.x * kChunkEdge) * voxelSize;
    const float chunkMinY = static_cast<float>(coord.y * kChunkEdge) * voxelSize;
    const float chunkMinZ = static_cast<float>(coord.z * kChunkEdge) * voxelSize;
    const float chunkMaxX = chunkMinX + (static_cast<float>(kChunkEdge) * voxelSize);
    const float chunkMaxY = chunkMinY + (static_cast<float>(kChunkEdge) * voxelSize);
    const float chunkMaxZ = chunkMinZ + (static_cast<float>(kChunkEdge) * voxelSize);
    const float boundaryEpsilon = 1.5F * voxelSize;
    for (std::size_t i = chunkVertexStart; i < output.vertices.size(); ++i) {
        const MeshVertex& v = output.vertices[i];
        const auto radial = sub(v.position, vertexCentroid);
        radialDistances.push_back(length(radial));
        if (!std::isfinite(v.position[0]) || !std::isfinite(v.position[1]) || !std::isfinite(v.position[2])) {
            ++nonFiniteVertexPositionCount;
        }
        if (!std::isfinite(v.normal[0]) || !std::isfinite(v.normal[1]) || !std::isfinite(v.normal[2])) {
            ++nonFiniteVertexNormalCount;
        }
        if (length(v.normal) <= (1.0e-6F * voxelSize)) {
            ++nearZeroVertexNormalCount;
        }
        const bool nearBoundary =
            (std::abs(v.position[0] - chunkMinX) <= boundaryEpsilon) ||
            (std::abs(v.position[1] - chunkMinY) <= boundaryEpsilon) ||
            (std::abs(v.position[2] - chunkMinZ) <= boundaryEpsilon) ||
            (std::abs(v.position[0] - chunkMaxX) <= boundaryEpsilon) ||
            (std::abs(v.position[1] - chunkMaxY) <= boundaryEpsilon) ||
            (std::abs(v.position[2] - chunkMaxZ) <= boundaryEpsilon);
        if (nearBoundary) {
            ++boundaryVertexCount;
        }
    }
    for (std::size_t i = chunkIndexStart; i + 2 < output.indices.size(); i += 3) {
        const uint32_t ia = output.indices[i];
        const uint32_t ib = output.indices[i + 1];
        const uint32_t ic = output.indices[i + 2];
        if (ia >= output.vertices.size() || ib >= output.vertices.size() || ic >= output.vertices.size()) {
            continue;
        }
        const auto ab = sub(output.vertices[ib].position, output.vertices[ia].position);
        const auto ac = sub(output.vertices[ic].position, output.vertices[ia].position);
        const auto face = cross(ab, ac);
        const float area = 0.5F * length(face);
        minTriangleArea = std::min(minTriangleArea, area);
        maxTriangleArea = std::max(maxTriangleArea, area);
        if (area < (1.0e-12F * voxelSize * voxelSize)) {
            ++degenerateTriangles;
        }
        const auto triCenter = scale(add(add(output.vertices[ia].position, output.vertices[ib].position), output.vertices[ic].position), 1.0F / 3.0F);
        const auto radial = sub(triCenter, vertexCentroid);
        const float orient = dot(face, radial);
        if (orient >= 0.0F) {
            ++outwardOrientedTriangles;
        } else {
            ++inwardOrientedTriangles;
        }
        const float faceLen = length(face);
        const float edgeAB = length(ab);
        const float edgeAC = length(ac);
        const auto bc = sub(output.vertices[ic].position, output.vertices[ib].position);
        const float edgeBC = length(bc);
        const float maxEdge = std::max({edgeAB, edgeAC, edgeBC});
        const float minEdge = std::max(std::min({edgeAB, edgeAC, edgeBC}), kGradientEpsilon);
        const float aspectRatio = maxEdge / minEdge;
        avgTriangleAspectRatioAccum += aspectRatio;
        maxTriangleAspectRatio = std::max(maxTriangleAspectRatio, aspectRatio);
        if (aspectRatio > 8.0F) {
            ++skinnyTriangles;
            const auto& vaPos = output.vertices[ia].position;
            const auto& vbPos = output.vertices[ib].position;
            const auto& vcPos = output.vertices[ic].position;
            const bool nearChunkBoundary =
                (std::abs(vaPos[0] - chunkMinX) <= boundaryEpsilon) || (std::abs(vaPos[1] - chunkMinY) <= boundaryEpsilon) ||
                (std::abs(vaPos[2] - chunkMinZ) <= boundaryEpsilon) || (std::abs(vaPos[0] - chunkMaxX) <= boundaryEpsilon) ||
                (std::abs(vaPos[1] - chunkMaxY) <= boundaryEpsilon) || (std::abs(vaPos[2] - chunkMaxZ) <= boundaryEpsilon) ||
                (std::abs(vbPos[0] - chunkMinX) <= boundaryEpsilon) || (std::abs(vbPos[1] - chunkMinY) <= boundaryEpsilon) ||
                (std::abs(vbPos[2] - chunkMinZ) <= boundaryEpsilon) || (std::abs(vbPos[0] - chunkMaxX) <= boundaryEpsilon) ||
                (std::abs(vbPos[1] - chunkMaxY) <= boundaryEpsilon) || (std::abs(vbPos[2] - chunkMaxZ) <= boundaryEpsilon) ||
                (std::abs(vcPos[0] - chunkMinX) <= boundaryEpsilon) || (std::abs(vcPos[1] - chunkMinY) <= boundaryEpsilon) ||
                (std::abs(vcPos[2] - chunkMinZ) <= boundaryEpsilon) || (std::abs(vcPos[0] - chunkMaxX) <= boundaryEpsilon) ||
                (std::abs(vcPos[1] - chunkMaxY) <= boundaryEpsilon) || (std::abs(vcPos[2] - chunkMaxZ) <= boundaryEpsilon);
            if (nearChunkBoundary) {
                ++skinnyBoundaryTriangles;
            }
        }
        if (faceLen > kGradientEpsilon) {
            const auto faceUnit = scale(face, 1.0F / faceLen);
            const std::array<float, 3> defaultLightDir{0.45F, 0.8F, 0.35F};
            const auto lightDir = normalizeVec(defaultLightDir);
            const float positiveDot = dot(faceUnit, lightDir);
            const float negativeDot = dot(faceUnit, scale(lightDir, -1.0F));
            if (positiveDot > 0.0F) {
                ++lightFacingPositiveDirTriangles;
            }
            if (negativeDot > 0.0F) {
                ++lightFacingNegativeDirTriangles;
            }
            avgLightDotPositiveDirAccum += positiveDot;
            avgLightDotNegativeDirAccum += negativeDot;
            ++lightDotSampleCount;
            const auto na = normalizeVec(output.vertices[ia].normal);
            const auto nb = normalizeVec(output.vertices[ib].normal);
            const auto nc = normalizeVec(output.vertices[ic].normal);
            const float da = dot(na, faceUnit);
            const float db = dot(nb, faceUnit);
            const float dc = dot(nc, faceUnit);
            const float minDot = std::min(da, std::min(db, dc));
            const float avgDot = (da + db + dc) / 3.0F;
            minVertexNormalFaceDot = std::min(minVertexNormalFaceDot, minDot);
            avgVertexNormalFaceDotAccum += avgDot;
            ++avgVertexNormalFaceDotCount;
            if (minDot < 0.0F) {
                ++invertedVertexNormalTriangles;
                if (aspectRatio > 8.0F) {
                    ++skinnyInvertedNormalTriangles;
                }
            }
        }
    }
    std::unordered_map<uint64_t, uint32_t> edgeUseCount;
    edgeUseCount.reserve((output.indices.size() - chunkIndexStart) * 2U);
    const auto addEdge = [&edgeUseCount](uint32_t a, uint32_t b) {
        const uint32_t lo = std::min(a, b);
        const uint32_t hi = std::max(a, b);
        const uint64_t key = (static_cast<uint64_t>(lo) << 32U) | static_cast<uint64_t>(hi);
        ++edgeUseCount[key];
    };
    for (std::size_t i = chunkIndexStart; i + 2 < output.indices.size(); i += 3) {
        const uint32_t ia = output.indices[i];
        const uint32_t ib = output.indices[i + 1];
        const uint32_t ic = output.indices[i + 2];
        if (ia >= output.vertices.size() || ib >= output.vertices.size() || ic >= output.vertices.size()) {
            continue;
        }
        addEdge(ia, ib);
        addEdge(ib, ic);
        addEdge(ic, ia);
    }
    for (const auto& [edgeKey, count] : edgeUseCount) {
        (void)edgeKey;
        if (count == 1U) {
            ++boundaryEdgeCount;
        } else if (count > 2U) {
            ++nonManifoldEdgeCount;
        }
    }
    float localDeviationMean = 0.0F;
    float localDeviationMax = 0.0F;
    uint32_t localDeviationSamples = 0;
    uint32_t localDeviationGtQuarterVoxel = 0;
    uint32_t localDeviationGtHalfVoxel = 0;
    uint32_t localDeviationGtThreeQuarterVoxel = 0;
    uint32_t localRadialDeviationGtQuarterVoxel = 0;
    uint32_t localRadialDeviationGtHalfVoxel = 0;
    uint32_t localRadialDeviationGtThreeQuarterVoxel = 0;
    float localRadialDeviationMax = 0.0F;
    if (chunkVertexCount > 0U) {
        std::vector<std::vector<uint32_t>> vertexNeighbors(chunkVertexCount);
        auto addNeighbor = [&](uint32_t a, uint32_t b) {
            if (a < chunkVertexStart || b < chunkVertexStart) {
                return;
            }
            const uint32_t la = a - static_cast<uint32_t>(chunkVertexStart);
            const uint32_t lb = b - static_cast<uint32_t>(chunkVertexStart);
            if (la >= chunkVertexCount || lb >= chunkVertexCount || la == lb) {
                return;
            }
            vertexNeighbors[la].push_back(lb);
            vertexNeighbors[lb].push_back(la);
        };
        for (std::size_t i = chunkIndexStart; i + 2 < output.indices.size(); i += 3) {
            const uint32_t ia = output.indices[i];
            const uint32_t ib = output.indices[i + 1];
            const uint32_t ic = output.indices[i + 2];
            addNeighbor(ia, ib);
            addNeighbor(ib, ic);
            addNeighbor(ic, ia);
        }
        for (uint32_t li = 0; li < static_cast<uint32_t>(chunkVertexCount); ++li) {
            auto& nbrs = vertexNeighbors[li];
            if (nbrs.empty()) {
                continue;
            }
            std::sort(nbrs.begin(), nbrs.end());
            nbrs.erase(std::unique(nbrs.begin(), nbrs.end()), nbrs.end());
            std::array<float, 3> neighborMean{0.0F, 0.0F, 0.0F};
            for (const uint32_t ln : nbrs) {
                const auto& p = output.vertices[chunkVertexStart + ln].position;
                neighborMean = add(neighborMean, p);
            }
            neighborMean = scale(neighborMean, 1.0F / static_cast<float>(nbrs.size()));
            const auto& selfPos = output.vertices[chunkVertexStart + li].position;
            const auto delta = sub(selfPos, neighborMean);
            const float deviation = length(delta);
            localDeviationMean += deviation;
            localDeviationMax = std::max(localDeviationMax, deviation);
            ++localDeviationSamples;
            if (deviation > (0.25F * voxelSize)) {
                ++localDeviationGtQuarterVoxel;
            }
            if (deviation > (0.5F * voxelSize)) {
                ++localDeviationGtHalfVoxel;
            }
            if (deviation > (0.75F * voxelSize)) {
                ++localDeviationGtThreeQuarterVoxel;
            }
            const auto radial = sub(selfPos, vertexCentroid);
            const float radialLen = length(radial);
            if (radialLen > kGradientEpsilon) {
                const auto radialDir = scale(radial, 1.0F / radialLen);
                const float radialDeviation = dot(delta, radialDir);
                localRadialDeviationMax = std::max(localRadialDeviationMax, radialDeviation);
                if (radialDeviation > (0.25F * voxelSize)) {
                    ++localRadialDeviationGtQuarterVoxel;
                }
                if (radialDeviation > (0.5F * voxelSize)) {
                    ++localRadialDeviationGtHalfVoxel;
                }
                if (radialDeviation > (0.75F * voxelSize)) {
                    ++localRadialDeviationGtThreeQuarterVoxel;
                }
            }
        }
        if (localDeviationSamples > 0U) {
            localDeviationMean /= static_cast<float>(localDeviationSamples);
        }
    }
    float radialDistanceMean = 0.0F;
    float radialDistanceStdDev = 0.0F;
    float radialDistanceMin = 0.0F;
    float radialDistanceMax = 0.0F;
    uint32_t radialOutlierVertices = 0;
    if (!radialDistances.empty()) {
        radialDistanceMin = std::numeric_limits<float>::max();
        radialDistanceMax = 0.0F;
        float accum = 0.0F;
        for (const float d : radialDistances) {
            accum += d;
            radialDistanceMin = std::min(radialDistanceMin, d);
            radialDistanceMax = std::max(radialDistanceMax, d);
        }
        radialDistanceMean = accum / static_cast<float>(radialDistances.size());
        float sqAccum = 0.0F;
        for (const float d : radialDistances) {
            const float delta = d - radialDistanceMean;
            sqAccum += delta * delta;
        }
        radialDistanceStdDev = std::sqrt(sqAccum / static_cast<float>(radialDistances.size()));
        const float sigmaGate = radialDistanceMean + (2.5F * radialDistanceStdDev);
        const float absoluteGate = radialDistanceMean + (0.08F * voxelSize);
        const float outlierGate = std::max(sigmaGate, absoluteGate);
        for (const float d : radialDistances) {
            if (d > outlierGate) {
                ++radialOutlierVertices;
            }
        }
    }
    if (minTriangleArea == std::numeric_limits<float>::max()) {
        minTriangleArea = 0.0F;
    }
    if (minFaceHintDot == std::numeric_limits<float>::max()) {
        minFaceHintDot = 0.0F;
        maxFaceHintDot = 0.0F;
    }
    if (avgVertexNormalFaceDotCount == 0U) {
        minVertexNormalFaceDot = 0.0F;
    }
    if (minCentroidHeuristicSignal == std::numeric_limits<float>::max()) {
        minCentroidHeuristicSignal = 0.0F;
        maxCentroidHeuristicSignal = 0.0F;
    }

    uint32_t radialPositiveVertexNormals = 0;
    uint32_t radialNegativeVertexNormals = 0;
    uint32_t radialNeutralVertexNormals = 0;
    float avgVertexNormalRadialDotAccum = 0.0F;
    uint32_t avgVertexNormalRadialDotCount = 0;
    for (std::size_t i = chunkVertexStart; i < output.vertices.size(); ++i) {
        const MeshVertex& v = output.vertices[i];
        const auto radial = sub(v.position, vertexCentroid);
        const float radialLen = length(radial);
        const float normalLen = length(v.normal);
        if (radialLen <= kGradientEpsilon || normalLen <= kGradientEpsilon) {
            ++radialNeutralVertexNormals;
            continue;
        }
        const auto radialUnit = scale(radial, 1.0F / radialLen);
        const auto normalUnit = scale(v.normal, 1.0F / normalLen);
        const float radialDot = dot(normalUnit, radialUnit);
        avgVertexNormalRadialDotAccum += radialDot;
        ++avgVertexNormalRadialDotCount;
        if (radialDot > 1.0e-4F) {
            ++radialPositiveVertexNormals;
        } else if (radialDot < -1.0e-4F) {
            ++radialNegativeVertexNormals;
        } else {
            ++radialNeutralVertexNormals;
        }
    }
    const float avgVertexNormalRadialDot =
        avgVertexNormalRadialDotCount > 0U ? (avgVertexNormalRadialDotAccum / static_cast<float>(avgVertexNormalRadialDotCount)) : 0.0F;
    const float avgVertexNormalFaceDot =
        avgVertexNormalFaceDotCount > 0U ? (avgVertexNormalFaceDotAccum / static_cast<float>(avgVertexNormalFaceDotCount)) : 0.0F;
    const std::string topologyData = [&]() {
        std::ostringstream oss;
        oss << "{\"chunk\":[" << coord.x << "," << coord.y << "," << coord.z
            << "],\"crossings\":" << crossings.size()
            << ",\"vertices\":" << (output.vertices.size() - chunkVertexStart)
            << ",\"indices\":" << (output.indices.size() - chunkIndexStart)
            << ",\"droppedIncompleteQuads\":" << diagnostics.droppedIncompleteQuads
            << ",\"droppedDuplicateQuads\":" << diagnostics.droppedDuplicateQuads << "}";
        return oss.str();
    }();
    const std::string windingData = [&]() {
        std::ostringstream oss;
        oss << "{\"chunk\":[" << coord.x << "," << coord.y << "," << coord.z
            << "],\"alignedQuads\":" << alignedQuads
            << ",\"flippedQuads\":" << flippedQuads
            << ",\"minFaceHintDot\":" << minFaceHintDot
            << ",\"maxFaceHintDot\":" << maxFaceHintDot << "}";
        return oss.str();
    }();
    const std::string qefData = [&]() {
        const uint32_t localIllConditioned = diagnostics.illConditionedQef - diagnosticsIllStart;
        const uint32_t localFallbacks = diagnostics.qefFallbacks - diagnosticsFallbackStart;
        std::ostringstream oss;
        oss << "{\"chunk\":[" << coord.x << "," << coord.y << "," << coord.z
            << "],\"illConditionedQef\":" << localIllConditioned
            << ",\"qefFallbacks\":" << localFallbacks
            << ",\"activeCells\":" << activeCellCount
            << ",\"cellsWithOneSample\":" << cellsWithOneSample
            << ",\"cellsWithTwoSamples\":" << cellsWithTwoSamples
            << ",\"cellsWithThreeOrMoreSamples\":" << cellsWithThreeOrMoreSamples
            << ",\"illByMinDiag\":" << qefReasonStats.illByMinDiag
            << ",\"illByRatio\":" << qefReasonStats.illByRatio
            << ",\"solveFailed\":" << qefReasonStats.solveFailed
            << ",\"residualRejected\":" << qefReasonStats.residualRejected
            << ",\"outOfCellRejected\":" << qefReasonStats.outOfCellRejected
            << ",\"outOfCellRejectedIllConditioned\":" << qefReasonStats.outOfCellRejectedIllConditioned
            << ",\"outOfCellRejectedWellConditioned\":" << qefReasonStats.outOfCellRejectedWellConditioned << "}";
        return oss.str();
    }();
    const std::string qualityData = [&]() {
        std::ostringstream oss;
        oss << "{\"chunk\":[" << coord.x << "," << coord.y << "," << coord.z
            << "],\"degenerateTriangles\":" << degenerateTriangles
            << ",\"minTriangleArea\":" << minTriangleArea
            << ",\"maxTriangleArea\":" << maxTriangleArea
            << ",\"boundaryVertexCount\":" << boundaryVertexCount << "}";
        return oss.str();
    }();
    const std::string orientationData = [&]() {
        std::ostringstream oss;
        oss << "{\"chunk\":[" << coord.x << "," << coord.y << "," << coord.z
            << "],\"outwardOrientedTriangles\":" << outwardOrientedTriangles
            << ",\"inwardOrientedTriangles\":" << inwardOrientedTriangles << "}";
        return oss.str();
    }();
    const uint32_t orientedTotal = outwardOrientedTriangles + inwardOrientedTriangles;
    const float inwardRatio = orientedTotal > 0U ? (static_cast<float>(inwardOrientedTriangles) / static_cast<float>(orientedTotal)) : 0.0F;
    const float invertedNormalRatio =
        orientedTotal > 0U ? (static_cast<float>(invertedVertexNormalTriangles) / static_cast<float>(orientedTotal)) : 0.0F;
    const uint32_t cleanupDropCount = localDroppedInvalidTriangles + localDroppedDegenerateTriangles + localDroppedDuplicateTriangles;
    const float cleanupDropRatio =
        chunkTriangles.empty() ? 0.0F : (static_cast<float>(cleanupDropCount) / static_cast<float>(chunkTriangles.size()));
    const bool forceTwoSidedFallback =
        nonManifoldEdgeCount > 0U || inwardRatio > 0.25F || invertedNormalRatio > 0.0005F || cleanupDropRatio > 0.2F || cleanedTriangles.empty();
    if (outForceTwoSidedFallback != nullptr) {
        *outForceTwoSidedFallback = forceTwoSidedFallback;
    }

    const std::string cullingRiskData = [&]() {
        std::ostringstream oss;
        oss << "{\"chunk\":[" << coord.x << "," << coord.y << "," << coord.z
            << "],\"orientedTotal\":" << orientedTotal
            << ",\"inwardTriangles\":" << inwardOrientedTriangles
            << ",\"inwardRatio\":" << inwardRatio
            << ",\"invertedVertexNormalTriangles\":" << invertedVertexNormalTriangles
            << ",\"invertedNormalRatio\":" << invertedNormalRatio
            << ",\"degenerateTriangles\":" << degenerateTriangles
            << ",\"cleanupDroppedTriangles\":" << cleanupDropCount
            << ",\"cleanupDropRatio\":" << cleanupDropRatio
            << ",\"forceTwoSidedFallback\":" << (forceTwoSidedFallback ? 1 : 0) << "}";
        return oss.str();
    }();
    const std::string lightingDirectionData = [&]() {
        std::ostringstream oss;
        const float inv = lightDotSampleCount > 0U ? (1.0F / static_cast<float>(lightDotSampleCount)) : 0.0F;
        oss << "{\"chunk\":[" << coord.x << "," << coord.y << "," << coord.z
            << "],\"lightFacingPositiveDirTriangles\":" << lightFacingPositiveDirTriangles
            << ",\"lightFacingNegativeDirTriangles\":" << lightFacingNegativeDirTriangles
            << ",\"avgLightDotPositiveDir\":" << (avgLightDotPositiveDirAccum * inv)
            << ",\"avgLightDotNegativeDir\":" << (avgLightDotNegativeDirAccum * inv) << "}";
        return oss.str();
    }();
    const std::string shadowBiasNormalsData = [&]() {
        std::ostringstream oss;
        oss << "{\"chunk\":[" << coord.x << "," << coord.y << "," << coord.z
            << "],\"invertedVertexNormalTriangles\":" << invertedVertexNormalTriangles
            << ",\"minVertexNormalFaceDot\":" << minVertexNormalFaceDot
            << ",\"avgVertexNormalFaceDot\":" << avgVertexNormalFaceDot << "}";
        return oss.str();
    }();
    const std::string manifoldData = [&]() {
        std::ostringstream oss;
        oss << "{\"chunk\":[" << coord.x << "," << coord.y << "," << coord.z
            << "],\"boundaryEdgeCount\":" << boundaryEdgeCount
            << ",\"nonManifoldEdgeCount\":" << nonManifoldEdgeCount
            << ",\"uniqueEdgeCount\":" << edgeUseCount.size() << "}";
        return oss.str();
    }();
    const std::string triangulationData = [&]() {
        std::ostringstream oss;
        oss << "{\"chunk\":[" << coord.x << "," << coord.y << "," << coord.z
            << "],\"emittedQuads\":" << emittedQuads
            << ",\"selectedAlternateDiagonalQuads\":" << selectedAlternateDiagonalQuads
            << ",\"twistedCurrentDiagonalQuads\":" << twistedCurrentDiagonalQuads
            << ",\"twistedAlternateDiagonalQuads\":" << twistedAlternateDiagonalQuads
            << ",\"alternateDiagonalHigherAreaQuads\":" << alternateDiagonalHigherAreaQuads
            << ",\"trianglesFlippedToMatchVertexNormals\":" << trianglesFlippedToMatchVertexNormals
            << ",\"trianglesFlippedToMatchRadialOrientation\":" << trianglesFlippedToMatchRadialOrientation << "}";
        return oss.str();
    }();
    const std::string componentOrientationHeuristicData = [&]() {
        std::ostringstream oss;
        oss << "{\"chunk\":[" << coord.x << "," << coord.y << "," << coord.z
            << "],\"componentCount\":" << orientationComponentCount
            << ",\"centroidHeuristicFlipCount\":" << centroidHeuristicFlipCount
            << ",\"centroidHeuristicWeakSignalCount\":" << centroidHeuristicWeakSignalCount
            << ",\"minCentroidHeuristicSignal\":" << minCentroidHeuristicSignal
            << ",\"maxCentroidHeuristicSignal\":" << maxCentroidHeuristicSignal << "}";
        return oss.str();
    }();
    const std::string vertexNormalRadialData = [&]() {
        std::ostringstream oss;
        oss << "{\"chunk\":[" << coord.x << "," << coord.y << "," << coord.z
            << "],\"radialPositiveVertexNormals\":" << radialPositiveVertexNormals
            << ",\"radialNegativeVertexNormals\":" << radialNegativeVertexNormals
            << ",\"radialNeutralVertexNormals\":" << radialNeutralVertexNormals
            << ",\"avgVertexNormalRadialDot\":" << avgVertexNormalRadialDot << "}";
        return oss.str();
    }();
    const std::string vertexPlacementData = [&]() {
        std::ostringstream oss;
        const float invCount = placementStats.solvedVertices > 0U ? (1.0F / static_cast<float>(placementStats.solvedVertices)) : 0.0F;
        const float invOutCount = placementStats.outOfCellVertices > 0U ? (1.0F / static_cast<float>(placementStats.outOfCellVertices)) : 0.0F;
        const float invRejectedOutCount =
            placementStats.rejectedOutOfCellVertices > 0U ? (1.0F / static_cast<float>(placementStats.rejectedOutOfCellVertices)) : 0.0F;
        const float invAcceptedOutIllCount = placementStats.acceptedOutOfCellIllConditioned > 0U
                                                 ? (1.0F / static_cast<float>(placementStats.acceptedOutOfCellIllConditioned))
                                                 : 0.0F;
        const float invAcceptedOutWellCount = placementStats.acceptedOutOfCellWellConditioned > 0U
                                                  ? (1.0F / static_cast<float>(placementStats.acceptedOutOfCellWellConditioned))
                                                  : 0.0F;
        const float invAcceptedOutSoftPullCount = placementStats.acceptedOutOfCellSoftPulledVertices > 0U
                                                      ? (1.0F / static_cast<float>(placementStats.acceptedOutOfCellSoftPulledVertices))
                                                      : 0.0F;
        oss << "{\"chunk\":[" << coord.x << "," << coord.y << "," << coord.z
            << "],\"solvedVertices\":" << placementStats.solvedVertices
            << ",\"clampedVertices\":" << placementStats.clampedVertices
            << ",\"boundaryPinnedVertices\":" << placementStats.boundaryPinnedVertices
            << ",\"outOfCellVertices\":" << placementStats.outOfCellVertices
            << ",\"rejectedOutOfCellVertices\":" << placementStats.rejectedOutOfCellVertices
            << ",\"acceptedOutOfCellIllConditioned\":" << placementStats.acceptedOutOfCellIllConditioned
            << ",\"acceptedOutOfCellWellConditioned\":" << placementStats.acceptedOutOfCellWellConditioned
            << ",\"acceptedOutWellLeQuarterVoxel\":" << placementStats.acceptedOutWellLeQuarterVoxel
            << ",\"acceptedOutWellLeHalfVoxel\":" << placementStats.acceptedOutWellLeHalfVoxel
            << ",\"acceptedOutWellLeThreeQuarterVoxel\":" << placementStats.acceptedOutWellLeThreeQuarterVoxel
            << ",\"acceptedOutWellLeFullVoxel\":" << placementStats.acceptedOutWellLeFullVoxel
            << ",\"acceptedOutOfCellSoftPulledVertices\":" << placementStats.acceptedOutOfCellSoftPulledVertices
            << ",\"avgClampDisplacement\":" << (placementStats.clampDisplacementAccum * invCount)
            << ",\"maxClampDisplacement\":" << placementStats.maxClampDisplacement
            << ",\"avgMassPointDistance\":" << (placementStats.massPointDistanceAccum * invCount)
            << ",\"maxMassPointDistance\":" << placementStats.maxMassPointDistance
            << ",\"avgOutOfCellDistance\":" << (placementStats.outOfCellDistanceAccum * invOutCount)
            << ",\"maxOutOfCellDistance\":" << placementStats.maxOutOfCellDistance
            << ",\"avgRejectedOutOfCellDistance\":" << (placementStats.rejectedOutOfCellDistanceAccum * invRejectedOutCount)
            << ",\"maxRejectedOutOfCellDistance\":" << placementStats.maxRejectedOutOfCellDistance
            << ",\"avgAcceptedOutOfCellIllDistance\":" << (placementStats.acceptedOutOfCellIllDistanceAccum * invAcceptedOutIllCount)
            << ",\"avgAcceptedOutOfCellWellDistance\":" << (placementStats.acceptedOutOfCellWellDistanceAccum * invAcceptedOutWellCount)
            << ",\"maxAcceptedOutOfCellIllDistance\":" << placementStats.maxAcceptedOutOfCellIllDistance
            << ",\"maxAcceptedOutOfCellWellDistance\":" << placementStats.maxAcceptedOutOfCellWellDistance
            << ",\"avgAcceptedOutOfCellSoftPullBlend\":" << (placementStats.acceptedOutOfCellSoftPullBlendAccum * invAcceptedOutSoftPullCount)
            << ",\"maxAcceptedOutOfCellSoftPullBlend\":" << placementStats.maxAcceptedOutOfCellSoftPullBlend << "}";
        return oss.str();
    }();
    const std::string triangleShapeData = [&]() {
        std::ostringstream oss;
        const uint32_t triCount = static_cast<uint32_t>((output.indices.size() - chunkIndexStart) / 3U);
        const float invTriCount = triCount > 0U ? (1.0F / static_cast<float>(triCount)) : 0.0F;
        oss << "{\"chunk\":[" << coord.x << "," << coord.y << "," << coord.z
            << "],\"triangleCount\":" << triCount
            << ",\"skinnyTriangles\":" << skinnyTriangles
            << ",\"skinnyBoundaryTriangles\":" << skinnyBoundaryTriangles
            << ",\"skinnyInvertedNormalTriangles\":" << skinnyInvertedNormalTriangles
            << ",\"skinnyRatio\":" << (static_cast<float>(skinnyTriangles) * invTriCount)
            << ",\"avgTriangleAspectRatio\":" << (avgTriangleAspectRatioAccum * invTriCount)
            << ",\"maxTriangleAspectRatio\":" << maxTriangleAspectRatio << "}";
        return oss.str();
    }();
    const std::string geometryValidityData = [&]() {
        std::ostringstream oss;
        oss << "{\"chunk\":[" << coord.x << "," << coord.y << "," << coord.z
            << "],\"nonFiniteVertexPositionCount\":" << nonFiniteVertexPositionCount
            << ",\"nonFiniteVertexNormalCount\":" << nonFiniteVertexNormalCount
            << ",\"nearZeroVertexNormalCount\":" << nearZeroVertexNormalCount << "}";
        return oss.str();
    }();
    const std::string radialOutlierData = [&]() {
        std::ostringstream oss;
        oss << "{\"chunk\":[" << coord.x << "," << coord.y << "," << coord.z
            << "],\"radialDistanceMean\":" << radialDistanceMean
            << ",\"radialDistanceStdDev\":" << radialDistanceStdDev
            << ",\"radialDistanceMin\":" << radialDistanceMin
            << ",\"radialDistanceMax\":" << radialDistanceMax
            << ",\"radialOutlierVertices\":" << radialOutlierVertices << "}";
        return oss.str();
    }();
    const std::string localProtrusionData = [&]() {
        std::ostringstream oss;
        oss << "{\"chunk\":[" << coord.x << "," << coord.y << "," << coord.z
            << "],\"localDeviationSamples\":" << localDeviationSamples
            << ",\"localDeviationMean\":" << localDeviationMean
            << ",\"localDeviationMax\":" << localDeviationMax
            << ",\"localDeviationGtQuarterVoxel\":" << localDeviationGtQuarterVoxel
            << ",\"localDeviationGtHalfVoxel\":" << localDeviationGtHalfVoxel
            << ",\"localDeviationGtThreeQuarterVoxel\":" << localDeviationGtThreeQuarterVoxel
            << ",\"localRadialDeviationGtQuarterVoxel\":" << localRadialDeviationGtQuarterVoxel
            << ",\"localRadialDeviationGtHalfVoxel\":" << localRadialDeviationGtHalfVoxel
            << ",\"localRadialDeviationGtThreeQuarterVoxel\":" << localRadialDeviationGtThreeQuarterVoxel
            << ",\"localRadialDeviationMax\":" << localRadialDeviationMax << "}";
        return oss.str();
    }();
    const std::string localProtrusionClampData = [&]() {
        std::ostringstream oss;
        const float invClampCount =
            localRadialClampVertices > 0U ? (1.0F / static_cast<float>(localRadialClampVertices)) : 0.0F;
        oss << "{\"chunk\":[" << coord.x << "," << coord.y << "," << coord.z
            << "],\"localRadialClampVertices\":" << localRadialClampVertices
            << ",\"avgLocalRadialClampDisplacement\":" << (localRadialClampDisplacementAccum * invClampCount)
            << ",\"maxLocalRadialClampDisplacement\":" << maxLocalRadialClampDisplacement << "}";
        return oss.str();
    }();
    const std::string normalRecomputeData = [&]() {
        std::ostringstream oss;
        oss << "{\"chunk\":[" << coord.x << "," << coord.y << "," << coord.z
            << "],\"normalContributionFaces\":" << normalContributionFaces
            << ",\"recomputedVertexNormals\":" << recomputedVertexNormals
            << ",\"unresolvedVertexNormals\":" << unresolvedVertexNormals << "}";
        return oss.str();
    }();

    // #region agent log
    appendDebugLog(
        "pre-fix",
        "H1",
        "engine/voxel/dual_contouring.cpp:meshSingleChunk",
        "Chunk mesh topological diagnostics",
        topologyData);
    // #endregion

    // #region agent log
    appendDebugLog(
        "pre-fix",
        "H2",
        "engine/voxel/dual_contouring.cpp:meshSingleChunk",
        "Quad winding alignment diagnostics",
        windingData);
    // #endregion

    // #region agent log
    appendDebugLog(
        "pre-fix",
        "H3",
        "engine/voxel/dual_contouring.cpp:meshSingleChunk",
        "QEF conditioning and fallback diagnostics",
        qefData);
    // #endregion

    // #region agent log
    appendDebugLog(
        "pre-fix",
        "H4_H5",
        "engine/voxel/dual_contouring.cpp:meshSingleChunk",
        "Triangle quality and boundary concentration",
        qualityData);
    // #endregion

    // #region agent log
    appendDebugLog(
        "pre-fix",
        "H2",
        "engine/voxel/dual_contouring.cpp:meshSingleChunk",
        "Triangle orientation coherence",
        orientationData);
    // #endregion

    // #region agent log
    appendDebugLog(
        "gap-pass1",
        "H26_H27",
        "engine/voxel/dual_contouring.cpp:meshSingleChunk",
        "Backface-culling risk diagnostics",
        cullingRiskData);
    // #endregion

    // #region agent log
    appendDebugLog(
        "lighting-sign-pass1",
        "H22",
        "engine/voxel/dual_contouring.cpp:meshSingleChunk",
        "Light direction sign viability from face normals",
        lightingDirectionData);
    // #endregion

    // #region agent log
    appendDebugLog(
        "shadow-scales-pass2",
        "H16",
        "engine/voxel/dual_contouring.cpp:meshSingleChunk",
        "Vertex-normal versus geometric-face alignment",
        shadowBiasNormalsData);
    // #endregion

    // #region agent log
    appendDebugLog(
        "pre-fix",
        "H7",
        "engine/voxel/dual_contouring.cpp:meshSingleChunk",
        "Mesh manifold edge diagnostics",
        manifoldData);
    // #endregion

    // #region agent log
    appendDebugLog(
        "pre-fix",
        "H8",
        "engine/voxel/dual_contouring.cpp:meshSingleChunk",
        "Quad triangulation stability diagnostics",
        triangulationData);
    // #endregion

    // #region agent log
    appendDebugLog(
        "orientation-pass3",
        "H30",
        "engine/voxel/dual_contouring.cpp:meshSingleChunk",
        "Component orientation heuristic diagnostics",
        componentOrientationHeuristicData);
    // #endregion

    // #region agent log
    appendDebugLog(
        "normal-radial-pass3",
        "H31",
        "engine/voxel/dual_contouring.cpp:meshSingleChunk",
        "Vertex normal radial alignment diagnostics",
        vertexNormalRadialData);
    // #endregion

    // #region agent log
    appendDebugLog(
        "vertex-placement-pass4",
        "H34_H35",
        "engine/voxel/dual_contouring.cpp:meshSingleChunk",
        "Vertex placement clamp and mass-point diagnostics",
        vertexPlacementData);
    // #endregion

    // #region agent log
    appendDebugLog(
        "triangle-shape-pass5",
        "H36_H37_H39",
        "engine/voxel/dual_contouring.cpp:meshSingleChunk",
        "Triangle shape and sliver diagnostics",
        triangleShapeData);
    // #endregion

    // #region agent log
    appendDebugLog(
        "geometry-validity-pass6",
        "H40_H41",
        "engine/voxel/dual_contouring.cpp:meshSingleChunk",
        "Geometry validity diagnostics",
        geometryValidityData);
    // #endregion

    // #region agent log
    appendDebugLog(
        "radial-outlier-pass7",
        "H42_H43",
        "engine/voxel/dual_contouring.cpp:meshSingleChunk",
        "Vertex radial outlier diagnostics",
        radialOutlierData);
    // #endregion

    // #region agent log
    appendDebugLog(
        "local-protrusion-pass8",
        "H44_H45",
        "engine/voxel/dual_contouring.cpp:meshSingleChunk",
        "Local neighborhood protrusion diagnostics",
        localProtrusionData);
    // #endregion

    // #region agent log
    appendDebugLog(
        "local-protrusion-fix-pass9",
        "H46",
        "engine/voxel/dual_contouring.cpp:meshSingleChunk",
        "Local radial protrusion clamp diagnostics",
        localProtrusionClampData);
    // #endregion

    // #region agent log
    appendDebugLog(
        "normal-recompute-pass10",
        "H47",
        "engine/voxel/dual_contouring.cpp:meshSingleChunk",
        "Post-adjustment vertex normal recompute diagnostics",
        normalRecomputeData);
    // #endregion

}

}  // namespace

MeshBuffers DualContouringMesher::remeshDirtyRegions(const VoxelWorld& world, const std::vector<ChunkCoord>& dirtyChunks) const {
    MeshBuffers mesh{};
    mesh.sourceTopologyVersion = world.versions().topologyVersion;
    MeshPatchBatch::Diagnostics diagnostics{};
    const auto lookup = [&world](const ChunkCoord& c) { return world.findChunk(c); };
    for (const ChunkCoord& coord : dirtyChunks) {
        if (world.findChunk(coord) == nullptr) {
            continue;
        }
        meshSingleChunk(coord, world.voxelSize(), lookup, mesh, diagnostics, nullptr);
    }
    return mesh;
}

MeshBuffers DualContouringMesher::remeshSnapshot(const JobSnapshot& snapshot) const {
    MeshBuffers mesh{};
    mesh.sourceTopologyVersion = snapshot.topologyVersion;
    MeshPatchBatch::Diagnostics diagnostics{};
    const auto lookup = [&snapshot](const ChunkCoord& c) -> const VoxelChunk* {
        const auto it = snapshot.immutableChunks.find(c);
        return it == snapshot.immutableChunks.end() ? nullptr : &it->second;
    };
    for (const ChunkCoord& coord : snapshot.meshTargetChunks) {
        if (lookup(coord) == nullptr) {
            continue;
        }
        meshSingleChunk(coord, snapshot.voxelSize, lookup, mesh, diagnostics, nullptr);
    }
    return mesh;
}

MeshPatchBatch DualContouringMesher::remeshSnapshotPatches(const JobSnapshot& snapshot) const {
    MeshPatchBatch batch{};
    batch.sourceTopologyVersion = snapshot.topologyVersion;
    batch.patches.reserve(snapshot.meshTargetChunks.size());
    const auto lookup = [&snapshot](const ChunkCoord& c) -> const VoxelChunk* {
        const auto it = snapshot.immutableChunks.find(c);
        return it == snapshot.immutableChunks.end() ? nullptr : &it->second;
    };
    for (const ChunkCoord& coord : snapshot.meshTargetChunks) {
        ChunkMeshPatch patch{};
        patch.coord = coord;
        patch.mesh.sourceTopologyVersion = snapshot.topologyVersion;
        if (lookup(coord) == nullptr) {
            patch.remove = true;
            batch.patches.push_back(std::move(patch));
            continue;
        }
        meshSingleChunk(coord, snapshot.voxelSize, lookup, patch.mesh, batch.diagnostics, &patch.forceTwoSidedFallback);
        if (patch.forceTwoSidedFallback) {
            ++batch.diagnostics.fallbackFlaggedChunks;
        }
        patch.remove = patch.mesh.vertices.empty() || patch.mesh.indices.empty();
        batch.patches.push_back(std::move(patch));
    }
    return batch;
}

std::array<float, 3> DualContouringMesher::normalize(const std::array<float, 3>& v) {
    return normalizeVec(v);
}

float DualContouringMesher::triangleArea(const MeshVertex& a, const MeshVertex& b, const MeshVertex& c) {
    const auto ab = sub(b.position, a.position);
    const auto ac = sub(c.position, a.position);
    return 0.5F * length(cross(ab, ac));
}

MeshStructuralStats DualContouringMesher::validateStructural(const MeshBuffers& buffers, float voxelSize) const {
    MeshStructuralStats stats{};
    for (const MeshVertex& v : buffers.vertices) {
        if (!std::isfinite(v.position[0]) || !std::isfinite(v.position[1]) || !std::isfinite(v.position[2]) ||
            !std::isfinite(v.normal[0]) || !std::isfinite(v.normal[1]) || !std::isfinite(v.normal[2])) {
            ++stats.nanCount;
        }
    }
    for (uint32_t idx : buffers.indices) {
        if (idx >= static_cast<uint32_t>(buffers.vertices.size())) {
            ++stats.invalidIndexCount;
        }
    }
    const float threshold = 1.0e-12F * voxelSize * voxelSize;
    for (std::size_t i = 0; i + 2 < buffers.indices.size(); i += 3) {
        const uint32_t ia = buffers.indices[i];
        const uint32_t ib = buffers.indices[i + 1];
        const uint32_t ic = buffers.indices[i + 2];
        if (ia >= buffers.vertices.size() || ib >= buffers.vertices.size() || ic >= buffers.vertices.size()) {
            continue;
        }
        const float area = triangleArea(buffers.vertices[ia], buffers.vertices[ib], buffers.vertices[ic]);
        if (area < threshold) {
            ++stats.degenerateCount;
        }
    }
    return stats;
}

}  // namespace oro::voxel
