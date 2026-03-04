#include "voxel/worker_runtime.h"

#include <chrono>
#include <utility>

namespace oro::voxel {

AsyncWorkerRuntime::AsyncWorkerRuntime() = default;

AsyncWorkerRuntime::~AsyncWorkerRuntime() {
    stop();
}

void AsyncWorkerRuntime::start() {
    std::scoped_lock lock(m_mutex);
    if (m_running) {
        return;
    }
    m_running = true;
    m_remeshThread = std::thread(&AsyncWorkerRuntime::remeshThreadMain, this);
    m_collisionThread = std::thread(&AsyncWorkerRuntime::collisionThreadMain, this);
}

void AsyncWorkerRuntime::stop() {
    {
        std::scoped_lock lock(m_mutex);
        if (!m_running) {
            return;
        }
        m_running = false;
    }
    m_cv.notify_all();
    if (m_remeshThread.joinable()) {
        m_remeshThread.join();
    }
    if (m_collisionThread.joinable()) {
        m_collisionThread.join();
    }
}

void AsyncWorkerRuntime::setQueueCapacity(std::size_t capacity) {
    std::scoped_lock lock(m_mutex);
    m_queueCapacity = capacity == 0 ? 1 : capacity;
}

void AsyncWorkerRuntime::setLatestRequiredMeshVersion(uint64_t version) {
    m_latestRequiredMeshVersion.store(version);
}

void AsyncWorkerRuntime::setLatestRequiredCollisionVersion(uint64_t version) {
    m_latestRequiredCollisionVersion.store(version);
}

void AsyncWorkerRuntime::setCurrentEpoch(uint64_t epoch) {
    m_currentEpoch.store(epoch);
}

void AsyncWorkerRuntime::enqueueRemesh(JobSnapshotHandle snapshot, VersionToken token) {
    if (!snapshot || token.version == 0) {
        return;
    }
    std::scoped_lock lock(m_mutex);
    if (m_remeshQueue.size() >= m_queueCapacity) {
        m_remeshQueue.pop_front();
        ++m_telemetry.remeshDropped;
    }
    m_remeshQueue.push_back({std::move(snapshot), token});
    ++m_telemetry.remeshQueued;
    m_cv.notify_all();
}

void AsyncWorkerRuntime::enqueueCollision(JobSnapshotHandle snapshot, VersionToken token) {
    if (!snapshot || token.version == 0) {
        return;
    }
    std::scoped_lock lock(m_mutex);
    if (m_collisionQueue.size() >= m_queueCapacity) {
        m_collisionQueue.pop_front();
        ++m_telemetry.collisionDropped;
    }
    m_collisionQueue.push_back({std::move(snapshot), token});
    ++m_telemetry.collisionQueued;
    m_cv.notify_all();
}

std::optional<RemeshJobResult> AsyncWorkerRuntime::popRemeshResult() {
    std::scoped_lock lock(m_mutex);
    if (m_remeshResults.empty()) {
        return std::nullopt;
    }
    RemeshJobResult result = std::move(m_remeshResults.front());
    m_remeshResults.pop_front();
    return result;
}

std::optional<CollisionJobResult> AsyncWorkerRuntime::popCollisionResult() {
    std::scoped_lock lock(m_mutex);
    if (m_collisionResults.empty()) {
        return std::nullopt;
    }
    CollisionJobResult result = std::move(m_collisionResults.front());
    m_collisionResults.pop_front();
    return result;
}

WorkerTelemetry AsyncWorkerRuntime::telemetry() const {
    std::scoped_lock lock(m_mutex);
    return m_telemetry;
}

void AsyncWorkerRuntime::remeshThreadMain() {
    for (;;) {
        JobEntry entry;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [this] { return !m_running || !m_remeshQueue.empty(); });
            if (!m_running && m_remeshQueue.empty()) {
                return;
            }
            entry = std::move(m_remeshQueue.front());
            m_remeshQueue.pop_front();
        }
        JobSnapshotHandle snapshot = std::move(entry.snapshot);
        if (!snapshot) {
            continue;
        }
        const uint64_t currentEpoch = m_currentEpoch.load();
        if (entry.token.rollbackEpoch != currentEpoch) {
            std::scoped_lock lock(m_mutex);
            ++m_telemetry.remeshStale;
            m_remeshResults.push_back({entry.token, {}, true, true, 0.0});
            continue;
        }
        const uint64_t required = m_latestRequiredMeshVersion.load();
        if (snapshot->topologyVersion < required) {
            std::scoped_lock lock(m_mutex);
            ++m_telemetry.remeshStale;
            m_remeshResults.push_back({entry.token, {}, true, false, 0.0});
            continue;
        }
        const auto start = std::chrono::steady_clock::now();
        MeshPatchBatch patchBatch = m_mesher.remeshSnapshotPatches(*snapshot);
        const auto end = std::chrono::steady_clock::now();
        const double computeMs = std::chrono::duration<double, std::milli>(end - start).count();
        std::scoped_lock lock(m_mutex);
        m_remeshResults.push_back({entry.token, std::move(patchBatch), false, false, computeMs});
    }
}

void AsyncWorkerRuntime::collisionThreadMain() {
    for (;;) {
        JobEntry entry;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [this] { return !m_running || !m_collisionQueue.empty(); });
            if (!m_running && m_collisionQueue.empty()) {
                return;
            }
            entry = std::move(m_collisionQueue.front());
            m_collisionQueue.pop_front();
        }
        JobSnapshotHandle snapshot = std::move(entry.snapshot);
        if (!snapshot) {
            continue;
        }
        const uint64_t currentEpoch = m_currentEpoch.load();
        if (entry.token.rollbackEpoch != currentEpoch) {
            std::scoped_lock lock(m_mutex);
            ++m_telemetry.collisionStale;
            m_collisionResults.push_back({entry.token, false, true, true, 0.0});
            continue;
        }
        const uint64_t required = m_latestRequiredCollisionVersion.load();
        if (snapshot->topologyVersion < required) {
            std::scoped_lock lock(m_mutex);
            ++m_telemetry.collisionStale;
            m_collisionResults.push_back({entry.token, false, true, false, 0.0});
            continue;
        }
        const auto start = std::chrono::steady_clock::now();
        uint64_t activeCellSum = 0;
        for (const auto& [coord, chunk] : snapshot->immutableChunks) {
            (void)coord;
            activeCellSum += chunk.activeVoxelCount;
        }
        const bool ready = activeCellSum > 0 || !snapshot->meshTargetChunks.empty();
        const auto end = std::chrono::steady_clock::now();
        const double computeMs = std::chrono::duration<double, std::milli>(end - start).count();
        std::scoped_lock lock(m_mutex);
        m_collisionResults.push_back({entry.token, ready, false, false, computeMs});
    }
}

}  // namespace oro::voxel
