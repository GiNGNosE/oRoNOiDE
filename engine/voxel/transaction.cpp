#include "voxel/transaction.h"

#include "core/log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>

namespace oro::voxel {

namespace {

constexpr float kMinEditRadius = 0.001F;
constexpr float kSeamEpsilon = 0.0001F;
constexpr float kEdtInf = 1.0e12F;
constexpr int kLowFidelityRedistanceNarrowBandVoxels = 8;

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

void appendDebugLog(const char* runId, const char* hypothesisId, const char* location, const char* message, const std::string& dataJson) {
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

ChunkCoord neighborCoord(const ChunkCoord& c, int dx, int dy, int dz) {
    return ChunkCoord{c.x + dx, c.y + dy, c.z + dz};
}

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

bool isInside(float phi) {
    return phi < kIsoValue;
}

std::size_t flatDenseIndex(int x, int y, int z, int dimX, int dimY) {
    return static_cast<std::size_t>(x + (dimX * (y + (dimY * z))));
}

void edt1dSquared(const std::vector<float>& f, int n, std::vector<float>& d) {
    if (n <= 0) {
        return;
    }
    std::vector<int> v(static_cast<std::size_t>(n), 0);
    std::vector<float> z(static_cast<std::size_t>(n + 1), 0.0F);
    int k = 0;
    v[0] = 0;
    z[0] = -kEdtInf;
    z[1] = kEdtInf;

    for (int q = 1; q < n; ++q) {
        float s = 0.0F;
        for (;;) {
            const int vk = v[static_cast<std::size_t>(k)];
            const float fq = f[static_cast<std::size_t>(q)];
            const float fvk = f[static_cast<std::size_t>(vk)];
            const float numerator = (fq + static_cast<float>(q * q)) - (fvk + static_cast<float>(vk * vk));
            const float denominator = static_cast<float>(2 * (q - vk));
            s = numerator / denominator;
            if (s > z[static_cast<std::size_t>(k)] || k == 0) {
                break;
            }
            --k;
        }
        ++k;
        v[static_cast<std::size_t>(k)] = q;
        z[static_cast<std::size_t>(k)] = s;
        z[static_cast<std::size_t>(k + 1)] = kEdtInf;
    }

    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[static_cast<std::size_t>(k + 1)] < static_cast<float>(q)) {
            ++k;
        }
        const int vk = v[static_cast<std::size_t>(k)];
        const float delta = static_cast<float>(q - vk);
        d[static_cast<std::size_t>(q)] = (delta * delta) + f[static_cast<std::size_t>(vk)];
    }
}

void edt3dSquared(std::vector<float>& grid, int dimX, int dimY, int dimZ) {
    std::vector<float> inLine;
    std::vector<float> outLine;
    inLine.reserve(static_cast<std::size_t>(std::max({dimX, dimY, dimZ})));
    outLine.reserve(inLine.capacity());

    std::vector<float> passX(grid.size(), kEdtInf);
    for (int z = 0; z < dimZ; ++z) {
        for (int y = 0; y < dimY; ++y) {
            inLine.assign(static_cast<std::size_t>(dimX), 0.0F);
            outLine.assign(static_cast<std::size_t>(dimX), 0.0F);
            for (int x = 0; x < dimX; ++x) {
                inLine[static_cast<std::size_t>(x)] = grid[flatDenseIndex(x, y, z, dimX, dimY)];
            }
            edt1dSquared(inLine, dimX, outLine);
            for (int x = 0; x < dimX; ++x) {
                passX[flatDenseIndex(x, y, z, dimX, dimY)] = outLine[static_cast<std::size_t>(x)];
            }
        }
    }

    std::vector<float> passY(grid.size(), kEdtInf);
    for (int z = 0; z < dimZ; ++z) {
        for (int x = 0; x < dimX; ++x) {
            inLine.assign(static_cast<std::size_t>(dimY), 0.0F);
            outLine.assign(static_cast<std::size_t>(dimY), 0.0F);
            for (int y = 0; y < dimY; ++y) {
                inLine[static_cast<std::size_t>(y)] = passX[flatDenseIndex(x, y, z, dimX, dimY)];
            }
            edt1dSquared(inLine, dimY, outLine);
            for (int y = 0; y < dimY; ++y) {
                passY[flatDenseIndex(x, y, z, dimX, dimY)] = outLine[static_cast<std::size_t>(y)];
            }
        }
    }

    for (int y = 0; y < dimY; ++y) {
        for (int x = 0; x < dimX; ++x) {
            inLine.assign(static_cast<std::size_t>(dimZ), 0.0F);
            outLine.assign(static_cast<std::size_t>(dimZ), 0.0F);
            for (int z = 0; z < dimZ; ++z) {
                inLine[static_cast<std::size_t>(z)] = passY[flatDenseIndex(x, y, z, dimX, dimY)];
            }
            edt1dSquared(inLine, dimZ, outLine);
            for (int z = 0; z < dimZ; ++z) {
                grid[flatDenseIndex(x, y, z, dimX, dimY)] = outLine[static_cast<std::size_t>(z)];
            }
        }
    }
}

}  // namespace

