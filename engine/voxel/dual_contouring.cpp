#include "voxel/dual_contouring.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace oro::voxel {

namespace {

constexpr float kGradientEpsilon = 1.0e-6F;
constexpr float kQefRegularization = 1.0e-6F;
constexpr float kQefPivotEpsilon = 1.0e-9F;
constexpr int kCellsPerAxis = kChunkEdge;
constexpr uint32_t kInvalidVertexIndex = std::numeric_limits<uint32_t>::max();

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

bool isValidCell(const CellCoord& c) {
    return c.x >= 0 && c.x < kCellsPerAxis &&
           c.y >= 0 && c.y < kCellsPerAxis &&
           c.z >= 0 && c.z < kCellsPerAxis;
}

std::size_t flatCellIndex(const CellCoord& c) {
    return static_cast<std::size_t>((c.z * kCellsPerAxis * kCellsPerAxis) + (c.y * kCellsPerAxis) + c.x);
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

template <typename ChunkLookup>
void accumulateEdgeCrossingsForAxis(Axis axis, const ChunkCoord& coord, float voxelSize, const ChunkLookup& chunkLookup,
                                    std::vector<CellWork>& cells, std::vector<CrossingEdge>& crossings) {
    const int xMax = axis == Axis::X ? kChunkEdge - 1 : kChunkEdge;
    const int yMax = axis == Axis::Y ? kChunkEdge - 1 : kChunkEdge;
    const int zMax = axis == Axis::Z ? kChunkEdge - 1 : kChunkEdge;

    for (int z = 0; z <= zMax; ++z) {
        for (int y = 0; y <= yMax; ++y) {
            for (int x = 0; x <= xMax; ++x) {
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
                sample.normal = gradientAt(coord, x, y, z, voxelSize, chunkLookup);
                sample.materialId = s0.phi < kIsoValue ? s0.materialId : s1.materialId;

                const std::array<CellCoord, 4> ring = incidentCells(axis, x, y, z);
                bool anyCell = false;
                for (const CellCoord& c : ring) {
                    if (!isValidCell(c)) {
                        continue;
                    }
                    anyCell = true;
                    CellWork& cell = cells[flatCellIndex(c)];
                    cell.active = true;
                    cell.qef.addSample(sample);
                }
                if (anyCell) {
                    crossings.push_back({axis, x, y, z, sample.normal});
                }
            }
        }
    }
}

MeshVertex solveCellVertex(const ChunkCoord& coord, const CellCoord& cellCoord, float voxelSize, const QefAccumulator& qef) {
    MeshVertex vertex{};
    if (qef.sampleCount == 0U) {
        return vertex;
    }

    std::array<std::array<float, 3>, 3> regularized = qef.ata;
    regularized[0][0] += kQefRegularization;
    regularized[1][1] += kQefRegularization;
    regularized[2][2] += kQefRegularization;

    std::array<float, 3> solved{};
    bool solvedOk = solveLinear3x3(regularized, qef.atb, solved);
    if (!solvedOk) {
        const float inv = 1.0F / static_cast<float>(qef.sampleCount);
        solved = {qef.massPoint[0] * inv, qef.massPoint[1] * inv, qef.massPoint[2] * inv};
    }

    const auto cellMin = latticePointWorld(coord, cellCoord.x, cellCoord.y, cellCoord.z, voxelSize);
    const auto cellMax = latticePointWorld(coord, cellCoord.x + 1, cellCoord.y + 1, cellCoord.z + 1, voxelSize);
    solved[0] = std::clamp(solved[0], std::min(cellMin[0], cellMax[0]), std::max(cellMin[0], cellMax[0]));
    solved[1] = std::clamp(solved[1], std::min(cellMin[1], cellMax[1]), std::max(cellMin[1], cellMax[1]));
    solved[2] = std::clamp(solved[2], std::min(cellMin[2], cellMax[2]), std::max(cellMin[2], cellMax[2]));

    vertex.position = solved;
    const float inv = 1.0F / static_cast<float>(qef.sampleCount);
    vertex.normal = normalizeVec({qef.normalSum[0] * inv, qef.normalSum[1] * inv, qef.normalSum[2] * inv});
    vertex.materialId = qef.materialId;
    return vertex;
}

void emitQuad(const std::array<uint32_t, 4>& quad, const std::array<float, 3>& normalHint, const std::vector<MeshVertex>& vertices,
              std::vector<uint32_t>& indices) {
    if (quad[0] == quad[1] || quad[1] == quad[2] || quad[2] == quad[3] || quad[3] == quad[0] || quad[0] == quad[2] || quad[1] == quad[3]) {
        return;
    }
    const auto& v0 = vertices[quad[0]].position;
    const auto& v1 = vertices[quad[1]].position;
    const auto& v2 = vertices[quad[2]].position;
    const auto face = cross(sub(v1, v0), sub(v2, v0));
    const bool aligned = dot(face, normalHint) >= 0.0F;
    if (aligned) {
        indices.push_back(quad[0]);
        indices.push_back(quad[1]);
        indices.push_back(quad[2]);
        indices.push_back(quad[0]);
        indices.push_back(quad[2]);
        indices.push_back(quad[3]);
        return;
    }
    indices.push_back(quad[0]);
    indices.push_back(quad[3]);
    indices.push_back(quad[2]);
    indices.push_back(quad[0]);
    indices.push_back(quad[2]);
    indices.push_back(quad[1]);
}

template <typename ChunkLookup>
void meshSingleChunk(const ChunkCoord& coord, float voxelSize, const ChunkLookup& chunkLookup, MeshBuffers& output) {
    std::vector<CellWork> cells(static_cast<std::size_t>(kCellsPerAxis * kCellsPerAxis * kCellsPerAxis));
    std::vector<CrossingEdge> crossings;
    crossings.reserve(4096);

    accumulateEdgeCrossingsForAxis(Axis::X, coord, voxelSize, chunkLookup, cells, crossings);
    accumulateEdgeCrossingsForAxis(Axis::Y, coord, voxelSize, chunkLookup, cells, crossings);
    accumulateEdgeCrossingsForAxis(Axis::Z, coord, voxelSize, chunkLookup, cells, crossings);

    for (int z = 0; z < kCellsPerAxis; ++z) {
        for (int y = 0; y < kCellsPerAxis; ++y) {
            for (int x = 0; x < kCellsPerAxis; ++x) {
                const CellCoord cellCoord{x, y, z};
                CellWork& cell = cells[flatCellIndex(cellCoord)];
                if (!cell.active || cell.qef.sampleCount == 0U) {
                    continue;
                }
                MeshVertex vertex = solveCellVertex(coord, cellCoord, voxelSize, cell.qef);
                cell.vertexIndex = static_cast<uint32_t>(output.vertices.size());
                output.vertices.push_back(vertex);
            }
        }
    }

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
            continue;
        }
        emitQuad(quad, crossing.normal, output.vertices, output.indices);
    }
}

}  // namespace