VoxelTransaction::VoxelTransaction(VoxelWorld& world)
    : m_world(world) {}

bool VoxelTransaction::precheck(const EditCommand& command, InvariantFailure& failure) const {
    if (!std::isfinite(command.center[0]) || !std::isfinite(command.center[1]) || !std::isfinite(command.center[2])) {
        failure = {InvariantCode::InvalidPhi, 0, 0, 0, "Edit center contains non-finite values"};
        return false;
    }
    switch (command.shape) {
        case EditShape::Sphere:
            if (!std::isfinite(command.radius) || command.radius < kMinEditRadius) {
                failure = {InvariantCode::InvalidPhi, 0, 0, 0, "Sphere edit radius is invalid"};
                return false;
            }
            break;
        case EditShape::Ellipsoid:
            if (!std::isfinite(command.radii[0]) || !std::isfinite(command.radii[1]) || !std::isfinite(command.radii[2]) ||
                command.radii[0] < kMinEditRadius || command.radii[1] < kMinEditRadius || command.radii[2] < kMinEditRadius) {
                failure = {InvariantCode::InvalidPhi, 0, 0, 0, "Ellipsoid radii are invalid"};
                return false;
            }
            break;
        case EditShape::NoisyStone:
            if (!std::isfinite(command.radius) || command.radius < kMinEditRadius) {
                failure = {InvariantCode::InvalidPhi, 0, 0, 0, "Noisy stone base radius is invalid"};
                return false;
            }
            if (!std::isfinite(command.noiseAmplitude) || command.noiseAmplitude < 0.0F ||
                !std::isfinite(command.noiseFrequency) || command.noiseFrequency <= 0.0F) {
                failure = {InvariantCode::InvalidPhi, 0, 0, 0, "Noisy stone parameters are invalid"};
                return false;
            }
            break;
    }
    return true;
}

bool VoxelTransaction::begin(const EditCommand& command) {
    if (m_open) {
        return false;
    }
    InvariantFailure failure{};
    if (!precheck(command, failure)) {
        return false;
    }
    m_command = command;
    m_metrics = {};
    m_redistanceStats = {};
    m_redistanceStats.highFidelityEnabled = m_highFidelityRedistanceEnabled;
    m_workingChunks.clear();
    m_targetVersion = m_world.versions().topologyVersion + 1;
    m_open = collectTouchedChunks();
    if (!m_open) {
        return false;
    }
    m_sideEffects.begin(m_targetVersion);
    return true;
}

bool VoxelTransaction::collectTouchedChunks() {
    const float chunkWorldEdge = static_cast<float>(kChunkEdge) * m_world.voxelSize();
    const auto center = m_command.center;
    float extentX = m_command.radius;
    float extentY = m_command.radius;
    float extentZ = m_command.radius;
    if (m_command.shape == EditShape::Ellipsoid) {
        extentX = m_command.radii[0];
        extentY = m_command.radii[1];
        extentZ = m_command.radii[2];
    } else if (m_command.shape == EditShape::NoisyStone) {
        const float noisePad = std::max(0.0F, m_command.noiseAmplitude);
        extentX += noisePad;
        extentY += noisePad;
        extentZ += noisePad;
    }
    const int minChunkX = static_cast<int>(std::floor((center[0] - extentX) / chunkWorldEdge)) - 1;
    const int minChunkY = static_cast<int>(std::floor((center[1] - extentY) / chunkWorldEdge)) - 1;
    const int minChunkZ = static_cast<int>(std::floor((center[2] - extentZ) / chunkWorldEdge)) - 1;
    const int maxChunkX = static_cast<int>(std::floor((center[0] + extentX) / chunkWorldEdge)) + 1;
    const int maxChunkY = static_cast<int>(std::floor((center[1] + extentY) / chunkWorldEdge)) + 1;
    const int maxChunkZ = static_cast<int>(std::floor((center[2] + extentZ) / chunkWorldEdge)) + 1;

    for (int z = minChunkZ; z <= maxChunkZ; ++z) {
        for (int y = minChunkY; y <= maxChunkY; ++y) {
            for (int x = minChunkX; x <= maxChunkX; ++x) {
                const ChunkCoord coord{x, y, z};
                const VoxelChunk* source = m_world.findChunk(coord);
                if (source == nullptr) {
                    VoxelChunk created{};
                    created.coord = coord;
                    auto [it, ok] = m_workingChunks.emplace(coord, created);
                    (void)ok;
                } else {
                    auto [it, ok] = m_workingChunks.emplace(coord, *source);
                    (void)ok;
                }
            }
        }
    }
    m_metrics.chunksDirty = static_cast<uint32_t>(m_workingChunks.size());
    return !m_workingChunks.empty();
}

float VoxelTransaction::signedDistanceForCommand(const std::array<float, 3>& position, const EditCommand& command) {
    switch (command.shape) {
        case EditShape::Sphere:
            return VoxelWorld::sphereSdf(position, command.center, command.radius);
        case EditShape::Ellipsoid:
            return VoxelWorld::ellipsoidSdf(position, command.center, command.radii);
        case EditShape::NoisyStone:
            return VoxelWorld::noisyStoneSdf(position,
                                             command.center,
                                             command.radius,
                                             command.noiseAmplitude,
                                             command.noiseFrequency,
                                             command.noiseSeed);
    }
    return VoxelWorld::sphereSdf(position, command.center, command.radius);
}

bool VoxelTransaction::applyShapeSdfToChunk(VoxelChunk& chunk, const EditCommand& command) {
    uint32_t active = 0;
    for (std::size_t i = 0; i < chunk.cells.size(); ++i) {
        int lx = 0;
        int ly = 0;
        int lz = 0;
        VoxelWorld::unpackIndex(i, lx, ly, lz);
        const auto pos = VoxelWorld::cellCenterWorld(chunk.coord, lx, ly, lz, m_world.voxelSize());
        const float shapePhi = signedDistanceForCommand(pos, command);

        VoxelCell& cell = chunk.cells[i];
        const float oldPhi = cell.phi;
        if (command.mode == EditMode::Carve) {
            cell.phi = std::max(cell.phi, -shapePhi);
        } else {
            cell.phi = std::min(cell.phi, shapePhi);
        }
        cell.material.initialized = true;

        if (std::abs(oldPhi - cell.phi) > kSeamEpsilon) {
            ++m_metrics.cellsDirty;
        }
        if ((oldPhi < kIsoValue && cell.phi >= kIsoValue) || (oldPhi >= kIsoValue && cell.phi < kIsoValue)) {
            ++m_metrics.surfaceCellsDirty;
        }
        if (cell.phi < kIsoValue) {
            ++active;
        }
    }
    chunk.activeVoxelCount = active;
    return true;
}