MeshBuffers DualContouringMesher::remeshDirtyRegions(const VoxelWorld& world, const std::vector<ChunkCoord>& dirtyChunks) const {
    MeshBuffers mesh{};
    mesh.sourceTopologyVersion = world.versions().topologyVersion;
    const auto lookup = [&world](const ChunkCoord& c) { return world.findChunk(c); };
    for (const ChunkCoord& coord : dirtyChunks) {
        if (world.findChunk(coord) == nullptr) {
            continue;
        }
        meshSingleChunk(coord, world.voxelSize(), lookup, mesh);
    }
    return mesh;
}

MeshBuffers DualContouringMesher::remeshSnapshot(const JobSnapshot& snapshot) const {
    MeshBuffers mesh{};
    mesh.sourceTopologyVersion = snapshot.topologyVersion;
    const auto lookup = [&snapshot](const ChunkCoord& c) -> const VoxelChunk* {
        const auto it = snapshot.immutableChunks.find(c);
        return it == snapshot.immutableChunks.end() ? nullptr : &it->second;
    };
    for (const ChunkCoord& coord : snapshot.dirtyChunks) {
        if (lookup(coord) == nullptr) {
            continue;
        }
        meshSingleChunk(coord, snapshot.voxelSize, lookup, mesh);
    }
    return mesh;
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