bool VoxelTransaction::applyLocalRedistance() {
    const auto start = std::chrono::steady_clock::now();
    if (m_workingChunks.empty()) {
        return true;
    }

    int minChunkX = std::numeric_limits<int>::max();
    int minChunkY = std::numeric_limits<int>::max();
    int minChunkZ = std::numeric_limits<int>::max();
    int maxChunkX = std::numeric_limits<int>::lowest();
    int maxChunkY = std::numeric_limits<int>::lowest();
    int maxChunkZ = std::numeric_limits<int>::lowest();
    for (const auto& [coord, unusedChunk] : m_workingChunks) {
        (void)unusedChunk;
        minChunkX = std::min(minChunkX, coord.x);
        minChunkY = std::min(minChunkY, coord.y);
        minChunkZ = std::min(minChunkZ, coord.z);
        maxChunkX = std::max(maxChunkX, coord.x);
        maxChunkY = std::max(maxChunkY, coord.y);
        maxChunkZ = std::max(maxChunkZ, coord.z);
    }

    const int chunkSpanX = (maxChunkX - minChunkX) + 1;
    const int chunkSpanY = (maxChunkY - minChunkY) + 1;
    const int chunkSpanZ = (maxChunkZ - minChunkZ) + 1;
    const int dimX = chunkSpanX * kChunkEdge;
    const int dimY = chunkSpanY * kChunkEdge;
    const int dimZ = chunkSpanZ * kChunkEdge;
    const std::size_t total = static_cast<std::size_t>(dimX) * static_cast<std::size_t>(dimY) * static_cast<std::size_t>(dimZ);
    if (total == 0U) {
        return true;
    }
    m_redistanceStats.voxelsConsidered = static_cast<uint64_t>(total);

    std::vector<uint8_t> insideMask(total, 0U);
    std::vector<uint8_t> interfaceMask(total, 0U);
    std::vector<float> distanceToOutsideSq(total, kEdtInf);
    std::vector<float> distanceToInsideSq(total, kEdtInf);
    std::vector<float> distanceToInterfaceSq(total, kEdtInf);

    auto denseIndexForChunkLocal = [&](const ChunkCoord& coord, int lx, int ly, int lz) -> std::size_t {
        const int dx = ((coord.x - minChunkX) * kChunkEdge) + lx;
        const int dy = ((coord.y - minChunkY) * kChunkEdge) + ly;
        const int dz = ((coord.z - minChunkZ) * kChunkEdge) + lz;
        return flatDenseIndex(dx, dy, dz, dimX, dimY);
    };

    auto sampleInsideWorkingOrWorld = [&](int gx, int gy, int gz) -> bool {
        const ChunkCoord coord{
            floorDiv(gx, kChunkEdge),
            floorDiv(gy, kChunkEdge),
            floorDiv(gz, kChunkEdge),
        };
        const int lx = floorMod(gx, kChunkEdge);
        const int ly = floorMod(gy, kChunkEdge);
        const int lz = floorMod(gz, kChunkEdge);
        const auto workingIt = m_workingChunks.find(coord);
        if (workingIt != m_workingChunks.end()) {
            return workingIt->second.cells[VoxelWorld::flatIndex(lx, ly, lz)].phi < kIsoValue;
        }
        const VoxelChunk* worldChunk = m_world.findChunk(coord);
        if (worldChunk == nullptr) {
            return false;
        }
        return worldChunk->cells[VoxelWorld::flatIndex(lx, ly, lz)].phi < kIsoValue;
    };

    constexpr std::array<std::array<int, 3>, 6> kOffsets = {
        std::array<int, 3>{1, 0, 0},
        std::array<int, 3>{-1, 0, 0},
        std::array<int, 3>{0, 1, 0},
        std::array<int, 3>{0, -1, 0},
        std::array<int, 3>{0, 0, 1},
        std::array<int, 3>{0, 0, -1},
    };

    uint64_t interfaceCellCount = 0;
    for (const auto& [coord, chunk] : m_workingChunks) {
        for (std::size_t i = 0; i < chunk.cells.size(); ++i) {
            int lx = 0;
            int ly = 0;
            int lz = 0;
            VoxelWorld::unpackIndex(i, lx, ly, lz);
            const std::size_t dense = denseIndexForChunkLocal(coord, lx, ly, lz);
            const bool inside = chunk.cells[i].phi < kIsoValue;
            insideMask[dense] = inside ? 1U : 0U;
            distanceToOutsideSq[dense] = inside ? kEdtInf : 0.0F;
            distanceToInsideSq[dense] = inside ? 0.0F : kEdtInf;
        }
    }

    for (const auto& [coord, chunk] : m_workingChunks) {
        for (std::size_t i = 0; i < chunk.cells.size(); ++i) {
            int lx = 0;
            int ly = 0;
            int lz = 0;
            VoxelWorld::unpackIndex(i, lx, ly, lz);
            const std::size_t dense = denseIndexForChunkLocal(coord, lx, ly, lz);
            const bool inside = insideMask[dense] != 0U;
            const int gx = (coord.x * kChunkEdge) + lx;
            const int gy = (coord.y * kChunkEdge) + ly;
            const int gz = (coord.z * kChunkEdge) + lz;
            bool interfaceCell = false;
            for (const auto& d : kOffsets) {
                if (sampleInsideWorkingOrWorld(gx + d[0], gy + d[1], gz + d[2]) != inside) {
                    interfaceCell = true;
                    break;
                }
            }
            if (!interfaceCell) {
                continue;
            }
            interfaceMask[dense] = 1U;
            distanceToInterfaceSq[dense] = 0.0F;
            ++interfaceCellCount;
        }
    }

    {
        std::ostringstream oss;
        oss << "{\"dim\":[" << dimX << "," << dimY << "," << dimZ
            << "],\"totalVoxels\":" << total
            << ",\"workingChunks\":" << m_workingChunks.size()
            << ",\"interfaceCells\":" << interfaceCellCount << "}";
        // #region agent log
        appendDebugLog(
            "pre-fix",
            "H1_H2",
            "engine/voxel/transaction.cpp:applyLocalRedistance",
            "Redistance domain and interface coverage",
            oss.str());
        // #endregion
    }

    edt3dSquared(distanceToOutsideSq, dimX, dimY, dimZ);
    edt3dSquared(distanceToInsideSq, dimX, dimY, dimZ);
    edt3dSquared(distanceToInterfaceSq, dimX, dimY, dimZ);

    const float voxelStep = m_world.voxelSize();
    const int bandVoxels = m_highFidelityRedistanceEnabled
                               ? kDefaultRedistanceNarrowBandVoxels
                               : kLowFidelityRedistanceNarrowBandVoxels;
    const float bandLimit = static_cast<float>(bandVoxels);
    const float bandLimitSq = bandLimit * bandLimit;
    uint64_t bandUpdated = 0;
    for (auto& [coord, chunk] : m_workingChunks) {
        uint32_t active = 0;
        for (std::size_t i = 0; i < chunk.cells.size(); ++i) {
            int lx = 0;
            int ly = 0;
            int lz = 0;
            VoxelWorld::unpackIndex(i, lx, ly, lz);
            const std::size_t dense = denseIndexForChunkLocal(coord, lx, ly, lz);
            const bool inside = insideMask[dense] != 0U;

            if (distanceToInterfaceSq[dense] <= bandLimitSq) {
                const float sourceDistanceSq = inside ? distanceToOutsideSq[dense] : distanceToInsideSq[dense];
                const float distanceVoxels = std::max(0.0F, std::sqrt(std::max(sourceDistanceSq, 0.0F)) - 0.5F);
                const float signedDistance = distanceVoxels * voxelStep;
                chunk.cells[i].phi = inside ? -signedDistance : signedDistance;
                chunk.cells[i].material.initialized = true;
                ++bandUpdated;
            }
            if (chunk.cells[i].phi < kIsoValue) {
                ++active;
            }
        }
        chunk.activeVoxelCount = active;
    }
    m_redistanceStats.narrowBandVoxelsUpdated = bandUpdated;
    {
        std::ostringstream oss;
        oss << "{\"highFidelity\":" << (m_highFidelityRedistanceEnabled ? 1 : 0)
            << ",\"bandVoxels\":" << bandVoxels
            << ",\"bandLimitSq\":" << bandLimitSq
            << ",\"updatedVoxels\":" << bandUpdated
            << ",\"voxelsConsidered\":" << total << "}";
        // #region agent log
        appendDebugLog(
            "pre-fix",
            "H1_H2",
            "engine/voxel/transaction.cpp:applyLocalRedistance",
            "Narrow-band redistance update coverage",
            oss.str());
        // #endregion
    }
    const auto end = std::chrono::steady_clock::now();
    m_redistanceStats.computeMs = std::chrono::duration<double, std::milli>(end - start).count();
    return true;
}

bool VoxelTransaction::apply() {
    if (!m_open) {
        return false;
    }
    for (auto& [coord, chunk] : m_workingChunks) {
        (void)coord;
        if (!applyShapeSdfToChunk(chunk, m_command)) {
            return false;
        }
    }
    if (!applyLocalRedistance()) {
        return false;
    }
    if (SideEffectOutbox* box = m_sideEffects.writable(); box != nullptr) {
        box->completedJobs.push_back("edit_apply");
        box->cacheOps.push_back("invalidate_dirty_region");
    }
    return true;
}

bool VoxelTransaction::checkChunkInvariants(const VoxelChunk& chunk, std::vector<InvariantFailure>& failures) const {
    uint32_t active = 0;
    for (std::size_t i = 0; i < chunk.cells.size(); ++i) {
        const VoxelCell& cell = chunk.cells[i];
        if (!std::isfinite(cell.phi)) {
            failures.push_back({InvariantCode::InvalidPhi, chunk.coord.x, chunk.coord.y, chunk.coord.z, "Found non-finite phi"});
            return false;
        }
        if (!cell.material.initialized) {
            failures.push_back(
                {InvariantCode::UninitializedMaterial, chunk.coord.x, chunk.coord.y, chunk.coord.z, "Material field is uninitialized"});
            return false;
        }
        if (cell.phi < kIsoValue) {
            ++active;
        }
    }
    if (active != chunk.activeVoxelCount) {
        failures.push_back(
            {InvariantCode::ActiveCountMismatch, chunk.coord.x, chunk.coord.y, chunk.coord.z, "Chunk active voxel count mismatch"});
        return false;
    }
    return true;
}

std::optional<VoxelCell> VoxelTransaction::cellFromWorld(const ChunkCoord& coord, int localX, int localY, int localZ) const {
    const VoxelChunk* chunk = m_world.findChunk(coord);
    if (chunk == nullptr) {
        return std::nullopt;
    }
    const std::size_t idx = VoxelWorld::flatIndex(localX, localY, localZ);
    return chunk->cells[idx];
}

std::optional<VoxelCell> VoxelTransaction::cellFromWorkingOrWorld(const ChunkCoord& coord, int localX, int localY, int localZ) const {
    const auto it = m_workingChunks.find(coord);
    if (it != m_workingChunks.end()) {
        const std::size_t idx = VoxelWorld::flatIndex(localX, localY, localZ);
        return it->second.cells[idx];
    }
    return cellFromWorld(coord, localX, localY, localZ);
}

bool VoxelTransaction::checkSeamInvariants(std::vector<InvariantFailure>& failures) const {
    for (const auto& [coord, chunk] : m_workingChunks) {
        const ChunkCoord nx = neighborCoord(coord, 1, 0, 0);
        const ChunkCoord ny = neighborCoord(coord, 0, 1, 0);
        const ChunkCoord nz = neighborCoord(coord, 0, 0, 1);
        const std::array<ChunkCoord, 3> neighbors = {nx, ny, nz};
        for (const ChunkCoord& neighbor : neighbors) {
            for (int i = 0; i < kChunkEdge; ++i) {
                for (int j = 0; j < kChunkEdge; ++j) {
                    std::optional<VoxelCell> a;
                    std::optional<VoxelCell> b;
                    if (neighbor.x == coord.x + 1) {
                        a = cellFromWorkingOrWorld(coord, kChunkEdge - 1, i, j);
                        b = cellFromWorkingOrWorld(neighbor, 0, i, j);
                    } else if (neighbor.y == coord.y + 1) {
                        a = cellFromWorkingOrWorld(coord, i, kChunkEdge - 1, j);
                        b = cellFromWorkingOrWorld(neighbor, i, 0, j);
                    } else {
                        a = cellFromWorkingOrWorld(coord, i, j, kChunkEdge - 1);
                        b = cellFromWorkingOrWorld(neighbor, i, j, 0);
                    }
                    if (!a.has_value() || !b.has_value()) {
                        continue;
                    }
                    if (isInside(a->phi) != isInside(b->phi)) {
                        failures.push_back(
                            {InvariantCode::SeamOccupancyMismatch, coord.x, coord.y, coord.z, "Seam occupancy mismatch at chunk border"});
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

bool VoxelTransaction::commitChecks(std::vector<InvariantFailure>& failures) const {
    if (!m_open) {
        failures.push_back({InvariantCode::VersionParityMismatch, 0, 0, 0, "Commit checks called without open transaction"});
        return false;
    }
    for (const auto& [coord, chunk] : m_workingChunks) {
        (void)coord;
        if (!checkChunkInvariants(chunk, failures)) {
            return false;
        }
    }
    if (!checkSeamInvariants(failures)) {
        return false;
    }
    const VersionFence v = m_world.versions();
    if (v.meshProducedVersion > v.topologyVersion || v.collisionVersion > v.topologyVersion ||
        v.meshVisibleVersion > v.meshProducedVersion) {
        failures.push_back({InvariantCode::VersionParityMismatch, 0, 0, 0, "Version fence parity violation"});
        return false;
    }
    return true;
}

TransactionResult VoxelTransaction::commit() {
    TransactionResult result{};
    result.metrics = m_metrics;
    if (!m_open) {
        return result;
    }
    std::vector<InvariantFailure> failures;
    if (!commitChecks(failures)) {
        result.failures = failures;
        rollback();
        return result;
    }

    for (const auto& [coord, working] : m_workingChunks) {
        VoxelChunk& dst = m_world.ensureChunk(coord);
        dst = working;
        m_world.markChunkDirty(coord);
        result.dirtyChunks.push_back(coord);
    }
    m_world.incrementTopologyVersion();
    result.committedTopologyVersion = m_world.versions().topologyVersion;

    if (SideEffectOutbox* box = m_sideEffects.writable(); box != nullptr) {
        box->completedJobs.push_back("commit_publish");
        box->targetTopologyVersion = result.committedTopologyVersion;
    }
    m_sideEffects.markComplete();
    result.success = true;

    m_workingChunks.clear();
    m_open = false;
    m_targetVersion = 0;
    return result;
}

void VoxelTransaction::rollback() {
    m_workingChunks.clear();
    m_metrics = {};
    m_open = false;
    m_targetVersion = 0;
    m_sideEffects.discard();
}

std::vector<ChunkCoord> VoxelTransaction::touchedChunks() const {
    std::vector<ChunkCoord> chunks;
    chunks.reserve(m_workingChunks.size());
    for (const auto& [coord, chunk] : m_workingChunks) {
        (void)chunk;
        chunks.push_back(coord);
    }
    return chunks;
}

}  // namespace oro::voxel
